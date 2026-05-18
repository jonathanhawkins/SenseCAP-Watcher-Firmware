/**
 * LiveKit WebRTC Connection for Aligned Tools
 *
 * Connects to LiveKit room using WHIP (WebRTC-HTTP Ingestion Protocol)
 * This enables voice communication with xAI Grok Realtime via the Aligned backend.
 *
 * Flow:
 * 1. Get credentials from aligned_client.cpp (URL, room, JWT token)
 * 2. Convert WSS LiveKit URL to HTTPS WHIP endpoint
 * 3. Create WebRTC offer and POST to WHIP endpoint with JWT auth
 * 4. Set remote description from WHIP response
 * 5. Stream audio bidirectionally
 */

#ifndef LINUX_BUILD
#include <driver/i2s.h>
#include <opus.h>
#endif

#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <string.h>

#include "main.h"

#define LIVEKIT_TICK_INTERVAL 15

// Global peer connection for LiveKit
static PeerConnection *livekit_peer_connection = NULL;

// Flag to track if voice is active
static bool g_voice_active = false;

#ifndef LINUX_BUILD
static StaticTask_t livekit_task_buffer;

/**
 * Audio sending task - runs continuously when connected
 */
static void livekit_send_audio_task(void *user_data) {
    ESP_LOGI(LOG_TAG, "Audio task starting, initializing encoder...");

    if (!oai_init_audio_encoder()) {
        ESP_LOGE(LOG_TAG, "Failed to initialize audio encoder! Audio task aborting.");
        g_voice_active = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(LOG_TAG, "Audio encoder initialized, starting send loop");

    int packet_count = 0;
    while (g_voice_active) {
        if (!oai_send_audio(livekit_peer_connection)) {
            ESP_LOGE(LOG_TAG, "Audio send failed, stopping task");
            break;
        }
        packet_count++;
        if (packet_count % 100 == 0) {
            ESP_LOGI(LOG_TAG, "Audio packets sent: %d", packet_count);
        }
        vTaskDelay(pdMS_TO_TICKS(LIVEKIT_TICK_INTERVAL));
    }

    ESP_LOGI(LOG_TAG, "Audio sending task stopped after %d packets", packet_count);
    vTaskDelete(NULL);
}
#endif

/**
 * Connection state change handler
 */
static void livekit_onconnectionstatechange(PeerConnectionState state, void *user_data) {
    ESP_LOGI(LOG_TAG, "LiveKit PeerConnectionState: %s", peer_connection_state_to_string(state));

    if (state == PEER_CONNECTION_DISCONNECTED || state == PEER_CONNECTION_CLOSED) {
        ESP_LOGW(LOG_TAG, "LiveKit connection lost");
        g_voice_active = false;
        ui_listening();  // Return to listening state
#ifndef LINUX_BUILD
        // Don't restart immediately - let user reconnect manually
        // esp_restart();
#endif
    } else if (state == PEER_CONNECTION_FAILED) {
        ESP_LOGE(LOG_TAG, "LiveKit connection failed");
        g_voice_active = false;
        ui_listening();
    } else if (state == PEER_CONNECTION_CONNECTED) {
        ESP_LOGI(LOG_TAG, "LiveKit connected! Starting audio stream...");
        g_voice_active = true;

#ifndef LINUX_BUILD
        // Start audio sending task FIRST (before UI which can block)
        ESP_LOGI(LOG_TAG, "Allocating audio task stack from SPIRAM...");
        StackType_t *stack_memory = (StackType_t *)heap_caps_malloc(
            40000 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);

        if (stack_memory) {
            ESP_LOGI(LOG_TAG, "Creating audio task...");
            xTaskCreateStaticPinnedToCore(livekit_send_audio_task, "lk_audio", 40000,
                                          NULL, 7, stack_memory, &livekit_task_buffer, 0);
            ESP_LOGI(LOG_TAG, "Audio task started successfully");
        } else {
            ESP_LOGE(LOG_TAG, "Failed to allocate audio task stack from SPIRAM");
            g_voice_active = false;
        }
#endif
        // Update UI AFTER starting audio (this can be slow/blocking)
        ESP_LOGI(LOG_TAG, "Updating UI to speaking state...");
        ui_switch_speaking();
        ESP_LOGI(LOG_TAG, "CONNECTED callback complete");
    }
}

/**
 * HTTP response buffer for WHIP endpoint
 */
static char g_whip_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
static int g_whip_output_len = 0;

// Large buffers moved to static to avoid stack overflow in voice_task (8KB stack)
static char g_whip_url[512] = {0};
static char g_auth_header[2048] = {0};
static char g_local_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};

