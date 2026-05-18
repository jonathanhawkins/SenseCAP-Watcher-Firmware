#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "cJSON.h"
#include "livekit.h"
#include "livekit_sandbox.h"
#include "media.h"
#include "board.h"
#include "example.h"
#include "aligned_client.h"
#include "ui.h"

static const char *TAG = "livekit_example";

static livekit_room_handle_t room_handle;
static bool agent_joined = false;
static bool s_leaving_room = false;  /* Re-entry guard for leave_room() */

/// Invoked when the room's connection state changes.
static void on_state_changed(livekit_connection_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "Room state changed: %s", livekit_connection_state_str(state));

    // Update UI based on connection state
    switch (state)
    {
        case LIVEKIT_CONNECTION_STATE_CONNECTED:
            ESP_LOGI(TAG, "✅ Connected to LiveKit room!");
            ui_listening();  // Show listening animation - ready for voice
            ui_set_voice_active(true);  // Update status bar: voice active
            break;
        case LIVEKIT_CONNECTION_STATE_CONNECTING:
        case LIVEKIT_CONNECTION_STATE_RECONNECTING:
            ui_wifi_connecting();  // Show connecting state
            break;
        case LIVEKIT_CONNECTION_STATE_DISCONNECTED:
            ui_set_voice_active(false);  // Update status bar: voice inactive
            ui_disconnecting();  // Show disconnected state
            break;
        case LIVEKIT_CONNECTION_STATE_FAILED:
            ESP_LOGE(TAG, "❌ Connection failed!");
            ui_set_voice_active(false);  // Update status bar: voice inactive
            ui_disconnecting();  // Show error state
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
        // Only handle agent participants for this example.
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

/// Invoked by a remote participant to set the state of an on-board LED.
static void set_led_state(const livekit_rpc_invocation_t *invocation, void *ctx)
{
    // LED control is currently disabled in this simplified firmware
    // as it requires specific BSP dependencies that are not included.
    // Use Watcher-specific LED code if needed later.
    if (invocation->payload == NULL)
    {
        livekit_rpc_return_error("Missing payload");
        return;
    }
    livekit_rpc_return_ok(NULL);
}

/// Invoked by a remote participant to get the current CPU temperature.
static void get_cpu_temp(const livekit_rpc_invocation_t *invocation, void *ctx)
{
    float temp = board_get_temp();
    char temp_string[16];
    snprintf(temp_string, sizeof(temp_string), "%.2f", temp);
    livekit_rpc_return_ok(temp_string);
}

void join_room()
{
    if (room_handle != NULL)
    {
        ESP_LOGE(TAG, "Room already created");
        return;
    }

    // Reinitialize media if it was cleaned up (e.g., after leaving a room)
    if (media_get_capturer() == NULL || media_get_renderer() == NULL)
    {
        ESP_LOGI(TAG, "Reinitializing media systems...");
        media_init();
    }

    livekit_room_options_t room_options
        = { .publish = { .kind = LIVEKIT_MEDIA_TYPE_AUDIO, .audio_encode = { .codec = LIVEKIT_AUDIO_CODEC_OPUS, .sample_rate = 48000, .channel_count = 1 }, .capturer = media_get_capturer() },
              .subscribe = { .kind = LIVEKIT_MEDIA_TYPE_AUDIO, .renderer = media_get_renderer() },
              .on_state_changed = on_state_changed,
              .on_participant_info = on_participant_info };
    if (livekit_room_create(&room_handle, &room_options) != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to create room");
        return;
    }

    // Register RPC handlers so they can be invoked by remote participants.
    livekit_room_rpc_register(room_handle, "set_led_state", set_led_state);
    livekit_room_rpc_register(room_handle, "get_cpu_temp", get_cpu_temp);

    // Get dynamic LiveKit credentials from Aligned backend
    ESP_LOGI(TAG, "Fetching LiveKit credentials from Aligned backend...");
    esp_err_t creds_err = aligned_get_livekit_credentials();
    if (creds_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get LiveKit credentials from Aligned backend");
        ESP_LOGE(TAG, "Make sure device token is set and backend is reachable");
        return;
    }

    const char *livekit_url = aligned_get_livekit_url();
    const char *livekit_token = aligned_get_livekit_token();

    if (!livekit_url || strlen(livekit_url) == 0 || !livekit_token || strlen(livekit_token) == 0)
    {
        ESP_LOGE(TAG, "Invalid credentials received from backend");
        return;
    }

    ESP_LOGI(TAG, "Connecting to LiveKit room: %s", aligned_get_room_name());
    ESP_LOGI(TAG, "Voice provider: %s ($0.05/min)", "xAI Grok Realtime");

    // Connect to LiveKit room using dynamic credentials
    livekit_err_t connect_res = livekit_room_connect(room_handle, livekit_url, livekit_token);

    if (connect_res != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to connect to room");
    }
}

void leave_room()
{
    if (room_handle == NULL)
    {
        ESP_LOGE(TAG, "Room not created");
        return;
    }

    /* Re-entry guard: prevent double leave_room() calls from crashing.
     * The button task and console commands can race. */
    if (s_leaving_room)
    {
        ESP_LOGW(TAG, "Already leaving room, ignoring duplicate request");
        return;
    }
    s_leaving_room = true;

    livekit_room_handle_t handle = room_handle;
    room_handle = NULL;  /* Clear immediately to prevent re-entry via room_is_active() */

    // Update UI: voice chat is no longer active
    ui_set_voice_active(false);

    // Clean up media systems FIRST to prevent GMF task hangs
    ESP_LOGI(TAG, "Stopping media pipelines...");
    media_cleanup();

    // Give pending media callbacks time to drain before closing the room.
    // This prevents the FreeRTOS mutex assertion crash (xQueueTakeMutexRecursive)
    // that occurs when livekit_room_close() races with in-flight media operations.
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Closing LiveKit room...");
    livekit_err_t close_err = livekit_room_close(handle);
    if (close_err != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to close room (err=%d), continuing with destroy...", close_err);
    }

    // Another small delay between close and destroy to let cleanup finish
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Destroying LiveKit room handle...");
    livekit_err_t destroy_err = livekit_room_destroy(handle);
    if (destroy_err != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to destroy room (err=%d)", destroy_err);
    }

    agent_joined = false;
    s_leaving_room = false;
    ESP_LOGI(TAG, "Room disconnected successfully");
}

bool room_is_active(void)
{
    return room_handle != NULL;
}
