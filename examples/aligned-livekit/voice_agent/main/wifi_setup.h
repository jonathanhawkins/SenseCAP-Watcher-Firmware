/**
 * @file wifi_setup.h
 * @brief WiFi status and setup UI screen for ESP32 SenseCAP Watcher
 *
 * Provides a WiFi status page that shows:
 * - Current connection status (SSID, IP, signal strength) when connected
 * - "Setup WiFi" prompt when disconnected
 * - Navigation to network scanning/selection
 *
 * Uses the Page Manager pattern for screen transitions and supports
 * both touch and encoder navigation on the 412x412 round LCD.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the WiFi setup UI module
 *
 * Creates the WiFi status screen and sets up event handlers.
 * Safe to call multiple times (will no-op if already initialized).
 *
 * @note Must be called after LVGL, Page Manager, and WiFi scan module are initialized
 */
void wifi_setup_init(void);

/**
 * @brief Deinitialize the WiFi setup module
 *
 * Cleans up the WiFi setup screen and frees resources.
 */
void wifi_setup_deinit(void);

/**
 * @brief Show the WiFi status/setup screen
 *
 * Displays the WiFi status page:
 * - If connected: shows current SSID, IP address, and signal strength
 *   with a "Change Network" button
 * - If not connected: shows "Not connected" with a "Scan Networks" button
 *
 * Uses the Page Manager to handle screen transitions.
 */
void wifi_setup_show(void);

/**
 * @brief Hide the WiFi setup screen and return to previous UI
 *
 * Uses the Page Manager to return to the previous screen.
 */
void wifi_setup_hide(void);

/**
 * @brief Check if WiFi setup screen is currently visible
 *
 * @return true if the WiFi setup screen is the active screen
 * @return false otherwise
 */
bool wifi_setup_is_visible(void);

/**
 * @brief Refresh the WiFi status display
 *
 * Updates the connection status, SSID, IP address, and signal strength
 * on the WiFi setup screen. Call this when connection state changes.
 *
 * @note Only updates if the WiFi setup screen is currently visible
 */
void wifi_setup_refresh(void);

#ifdef __cplusplus
}
#endif
