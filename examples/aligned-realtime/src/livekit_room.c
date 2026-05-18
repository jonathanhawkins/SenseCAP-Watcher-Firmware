/**
 * LiveKit Room Connection for Aligned Tools
 *
 * Uses the official LiveKit ESP32 SDK for proper signaling and WebRTC.
 * This replaces the WHIP-based implementation with proper SDK integration.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "livekit.h"
#include "media.h"
#include "board.h"
#include "aligned_client.h"
#include "ui/ui.h"

static const char *TAG = "livekit_room";

static livekit_room_handle_t room_handle = NULL;
static bool agent_joined = false;
static bool g_voice_active = false;

/// Invoked when the room's connection state changes.
static void on_state_changed(livekit_connection_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "Room state changed: %s", livekit_connection_state_str(state));

    switch (state)
    {
        case LIVEKIT_CONNECTION_STATE_CONNECTED:
            ESP_LOGI(TAG, "Connected to LiveKit room!");
            g_voice_active = true;
            ui_switch_speaking();
            break;
        case LIVEKIT_CONNECTION_STATE_CONNECTING:
        case LIVEKIT_CONNECTION_STATE_RECONNECTING:
            ui_wifi_connecting();
            break;
        case LIVEKIT_CONNECTION_STATE_DISCONNECTED:
            g_voice_active = false;
            ui_listening();
            break;
        case LIVEKIT_CONNECTION_STATE_FAILED:
            ESP_LOGE(TAG, "Connection failed!");
            g_voice_active = false;
            ui_listening();
            break;
    }

    livekit_failure_reason_t reason = livekit_room_get_failure_reason(room_handle);
    if (reason != LIVEKIT_FAILURE_REASON_NONE)
    {
        ESP_LOGE(TAG, "Failure reason: %s", livekit_failure_reason_str(reason));
    }
}

/// Invoked when participant information is received.
static void on_participant_info(const livekit_participant_info_t *info, void *ctx)
{
    if (info->kind != LIVEKIT_PARTICIPANT_KIND_AGENT)
    {
        return;
    }
    bool joined = false;
    switch (info->state)
    {
        case LIVEKIT_PARTICIPANT_STATE_ACTIVE:
            joined = true;
            break;
        case LIVEKIT_PARTICIPANT_STATE_DISCONNECTED:
            joined = false;
            break;
        default:
            return;
    }
    if (joined != agent_joined)
    {
        ESP_LOGI(TAG, "Agent has %s the room", joined ? "joined" : "left");
        agent_joined = joined;
    }
}

/// RPC handler for CPU temperature
static void get_cpu_temp(const livekit_rpc_invocation_t *invocation, void *ctx)
{
    // Return a placeholder temperature
    livekit_rpc_return_ok("40.0");
}

/**
 * Join LiveKit room using SDK signaling
 */
void join_room(void)
{
    if (room_handle != NULL)
    {
        ESP_LOGE(TAG, "Room already created");
        return;
    }

    livekit_room_options_t room_options = {
        .publish = {
            .kind = LIVEKIT_MEDIA_TYPE_AUDIO,
            .audio_encode = {
                .codec = LIVEKIT_AUDIO_CODEC_OPUS,
                .sample_rate = 48000,
                .channel_count = 1
            },
            .capturer = media_get_capturer()
        },
        .subscribe = {
            .kind = LIVEKIT_MEDIA_TYPE_AUDIO,
            .renderer = media_get_renderer()
        },
        .on_state_changed = on_state_changed,
        .on_participant_info = on_participant_info
    };

    if (livekit_room_create(&room_handle, &room_options) != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to create room");
        return;
    }

    // Register RPC handlers
    livekit_room_rpc_register(room_handle, "get_cpu_temp", get_cpu_temp);

    // Get credentials from Aligned backend
    ESP_LOGI(TAG, "Fetching LiveKit credentials from Aligned backend...");
    esp_err_t creds_err = aligned_get_livekit_credentials();
    if (creds_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get LiveKit credentials from Aligned backend");
        livekit_room_destroy(room_handle);
        room_handle = NULL;
        return;
    }

    const char *livekit_url = aligned_get_livekit_url();
    const char *livekit_token = aligned_get_livekit_token();

    if (!livekit_url || strlen(livekit_url) == 0 || !livekit_token || strlen(livekit_token) == 0)
    {
        ESP_LOGE(TAG, "Invalid credentials received from backend");
        livekit_room_destroy(room_handle);
        room_handle = NULL;
        return;
    }

    ESP_LOGI(TAG, "Connecting to LiveKit room: %s", aligned_get_room_name());
    ESP_LOGI(TAG, "Voice provider: xAI Grok Realtime ($0.05/min)");

    // Connect using LiveKit SDK signaling
    livekit_err_t connect_res = livekit_room_connect(room_handle, livekit_url, livekit_token);

    if (connect_res != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to connect to room");
        livekit_room_destroy(room_handle);
        room_handle = NULL;
    }
}

