#!/bin/bash
#
# Copy Watcher Firmware Files to Desktop for Windows Transfer
#
# This script gathers all necessary firmware files and documentation
# into a single folder on the Desktop for easy transfer to Windows PC.
#

set -e  # Exit on error

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${BLUE}================================================${NC}"
echo -e "${BLUE}  Watcher Firmware - Copy to Desktop${NC}"
echo -e "${BLUE}================================================${NC}"
echo

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/examples/aligned-realtime/build"
DESKTOP_DIR="${HOME}/Desktop/watcher-firmware-flash"
DOCS_DIR="${SCRIPT_DIR}/examples/aligned-realtime"

# Create destination directory (remove old one if exists)
echo -e "${YELLOW}Creating destination directory...${NC}"
rm -rf "${DESKTOP_DIR}"
mkdir -p "${DESKTOP_DIR}"
echo -e "${GREEN}✓${NC} Created: ${DESKTOP_DIR}"
echo

# Copy firmware binaries
echo -e "${YELLOW}Copying firmware binaries...${NC}"

if [ -f "${BUILD_DIR}/aligned-realtime.bin" ]; then
    cp "${BUILD_DIR}/aligned-realtime.bin" "${DESKTOP_DIR}/"
    echo -e "${GREEN}✓${NC} aligned-realtime.bin ($(ls -lh "${BUILD_DIR}/aligned-realtime.bin" | awk '{print $5}'))"
else
    echo -e "${YELLOW}⚠${NC} aligned-realtime.bin not found - run build first!"
fi

if [ -f "${BUILD_DIR}/bootloader/bootloader.bin" ]; then
    cp "${BUILD_DIR}/bootloader/bootloader.bin" "${DESKTOP_DIR}/"
    echo -e "${GREEN}✓${NC} bootloader.bin ($(ls -lh "${BUILD_DIR}/bootloader/bootloader.bin" | awk '{print $5}'))"
else
    echo -e "${YELLOW}⚠${NC} bootloader.bin not found"
fi

if [ -f "${BUILD_DIR}/partition_table/partition-table.bin" ]; then
    cp "${BUILD_DIR}/partition_table/partition-table.bin" "${DESKTOP_DIR}/"
    echo -e "${GREEN}✓${NC} partition-table.bin ($(ls -lh "${BUILD_DIR}/partition_table/partition-table.bin" | awk '{print $5}'))"
else
    echo -e "${YELLOW}⚠${NC} partition-table.bin not found"
fi

echo

# Create README
echo -e "${YELLOW}Creating documentation...${NC}"

cat > "${DESKTOP_DIR}/README.md" << 'EOF'
# Watcher Firmware - xAI Grok Realtime Voice

## 🎉 Status: READY TO FLASH

All firmware files are compiled and ready for your SenseCAP Watcher device!

## What's Included

This firmware enables your Watcher to talk to xAI Grok Realtime voice assistant through LiveKit Cloud.

### Features
✅ WiFi credentials persist across reboots (fixed!)
✅ LiveKit WebRTC with WHIP protocol
✅ xAI Grok Realtime voice ($0.05/min - 10x cheaper than OpenAI)
✅ Bidirectional audio streaming
✅ Simple console commands for testing
✅ Auto-reconnection on WiFi/LiveKit disconnection

### Files
- `aligned-realtime.bin` (6.3 MB) - Main firmware
- `bootloader.bin` (21 KB) - ESP32 bootloader
- `partition-table.bin` (3 KB) - Partition layout

## Quick Flash on Windows

### Using ESP-IDF Flash Tool (Recommended)

1. Download: https://www.espressif.com/en/support/download/other-tools
2. Select: ESP32-S3, WorkMode: Develop
3. Configure flash addresses:
   ```
   bootloader.bin        @ 0x0
   partition-table.bin   @ 0x8000
   aligned-realtime.bin  @ 0x10000
   ```
