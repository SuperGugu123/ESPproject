# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP-IDF (Espressif IoT Development Framework) project targeting the **ESP32-S3** chip. It implements a smart home sensor node that combines:
- **Wi-Fi connectivity** with CSI (Channel State Information) collection
- **MQTT integration** for Home Assistant
- **Radar-based presence/motion detection** using ESP-Radar
- **Microphone input** for speech recognition (INMP441 I2S)
- **Servo motor control** (SG90舵机)
- **OTA firmware updates** via HTTP

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
├── main.c           # app_main() - entry point, initializes all components
├── Kconfig.projbuild # Project config (MQTT broker URL/username/password)
└── CMakeLists.txt

components/
├── WIFI/            # Wi-Fi station mode + CSI collection
│   ├── wifista.c/h  # Wi-Fi connection management
│   └── wificsi.c/h  # Channel State Information handling
├── MQTT/            # MQTT client for Home Assistant
│   └── app_mqtt.c/h # Async MQTT with command subscription and radar state publishing
├── RADAR/           # ESP-Radar presence/motion detection
│   ├── radar.c/h    # State machine with NVS-persisted thresholds
│   └── app_sr.c/h   # Speech recognition integration
├── OTA/             # Over-the-air firmware updates
│   └── app_ota.c/h  # HTTP download, version check, rollback protection
├── INMP441/         # I2S microphone driver
├── SG90/            # Servo motor control (LEDC PWM, mutex-protected)
├── LCD/             # LCD display (stub, not yet implemented)
└── CMakeLists.txt
```

**Current active flow:** `app_main()` initializes SG90, Wi-Fi, MQTT, radar, starts router ping, creates a `radar_task` that publishes radar state (empty/occupied) to MQTT, runs radar training if enabled, then initializes INMP441 and speech recognition.

## Key Dependencies

Defined in `dependencies.lock`:
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

MQTT broker settings are hardcoded in `components/MQTT/app_mqtt.h`:
```c
#define MQTT_BROKER_URL "mqtt://10.45.91.78:1883"
#define MQTT_BROKER_USERNAME "supergu"
#define MQTT_BROKER_PASSWORD "1284529154"
```

OTA update URL is defined in `components/OTA/app_ota.h` (`OTA_UPDATE_HTTP_URL`).

## Partition Table

The project uses a custom [partitions.csv](partitions.csv) with OTA support:
- `factory` (4M) — factory app partition
- `ota_0` / `ota_1` (1M each) — two OTA slots for fail-safe updates
- `otadata` — OTA state tracking (which slot to boot, rollback info)
- `storage` (5M SPIFFS) and `model` (4M SPIFFS) — data partitions

## Key Implementation Details

**MQTT Flow:** The client is fully asynchronous. `mqtt_app_start()` creates the client and registers an event handler. Connection state is tracked via `mqtt_is_connected()`. Inbound commands arrive on topic `esp32/sg90/cmd` with JSON `{"angle":90}` and are parsed to control the servo. Radar state is published to MQTT via `mqtt_radar_state_use()` / `mqtt_radar_state_empty()`.

**Radar State Machine:** Uses hysteresis (exit thresholds < enter thresholds). State only changes after `RADAR_STATE_CONFIRM_FRAMES` consecutive matching frames. Thresholds are trained against empty-room baseline and scaled per detection type. Training runs at boot if `RADAR_FORCE_TRAIN_ON_BOOT` is set, and thresholds persist in NVS.

**CSI Collection:** Wi-Fi CSI data is collected via `esp_wifi/radar` APIs and processed in `radar_rx_cb`.

**OTA:** Fetches firmware from an HTTP URL, validates the image header (version check against current and last-invalid firmware), writes to the inactive OTA slot. On boot, `ota_start()` checks if the new image booted successfully and marks it valid (or triggers rollback). Uses `esp_ota_begin` / `esp_ota_write` / `esp_ota_end` with explicit rollback handling.

**SG90:** LEDC PWM control on GPIO 7, mutex-protected. `sg90_set_angle()` clamps to [0,180], converts angle to pulse width (500-2500us at 50Hz), and applies via LEDC duty cycle.

**Main loop:** The `radar_task` polls `radar_get_state()` every 500ms and publishes MQTT state changes (1 when MOTION/PRESENCE, 0 when EMPTY) — but only when MQTT is connected.
