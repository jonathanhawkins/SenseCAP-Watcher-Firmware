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

// Configuration
#define ALIGNED_API_BASE_DEFAULT "https://aligned.tools"  // Production server
#define ALIGNED_WATCHER_CONNECT "/api/py/watcher/connect"
#define ALIGNED_WATCHER_CLAIM_TOKEN "/api/py/watcher/claim-token"
#define ALIGNED_WATCHER_REFRESH "/api/py/watcher/refresh-token"
#define MAX_HTTP_OUTPUT_BUFFER 4096

/**
 * Set API server URL (saved to NVS)
 * Use "default" or empty string to reset to production server
 */
void aligned_set_server_url(const char *url);

/**
 * Get current API server URL
 */
const char* aligned_get_server_url(void);

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
 * Poll for device token using hardware ID
 *
 * This is used during QR-based device registration:
 * 1. Device displays QR code with its hardware_id (from MAC address)
 * 2. User scans QR → registers device on web → token stored in database
 * 3. Device polls this endpoint until token is available
 * 4. Once token is claimed, it's stored in NVS automatically
 *
 * @param hardware_id Device hardware ID (WATCHER_XXXXXXXXXXXX from MAC)
 * @return true if token was claimed and stored, false if not registered yet
 */
bool aligned_poll_for_token(const char *hardware_id);

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
 * Refresh (extend) the device token's expiration on the backend.
 *
 * POSTs to /api/py/watcher/refresh-token with the stored device_token. The
 * backend extends expires_at (default +90 days) ONLY while the token is still
 * valid (the refresh RPC refuses already-expired tokens). Call periodically
 * during active use so the 90-day token never lapses.
 *
 * Uses a dedicated response buffer + event handler, so it is safe to call
 * from a background task concurrently with aligned_get_livekit_credentials().
 *
 * @return ESP_OK if the backend confirmed the extension; ESP_ERR_INVALID_STATE
 *         if no token is configured; ESP_FAIL on HTTP/parse failure (e.g. a
 *         401 means the token already lapsed and must be re-provisioned).
 */
esp_err_t aligned_refresh_token(void);

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
 * Set the connect mode for the next aligned_get_livekit_credentials() call.
 * "voice" (default conversational agent) or "meeting" (silent live-meeting
 * transcription). Persists until changed.
 */
void aligned_set_connect_mode(const char *mode);

/**
 * Get the meeting id returned by the backend for a mode="meeting" connect.
 * Returns "" if the last connect was not a meeting.
 */
const char* aligned_get_meeting_id(void);

/**
 * Get LiveKit room name
 */
const char* aligned_get_room_name(void);

/**
 * Get session ID
 */
const char* aligned_get_session_id(void);

/**
 * Get a brief, user-readable string describing the most recent failure of
 * aligned_get_livekit_credentials(). Returns "" when there is no failure
 * to report. The returned string is a static const literal — do not free.
 */
const char* aligned_get_last_error_msg(void);

#ifdef __cplusplus
}
#endif

#endif // ALIGNED_CLIENT_H