/**
 * HTTP event handler for WHIP request
 */
static esp_err_t livekit_whip_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->user_data) {
                int copy_len = evt->data_len;
                if (g_whip_output_len + copy_len < (MAX_HTTP_OUTPUT_BUFFER - 1)) {
                    memcpy(((char *)evt->user_data) + g_whip_output_len, evt->data, copy_len);
                    g_whip_output_len += copy_len;
                    ((char *)evt->user_data)[g_whip_output_len] = '\0';
                }
            }
            break;

        case HTTP_EVENT_ON_FINISH:
        case HTTP_EVENT_DISCONNECTED:
            // Reset for next request
            break;

        case HTTP_EVENT_ERROR:
            ESP_LOGE(LOG_TAG, "WHIP HTTP error");
            break;

        default:
            break;
    }

    return ESP_OK;
}

/**
 * Send SDP offer to LiveKit WHIP endpoint and get answer
 *
 * WHIP endpoint format: https://{host}/rtc/whip?token={jwt}
 *
 * Request:
 *   POST /rtc/whip?token=<jwt>
 *   Content-Type: application/sdp
 *   Authorization: Bearer <jwt>
 *   Body: SDP offer
 *
 * Response:
 *   201 Created
 *   Content-Type: application/sdp
 *   Body: SDP answer
 */
