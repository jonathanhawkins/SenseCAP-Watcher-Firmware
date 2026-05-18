#!/Users/light/.espressif/python_env/idf5.4_py3.13_env/bin/python3
"""
Benchmark different block sizes for flashing to WCH CH343 USB-to-serial bridge.
Tests actual flash writes with each block size to find the fastest that works.

NOTE (2026-05-18): This script writes 8 KB to a scratch flash region —
small enough that it misses corruption that only shows up at production scale.
Full-firmware (7.8 MB) re-runs with esptool --verify on 2026-05-18 found:
  - 64 / 128 / 192 / 224 / 252 / 256 B all hash-verified clean
  - >= 384 B fails with ROM "invalid message format" (0x0105)
The current production default in esptool_small_blocks.py is 252 B at 460800
baud → 191.6 kbit/s, ~5:27 for the 7.83 MB voice_agent.bin. See
.claude/rules/hardware.md for full root-cause notes.
"""

import sys
import os
import time
import tempfile
import esptool
from esptool.loader import ESPLoader

PORT = '/dev/cu.usbmodem56D50186503'
BAUD = 460800

# Create 8KB of test data (small but meaningful)
TEST_DATA = bytes([i % 256 for i in range(8192)])

def patch_block_size(block_size):
    """Patch all ESP loader classes to use specific block size."""
    # Patch all ESP loader classes
    for cls_name in dir(esptool.targets):
        cls = getattr(esptool.targets, cls_name)
        if isinstance(cls, type) and issubclass(cls, ESPLoader):
            cls.FLASH_WRITE_SIZE = block_size
    ESPLoader.FLASH_WRITE_SIZE = block_size

def test_flash_write(block_size, temp_file):
    """Test actual flash write with specific block size."""
    print(f"\n{'='*60}")
    print(f"Testing block size: {block_size} bytes ({block_size/1024:.1f} KB)")
    print(f"{'='*60}")

    patch_block_size(block_size)
    print(f"[PATCHED] FLASH_WRITE_SIZE = {block_size}")

    # Build esptool args - write to safe test address (0x200000)
    sys.argv = [
        'esptool.py',
        '--port', PORT,
        '--baud', str(BAUD),
        '--chip', 'esp32s3',
        '--no-stub',  # Use ROM bootloader, not stub
        'write_flash',
        '--flash_mode', 'dio',
        '--flash_freq', '80m',
        '--flash_size', '8MB',
        '0x200000',  # Safe test address
        temp_file
    ]

    start_time = time.time()
    try:
        esptool.main()
        elapsed = time.time() - start_time
        speed = len(TEST_DATA) / elapsed / 1024  # KB/s
        print(f"\n✅ SUCCESS: {elapsed:.1f}s ({speed:.1f} KB/s)")
        return True, elapsed

    except SystemExit as e:
        elapsed = time.time() - start_time
        if e.code == 0:
            speed = len(TEST_DATA) / elapsed / 1024
            print(f"\n✅ SUCCESS: {elapsed:.1f}s ({speed:.1f} KB/s)")
            return True, elapsed
        else:
            print(f"\n❌ FAILED (exit code {e.code})")
            return False, None

    except Exception as e:
        print(f"\n❌ ERROR: {e}")
        return False, None

def main():
    print("=" * 60)
    print("WCH CH343 Flash Write Block Size Benchmark")
    print("=" * 60)
    print(f"Port: {PORT}")
    print(f"Baud: {BAUD}")
    print(f"Test data: {len(TEST_DATA)} bytes")
    print()

    # Check port
    if not os.path.exists(PORT):
        print(f"ERROR: Port {PORT} not found!")
        print("Please connect the Watcher and put it in download mode")
        return 1

    # Create temp file with test data
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(TEST_DATA)
        temp_file = f.name

    try:
        # Block sizes to test - from what we know works to larger sizes
        block_sizes = [64, 128, 256, 512, 1024, 2048]
        results = {}

        for block_size in block_sizes:
            success, elapsed = test_flash_write(block_size, temp_file)
            results[block_size] = (success, elapsed)

            if not success:
                print(f"\n⚠️  Block size {block_size} failed - larger sizes unlikely to work")
                # Still try the next one to be sure
                continue

            time.sleep(3)  # Give device time to reset

        # Print summary
        print("\n" + "=" * 60)
        print("BENCHMARK RESULTS")
        print("=" * 60)
        print(f"{'Block Size':>12} | {'Status':^10} | {'Time':^10} | {'Speed':^12}")
        print("-" * 52)

        best_size = 64
        best_speed = 0

        for size in sorted(results.keys()):
            success, elapsed = results[size]
            if success and elapsed:
                speed = len(TEST_DATA) / elapsed / 1024
                print(f"{size:>10} B | {'✅ PASS':^10} | {elapsed:>8.1f}s | {speed:>9.1f} KB/s")
                if speed > best_speed:
                    best_speed = speed
                    best_size = size
            else:
                print(f"{size:>10} B | {'❌ FAIL':^10} |       -   |          -")

        print("=" * 52)

        if best_speed > 0:
            print(f"\n🎯 RECOMMENDED: {best_size} bytes")
            print(f"   Speed: {best_speed:.1f} KB/s")
            speedup = best_size / 64
            print(f"   Speedup vs 64-byte: ~{speedup:.1f}x faster")
        else:
            print("\n❌ All tests failed!")

    finally:
        os.unlink(temp_file)

    return 0

if __name__ == '__main__':
    sys.exit(main())
