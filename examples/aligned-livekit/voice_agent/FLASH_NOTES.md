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
