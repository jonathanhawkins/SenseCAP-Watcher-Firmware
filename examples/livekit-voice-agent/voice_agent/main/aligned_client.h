/**
 * Aligned Tools Client for SenseCAP Watcher + LiveKit SDK
 *
 * Handles device authentication and LiveKit connection setup.
 * Fetches dynamic credentials from Aligned backend API.
 */

#ifndef ALIGNED_CLIENT_H
#define ALIGNED_CLIENT_H

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configuration defaults
// Production server (used if no NVS override is set)
#define ALIGNED_API_BASE_DEFAULT "https://aligned.tools"
#define ALIGNED_WATCHER_CONNECT "/api/py/watcher/connect"
#define MAX_HTTP_OUTPUT_BUFFER 4096

/**
 * Set device token (from CLI or programmatically)
 * Token is saved to NVS for persistence across reboots
 */
void aligned_set_device_token(const char *token);

/**
 * Get current device token from memory (loads from NVS if not cached)
 */
const char* aligned_get_device_token(void);

/**
 * Check if device token is configured
 */
bool aligned_has_token(void);

/**
 * Set API URL override (for local development)
 * Pass NULL or empty string to reset to production default
 * URL is saved to NVS for persistence across reboots
 */
void aligned_set_api_url(const char *url);

/**
 * Get effective API base URL (NVS override or default)
 */
const char* aligned_get_api_base(void);

/**
 * Get LiveKit credentials from Aligned backend
 *
 * Makes HTTP POST to /api/py/watcher/device/connect with device_token.
 * Stores credentials in internal buffers for getter functions.
 *
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
esp_err_t aligned_get_livekit_credentials(void);

/**
 * Connect to Aligned backend and fetch LiveKit credentials
 *
 * @param device_token Device authentication token (optional if already set)
 * @return true if credentials were successfully fetched, false otherwise
 */
bool aligned_connect(const char* device_token);

/**
 * Check if connected (has valid LiveKit credentials)
 */
bool aligned_is_connected(void);

/**
 * Get LiveKit WebSocket URL (e.g., "wss://aligned-tools-xxx.livekit.cloud")
 */
const char* aligned_get_livekit_url(void);

/**
 * Get LiveKit participant JWT token
 */
const char* aligned_get_livekit_token(void);

/**
 * Get LiveKit participant token (alias for aligned_get_livekit_token)
 */
const char* aligned_get_participant_token(void);

/**
 * Get LiveKit room name
 */
const char* aligned_get_room_name(void);

/**
 * Get session ID
 */
const char* aligned_get_session_id(void);

#ifdef __cplusplus
}
#endif

#endif // ALIGNED_CLIENT_H
