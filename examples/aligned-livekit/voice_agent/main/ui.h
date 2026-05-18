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
 * "Hold knob to disconnect" hint at bottom of screen.
 *
 * @param active true when connected to voice agent, false when disconnected
 */
void ui_set_voice_active(bool active);

#endif // UI_H
