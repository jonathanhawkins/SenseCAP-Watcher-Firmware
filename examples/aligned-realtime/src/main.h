#ifndef MAIN_H
#define MAIN_H

#include "sensecap-watcher.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "ui/ui.h"

#define LOG_TAG "aligned-voice"
#define MAX_HTTP_OUTPUT_BUFFER 4096

// Aligned Tools API endpoints
// Default: Production server (works for all users)
// Override: Use 'aligned_api' command to set local dev URL (stored in NVS)
#define ALIGNED_API_BASE_DEFAULT "https://aligned.tools"
#define ALIGNED_WATCHER_CONNECT "/api/py/watcher/connect"

// WiFi functions
void oai_wifi(void);
void oai_wifi_init(void);

// Audio is now handled by media.c (ESP audio framework)
// Legacy OpenAI audio functions removed - no longer using libpeer

// LiveKit Room functions (SDK-based, proper signaling)
// Implemented in livekit_room.c
void join_room(void);           // Join LiveKit room
void leave_room(void);          // Leave LiveKit room
bool room_is_active(void);      // Check if room is active
bool livekit_is_active(void);   // Check if voice is active
void livekit_stop(void);        // Stop LiveKit connection (alias for leave_room)

// Board initialization (implemented in board.c)
void board_init(void);

// Aligned-specific functions (implemented in aligned_client.cpp)
void aligned_set_device_token(const char *token);
const char* aligned_get_device_token(void);
void aligned_set_api_url(const char *url);  // Set local dev URL (NULL = reset to production)
const char* aligned_get_api_base(void);     // Get effective API base URL
const char* aligned_get_room_name(void);
const char* aligned_get_livekit_url(void);
const char* aligned_get_participant_token(void);
const char* aligned_get_livekit_token(void);
const char* aligned_get_session_id(void);
bool aligned_connect(const char* device_token);
bool aligned_is_connected(void);
bool aligned_has_token(void);
esp_err_t aligned_get_livekit_credentials(void);

// CLI commands
int cmd_init(void);

#ifdef __cplusplus
}
#endif

#endif // MAIN_H
