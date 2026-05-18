/**
 * Aligned Tools Client for SenseCAP Watcher + LiveKit SDK
 *
 * Handles device authentication and LiveKit connection setup.
 */

#include <esp_http_client.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>
#include <cJSON.h>

#include "aligned_client.h"

static const char *TAG = "aligned_client";

// NVS namespace for Aligned configuration
#define ALIGNED_NVS_NAMESPACE "aligned"
#define ALIGNED_NVS_TOKEN_KEY "device_token"
#define ALIGNED_NVS_API_URL_KEY "api_url"

// Global buffer for device token (stored in NVS)
static char g_aligned_device_token[256] = {0};

// Global buffer for API URL override (stored in NVS, empty = use default)
static char g_aligned_api_url[256] = {0};
static bool g_api_url_loaded = false;

// Credentials received from Aligned API
static char g_livekit_url[256] = {0};
static char g_livekit_token[2048] = {0};
static char g_room_name[128] = {0};
static char g_voice_provider[32] = "xai";
static char g_session_id[64] = {0};

// Connection state tracking
static bool g_aligned_connected = false;

// HTTP response buffer (keep off stack to avoid overflow)
static char g_http_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

/**
 * Load device token from NVS storage
 */
static esp_err_t aligned_load_token_from_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ALIGNED_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found, token not set");
        return err;
    }

    size_t required_size = sizeof(g_aligned_device_token);
    err = nvs_get_str(handle, ALIGNED_NVS_TOKEN_KEY, g_aligned_device_token, &required_size);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded device token from NVS (length: %zu)", strlen(g_aligned_device_token));
    } else {
        ESP_LOGW(TAG, "No device token in NVS");
    }

    return err;
}

/**
 * Save device token to NVS storage
 */
static esp_err_t aligned_save_token_to_nvs(const char *token) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ALIGNED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, ALIGNED_NVS_TOKEN_KEY, token);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save token: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Device token saved to NVS");
    }

    return err;
}

/**
 * Set device token (from CLI or programmatically)
 */
void aligned_set_device_token(const char *token) {
    if (token == NULL || strlen(token) == 0) {
        ESP_LOGE(TAG, "Invalid token");
        return;
    }

    strncpy(g_aligned_device_token, token, sizeof(g_aligned_device_token) - 1);
    g_aligned_device_token[sizeof(g_aligned_device_token) - 1] = '\0';

    aligned_save_token_to_nvs(token);
    ESP_LOGI(TAG, "Device token set (length: %zu)", strlen(token));
}

/**
 * Get current device token
 */
const char* aligned_get_device_token(void) {
    if (strlen(g_aligned_device_token) == 0) {
        aligned_load_token_from_nvs();
    }
    return g_aligned_device_token;
}

/**
 * Check if device token is configured
 */
bool aligned_has_token(void) {
    const char *token = aligned_get_device_token();
    return token != NULL && strlen(token) > 0;
}

/**
 * Load API URL override from NVS storage
 */
static esp_err_t aligned_load_api_url_from_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ALIGNED_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = sizeof(g_aligned_api_url);
    err = nvs_get_str(handle, ALIGNED_NVS_API_URL_KEY, g_aligned_api_url, &required_size);
    nvs_close(handle);

    if (err == ESP_OK && strlen(g_aligned_api_url) > 0) {
        ESP_LOGI(TAG, "Using custom API URL: %s", g_aligned_api_url);
    }

    return err;
}

/**
 * Set API URL override (for local development)
 */
void aligned_set_api_url(const char *url) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ALIGNED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    if (url == NULL || strlen(url) == 0) {
        // Clear the override, use default
        nvs_erase_key(handle, ALIGNED_NVS_API_URL_KEY);
        g_aligned_api_url[0] = '\0';
        ESP_LOGI(TAG, "API URL reset to default: %s", ALIGNED_API_BASE_DEFAULT);
    } else {
        strncpy(g_aligned_api_url, url, sizeof(g_aligned_api_url) - 1);
        g_aligned_api_url[sizeof(g_aligned_api_url) - 1] = '\0';
        nvs_set_str(handle, ALIGNED_NVS_API_URL_KEY, g_aligned_api_url);
        ESP_LOGI(TAG, "API URL set to: %s", g_aligned_api_url);
    }

    nvs_commit(handle);
    nvs_close(handle);
    g_api_url_loaded = true;
}

/**
 * Get effective API base URL (NVS override or default)
 */
const char* aligned_get_api_base(void) {
    // Load from NVS if not already loaded
    if (!g_api_url_loaded) {
        aligned_load_api_url_from_nvs();
        g_api_url_loaded = true;
    }

    // Return override if set, otherwise default
    if (strlen(g_aligned_api_url) > 0) {
        return g_aligned_api_url;
    }
    return ALIGNED_API_BASE_DEFAULT;
}

/**
 * HTTP event handler for Aligned API requests
 */
static esp_err_t aligned_http_event_handler(esp_http_client_event_t *evt) {
    static int output_len = 0;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->user_data) {
                int copy_len = evt->data_len;
                if (output_len + copy_len < (MAX_HTTP_OUTPUT_BUFFER - 1)) {
                    memcpy(((char *)evt->user_data) + output_len, evt->data, copy_len);
                    output_len += copy_len;
                    ((char *)evt->user_data)[output_len] = '\0';
                }
            }
            break;

        case HTTP_EVENT_ON_FINISH:
        case HTTP_EVENT_DISCONNECTED:
            output_len = 0;
            break;

        default:
            break;
    }

    return ESP_OK;
}

