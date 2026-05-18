# Flash Configuration Notes

## CRITICAL: Use 32MB Flash Size, NOT 8MB!

The SenseCAP Watcher has 32MB flash. Always use:
```
--flash_size 32MB
```

Using 8MB causes:
- Missing model partition at 0xd10000
- Audio crashes
- Weird behavior

## Full Flash Command (64-byte blocks for WCH CH55x)
```bash
./scripts/esptool_small_blocks.py --port /dev/cu.usbmodem56D50186503 --baud 460800 --chip esp32s3 --no-stub write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/voice_agent.bin
```

## Block Size Notes
- 64 bytes: ~5.7 KB/s - RELIABLE
- 256 bytes: CAUSES FLASH CORRUPTION (boot loops)
- Always use 64-byte blocks with this USB chip

## Expected Boot Log (correct 32MB)
```
boot.esp32s3: SPI Flash Size : 32MB
```

If you see `SPI Flash Size : 8MB`, the flash_size parameter is wrong!

## Serial Monitor (IDF Monitor)

Use IDF monitor for proper symbol decoding and crash analysis:
```bash
/Users/light/.espressif/python_env/idf5.4_py3.13_env/bin/python \
  /Users/light/esp/esp-idf-v5.4.3/tools/idf_monitor.py \
  --port /dev/cu.usbmodem56D50186503 \
  --baud 115200 \
  hardware/watcher-firmware/examples/livekit-voice-agent/voice_agent/build/voice_agent.elf
```

**Note:** Use the `.elf` file from the build for symbol decoding. For aligned-realtime, use `aligned-realtime.elf`.

## LCD SPI Fix (SPD2010 Display)

If you see LCD SPI errors like:
```
E (2398) lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
E (2398) spd2010: panel_spd2010_draw_bitmap(698): send color failed
```

**Fix in `main/board.c`:**

1. **Reduce pixel clock from 20MHz to 10MHz:**
```c
#define DRV_LCD_PIXEL_CLK_HZ      (10 * 1000 * 1000)  // Was 20MHz
```

2. **Reduce SPI transaction queue depth from 10 to 2:**
```c
.trans_queue_depth = 2,  // Was 10
```

These changes reduce SPI bus contention and prevent queue overflow errors.
The display will still work at 10MHz - just slightly slower refresh.

## Backlight Fix (Black Screen After Boot)

If the screen flashes briefly at startup then goes black, the backlight is being disabled.

**Root Cause:** GPIO8 (LCD backlight) is being reconfigured as a regular GPIO output AFTER the LCD init sets it up as LEDC PWM. This happens when `bsp_i2c0_bus_init()` is called during audio codec initialization.

**Symptom:**
- Screen flashes at boot
- LCD init logs show success
- Screen goes black and stays black
- LVGL is rendering (can verify with debug background color)

**Fix in `main/board.c` or `src/board.c`:**

In the `bsp_i2c0_bus_init()` function, remove GPIO8 from the "silence" code:

```c
// ❌ WRONG - This kills the backlight LEDC PWM:
const gpio_config_t io_config = {
    .pin_bit_mask = ... | (1ULL << BSP_LCD_GPIO_BL),  // DON'T include GPIO8
    ...
};
gpio_set_level(BSP_LCD_GPIO_BL, 0);  // DON'T set backlight to 0

// ✅ CORRECT - Only configure LCD/SPI pins, NOT backlight:
const gpio_config_t io_config = {
    .pin_bit_mask = (1ULL << BSP_SPI3_HOST_PCLK) | (1ULL << BSP_SPI3_HOST_DATA0)
                    | (1ULL << BSP_SPI3_HOST_DATA1) | (1ULL << BSP_SPI3_HOST_DATA2)
                    | (1ULL << BSP_SPI3_HOST_DATA3) | (1ULL << BSP_LCD_SPI_CS),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
};
gpio_config(&io_config);
gpio_set_level(BSP_LCD_SPI_CS, 0);
gpio_set_level(BSP_SPI3_HOST_PCLK, 0);
// Note: No gpio_set_level for backlight - let LEDC PWM control it
```

