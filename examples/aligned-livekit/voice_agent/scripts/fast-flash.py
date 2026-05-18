#!/opt/homebrew/bin/python3
"""
fast-flash.py — incremental flash for the SenseCAP Watcher (CH343 USB-Serial).

Combines:
  - esptool 5.2.0+ `--diff-with --no-diff-verify` (only writes 4 KB sectors
    that changed since the last successful flash)
  - 128-byte ESP block size, tuned for the CH343 USB OUT FIFO
  - Automatic cache of the last-flashed binary

Usage (from the voice_agent directory after `idf.py build`):

    ./scripts/fast-flash.py                    # auto path
    ./scripts/fast-flash.py --full             # force full flash (rebuild cache)
    ./scripts/fast-flash.py --port /dev/...    # override port autodetect

Typical timings on this hardware:
  - First run / cache missing       -> full flash at 128 B / 460800 baud
                                       (~7-8 min for the 7.83 MB voice_agent.bin)
  - No change since last flash      -> ~1.2 s (handshake + MD5, no writes)
  - Single-line code change         -> ~2-3 min (26% of binary typically
                                       differs after a function-layout shift;
                                       diff-with sends only those sectors)
  - Pure asset-data tweak           -> seconds (very few sectors change)

For the truly battle-tested full-flash path used in CI, see
`esptool_small_blocks.py` (esptool 4.10 / 252 B blocks, 5:27). esptool 5.x
rejects 252-byte blocks at the ROM bootloader with `0105 invalid message
format` mid-stream, so this script stays at 128 B.

Root cause of the block-size constraints is documented in
`.claude/rules/hardware.md`.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

# Use esptool 5.2.0+ from the system Python (has --diff-with).
# The IDF-managed esptool is pinned at 4.10 which lacks --diff-with.
ESPTOOL_PY = "/opt/homebrew/bin/python3"

CACHE_DIR = os.path.expanduser("~/.cache/aligned-tools/watcher-flash")
CACHE_BIN = os.path.join(CACHE_DIR, "last_flashed_voice_agent.bin")
APP_OFFSET = "0x10000"


def autodetect_port() -> str | None:
    # Watcher exposes the CH343 with two CDC interfaces. Port names look
    # like /dev/cu.usbmodem56D50186501 (Himax) and ...503 (ESP32-S3).
    # The interface index is appended as the LAST char (1 vs 3).
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    esp32_ports = [p for p in ports if p.endswith("3")]
    return esp32_ports[0] if esp32_ports else None


def run_esptool(args: list[str]) -> int:
    """Run esptool 5.x with the 128-byte CH343 patch applied at import time.

    Returns esptool's exit code (0 = success). Uses subprocess so the
    caller can update the cache only on a successful flash.
    """
    patch_script = "/tmp/_fast_flash_patched.py"
    with open(patch_script, "w") as f:
        f.write(
            "import sys, esptool\n"
            "from esptool.loader import ESPLoader\n"
            "BLK = 0x80\n"  # 128 bytes — safe in both esptool 4.x and 5.x on CH343
            "for cls_name in dir(esptool.targets):\n"
            "    cls = getattr(esptool.targets, cls_name)\n"
            "    if isinstance(cls, type) and issubclass(cls, ESPLoader):\n"
            "        cls.FLASH_WRITE_SIZE = BLK\n"
            "        cls.ESP_RAM_BLOCK = BLK\n"
            "ESPLoader.FLASH_WRITE_SIZE = BLK\n"
            "ESPLoader.ESP_RAM_BLOCK = BLK\n"
            "esptool.main()\n"
        )
    os.chmod(patch_script, 0o755)
    proc = subprocess.run([ESPTOOL_PY, patch_script] + args)
    return proc.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", nargs="?", default="build/voice_agent.bin",
                        help="App binary to flash (default: build/voice_agent.bin)")
    parser.add_argument("--port", default=None, help="Serial port (default: autodetect *0503)")
    parser.add_argument("--full", action="store_true", help="Force a full flash and rebuild the cache")
    parser.add_argument("--baud", default="460800", help="Baud rate (default: 460800)")
    args = parser.parse_args()

    if not os.path.exists(args.binary):
        print(f"error: {args.binary} not found — did you run `idf.py build`?", file=sys.stderr)
        return 1

    port = args.port or autodetect_port()
    if not port:
        print("error: no Watcher serial port found (looking for /dev/cu.usbmodem*0503)", file=sys.stderr)
        return 1

    os.makedirs(CACHE_DIR, exist_ok=True)

    base = [
        "--port", port, "--baud", args.baud,
        "--chip", "esp32s3", "--no-stub",
        "write-flash",
        "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "32MB",
    ]

    cache_present = os.path.exists(CACHE_BIN) and not args.full
    if cache_present:
        print(f"[fast-flash] diff-with cache: {CACHE_BIN}")
        cmd = base + [
            "--diff-with", CACHE_BIN,
            "--no-diff-verify",
            APP_OFFSET, args.binary,
        ]
        mode = "incremental"
    else:
        if args.full:
            print("[fast-flash] --full requested, doing full flash")
        else:
            print("[fast-flash] no cache, doing full flash (will cache on success)")
        cmd = base + [APP_OFFSET, args.binary]
        mode = "full"

    # The cache represents what's currently on the chip — so we MUST update
    # it AFTER esptool succeeds, never before. Otherwise --diff-with would
    # compare the new binary against itself and skip every write.
    start = time.time()
    print(f"[fast-flash] running esptool 5.x ({mode}, 128 B blocks, baud {args.baud})")
    rc = run_esptool(cmd)
    elapsed = time.time() - start

    if rc != 0:
        print(f"[fast-flash] flash FAILED in {elapsed:.1f}s (exit {rc}); cache NOT updated", file=sys.stderr)
        print(f"[fast-flash] if device is in a bad state, recover with: ./scripts/fast-flash.py --full", file=sys.stderr)
        return rc

    print(f"[fast-flash] flash OK in {elapsed:.1f}s; updating cache -> {CACHE_BIN}")
    shutil.copy2(args.binary, CACHE_BIN)
    return 0


if __name__ == "__main__":
    sys.exit(main())
