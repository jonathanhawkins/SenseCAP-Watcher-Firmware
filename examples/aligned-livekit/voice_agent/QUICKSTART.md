# Quick Start: LiveKit Voice Agent for Watcher

This guide helps you build and flash the LiveKit-based voice firmware to your SenseCAP Watcher.

## Prerequisites

1. **ESP-IDF v5.4.3** installed at `~/esp/esp-idf-v5.4.3` — see install
   instructions in `aligned-tools/.claude/rules/hardware.md` (the parent
   repo). Or briefly:
   ```bash
   mkdir -p ~/esp && cd ~/esp
   git clone -b v5.4.3 --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.4.3
   cd esp-idf-v5.4.3 && ./install.sh esp32s3
   ```
2. **esptool 5.2.0+** for the incremental fast-flash path:
   ```bash
   brew install python && pip3 install --upgrade 'esptool>=5.2.0'
   /opt/homebrew/bin/esptool version   # expect 5.2.0 or newer
   ```
3. **Watcher device** connected via USB — verify with
   `ls /dev/cu.usbmodem*` (should show two ports ending in `01` and `03`).
4. **Aligned backend** running (locally or production), and a
   **device token** from `/api/py/watcher/device/register`.

## Step 1: Configure WiFi

Edit `sdkconfig.watcher`:

```ini
CONFIG_LK_EXAMPLE_WIFI_SSID="YourNetworkName"
CONFIG_LK_EXAMPLE_WIFI_PASSWORD="YourPassword"
```

## Step 2: Update Backend URL

Edit `main/aligned_client.h` line 13:

```c
// For local development:
#define ALIGNED_API_BASE "http://192.168.1.71:3000"

// For production:
// #define ALIGNED_API_BASE "https://your-production-domain.com"
```

## Step 3: Source ESP-IDF Environment

```bash
cd /Users/light/dev/web-apps/aligned-tools/hardware/watcher-firmware/examples/livekit-voice-agent/voice_agent
. ~/esp/esp-idf-v5.4.3/export.sh
```

## Step 4: Set Target and Build

```bash
# Set ESP32-S3 target
idf.py set-target esp32s3

# Load Watcher-specific configuration
cp sdkconfig.watcher sdkconfig.defaults

# Build the firmware
idf.py build
```

## Step 5: Flash to Device

```bash
# Find your device port (usually /dev/cu.usbmodem*)
ls /dev/cu.usbmodem*

# Flash and monitor
idf.py -p /dev/cu.usbmodem56D50186503 flash monitor
```

## Step 6: Set Device Token

After firmware boots, use the console to set your device token:

```
Aligned> aligned_set_token watcher_xxxxxxxxxxxxxx
```

Or hardcode it in `main.c` for testing:

```c
void app_main(void) {
    // ... initialization code ...
    aligned_set_device_token("watcher_your_token_here");
    join_room();
}
```

## Step 7: Test Voice Connection

1. Device should connect to WiFi
2. Fetch credentials from Aligned backend
3. Connect to LiveKit room
4. Agent joins and voice conversation begins!

## Troubleshooting

### "No device token configured"
- Set token via console or hardcode in main.c

### "HTTP request failed"
- Check ALIGNED_API_BASE URL
- Verify backend is reachable from device's network
- Check WiFi credentials

### "API returned error status: 401"
- Device token is invalid or expired
- Register device again via backend

### "Failed to connect to room"
- Check LiveKit credentials
- Verify agent is running (`livekit-agent`)
- Check LiveKit Cloud dashboard for room status

## Architecture

```
Watcher Device (ESP32-S3)
         ↓
   [WiFi Connection]
         ↓
   Aligned Backend API
   POST /api/py/watcher/device/connect
         ↓
   {livekit_url, participant_token, room_name}
         ↓
   LiveKit Cloud (WebSocket Signaling)
         ↓
   LiveKit Agent (Python)
         ↓
   xAI Grok Realtime ($0.05/min)
```

## Next Steps

- Customize RPC handlers in `example.c` for Watcher hardware
- Adjust audio settings in `media.c` for Watcher's microphone/speaker
- Add Watcher-specific features (camera, sensors, display)

## Resources

- [LiveKit ESP32 SDK Docs](https://github.com/livekit/client-sdk-esp32)
- [ESP-IDF Getting Started](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
- [Aligned Backend API](../../../api/routes/watcher/)
