/**
 * @file qr_setup.h
 * @brief QR Code Setup Screen for Device Registration
 *
 * Displays QR codes for device registration flow:
 * 1. Device ID QR - User scans to register on web
 * 2. Token QR scanner - Device scans token QR from web
 */

#ifndef QR_SETUP_H
#define QR_SETUP_H

#include "lvgl.h"
#include <stdbool.h>

/**
 * @brief Initialize the QR setup screen
 *
 * Creates the setup screen with device ID QR code.
 * Must be called after LVGL is initialized.
 */
void qr_setup_init(void);

/**
 * @brief Show the QR setup screen
 *
 * Displays the screen with device registration QR code.
 * Uses the overlay pattern - doesn't affect navigation stack.
 */
void qr_setup_show(void);

/**
 * @brief Hide the QR setup screen
 *
 * Closes the QR setup overlay and returns to previous screen.
 */
void qr_setup_hide(void);

/**
 * @brief Check if QR setup screen is currently visible
 */
bool qr_setup_is_visible(void);

/**
 * @brief Get the device MAC address as a string
 *
 * @param buf Buffer to store MAC string (at least 18 bytes: XX:XX:XX:XX:XX:XX + null)
 * @param buf_len Size of the buffer
 * @return Pointer to buf on success, NULL on error
 */
char *qr_setup_get_mac_address(char *buf, size_t buf_len);

/**
 * @brief Get the device ID for registration
 *
 * Returns a unique identifier for this device based on MAC address.
 * Format: "WATCHER_XXXXXXXXXXXX" where X is MAC without colons.
 *
 * @param buf Buffer to store device ID (at least 32 bytes)
 * @param buf_len Size of the buffer
 * @return Pointer to buf on success, NULL on error
 */
char *qr_setup_get_device_id(char *buf, size_t buf_len);

/**
 * @brief Process a scanned QR code
 *
 * Called when camera successfully scans a QR code.
 * Parses the QR data and extracts the device token if valid.
 *
 * @param qr_data The scanned QR code data string
 * @return true if token was successfully extracted and stored
 */
bool qr_setup_process_scanned_qr(const char *qr_data);

#endif // QR_SETUP_H
