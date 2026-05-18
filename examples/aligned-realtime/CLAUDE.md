# CLAUDE.md - SenseCAP Watcher Aligned Realtime

## Flash Configuration

### CRITICAL: Use 32MB Flash Size, NOT 8MB!

The SenseCAP Watcher has 32MB flash. Always use:
```
--flash_size 32MB
```

Using 8MB causes:
- Missing model partition at 0xd10000
- Audio crashes
- Weird behavior

### Full Flash Command (64-byte blocks for WCH CH55x)

```bash
/Users/light/.espressif/python_env/idf5.4_py3.13_env/bin/python \
  /Users/light/dev/web-apps/aligned-tools/hardware/watcher-firmware/examples/livekit-voice-agent/voice_agent/scripts/esptool_small_blocks.py \
  --port /dev/cu.usbmodem56D50186503 \
  --baud 460800 \
  --chip esp32s3 \
  --no-stub \
  write_flash \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 32MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x110000 build/aligned-realtime.bin
```

### Block Size Notes
- 64 bytes: ~5.7 KB/s - RELIABLE
- 256 bytes: CAUSES FLASH CORRUPTION (boot loops)
- Always use 64-byte blocks with this USB chip (WCH CH55x)
- MUST use `--no-stub` flag to avoid checksum errors

### Expected Boot Log (correct 32MB)
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
  build/aligned-realtime.elf
