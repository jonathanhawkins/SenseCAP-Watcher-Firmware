/**
 * @file volume_control.h
 * @brief Volume control with rotary encoder wheel support and HUD overlay
 *
 * This module provides runtime volume control via the rotary encoder wheel:
 * - Counter-clockwise rotation = volume UP
 * - Clockwise rotation = volume DOWN
 *
 * A visual HUD overlay shows the current volume level when adjusting.
 */

#ifndef VOLUME_CONTROL_H
#define VOLUME_CONTROL_H

#include "esp_err.h"
#include "sdkconfig.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Default speaker volume (0-100)
 */
#ifdef CONFIG_LK_EXAMPLE_SPEAKER_VOLUME
#define VOLUME_DEFAULT      CONFIG_LK_EXAMPLE_SPEAKER_VOLUME
#else
#define VOLUME_DEFAULT      100
#endif

/**
 * @brief Minimum speaker volume
 */
#define VOLUME_MIN          0

/**
 * @brief Maximum speaker volume
 *
 * 100 = the top of esp_codec_dev's default volume curve (0 dB). The board's
 * hw_gain (pa_voltage 5.0 / codec_dac_voltage 3.3 in board.c → -3.6 dB) then
 * lands the ES8311 DAC volume register (REG32) at 0xC6/+3.6 dB — the board's
 * *modeled* PA-saturation ceiling (the loudest the stock curve produces).
 *
 * NOTE: this is the safe/clean max, NOT the register max. REG32 goes to
 * 0xFF/+32 dB, but that range is past PA saturation → clipping + speaker
 * stress. To get more CLEAN output you'd raise the curve ceiling via
 * esp_codec_dev_set_vol_curve (each +0.5 dB = +1 register code) and verify
 * by ear against the real Watcher PA/speaker rating — the 5.0/3.3 V in
 * board.c are generic placeholders, so the true safe headroom is unknown.
 * Capping the wheel below 100 just left clean loudness unused.
 */
#define VOLUME_MAX          100

/**
 * @brief Volume step per encoder tick
 */
#define VOLUME_STEP         5

/**
 * @brief HUD display duration in milliseconds before auto-fade
 */
#define VOLUME_HUD_DISPLAY_MS   1500

/**
 * @brief Initialize the volume control module
 *
 * This sets the initial volume and prepares the HUD overlay.
 * Must be called after LVGL and the audio codec are initialized.
 *
 * @param encoder LVGL encoder input device for registering callbacks
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t volume_control_init(lv_indev_t *encoder);

/**
 * @brief Deinitialize the volume control module
 *
 * Cleans up the HUD overlay and unregisters callbacks.
 */
void volume_control_deinit(void);

/**
 * @brief Get the current volume level
 *
 * @return Current volume (0-100)
 */
int volume_control_get(void);

/**
 * @brief Set the volume level
 *
 * @param volume Volume level (0-100, will be clamped)
 * @return ESP_OK on success
 */
esp_err_t volume_control_set(int volume);

/**
 * @brief Increase volume by one step
 *
 * Also shows the volume HUD overlay.
 */
void volume_control_up(void);

/**
 * @brief Decrease volume by one step
 *
 * Also shows the volume HUD overlay.
 */
void volume_control_down(void);

/**
 * @brief Show the volume HUD overlay
 *
 * The HUD will auto-hide after VOLUME_HUD_DISPLAY_MS.
 */
void volume_control_show_hud(void);

/**
 * @brief Hide the volume HUD overlay immediately
 */
void volume_control_hide_hud(void);

#ifdef __cplusplus
}
#endif

#endif // VOLUME_CONTROL_H
