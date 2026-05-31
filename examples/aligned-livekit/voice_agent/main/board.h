#pragma once

#include <stdbool.h>
#include "esp_codec_dev.h"
#include "esp_io_expander.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize board.
void board_init(void);

/// Read the chip's internal temperature in degrees Celsius.
float board_get_temp(void);

/// Get record (microphone) handle
esp_codec_dev_handle_t get_record_handle(void);

/// Get playback (speaker) handle
esp_codec_dev_handle_t get_playback_handle(void);

/// Write a single ES8311 codec register directly over I2C. Used for the
/// half-duplex mic mute (ADC digital-volume REG17) that esp_codec_dev doesn't
/// expose. Returns 0 on success, -1 on failure.
int board_codec_write_reg(uint8_t reg, uint8_t val);

/// Initialize IO Expander.
esp_io_expander_handle_t bsp_io_expander_init(void);

/// Initialize Touch hardware.
esp_lcd_touch_handle_t bsp_touch_init(void);

/// Get LVGL touch input device handle.
lv_indev_t *bsp_get_touch_indev(void);

/// Return true when the knob button is pressed (active-low).
bool board_is_knob_pressed(void);

/// Enter deep sleep mode. Wake on button press or timer (if time_in_sec > 0).
/// Pass 0 for infinite sleep until button press.
void bsp_system_deep_sleep(uint32_t time_in_sec);

/// Perform a software reboot.
void bsp_system_reboot(void);

/// Complete power off via IO expander. Wake on button press or USB power.
void bsp_system_shutdown(void);

/// Battery state of charge, 0..100 (ADC voltage → curve-fit; samples 10x).
uint8_t board_get_battery_percent(void);

/// True when the charger reports charging (PCA9535 CHRG_DET).
bool board_is_charging(void);

/// True when a battery cell is present (PCA9535 BAT_DET, active-low).
bool board_is_battery_present(void);

/// Enable/disable touch coordinate debug logging.
/// When enabled, touch coordinates will be logged to the console.
/// Use this to verify touch is working and see where touches register.
void bsp_touch_debug_enable(bool enable);

/// Check if touch debug logging is currently enabled.
bool bsp_touch_debug_is_enabled(void);

#ifdef __cplusplus
}
#endif
