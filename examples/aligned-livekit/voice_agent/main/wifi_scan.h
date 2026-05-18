/**
 * @file wifi_scan.h
 * @brief WiFi scanning and connection management for ESP32 SenseCAP Watcher
 *
 * Provides a simple API for scanning WiFi networks, checking connection status,
 * and managing WiFi connections. Uses static memory allocation for embedded use.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of access points to store from a scan */
#define WIFI_SCAN_MAX_AP 40

/** Maximum SSID length (32 chars + null terminator) */
#define WIFI_SSID_MAX_LEN 33

/** Maximum password length (63 chars + null terminator) */
#define WIFI_PASSWORD_MAX_LEN 64

/** Maximum IP address string length */
#define WIFI_IP_MAX_LEN 16

/**
 * @brief Authentication mode for a WiFi network
 */
typedef enum {
    WIFI_SCAN_AUTH_OPEN = 0,      /**< Open network (no password) */
    WIFI_SCAN_AUTH_WEP = 1,       /**< WEP (legacy, insecure) */
    WIFI_SCAN_AUTH_WPA = 2,       /**< WPA/WPA2/WPA3 (requires password) */
} wifi_scan_auth_t;

/**
 * @brief Result for a single scanned WiFi network
 */
typedef struct {
    char ssid[WIFI_SSID_MAX_LEN];  /**< Network name (null-terminated) */
    int8_t rssi;                    /**< Signal strength in dBm (-100 to 0, higher is better) */
    wifi_scan_auth_t authmode;      /**< Authentication type */
    bool is_connected;              /**< True if currently connected to this network */
} wifi_scan_result_t;

/**
 * @brief WiFi connection state
 */
typedef enum {
    WIFI_STATE_DISCONNECTED = 0,  /**< Not connected to any network */
    WIFI_STATE_CONNECTING,        /**< Connection in progress */
    WIFI_STATE_CONNECTED,         /**< Connected and has IP address */
    WIFI_STATE_FAILED,            /**< Connection attempt failed */
} wifi_connection_state_t;

/**
 * @brief Initialize WiFi scanning module
 *
 * Must be called before using any other wifi_scan functions.
 * Safe to call multiple times (will no-op if already initialized).
 *
 * @note Requires NVS and default event loop to be initialized first
 */
void wifi_scan_init(void);

/**
 * @brief Start an asynchronous WiFi scan
 *
 * Initiates a WiFi scan in the background. Check completion with
 * wifi_scan_is_complete() and retrieve results with wifi_scan_get_results().
 *
 * @note Can be called while connected to a network
 * @note Previous scan results are cleared when a new scan starts
 */
void wifi_scan_start(void);

/**
 * @brief Check if the current scan has completed
 *
 * @return true if scan is complete or no scan in progress
 * @return false if scan is still running
 */
bool wifi_scan_is_complete(void);

/**
 * @brief Get the number of networks found in the last scan
 *
 * @return Number of networks found (0 to WIFI_SCAN_MAX_AP)
 */
uint16_t wifi_scan_get_count(void);

/**
 * @brief Get scan results from the last completed scan
 *
 * Returns a pointer to an internal array of scan results, sorted by
 * signal strength (strongest first). The currently connected network
 * (if any) will have is_connected set to true.
 *
 * @return Pointer to array of wifi_scan_result_t, valid until next scan
 * @return NULL if no scan has been performed
 *
 * @note Results are sorted by RSSI (strongest signal first)
 * @note Array is valid until wifi_scan_start() is called again
 * @note For thread-safe access, use wifi_scan_copy_results() instead
 */
const wifi_scan_result_t* wifi_scan_get_results(void);

/**
 * @brief Copy scan results to a user-provided buffer (thread-safe)
 *
 * Copies scan results to the provided buffer with mutex protection.
 * This is the preferred way to access results when thread safety is needed.
 *
 * @param out_results Buffer to copy results into
 * @param max_count Maximum number of results to copy
 * @return Number of results actually copied
 */
uint16_t wifi_scan_copy_results(wifi_scan_result_t *out_results, uint16_t max_count);

/**
 * @brief Check if currently connected to a WiFi network
 *
 * @return true if connected with valid IP address
 * @return false if disconnected or connecting
 */
bool wifi_is_connected(void);

/**
 * @brief Get current WiFi connection state
 *
 * @return Current connection state
 */
wifi_connection_state_t wifi_get_state(void);

/**
 * @brief Get the SSID of the currently connected network
 *
 * @return Pointer to null-terminated SSID string
 * @return Empty string ("") if not connected
 *
 * @note String is valid until connection state changes
 */
const char* wifi_get_current_ssid(void);

