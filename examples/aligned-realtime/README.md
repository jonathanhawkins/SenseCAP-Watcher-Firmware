# Aligned Tools Voice Assistant for SenseCAP Watcher

This firmware connects the SenseCAP Watcher to **Aligned Tools voice assistant** powered by xAI Grok Realtime.

## Features

- **10x cheaper** than OpenAI Realtime ($0.05/min vs $0.50/min)
- **Full integration** with Aligned Tools:
  - Work Queue management
  - JIRA integration
  - Gmail integration
  - Calendar queries
  - mem0 memory persistence
- **Same experience** as the web voice dashboard

## Architecture

```
[Watcher] → WebRTC Audio → [LiveKit Cloud] → [xAI Grok Realtime Agent]
                                                        ↓
                                              Your tools & memory
                                                        ↓
[Watcher] ← WebRTC Audio ← [LiveKit Cloud] ← [Agent Response]
```

## Setup

### 1. Register Your Device

First, register your Watcher device with your Aligned Tools account:

```bash
# Using curl (replace YOUR_CLERK_TOKEN with your auth token)
curl -X POST https://aligned.tools/api/py/watcher/register \
  -H "Authorization: Bearer YOUR_CLERK_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"device_name": "My Watcher"}'
```

This returns:
```json
{
  "success": true,
  "device_id": "device_abc123",
  "device_token": "watcher_xxxxx...",
  "message": "Device registered successfully. Save this token securely!"
}
```

### 2. Configure the Watcher

Flash the firmware and configure via serial console:

```sh
# Connect to serial console (115200 baud)
# Set your device token
aligned_token -t watcher_xxxxx...

# Connect to WiFi
wifi_sta -s YOUR_WIFI_SSID -p YOUR_WIFI_PASSWORD
```

### 3. Build from Source

#### Prerequisites

- ESP-IDF v5.2.1 or later
- Python 3.8+

```sh
# Clone the repo (if you haven't already)
git clone https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware
cd SenseCAP-Watcher-Firmware/examples/aligned-realtime

# Initialize submodules
git submodule update --init --recursive

# Build
idf.py set-target esp32s3
idf.py build

# Flash
idf.py -p /dev/ttyUSB0 flash monitor
```

### 4. Pre-built Firmware

You can also flash pre-built firmware:

```sh
cd firmware
pip3 install --upgrade esptool
esptool.py --chip esp32s3 -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 32MB --flash_freq 80m \
  0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x110000 aligned-realtime.bin
```

## Usage

1. **Power on** the Watcher
2. **LED indicators**:
   - 🔵 Blue pulse: Listening
   - 🟡 Yellow: Processing
   - 🟢 Green flash: Action completed
   - 🔴 Red: Error/Disconnected
3. **Speak naturally** - same commands as the web voice dashboard:
   - "Start work queue"
   - "What's on my calendar?"
   - "Create a JIRA ticket for..."
   - "Summarize my emails"

## Differences from OpenAI Version

| Feature | OpenAI Version | Aligned Version |
|---------|---------------|-----------------|
| Cost | $0.50/min | $0.05/min |
| AI Provider | OpenAI | xAI Grok |
| Tools | None | 20+ (JIRA, Gmail, etc.) |
| Memory | None | mem0 persistent memory |
| RAG | None | Organizational knowledge |

## Troubleshooting

### Device won't connect
1. Verify WiFi credentials
2. Check device token is correct
3. Verify internet connectivity

### No audio response
1. Check speaker volume (should be 100%)
2. Verify LiveKit connection in logs
3. Check Aligned Tools credits

### LED stays red
1. Check serial logs for error details
2. Try power cycling the device
3. Re-register device if token expired

## API Reference

### Device Token Configuration

The device token is stored in NVS (non-volatile storage) and persists across reboots.

```c
// Set device token programmatically
aligned_set_device_token("watcher_xxxxx...");

// Get current token
const char* token = aligned_get_device_token();
```

## License

Apache 2.0 - Same as original SenseCAP Watcher firmware.
