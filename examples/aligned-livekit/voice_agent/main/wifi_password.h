/**
 * @file wifi_password.h
 * @brief WiFi password entry UI with on-screen keyboard
 *
 * Provides a full on-screen keyboard for entering WiFi passwords on the
 * 412x412 round LCD display. Supports alphanumeric characters, uppercase,
 * numbers, and special characters.
 *
 * Uses LVGL's lv_keyboard widget with custom styling to match the device theme.
 * Supports both touch input and rotary encoder navigation.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the WiFi password module
 *
 * Creates the password entry screen with keyboard.
 * Safe to call multiple times (will no-op if already initialized).
 */
void wifi_password_init(void);

/**
 * @brief Deinitialize the WiFi password module
 *
 * Cleans up screen resources. Safe to call even if not initialized.
 */
void wifi_password_deinit(void);

/**
 * @brief Show the password entry screen for a network
 *
 * Opens the password entry screen with the keyboard.
 * Should be called after selecting a secured network from the list.
 *
 * @param ssid The SSID of the network to connect to
 */
void wifi_password_show(const char *ssid);

/**
 * @brief Hide the password entry screen
 *
 * Returns to the previous screen (network list or status).
 */
void wifi_password_hide(void);

/**
 * @brief Check if the password screen is visible
 *
 * @return true if currently showing the password entry screen
 * @return false otherwise
 */
bool wifi_password_is_visible(void);

/**
 * @brief Get the entered password
 *
 * Returns the password string entered by the user.
 *
 * @return Pointer to the password string (empty if none entered)
 * @note String is valid until next wifi_password_show() call
 */
const char* wifi_password_get_password(void);

/**
 * @brief Clear the entered password
 *
 * Clears the text field and resets the password buffer.
 */
void wifi_password_clear(void);

#ifdef __cplusplus
}
#endif
