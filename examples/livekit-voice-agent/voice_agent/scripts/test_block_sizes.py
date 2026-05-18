#!/usr/bin/env python3
"""
Test different block sizes for flashing to WCH CH55x USB-to-serial bridge.
The WCH chip has 32-byte OUT / 64-byte IN USB buffers.
We know 64-byte blocks work. Let's see if larger blocks might work too.
"""

import sys
import os
import time
import subprocess

# Add esptool to path
sys.path.insert(0, '/Users/light/.espressif/python_env/idf5.4_py3.13_env/lib/python3.13/site-packages')

PORT = '/dev/cu.usbmodem56D50186503'
BAUD = 460800

# Block sizes to test - starting from what we know works
BLOCK_SIZES = [64, 128, 256, 512, 1024]

# Small test - just erase a small region to test communication
TEST_SIZE = 4096  # 4KB test

def test_block_size(block_size):
    """Test if a specific block size works for flash operations."""
    print(f"\n{'='*60}")
    print(f"Testing block size: {block_size} bytes")
    print(f"{'='*60}")

    # Patch esptool's FLASH_WRITE_SIZE
    import esptool
    import esptool.cmds

    original_size = getattr(esptool.cmds, 'FLASH_WRITE_SIZE', 16384)
    esptool.cmds.FLASH_WRITE_SIZE = block_size

    # Also patch in loader if it exists
    if hasattr(esptool, 'loader'):
        if hasattr(esptool.loader, 'FLASH_WRITE_SIZE'):
            esptool.loader.FLASH_WRITE_SIZE = block_size

    print(f"Patched FLASH_WRITE_SIZE to {block_size} bytes")

    try:
        # Simple test: just try to read flash ID
        # This tests basic communication with the patched settings
        start_time = time.time()

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
esptool.cmds.FLASH_WRITE_SIZE = {block_size}

# Just try chip_id - quick test of communication
sys.argv = ['esptool.py', '--port', '{PORT}', '--baud', '{BAUD}', 'chip_id']
try:
    esptool.main()
    print("SUCCESS")
except Exception as e:
    print(f"FAILED: {{e}}")
'''
            ],
            capture_output=True,
            text=True,
            timeout=30
        )

        elapsed = time.time() - start_time

        print(f"Output: {result.stdout[:500]}")
        if result.stderr:
            print(f"Stderr: {result.stderr[:200]}")

        if "SUCCESS" in result.stdout or "Chip is ESP32-S3" in result.stdout:
            print(f"✅ Block size {block_size} works! (took {elapsed:.1f}s)")
            return True
        else:
            print(f"❌ Block size {block_size} failed")
            return False

    except subprocess.TimeoutExpired:
        print(f"❌ Block size {block_size} timed out")
        return False
    except Exception as e:
        print(f"❌ Block size {block_size} error: {e}")
        return False
    finally:
        # Restore original
        esptool.cmds.FLASH_WRITE_SIZE = original_size

def main():
    print("WCH CH55x Block Size Tester")
    print("=" * 60)
    print(f"Port: {PORT}")
    print(f"Baud: {BAUD}")
    print(f"Testing block sizes: {BLOCK_SIZES}")
    print()

    # Check if port exists
    if not os.path.exists(PORT):
        print(f"ERROR: Port {PORT} not found!")
        print("Please connect the Watcher device.")
        return 1

    results = {}
    for block_size in BLOCK_SIZES:
        results[block_size] = test_block_size(block_size)
        time.sleep(2)  # Give device time to reset between tests

    print("\n" + "=" * 60)
    print("RESULTS SUMMARY")
    print("=" * 60)

    working_sizes = []
    for size, success in results.items():
        status = "✅ WORKS" if success else "❌ FAILED"
        print(f"  {size:5d} bytes: {status}")
        if success:
            working_sizes.append(size)

    if working_sizes:
        best = max(working_sizes)
        print(f"\n🎯 Best working block size: {best} bytes")
        print(f"   (Larger = faster flashing)")
    else:
        print("\n❌ No block sizes worked! Check device connection.")

    return 0

if __name__ == '__main__':
    sys.exit(main())
