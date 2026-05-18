/**
 * @file wifi_list.h
 * @brief WiFi network list UI for ESP32 SenseCAP Watcher
 *
 * Provides a scrollable list of available WiFi networks that users can
 * select from. Integrates with the WiFi scan module for network discovery
 * and supports both touch and encoder navigation.
 *
 * Features:
 * - Scrollable list of scanned WiFi networks
 * - Signal strength indicators
 * - Security status icons (locked/open)
 * - Highlights currently connected network
 * - Encoder navigation with focus styling
 *
 * Uses the Page Manager pattern for screen transitions, matching the
 * volume_control.c and wifi_setup.c patterns.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the WiFi network list module
 *
 * Creates the network list screen and sets up event handlers.
 * Safe to call multiple times (will no-op if already initialized).
 *
 * @note Must be called after LVGL, Page Manager, and WiFi scan module are initialized
 */
void wifi_list_init(void);

/**
 * @brief Deinitialize the WiFi list module
 *
 * Cleans up the WiFi list screen and frees resources.
 */
void wifi_list_deinit(void);

/**
 * @brief Show the network list screen
 *
 * Displays a scrollable list of available WiFi networks.
 * Automatically triggers a WiFi scan when shown.
 *
 * The list shows:
 * - Network SSID with WiFi icon
 * - Signal strength (dBm)
 * - Lock icon for secured networks
 * - Green highlight for currently connected network
 *
 * Uses the Page Manager to handle screen transitions.
 */
void wifi_list_show(void);

/**
 * @brief Hide the network list and return to WiFi status
 *
 * Uses the Page Manager to return to the previous screen
 * (typically the WiFi setup/status screen).
 */
void wifi_list_hide(void);

/**
 * @brief Check if list screen is visible
 *
 * @return true if the WiFi list screen is the active screen
 * @return false otherwise
 */
bool wifi_list_is_visible(void);

/**
 * @brief Refresh the network list
 *
 * Updates the list with current scan results.
 * Call this after a WiFi scan completes.
 *
 * @note Only updates if the list screen is currently visible
 */
void wifi_list_refresh(void);

/**
 * @brief Get the currently selected SSID
 *
 * Returns the SSID of the network that was last selected by the user.
 * This is used by the password keyboard screen (Phase 4) to know
 * which network to connect to.
 *
 * @return Pointer to null-terminated SSID string
 * @return Empty string ("") if no network selected
 */
const char* wifi_list_get_selected_ssid(void);

/**
 * @brief Check if the selected network requires a password
 *
 * @return true if the selected network is secured (WEP/WPA/WPA2/WPA3)
 * @return false if the network is open or no network selected
 */
bool wifi_list_selected_needs_password(void);

#ifdef __cplusplus
}
#endif
