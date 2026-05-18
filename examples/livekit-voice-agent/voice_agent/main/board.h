#pragma once

#include "esp_codec_dev.h"
#include "esp_io_expander.h"
#include "esp_lcd_touch.h"

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

/// Initialize IO Expander.
esp_io_expander_handle_t bsp_io_expander_init(void);

/// Initialize Touch.
esp_lcd_touch_handle_t bsp_touch_init(void);

#ifdef __cplusplus
}
#endif