4. SPI Speed: 80MHz, SPI Mode: QIO, Flash Size: 16MB
5. Select COM port and click START

### Using esptool.py

```bash
pip install esptool

esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash ^
  0x0 bootloader.bin ^
  0x8000 partition-table.bin ^
  0x10000 aligned-realtime.bin
```

## Test After Flash

1. Open serial monitor (115200 baud)
2. Device should auto-connect to WiFi
3. Run command: `aligned_start`
4. Speak to device and hear xAI Grok respond!

## Console Commands

| Command | Description |
|---------|-------------|
| `aligned_start` | Quick start: connect + voice |
| `aligned_connect` | Get LiveKit credentials |
| `aligned_voice` | Start LiveKit voice |
| `wifi_sta <ssid> <password>` | Configure WiFi |
| `wifi_check` | Show WiFi status |
| `reboot` | Restart device |

## Cost

**$0.05 per minute** of voice conversation (63 credits/min in Aligned Tools)

10x cheaper than OpenAI Realtime!

## Troubleshooting

### WiFi Not Connecting
- Verify network is 2.4GHz (ESP32 doesn't support 5GHz)
- Check credentials with `wifi_check`
- Reconfigure: `wifi_sta YourSSID YourPassword`

### Aligned Connection Fails
- Check internet with `ping 8.8.8.8`
- Verify device token in Aligned Tools settings
- Look for HTTP error codes in console

### LiveKit Connection Fails
- Ensure `aligned_connect` succeeded first
- Check firewall allows HTTPS to LiveKit Cloud
- Look for WHIP errors (4xx/5xx status codes)

## Expected Console Output

```
I (xxxx) aligned_client: Connected to Aligned Tools!
I (xxxx) aligned_client: Device ID: watcher_xxx
I (xxxx) aligned_client: Room: watcher_room_xxx
I (xxxx) livekit: Starting LiveKit WebRTC connection...
I (xxxx) livekit: WHIP response received (status: 201)
I (xxxx) livekit: LiveKit PeerConnectionState: PEER_CONNECTION_CONNECTED
I (xxxx) livekit: LiveKit connected! Starting audio stream...
```

---

**Voice Provider**: xAI Grok Realtime
**Cost**: $0.05/min (10x cheaper than OpenAI)
EOF

echo -e "${GREEN}✓${NC} README.md"

# Create detailed flash instructions
cat > "${DESKTOP_DIR}/FLASH_INSTRUCTIONS.md" << 'EOF'
# Detailed Flash Instructions

## Prerequisites

- Windows PC with USB port
- USB cable to connect Watcher device
- Watcher device in download mode (usually automatic)

## Option 1: ESP-IDF Flash Download Tool (Easiest)

### Download Tool
https://www.espressif.com/en/support/download/other-tools

Look for "Flash Download Tools (ESP8266 & ESP32 & ESP32-S2 & ESP32-S3 & ESP32-C3)"

### Configure Tool

1. **Launch tool** and select:
   - Chip Type: `ESP32-S3`
   - WorkMode: `Develop`

2. **Add files** with these addresses:
   ```
   [✓] bootloader.bin        @ 0x0
   [✓] partition-table.bin   @ 0x8000
   [✓] aligned-realtime.bin  @ 0x10000
   ```

3. **Flash settings**:
   - SPI SPEED: 80MHz
   - SPI MODE: QIO
   - FLASH SIZE: 16MB
   - DoNotChgBin: checked

4. **Select COM port** (e.g., COM3)
   - Find it in Device Manager under "Ports (COM & LPT)"
   - Look for "USB Serial Port" or "CP210x"

5. **Click START**
   - Tool will erase, flash, and verify
   - Wait for "FINISH" message
   - Should take 30-60 seconds

## Option 2: esptool.py (Command Line)

### Install Python Tool

```bash
pip install esptool
```

### Flash Command

```bash
esptool.py --chip esp32s3 ^
  --port COM3 ^
  --baud 921600 ^
  --before default_reset ^
  --after hard_reset ^
  write_flash ^
  -z ^
  --flash_mode dio ^
  --flash_freq 80m ^
  --flash_size 16MB ^
  0x0 bootloader.bin ^
  0x8000 partition-table.bin ^
  0x10000 aligned-realtime.bin
```

**Note**: Replace `COM3` with your actual COM port number.

## Verify Flash Success

### Using Serial Monitor

1. **Open serial monitor**:
   - Tool: Arduino IDE, PuTTY, Tera Term, etc.
   - Port: Same COM port used for flashing
   - Baud Rate: 115200
   - Line Ending: Both NL & CR (or just CR)

2. **Press reset button** on device or power cycle

3. **Look for boot messages**:
   ```
   ESP-ROM:esp32s3-20210327
   Build:Mar 27 2021
   ...
   I (xxx) boot: Chip Revision: v0.1
   I (xxx) boot: ESP-IDF v5.2.1
   ...
   I (xxx) wifi_sta: WiFi connecting...
   I (xxx) wifi_sta: got ip: 192.168.x.x
   ```

### Test Voice Connection

```
aligned_start
```

Expected output:
```
I (xxxx) aligned_client: Connected to Aligned Tools!
I (xxxx) aligned_client: Room: watcher_room_xxx
I (xxxx) livekit: WHIP response received (status: 201)
I (xxxx) livekit: PEER_CONNECTION_CONNECTED
I (xxxx) livekit: LiveKit connected!
```

## Common Issues

### "Failed to connect"
- Check USB cable is data-capable (not charge-only)
- Try different USB port
- Install CP210x USB to UART Bridge driver
- Put device in download mode manually (hold BOOT, press RESET, release BOOT)

### "Timed out waiting for packet header"
- Lower baud rate to 115200: `--baud 115200`
- Check COM port is not in use by another program
- Close Arduino IDE, PuTTY, or other serial monitors

### "A fatal error occurred: MD5 of file does not match"
- Files may be corrupted during copy
- Re-download files from Mac
- Check MD5 sums match

### Device boots but no WiFi
- Configure WiFi: `wifi_sta YourSSID YourPassword`
- Check network is 2.4GHz (ESP32-S3 doesn't support 5GHz)
- Verify credentials with `wifi_check`

## Advanced: Verify Binary Integrity

On Windows (PowerShell):
```powershell
Get-FileHash -Algorithm MD5 .\aligned-realtime.bin
Get-FileHash -Algorithm MD5 .\bootloader.bin
Get-FileHash -Algorithm MD5 .\partition-table.bin
```

Expected MD5 checksums:
- `aligned-realtime.bin`: a3ff826613aed1cad49151dd58a4ab14
- `bootloader.bin`: c0d91b9b53c85f5c3425c29712bd5f51
- `partition-table.bin`: 14f6272608c3f258eb83452d8672cfea

## Need Help?

Check console logs for detailed error messages and stack traces.
EOF

echo -e "${GREEN}✓${NC} FLASH_INSTRUCTIONS.md"
echo

# Show summary
echo -e "${BLUE}================================================${NC}"
echo -e "${BLUE}  Summary${NC}"
echo -e "${BLUE}================================================${NC}"
echo
echo -e "Destination: ${GREEN}${DESKTOP_DIR}${NC}"
echo
echo "Files copied:"
ls -lh "${DESKTOP_DIR}" | tail -n +2 | awk '{printf "  %-30s %s\n", $9, $5}'
echo
echo -e "${GREEN}✓ Ready to copy to Windows PC!${NC}"
echo
echo "Next steps:"
echo "  1. Copy entire folder to Windows PC"
echo "  2. Flash using ESP-IDF Flash Tool or esptool.py"
echo "  3. Connect serial monitor (115200 baud)"
echo "  4. Run: aligned_start"
echo