/**
 * @brief Get the current IP address
 *
 * @return Pointer to IP address string (e.g., "192.168.1.100")
 * @return Empty string ("") if not connected
 *
 * @note String is valid until connection state changes
 */
const char* wifi_get_current_ip(void);

/**
 * @brief Get the reason code of the most recent WiFi disconnect
 *
 * Returns the `reason` field from the last WIFI_EVENT_STA_DISCONNECTED
 * event. Codes are defined in `esp_wifi_types_generic.h` (e.g. 2 = AUTH_EXPIRE,
 * 15 = 4WAY_HANDSHAKE_TIMEOUT, 201 = NO_AP_FOUND, 202 = AUTH_FAIL,
 * 205 = CONNECTION_FAIL). Returns 0 if no disconnect has occurred yet.
 *
 * @return Reason code of most recent disconnect, or 0 if none.
 */
int wifi_get_last_disconnect_reason(void);

/**
 * @brief Connect to a WiFi network
 *
 * Initiates connection to the specified network. Connection happens
 * asynchronously - check wifi_get_state() or wifi_is_connected() for status.
 *
 * Credentials are automatically saved to flash on successful connection.
 *
 * @param ssid Network name (max 32 characters)
 * @param password Network password (max 63 characters, NULL for open networks)
 * @return true if connection attempt started successfully
 * @return false on error (invalid parameters, WiFi not initialized)
 */
bool wifi_connect(const char* ssid, const char* password);

/**
 * @brief Disconnect from the current network
 *
 * Disconnects from the current WiFi network if connected.
 * Does not clear saved credentials.
 */
void wifi_disconnect(void);

/**
 * @brief Reset WiFi connection state (retry counter and state machine)
 *
 * Call this before manually reconnecting to ensure a clean state.
 * Resets retry counter to 0 and sets state to CONNECTING.
 */
void wifi_reset_connection_state(void);

/**
 * @brief Clear saved WiFi credentials from flash
 *
 * Removes the saved SSID and password from NVS storage.
 * Does not disconnect from current network.
 */
void wifi_clear_credentials(void);

/**
 * @brief Check if WiFi credentials are saved in flash
 *
 * @return true if credentials exist
 * @return false if no credentials saved
 */
bool wifi_has_saved_credentials(void);

/** Maximum number of saved network credentials (LRU eviction beyond this). */
#define WIFI_MAX_SAVED_NETWORKS 8

/**
 * @brief Look up a saved password for the given SSID.
 *
 * Searches the multi-network credential store (NVS namespace "wifi_creds").
 *
 * @param ssid Network name to look up
 * @param out_pass Buffer to receive the password (null-terminated)
 * @param out_len Size of out_pass buffer (including null terminator)
 * @return true if found and password copied into out_pass
 * @return false if SSID not found or arguments invalid
 */
bool wifi_get_saved_password(const char *ssid, char *out_pass, size_t out_len);

/**
 * @brief Save the SSID/password pair to flash.
 *
 * LRU semantics: the saved pair becomes slot 0 (most recently used). If the
 * SSID is already saved, its slot is promoted to 0 (older slots shift down).
 * If all WIFI_MAX_SAVED_NETWORKS slots are full and the SSID is new, the
 * oldest entry is evicted.
 *
 * @param ssid Network name (1..32 chars)
 * @param password Network password (NULL or empty for open networks)
 * @return true on successful NVS commit
 */
bool wifi_save_credentials_for_ssid(const char *ssid, const char *password);

/**
 * @brief Remove the SSID's saved entry, if present.
 *
 * @param ssid Network name to forget
 * @return true if an entry was removed and the commit succeeded
 * @return false if SSID not found
 */
bool wifi_forget_credentials_for_ssid(const char *ssid);

/**
 * @brief Copy up to `max` saved SSIDs into the supplied array.
 *
 * Slots are returned in LRU order (slot 0 = most recently used).
 *
 * @param ssids Destination array, each entry sized WIFI_SSID_MAX_LEN
 * @param max Maximum number of entries to copy
 * @return Number of SSIDs copied (0 to min(max, WIFI_MAX_SAVED_NETWORKS))
 */
int wifi_list_saved_ssids(char ssids[][WIFI_SSID_MAX_LEN], int max);

/**
 * @brief Quick check whether credentials are saved for the given SSID.
 *
 * Cheaper than wifi_get_saved_password() when the caller only needs to know
 * existence (e.g. to decide whether to render a "saved" badge on a scan row).
 *
 * @param ssid Network name to check
 * @return true if credentials are stored for this SSID
 */
bool wifi_has_saved_for_ssid(const char *ssid);

#ifdef __cplusplus
}
#endif
