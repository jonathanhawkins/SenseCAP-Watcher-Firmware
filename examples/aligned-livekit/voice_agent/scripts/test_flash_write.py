#!/usr/bin/env python3
"""
Test actual flash write with different block sizes.
This tests the real flash writing path, not just chip detection.
"""

import sys
import os
import time
import subprocess
import tempfile

PORT = '/dev/cu.usbmodem56D50186503'
BAUD = 460800

# Create a small test binary
TEST_DATA = bytes([0x00] * 4096)  # 4KB of zeros

def test_flash_write(block_size):
    """Test actual flash write with specific block size."""
    print(f"\n{'='*60}")
    print(f"Testing FLASH WRITE with block size: {block_size} bytes")
    print(f"{'='*60}")

    # Create temp file with test data
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(TEST_DATA)
        temp_file = f.name

    try:
        start_time = time.time()

        # Use subprocess to run esptool with patched block size
        # Write to a safe address (not bootloader) - address 0x200000 is in app partition area
        result = subprocess.run(
            [
                sys.executable,
                '-c',
                f'''
import sys
sys.path.insert(0, '/Users/light/.espressif/python_env/idf5.4_py3.13_env/lib/python3.13/site-packages')
import esptool
import esptool.cmds

# Patch the block size
original = esptool.cmds.FLASH_WRITE_SIZE
esptool.cmds.FLASH_WRITE_SIZE = {block_size}
print(f"[PATCHED] Using FLASH_WRITE_SIZE = {block_size} bytes (was {{original}})")

# Try to write a small test file
sys.argv = [
    'esptool.py',
    '--port', '{PORT}',
    '--baud', '{BAUD}',
    '--no-stub',  # Skip stub upload - use ROM bootloader directly
    'write_flash',
    '0x200000',   # Safe test address in app partition
    '{temp_file}'
]

try:
    esptool.main()
    print("FLASH_WRITE_SUCCESS")
except Exception as e:
    print(f"FLASH_WRITE_FAILED: {{e}}")
'''
            ],
            capture_output=True,
            text=True,
            timeout=120
        )

        elapsed = time.time() - start_time

        # Print output
        for line in result.stdout.split('\n')[-20:]:  # Last 20 lines
            if line.strip():
                print(f"  {line}")
        if result.stderr:
            for line in result.stderr.split('\n')[-5:]:
                if line.strip():
                    print(f"  [ERR] {line}")

        if "FLASH_WRITE_SUCCESS" in result.stdout or "Hash of data verified" in result.stdout:
            speed = len(TEST_DATA) / elapsed / 1024  # KB/s
            print(f"\n✅ Block size {block_size}: SUCCESS! ({elapsed:.1f}s, ~{speed:.1f} KB/s)")
            return True, elapsed
        else:
            print(f"\n❌ Block size {block_size}: FAILED")
            return False, None

    except subprocess.TimeoutExpired:
        print(f"\n❌ Block size {block_size}: TIMED OUT")
        return False, None
    except Exception as e:
        print(f"\n❌ Block size {block_size}: ERROR - {e}")
        return False, None
    finally:
        os.unlink(temp_file)
        time.sleep(2)  # Allow device to reset

def main():
    print("Flash Write Block Size Benchmark")
    print("=" * 60)
    print(f"Port: {PORT}")
    print(f"Test size: {len(TEST_DATA)} bytes")
    print()

    # Check port
    if not os.path.exists(PORT):
        print(f"ERROR: Port {PORT} not found!")
        return 1

    # Test block sizes from small to large
    block_sizes = [64, 128, 256, 512, 1024, 2048, 4096]
    results = {}

    for block_size in block_sizes:
        success, elapsed = test_flash_write(block_size)
        results[block_size] = (success, elapsed)
        if not success:
            print(f"   Stopping tests - {block_size} bytes failed")
            break

    print("\n" + "=" * 60)
    print("BENCHMARK RESULTS")
    print("=" * 60)
    print(f"{'Size':>8} | {'Status':^10} | {'Time':^8} | {'Speed':^10}")
    print("-" * 45)

    best_size = None
    best_time = float('inf')

    for size in block_sizes:
        if size in results:
            success, elapsed = results[size]
            if success:
                speed = len(TEST_DATA) / elapsed / 1024
                print(f"{size:>6}B | {'✅ PASS':^10} | {elapsed:>6.1f}s | {speed:>7.1f} KB/s")
                if elapsed < best_time:
                    best_time = elapsed
                    best_size = size
            else:
                print(f"{size:>6}B | {'❌ FAIL':^10} |    -    |     -")

    if best_size:
        print(f"\n🎯 Recommended block size: {best_size} bytes")
        print(f"   Expected speedup: ~{1024/best_size:.0f}x faster than 64-byte blocks")
    else:
        print("\n❌ No working block sizes found!")

    return 0

if __name__ == '__main__':
    sys.exit(main())
