#ifndef UI_H
#define UI_H

#include "lvgl.h"
#include <stdbool.h>

void ui_init(void);
void ui_switch_speaking(void);
void ui_listening(void);
void ui_wifi_connecting(void);
void ui_disconnecting(void);
void ui_powering_off(void);

/**
 * @brief Show WiFi setup button at bottom of screen
 *
 * Button appears when WiFi is disconnected, hidden when connected.
 * Automatically checks WiFi status every 2 seconds.
 * Also creates the status bar with WiFi/voice indicators.
 */
void ui_show_wifi_button(void);

/**
 * @brief Hide and cleanup WiFi button
 */
void ui_hide_wifi_button(void);

/**
 * @brief Set voice chat active state and update UI indicators
 *
 * When active, shows green voice icon in status bar and
 * "Hold knob to disconnect" hint just below the status bar.
 *
 * @param active true when connected to voice agent, false when disconnected
 */
void ui_set_voice_active(bool active);

/**
 * @brief Show a connection-failure overlay with a user-readable reason.
 *
 * Displays @p reason_text centered in red with a "Press knob to retry"
 * hint below. The status bar and WiFi button stay visible. The overlay
 * auto-clears the next time ui_wifi_connecting() or ui_listening() runs.
 *
 * @param reason_text Short user-facing string (≤24 chars recommended).
 */
void ui_connection_failed(const char *reason_text);

/**
 * @brief Clear any connection-failure overlay if one is showing.
 *
 * Safe to call regardless of state. Called automatically when the user
 * retries a connection.
 */
void ui_clear_connection_failure(void);

/**
 * @brief Indicate that the user is currently pressing/holding the knob.
 *
 * During an active voice session, swaps the hint text to "Hold to
 * disconnect..." in white so the user knows the press is registered.
 * No-op when not in a voice session. Pair with ui_knob_hold_end() on
 * the release edge.
 */
void ui_knob_hold_start(void);

/**
 * @brief Indicate the user released the knob.
 *
 * Reverts the hint to "Hold knob to disconnect" if the voice session
 * is still active and we're not already mid-disconnect.
 */
void ui_knob_hold_end(void);

#endif // UI_H
