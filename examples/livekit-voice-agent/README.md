# LiveKit Voice Agent for SenseCAP Watcher

This example uses the **official LiveKit ESP32 Client SDK** for bidirectional voice conversations with xAI Grok Realtime via the Aligned Tools backend.

## Why This Approach?

The previous `aligned-realtime` firmware used WHIP (WebRTC-HTTP Ingestion Protocol), which is:
- ❌ **One-way only** (client → server streaming)
- ❌ **Not designed for conversations** (requires separate WHEP for receiving audio)
- ❌ **Custom implementation** (not officially supported by LiveKit)

This new firmware uses the **official LiveKit ESP32 Client SDK**, which provides:
- ✅ **Bidirectional audio** (proper WebRTC signaling)
- ✅ **Official LiveKit support** (built with Espressif)
- ✅ **Voice AI examples** (optimized for agent conversations)
- ✅ **Hardware-optimized** (uses ESP32-S3's capabilities efficiently)

## Architecture

```
┌─────────────────┐       WebSocket        ┌──────────────────┐
│  Watcher Device │ ◄──────Signaling──────► │ LiveKit Cloud    │
│   (ESP32-S3)    │                         │                  │
└─────────────────┘                         └──────────────────┘
         │                                           │
         │ WebRTC Audio (bidirectional)             │
         └───────────────────────────────────────────┘
                                                     │
                                        ┌────────────▼──────────────┐
                                        │  LiveKit Agent (Python)   │
                                        │  ► xAI Grok Realtime      │
                                        └───────────────────────────┘
```

## Requirements

- **ESP-IDF v5.4+** (tested with v5.4.3)
- **ESP32-S3** chip (SenseCAP Watcher has this)
- **LiveKit ESP32 SDK v0.3.4**
- **Aligned Tools backend** (for device registration and credentials)

## Setup

1. **Source ESP-IDF v5.4 environment:**
   ```bash
   . ~/esp/esp-idf-v5.4.3/export.sh
   ```

2. **Create project from LiveKit Voice AI example:**
   ```bash
   cd /Users/light/dev/web-apps/aligned-tools/hardware/watcher-firmware/examples/livekit-voice-agent
   idf.py create-project-from-example "livekit/livekit=0.3.4:voice_agent"
   ```

3. **Configure for Watcher hardware:**
   ```bash
   idf.py menuconfig
   # Set target to ESP32-S3
   # Configure WiFi credentials
   # Configure audio I2S pins (match Watcher hardware)
   ```

4. **Build and flash:**
   ```bash
   idf.py build
   idf.py -p /dev/cu.usbmodem* flash monitor
   ```

## Configuration

The firmware will:
1. Connect to WiFi
2. Register with Aligned backend (`POST /api/py/watcher/device/register`)
3. Get LiveKit credentials (`POST /api/py/watcher/device/connect`)
4. Connect to LiveKit room using official SDK
5. Start bidirectional voice conversation with xAI Grok Realtime agent

## Backend Integration

Uses the same backend endpoints as the previous firmware:
- `/api/py/watcher/device/register` - Register device and get token
- `/api/py/watcher/device/connect` - Get LiveKit URL and participant token

The backend already creates rooms and dispatches agents - the only change is the device now uses proper WebSocket signaling instead of WHIP.

## Cost

- **xAI Grok Realtime**: $0.05/minute (10x cheaper than OpenAI)
- Same cost as web dashboard voice feature

## Resources

- [LiveKit ESP32 SDK GitHub](https://github.com/livekit/client-sdk-esp32)
- [LiveKit ESP32 Blog Post](https://blog.livekit.io/livekit-sdk-for-esp32-bringing-voice-ai-to-embedded-devices/)
- [LiveKit Documentation](https://docs.livekit.io/)
- [Aligned Tools Backend](https://github.com/yourusername/aligned-tools)

## Differences from Previous Firmware

| Feature | `aligned-realtime` (WHIP) | `livekit-voice-agent` (SDK) |
|---------|---------------------------|----------------------------|
| Protocol | WHIP (HTTP-based, one-way) | WebSocket (bidirectional) |
| Audio Direction | Send only (needed WHEP for receive) | Send + Receive |
| Implementation | Custom WebRTC stack | Official LiveKit SDK |
| Agent Integration | Agent waits forever | Agent recognizes participant |
| Maintenance | Custom code | Officially supported |
| ESP-IDF Version | v5.2.1 | v5.4.3+ |

## Status

🚧 **In Development** - Testing official LiveKit SDK approach to replace WHIP implementation.
