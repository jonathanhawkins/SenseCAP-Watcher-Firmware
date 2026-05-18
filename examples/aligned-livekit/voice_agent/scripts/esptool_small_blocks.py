#!/Users/light/.espressif/python_env/idf5.4_py3.13_env/bin/python3
"""
Patched esptool that uses tuned block sizes for the SenseCAP Watcher's
WCH CH343 USB-Serial bridge (idVendor=0x1A86 idProduct=0x55D2, "USB Dual_Serial").

Root cause of the block-size constraints (verified 2026-05-18):

  1. CH343 USB descriptor declares wMaxPacketSize=32 on the OUT bulk endpoints
     (Intf 1 EP 0x02, Intf 3 EP 0x03). Verified via libusb. Host MUST fragment
     larger ESP blocks across multiple 32-byte USB transactions.
     -> Per USB-FS frame timing, 32 bytes arrive at the chip every ~125 µs
        but drain to the UART at 460800 baud in ~700 µs. Buffer fills at
        ~26 bytes/packet net. The chip's internal FIFO handles up to roughly
        7-8 max packets in flight before bytes are lost.

  2. ESP32-S3 ROM bootloader rejects commands with payload >= 384 bytes
     ("invalid message format", status 0x0105). Hard ceiling for --no-stub.

Block sizes tested with full-firmware write + esptool --verify hash check:

  - 64 bytes  -> 97.8 kbit/s ✓   (original default; very conservative)
  - 128 bytes -> 145.8 kbit/s ✓  (verified 2026-05-18)
  - 252 bytes -> 191.6 kbit/s ✓  (verified 2026-05-18, current default)
  - 256 bytes -> 204.7 kbit/s ✓  (verified 256 KB scratch only; historic
                                    comment claimed boot-loop at production
                                    scale, kept one step under for margin)
  - >= 384 bytes -> FAIL (ROM rejects oversize commands)

Other things tested 2026-05-18 that did NOT help:

  - Stub mode + FLASH_DEFL_DATA (compressed writes): stub uploads cleanly
    but the chip silently corrupts high-entropy compressed bytes mid-
    transfer ("Bad data checksum C100 after seq 0"). Stay on --no-stub.
  - 921600 baud: CDC baud renegotiation succeeds but subsequent commands
    return garbled bytes ("Invalid head of packet 0x0D"). Either macOS
    AppleUSBCDCACM doesn't deliver the SET_LINE_CODING correctly or the
    CH343 baud-divider precision degrades at this rate. Stay at 460800.
  - mac-flasher.py libusb path: explicitly uses 16-byte chunks with 5ms
    inter-chunk delay for reliability -> ~3 KB/s. Slower, not faster.
  - Native ESP32-S3 USB Serial/JTAG (~480 KB/s if available): NOT wired
    to the USB-C connector on this board. Would require soldering to
    GPIO19/20 to access.

Usage:
    ./esptool_small_blocks.py --port /dev/cu.usbmodem*0503 write_flash \\
        --verify 0x10000 build/voice_agent.bin
"""

import sys
import esptool
from esptool.loader import ESPLoader

# 252-byte blocks: 7 full USB packets + 1 short, hash-verified end-to-end
# against 7.83 MB voice_agent.bin at 460800 baud. Sits just under the
# (historically reported, not reproduced) 256-byte fail boundary.
SMALL_FLASH_BLOCK = 0xFC    # 252 bytes for flash writes
SMALL_RAM_BLOCK = 0xFC      # 252 bytes for RAM/stub uploads

# Store original values
original_flash_write_size = ESPLoader.FLASH_WRITE_SIZE
original_ram_block_size = ESPLoader.ESP_RAM_BLOCK

# Patch all ESP loader classes (both flash and RAM block sizes)
for cls_name in dir(esptool.targets):
    cls = getattr(esptool.targets, cls_name)
    if isinstance(cls, type) and issubclass(cls, ESPLoader):
        cls.FLASH_WRITE_SIZE = SMALL_FLASH_BLOCK
        cls.ESP_RAM_BLOCK = SMALL_RAM_BLOCK

# Also patch the base class
ESPLoader.FLASH_WRITE_SIZE = SMALL_FLASH_BLOCK
ESPLoader.ESP_RAM_BLOCK = SMALL_RAM_BLOCK

print(f"[PATCHED] FLASH_WRITE_SIZE = {SMALL_FLASH_BLOCK} bytes (was {original_flash_write_size})")
print(f"[PATCHED] ESP_RAM_BLOCK = {SMALL_RAM_BLOCK} bytes (was {original_ram_block_size})")
print()

# Run esptool with remaining arguments
if __name__ == "__main__":
    esptool.main()