```

**Note:** Use the `.elf` file from the build for symbol decoding.

## Build Command

```bash
source /Users/light/esp/esp-idf-v5.4.3/export.sh && idf.py build
```

## USB Ports

The SenseCAP Watcher has TWO USB ports:
- `/dev/cu.usbmodem56D50186501` - ROM bootloader (garbled output)
- `/dev/cu.usbmodem56D50186503` - Console/Monitor AND flashing

**Always use port 56D50186503 for both flashing and monitoring.**

## LCD SPI Fix (SPD2010 Display)

If you see LCD SPI errors like:
```
E (2398) lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
E (2398) spd2010: panel_spd2010_draw_bitmap(698): send color failed
```

**Fix in `src/board.c`:**

1. **Reduce SPI transaction queue depth from 10 to 2:**
```c
.trans_queue_depth = 2,  // Was 10
```

This reduces SPI bus contention and prevents queue overflow errors.

## Display "Half Static" / Corruption at Boot

**Problem:** Display shows static/corruption from boot (half the screen is garbled).

**Root Cause:** LVGL display buffer configuration doesn't match factory firmware:
- Factory uses SPIRAM for display buffers (`buff_spiram = true`)
- Factory uses double buffering (`LVGL_DRAW_BUFF_DOUBLE = 1`)
- These settings provide stable memory allocation and prevent tearing/corruption

**Fix in `src/board.c`:**

1. **Enable SPIRAM for display buffers:**
```c
const lvgl_port_display_cfg_t disp_cfg = {
    // ...
    .flags = {
        .buff_dma = false,
        .buff_spiram = true,  // Match factory - use SPIRAM for display buffer
    },
};
```

2. **Enable double buffering:**
```c
#define LVGL_DRAW_BUFF_DOUBLE (1)  // Match factory - enable double buffering
```

3. **Use 20MHz pixel clock:**
```c
#define DRV_LCD_PIXEL_CLK_HZ      (20 * 1000 * 1000)  // Balance between stability and refresh
```

**Why this works:** Factory firmware allocates LVGL buffers in SPIRAM (8MB available) with double buffering:
- More stable memory for large display buffers (412x412 @ 16bpp = 339KB per buffer)
- Double buffering prevents partial frame display
- Reduces internal RAM pressure for audio, networking, etc.

## LiveKit Protocol Decode Fix

If you see protobuf decode errors like:
```
E (xxx) livekit_protocol: Failed to decode signal res: type=16, error=parent stream too short
E (xxx) livekit_protocol: Failed to decode signal res: type=11, error=wrong wire type
```

**Fix in `managed_components/livekit__livekit/protocol/protobufs/livekit_rtc.options`:**

Change all `FT_IGNORE` entries for SignalResponse oneof fields to `FT_CALLBACK`:
```
livekit_pb.SignalResponse.track_published type:FT_CALLBACK
livekit_pb.SignalResponse.room_update type:FT_CALLBACK
livekit_pb.SignalResponse.refresh_token type:FT_CALLBACK
# ... all other SignalResponse.* fields
```

`FT_IGNORE` breaks oneof decoding in nanopb. `FT_CALLBACK` properly registers fields for skipping.

## Backlight Fix (Black Screen After Boot)

**Problem:** Screen flashes at boot then goes black.

**Root Cause:** GPIO8 (LCD backlight) is controlled by LEDC PWM after LCD init. The `bsp_i2c0_bus_init()` function (called during audio codec init) had GPIO8 in its "silence" code, which reconfigured it as a regular GPIO output set to 0 (OFF).

**Fix in `src/board.c`:**

Remove GPIO8 from the GPIO config in `bsp_i2c0_bus_init()`:
```c
// ✅ CORRECT - Only configure LCD/SPI pins, NOT backlight (GPIO8):
const gpio_config_t io_config = {
    .pin_bit_mask = (1ULL << BSP_SPI3_HOST_PCLK) | (1ULL << BSP_SPI3_HOST_DATA0)
                    | (1ULL << BSP_SPI3_HOST_DATA1) | (1ULL << BSP_SPI3_HOST_DATA2)
                    | (1ULL << BSP_SPI3_HOST_DATA3) | (1ULL << BSP_LCD_SPI_CS),
    // ... rest of config
};
// Note: No gpio_set_level for BSP_LCD_GPIO_BL - LEDC PWM controls it
```

## I2C Driver Already Installed Fix

**Problem:** `i2c driver install error` and microphone codec fails.

**Root Cause:** sensecap-watcher BSP's `bsp_i2c_bus_init()` already installed I2C0. Our `bsp_i2c0_bus_init()` fails with `ESP_FAIL` and returns early without setting `i2c0_port`.

**⚠️ CRITICAL:** ESP-IDF returns `ESP_FAIL`, NOT `ESP_ERR_INVALID_STATE`! Checking for the wrong error code is a common mistake that breaks microphone initialization.

**Fix in `src/board.c`:**
```c
ret = i2c_driver_install(BSP_GENERAL_I2C_NUM, conf.mode, 0, 0, ESP_INTR_FLAG_SHARED);
// ⚠️ Must check ESP_FAIL, not ESP_ERR_INVALID_STATE!
if (ret == ESP_FAIL) {
    ESP_LOGW(TAG, "I2C%d already installed; continuing", BSP_GENERAL_I2C_NUM);
    ret = ESP_OK;
}
if (ret != ESP_OK) return ret;
i2c0_port = BSP_GENERAL_I2C_NUM;  // CRITICAL: Always set port
return ESP_OK;
```

## Touch Controller Fix (I2C1 Bus)

**Problem:** Touch doesn't respond.

**Root Cause:** GPIO38/39 (I2C1 SDA/SCL) were in a GPIO config as push-pull outputs, breaking the I2C bus.

**Fix:** Remove GPIO38/39 from any GPIO configuration. Use sensecap-watcher's `bsp_io_expander_init()` instead of custom `init_gpio_ext()`.

## FreeRTOS Timer Task Stack Overflow Fix

**Problem:** Device crashes with stack overflow during disconnect:
```
***ERROR*** A stack overflow in task Tmr Svc has been detected.
```

**Root Cause:** The default FreeRTOS Timer Service task stack is only 2048 bytes. When disconnecting, LVGL timer operations (`lv_timer_del()`) and UI updates consume too much stack space, causing overflow.

**Symptoms:**
- Device crashes when pressing wheel to disconnect voice session
- LiveKit room doesn't cleanly disconnect (may continue billing!)
- Device reboots before cleanup completes

**Fix in `sdkconfig`:**
```
CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=4096
CONFIG_TIMER_TASK_STACK_DEPTH=4096
```

**Note:** The default 2048 bytes is insufficient for LVGL timer callbacks during disconnect. Doubling to 4096 provides adequate headroom.