/**
 * Leave LiveKit room with proper cleanup for reconnection
 */
void leave_room(void)
{
    if (room_handle == NULL)
    {
        ESP_LOGI(TAG, "Room not active, nothing to leave");
        return;
    }

    ESP_LOGI(TAG, "Leaving room - starting clean disconnect...");
    g_voice_active = false;

    // Step 1: Stop media streams first to prevent audio glitches
    ESP_LOGI(TAG, "Step 1: Stopping media streams...");
    media_stop();

    // Step 2: Close the room (sends leave signal and waits for disconnect)
    ESP_LOGI(TAG, "Step 2: Closing room connection...");
    livekit_err_t close_err = livekit_room_close(room_handle);
    if (close_err != LIVEKIT_ERR_NONE)
    {
        ESP_LOGW(TAG, "Room close returned error: %d (continuing cleanup)", close_err);
    }

    // Step 3: Wait for the disconnect to complete
    // The engine processes EV_CMD_CLOSE asynchronously
    ESP_LOGI(TAG, "Step 3: Waiting for clean disconnect...");
    int wait_count = 0;
    const int max_wait = 30;  // 3 seconds max wait (30 * 100ms)
    while (wait_count < max_wait)
    {
        livekit_connection_state_t state = livekit_room_get_state(room_handle);
        if (state == LIVEKIT_CONNECTION_STATE_DISCONNECTED ||
            state == LIVEKIT_CONNECTION_STATE_FAILED)
        {
            ESP_LOGI(TAG, "Disconnect confirmed after %d ms", wait_count * 100);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }

    if (wait_count >= max_wait)
    {
        ESP_LOGW(TAG, "Disconnect wait timeout - forcing cleanup");
    }

    // Step 4: Destroy the room handle
    ESP_LOGI(TAG, "Step 4: Destroying room handle...");
    livekit_err_t destroy_err = livekit_room_destroy(room_handle);
    if (destroy_err != LIVEKIT_ERR_NONE)
    {
        ESP_LOGW(TAG, "Room destroy returned error: %d", destroy_err);
    }
    room_handle = NULL;

    // Step 5: Reset media for next connection
    ESP_LOGI(TAG, "Step 5: Resetting media systems...");
    if (media_reset() != 0)
    {
        ESP_LOGE(TAG, "Failed to reset media systems - reconnection may fail");
    }

    // Step 6: Reset agent state
    agent_joined = false;

    ESP_LOGI(TAG, "Room cleanup complete - ready for reconnection");
    ui_listening();
}

/**
 * Check if room is active
 */
bool room_is_active(void)
{
    return room_handle != NULL;
}

/**
 * Check if voice is currently active
 */
bool livekit_is_active(void)
{
    return g_voice_active;
}

/**
 * Stop LiveKit connection (alias for leave_room)
 */
void livekit_stop(void)
{
    leave_room();
}
