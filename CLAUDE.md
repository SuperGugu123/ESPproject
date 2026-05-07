# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP-IDF (Espressif IoT Development Framework) project targeting the **ESP32-S3** chip. It implements a smart home sensor node that combines:
- **Wi-Fi connectivity** with CSI (Channel State Information) collection
- **MQTT integration** for Home Assistant
- **Radar-based presence/motion detection** using ESP-Radar
- **Microphone input** for speech recognition (INMP441 I2S)
- **Servo motor control** (SG90舵机)

## Build Commands

```bash
idf.py build              # Build the project
idf.py flash              # Flash to device
idf.py monitor            # Monitor serial output
idf.py build && idf.py flash && idf.py monitor  # Full cycle
idf.py set-target esp32s3 # Set target chip (only needed once)
idf.py menuconfig         # Configure project settings (Wi-Fi SSID, MQTT broker)
```

To run a single test (if test apps exist):
```bash
cd managed_components/<component>/test_apps && idf.py build
```

## Architecture

```
main/
├── main.c           # app_main() - entry point, initializes components
├── Kconfig.projbuild # Project config (MQTT broker URL/username/password)
└── CMakeLists.txt

components/
├── WIFI/            # Wi-Fi station mode + CSI collection
│   ├── wifista.c/h  # Wi-Fi connection management
│   └── wificsi.c/h  # Channel State Information handling
├── MQTT/            # MQTT client for Home Assistant
│   └── app_mqtt.c/h # Async MQTT with command subscription
├── RADAR/           # ESP-Radar presence/motion detection
│   ├── radar.c/h    # State machine with NVS-persisted thresholds
│   └── app_sr.c/h   # Speech recognition integration
├── INMP441/         # I2S microphone driver
├── SG90/            # Servo motor control
└── CMakeLists.txt
```

**Active vs Disabled Features:** `app_main()` currently runs only Wi-Fi + MQTT. Radar, INMP441, and SG90 task are commented out. The commented-out code shows how to re-enable them.

## Key Dependencies

All managed components are defined in `dependencies.lock`:
- `espressif/esp-radar` (v0.3.2) - Wi-Fi radar sensing
- `espressif/esp-sr` (v2.4.1) - Speech recognition
- `espressif/esp_csi_gain_ctrl` - CSI gain control
- `espressif/esp_radar_motion_dec` - Motion decision
- `espressif/cjson` - JSON parsing
- ESP-IDF v5.5.3

## Configuration

Wi-Fi SSID/password are hardcoded in `components/WIFI/wifista.h`:
```c
#define DEFAULT_SSID "SuperGu"
#define DEFAULT_PWD "qwer123456"
```

MQTT broker settings are configurable via `idf.py menuconfig` → Example Configuration:
- `CONFIG_BROKER_URL` (default: `mqtt://homeassistant.local:1883`)
- `CONFIG_BROKER_USERNAME`
- `CONFIG_BROKER_PASSWORD`

Radar thresholds are trained at boot (`RADAR_FORCE_TRAIN_ON_BOOT=1`) and persisted to NVS.

## Key Implementation Details

**MQTT Flow:** The MQTT client is fully asynchronous. `mqtt_app_start()` creates the client and registers an event handler. All publishing functions check `mqtt_connected` before sending. Commands arrive via `MQTT_EVENT_DATA` on topic `esp32/sg90/cmd` with JSON payload `{"angle":90}`.

**Radar State Machine:** Uses hysteresis to avoid rapid state flipping (exit thresholds < enter thresholds). State only changes after `RADAR_STATE_CONFIRM_FRAMES` consecutive matching frames. Thresholds are trained against empty-room baseline and scaled per detection type.

**CSI Collection:** Wi-Fi CSI data is collected via `esp_wifi/radar` APIs and processed in a callback (`radar_rx_cb`).