static void livekit_whip_request(char *offer, char *answer) {
    const char *whip_url_from_api = aligned_get_whip_url();
    const char *whip_token_from_api = aligned_get_whip_token();
    const char *livekit_url = aligned_get_livekit_url();
    const char *participant_token = aligned_get_participant_token();

    // Use WHIP URL from API if available (preferred for LiveKit Cloud)
    memset(g_whip_url, 0, sizeof(g_whip_url));

    if (whip_url_from_api && strlen(whip_url_from_api) > 0) {
        // Use direct WHIP URL from ingress API
        if (whip_token_from_api && strlen(whip_token_from_api) > 0) {
            // Stream key format: URL/stream_key
            snprintf(g_whip_url, sizeof(g_whip_url), "%s/%s", whip_url_from_api, whip_token_from_api);
        } else {
            strncpy(g_whip_url, whip_url_from_api, sizeof(g_whip_url) - 1);
        }
        ESP_LOGI(LOG_TAG, "Using WHIP URL from API: %s", g_whip_url);
    } else {
        // Fallback: construct from LiveKit WSS URL (for self-hosted LiveKit)
        if (!livekit_url || strlen(livekit_url) == 0) {
            ESP_LOGE(LOG_TAG, "No LiveKit URL configured");
            return;
        }
        if (!participant_token || strlen(participant_token) == 0) {
            ESP_LOGE(LOG_TAG, "No LiveKit token configured");
            return;
        }

        // Convert WSS URL to HTTPS WHIP endpoint
        // wss://host.livekit.cloud -> https://host.livekit.cloud/rtc/whip?token=...
        const char *host_start = livekit_url;
        if (strncmp(host_start, "wss://", 6) == 0) {
            host_start += 6;
        } else if (strncmp(host_start, "ws://", 5) == 0) {
            host_start += 5;
        }
        snprintf(g_whip_url, sizeof(g_whip_url), "https://%s/rtc/whip?token=%s", host_start, participant_token);
        ESP_LOGI(LOG_TAG, "Constructed WHIP URL: https://%s/rtc/whip?token=<redacted>", host_start);
    }

    // Reset response buffer
    memset(g_whip_response_buffer, 0, sizeof(g_whip_response_buffer));
    g_whip_output_len = 0;

    // Configure HTTP client
    esp_http_client_config_t config = {};
    config.url = g_whip_url;
    config.event_handler = livekit_whip_event_handler;
    config.user_data = g_whip_response_buffer;
    config.timeout_ms = 15000;  // 15 second timeout for WHIP
    config.buffer_size = MAX_HTTP_OUTPUT_BUFFER;
    config.buffer_size_tx = MAX_HTTP_OUTPUT_BUFFER;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(LOG_TAG, "Failed to create HTTP client");
        return;
    }

    // Set up WHIP request
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/sdp");

    // Authorization header - only needed for fallback mode (not ingress)
    // WHIP ingress uses stream_key in URL, not Bearer token
    if (!whip_url_from_api && participant_token && strlen(participant_token) > 0) {
        memset(g_auth_header, 0, sizeof(g_auth_header));
        snprintf(g_auth_header, sizeof(g_auth_header), "Bearer %s", participant_token);
        esp_http_client_set_header(client, "Authorization", g_auth_header);
    }

    // Set SDP offer as body
    size_t offer_len = strlen(offer);
    esp_http_client_set_post_field(client, offer, offer_len);

    ESP_LOGI(LOG_TAG, "Sending WHIP offer to LiveKit (offer size: %d bytes)...", (int)offer_len);
    ESP_LOGI(LOG_TAG, "SDP offer preview: %.200s...", offer);

    // Perform the request
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    if (err != ESP_OK) {
        ESP_LOGE(LOG_TAG, "WHIP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    // WHIP returns 201 Created on success
    if (status_code != 201 && status_code != 200) {
        ESP_LOGE(LOG_TAG, "WHIP error status: %d", status_code);
        ESP_LOGE(LOG_TAG, "Response: %.200s...", g_whip_response_buffer);
        esp_http_client_cleanup(client);
        return;
    }

    ESP_LOGI(LOG_TAG, "WHIP response received (status: %d, len: %d)", status_code, g_whip_output_len);

    // Debug: Print first 100 chars of response
    if (g_whip_output_len > 0) {
        int debug_len = g_whip_output_len > 100 ? 100 : g_whip_output_len;
        ESP_LOGI(LOG_TAG, "WHIP response preview: %.*s", debug_len, g_whip_response_buffer);
        ESP_LOGI(LOG_TAG, "First bytes: 0x%02x 0x%02x 0x%02x 0x%02x",
                 (uint8_t)g_whip_response_buffer[0],
                 (uint8_t)g_whip_response_buffer[1],
                 (uint8_t)g_whip_response_buffer[2],
                 (uint8_t)g_whip_response_buffer[3]);
    }

    // Copy SDP answer to output buffer using memcpy to avoid truncation warning
    size_t copy_len = strlen(g_whip_response_buffer);
    if (copy_len >= MAX_HTTP_OUTPUT_BUFFER) {
        copy_len = MAX_HTTP_OUTPUT_BUFFER - 1;
    }
    memcpy(answer, g_whip_response_buffer, copy_len);
    answer[copy_len] = '\0';

    esp_http_client_cleanup(client);
}

/**
 * ICE candidate handler - sends SDP to WHIP endpoint
 */
static void livekit_on_icecandidate(char *description, void *user_data) {
    // Use global buffer to avoid stack overflow
    memset(g_local_buffer, 0, sizeof(g_local_buffer));

    ESP_LOGI(LOG_TAG, "ICE candidate generated, sending to LiveKit WHIP...");

    livekit_whip_request(description, g_local_buffer);

    if (strlen(g_local_buffer) > 0) {
        ESP_LOGI(LOG_TAG, "Setting remote description from WHIP response");
        peer_connection_set_remote_description(livekit_peer_connection, g_local_buffer);
    } else {
        ESP_LOGE(LOG_TAG, "Empty WHIP response - connection may fail");
    }
}

/**
 * Start LiveKit WebRTC connection
 *
 * Prerequisites: aligned_connect() must have been called successfully
 * to obtain LiveKit credentials (URL, room, token)
 */
void livekit_webrtc(void) {
    // Verify we have credentials
    if (!aligned_is_connected()) {
        ESP_LOGE(LOG_TAG, "Not connected to Aligned. Call aligned_connect first.");
        return;
    }

    const char *livekit_url = aligned_get_livekit_url();
    const char *room_name = aligned_get_room_name();

    if (!livekit_url || strlen(livekit_url) == 0) {
        ESP_LOGE(LOG_TAG, "No LiveKit URL available");
        return;
    }

    ESP_LOGI(LOG_TAG, "Starting LiveKit WebRTC connection...");
    ESP_LOGI(LOG_TAG, "  Room: %s", room_name);
    ESP_LOGI(LOG_TAG, "  URL: %s", livekit_url);

    // Configure peer connection for LiveKit
    // LiveKit uses standard ICE servers (STUN/TURN are handled by LiveKit infrastructure)
    PeerConfiguration peer_connection_config = {
        .ice_servers = {
            // LiveKit provides its own TURN servers, but we can add Google STUN as fallback
            {.urls = "stun:stun.l.google.com:19302", .username = NULL, .credential = NULL},
        },
        .audio_codec = CODEC_OPUS,
        .video_codec = CODEC_NONE,
        .datachannel = DATA_CHANNEL_NONE,
        .onaudiotrack = [](uint8_t *data, size_t size, void *userdata) -> void {
#ifndef LINUX_BUILD
            // Decode incoming audio from LiveKit (xAI agent response)
            oai_audio_decode(data, size);
#endif
        },
        .onvideotrack = NULL,
        .on_request_keyframe = NULL,
        .user_data = NULL,
    };

    // Create peer connection
    livekit_peer_connection = peer_connection_create(&peer_connection_config);
    if (livekit_peer_connection == NULL) {
        ESP_LOGE(LOG_TAG, "Failed to create LiveKit peer connection");
        return;
    }

    // Set up callbacks
    peer_connection_oniceconnectionstatechange(livekit_peer_connection, livekit_onconnectionstatechange);
    peer_connection_onicecandidate(livekit_peer_connection, livekit_on_icecandidate);

    // Create offer to start the connection process
    ESP_LOGI(LOG_TAG, "Creating WebRTC offer...");
    peer_connection_create_offer(livekit_peer_connection);

    // Run the peer connection loop
    ESP_LOGI(LOG_TAG, "Entering LiveKit peer connection loop...");
    while (g_voice_active || livekit_peer_connection != NULL) {
        if (livekit_peer_connection == NULL) {
            ESP_LOGI(LOG_TAG, "Peer connection destroyed, exiting loop");
            break;
        }
        peer_connection_loop(livekit_peer_connection);
        vTaskDelay(pdMS_TO_TICKS(LIVEKIT_TICK_INTERVAL));
    }

    ESP_LOGI(LOG_TAG, "LiveKit peer connection loop ended");
}

/**
 * Stop LiveKit WebRTC connection
 */
void livekit_stop(void) {
    g_voice_active = false;

    if (livekit_peer_connection) {
        ESP_LOGI(LOG_TAG, "Closing LiveKit connection...");
        peer_connection_close(livekit_peer_connection);
        peer_connection_destroy(livekit_peer_connection);
        livekit_peer_connection = NULL;
    }

    ui_listening();
}

/**
 * Check if LiveKit voice is currently active
 */
bool livekit_is_active(void) {
    return g_voice_active;
}

/**
 * Get the peer connection (for audio sending)
 */
PeerConnection* livekit_get_peer_connection(void) {
    return livekit_peer_connection;
}
