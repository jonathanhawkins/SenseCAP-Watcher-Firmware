#!/Users/light/.espressif/python_env/idf5.4_py3.13_env/bin/python3
"""
Patched esptool that uses smaller block sizes for flash AND RAM uploads.
The WCH CH55x USB-to-serial chip on SenseCap Watcher corrupts large packets.

Block sizes tested:
- 64 bytes:  5.7 KB/s ✓
- 128 bytes: 6.7 KB/s ✓
- 256 bytes: 7.4 KB/s ✓ (FASTEST WORKING for flash)
- 512 bytes: FAILS (message format error)

Usage:
    ./esptool_small_blocks.py --port /dev/cu.usbmodem56D50186503 write_flash 0x0 bootloader.bin
"""

import sys
import esptool
from esptool.loader import ESPLoader

# Block sizes for WCH CH55x compatibility
# 256 bytes causes flash corruption (boot loop). 128 bytes untested. 64 bytes = RELIABLE
SMALL_FLASH_BLOCK = 0x40    # 64 bytes for flash writes (slow but reliable)
SMALL_RAM_BLOCK = 0x40      # 64 bytes for RAM/stub uploads

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