**Why this happens:** The audio codec init calls `bsp_i2c0_bus_init()` which runs AFTER `bsp_lcd_panel_init()`. When the GPIO config includes GPIO8, it reconfigures the pin from LEDC PWM (controlled by backlight brightness setting) to a regular GPIO output set to 0 (OFF).

## I2C Driver Already Installed Fix

If you see:
```
E (xxx) i2c: i2c driver install error
E (xxx) i2c: i2c_master_cmd_begin(xxx): i2c number error
Failed to find/create ES7243/E codec
```

**Root Cause:** The sensecap-watcher BSP's `bsp_i2c_bus_init()` already installed the I2C0 driver. When aligned-realtime's `bsp_i2c0_bus_init()` calls `i2c_driver_install()`, it fails with `ESP_FAIL`. The code was returning early without setting `i2c0_port`, causing subsequent I2C calls to use port -1.

**⚠️ CRITICAL:** ESP-IDF returns `ESP_FAIL`, NOT `ESP_ERR_INVALID_STATE`!
This is a common mistake - checking for the wrong error code will not work.

**Fix in `main/board.c` or `src/board.c`:**

```c
ret = i2c_driver_install(BSP_GENERAL_I2C_NUM, conf.mode, 0, 0, ESP_INTR_FLAG_SHARED);
// ⚠️ ESP_FAIL (not ESP_ERR_INVALID_STATE!) means driver already installed - that's OK
if (ret == ESP_FAIL) {
    ESP_LOGW(TAG, "I2C%d already installed; continuing", BSP_GENERAL_I2C_NUM);
    ret = ESP_OK;
}
if (ret != ESP_OK) {
    return ret;
}
// CRITICAL: Always set the port number so subsequent I2C operations work
i2c0_port = BSP_GENERAL_I2C_NUM;
return ESP_OK;
```

## Display "Half Static" / Corruption at Boot

If the display shows static or corruption from the moment the device boots (half the screen is garbled), the LVGL display buffer configuration doesn't match factory firmware.

**Root Cause:** Factory firmware uses different LVGL buffer settings:
- SPIRAM for display buffers (more stable than internal RAM)
- Double buffering enabled (prevents tearing)
- These provide stable memory for the large 412x412 display

**Fix in `board.c` (or `src/board.c` for aligned-realtime):**

1. **Enable SPIRAM for display buffers:**
```c
const lvgl_port_display_cfg_t disp_cfg = {
    .io_handle = panel_io_handle,
    .panel_handle = panel_handle,
    .buffer_size = DRV_LCD_H_RES * LVGL_DRAW_BUFF_HEIGHT,
    .double_buffer = LVGL_DRAW_BUFF_DOUBLE,
    .hres = DRV_LCD_H_RES,
    .vres = DRV_LCD_V_RES,
    .flags = {
        .buff_dma = false,
        .buff_spiram = true,  // ✅ Match factory - use SPIRAM for display buffer
    },
};
```

2. **Enable double buffering:**
```c
#define LVGL_DRAW_BUFF_DOUBLE (1)  // ✅ Match factory - enable double buffering
```

3. **Use 20MHz pixel clock (balance between stability and refresh rate):**
```c
#define DRV_LCD_PIXEL_CLK_HZ      (20 * 1000 * 1000)  // 20MHz works well
```

**Why this works:**
- Factory uses `buff_spiram = true` (see `sensecap-watcher.c` line 809)
- Factory uses `LVGL_DRAW_BUFF_DOUBLE = 1` (see `sensecap-watcher.h` line 168)
- Factory uses 40MHz pixel clock but 20MHz provides good balance
- SPIRAM has 8MB available vs limited internal RAM
- Double buffering (2x 339KB buffers) prevents partial frame display
- Reduces internal RAM pressure for audio, networking, etc.

**Factory firmware reference:**
```c
// components/sensecap-watcher/sensecap-watcher.c line 805-810
.buffer_size = DRV_LCD_H_RES * LVGL_DRAW_BUFF_HEIGHT,
.double_buffer = LVGL_DRAW_BUFF_DOUBLE,
// ...
.flags = {
    .buff_dma = false,
    .buff_spiram = true,
},
```