/**
 * Get LiveKit credentials from Aligned API
 *
 * POST /api/py/watcher/device/connect
 * Body: {"device_token": "watcher_xxx..."}
 *
 * Response:
 * {
 *   "success": true,
 *   "livekit_url": "wss://...",
 *   "room_name": "watcher_room_xxx",
 *   "participant_token": "jwt...",
 *   "voice_provider": "xai",
 *   "session_id": "xxx"
 * }
 */
esp_err_t aligned_get_livekit_credentials(void) {
    if (!aligned_has_token()) {
        ESP_LOGE(TAG, "No device token configured");
        return ESP_ERR_INVALID_STATE;
    }

    memset(g_http_response_buffer, 0, sizeof(g_http_response_buffer));
    char url[512];
    const char *api_base = aligned_get_api_base();
    snprintf(url, sizeof(url), "%s%s", api_base, ALIGNED_WATCHER_CONNECT);

    ESP_LOGI(TAG, "Requesting LiveKit credentials from %s...", api_base);
    ESP_LOGI(TAG, "URL: %s", url);

    // Prepare request body
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "device_token", g_aligned_device_token);
    char *request_body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = aligned_http_event_handler,
        .user_data = g_http_response_buffer,
        .timeout_ms = 10000
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, request_body, strlen(request_body));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    free(request_body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "API returned error status: %d", status_code);
        ESP_LOGE(TAG, "Response: %s", g_http_response_buffer);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_cleanup(client);

    // Parse JSON response
    cJSON *response = cJSON_Parse(g_http_response_buffer);
    if (response == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }

    cJSON *success = cJSON_GetObjectItem(response, "success");
    if (!cJSON_IsTrue(success)) {
        ESP_LOGE(TAG, "API returned success=false");
        cJSON_Delete(response);
        return ESP_FAIL;
    }

    // Extract credentials
    cJSON *livekit_url = cJSON_GetObjectItem(response, "livekit_url");
    cJSON *room_name = cJSON_GetObjectItem(response, "room_name");
    cJSON *participant_token = cJSON_GetObjectItem(response, "participant_token");
    cJSON *voice_provider = cJSON_GetObjectItem(response, "voice_provider");
    cJSON *session_id = cJSON_GetObjectItem(response, "session_id");

    if (livekit_url && cJSON_IsString(livekit_url)) {
        strncpy(g_livekit_url, livekit_url->valuestring, sizeof(g_livekit_url) - 1);
        g_livekit_url[sizeof(g_livekit_url) - 1] = '\0';
    }
    if (room_name && cJSON_IsString(room_name)) {
        strncpy(g_room_name, room_name->valuestring, sizeof(g_room_name) - 1);
        g_room_name[sizeof(g_room_name) - 1] = '\0';
    }
    if (participant_token && cJSON_IsString(participant_token)) {
        strncpy(g_livekit_token, participant_token->valuestring, sizeof(g_livekit_token) - 1);
        g_livekit_token[sizeof(g_livekit_token) - 1] = '\0';
    }
    if (voice_provider && cJSON_IsString(voice_provider)) {
        strncpy(g_voice_provider, voice_provider->valuestring, sizeof(g_voice_provider) - 1);
        g_voice_provider[sizeof(g_voice_provider) - 1] = '\0';
    }
    if (session_id && cJSON_IsString(session_id)) {
        strncpy(g_session_id, session_id->valuestring, sizeof(g_session_id) - 1);
        g_session_id[sizeof(g_session_id) - 1] = '\0';
    }

    cJSON_Delete(response);

    ESP_LOGI(TAG, "✅ LiveKit credentials received:");
    ESP_LOGI(TAG, "   - URL: %s", g_livekit_url);
    ESP_LOGI(TAG, "   - Room: %s", g_room_name);
    ESP_LOGI(TAG, "   - Voice Provider: %s ($0.05/min)", g_voice_provider);
    ESP_LOGI(TAG, "   - Session: %s", g_session_id);

    // Mark as connected so caller knows we have credentials
    g_aligned_connected = true;

    return ESP_OK;
}

/**
 * Get LiveKit URL for WebRTC connection
 */
const char* aligned_get_livekit_url(void) {
    return g_livekit_url;
}

/**
 * Get LiveKit participant token
 */
const char* aligned_get_livekit_token(void) {
    return g_livekit_token;
}

/**
 * Get room name
 */
const char* aligned_get_room_name(void) {
    return g_room_name;
}

/**
 * Get session ID
 */
const char* aligned_get_session_id(void) {
    return g_session_id;
}

/**
 * Get participant token (alias for aligned_get_livekit_token)
 */
const char* aligned_get_participant_token(void) {
    return g_livekit_token;
}

/**
 * Connect to Aligned backend and get LiveKit credentials
 * Returns true if successful, false otherwise
 */
bool aligned_connect(const char* device_token) {
    if (device_token && strlen(device_token) > 0) {
        aligned_set_device_token(device_token);
    }

    esp_err_t err = aligned_get_livekit_credentials();
    g_aligned_connected = (err == ESP_OK && strlen(g_livekit_url) > 0);

    return g_aligned_connected;
}

/**
 * Check if connected to Aligned (has valid credentials)
 */
bool aligned_is_connected(void) {
    return g_aligned_connected && strlen(g_livekit_url) > 0;
}
