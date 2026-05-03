# F1 Sensor - ESP32 Client

ESP32 firmware that connects to the official F1 Live Timing service via SignalR and drives a WS2812B LED strip to reflect real-time track status (flags, safety car, session state).

## Features

- **Live F1 data** — subscribes to `TrackStatus`, `SessionStatus`, and `SessionInfo` streams from `livetiming.formula1.com`
- **LED animations** — distinct effects per flag state: comet chase (idle/yellow), alternating segments (VSC/SC/red), timed green pulses (10-second clear window), chequered sweep (finish)
- **Configurable delay** — queues flag changes with a configurable delay (default 45s) so LED state can match broadcast timing
- **Web UI** — serves a local dashboard over HTTP with real-time SSE updates for status and configuration
- **WiFi provisioning** — uses WiFiManager captive portal on first boot (AP: `F1-Sensor-Setup`); LED 0 blinks blue while WiFi setup is in progress (slow = connecting, fast = AP portal open)
- **mDNS** — accessible at `http://f1sensor.local`
- **Persistent config** — LED count, brightness, and delay stored in NVS

## Hardware

- ESP32 dev board (defaults target ESP32-C3 DevKitM-1)
- WS2812B LED strip on GPIO 4 (configurable in `src/config.h`)
- Up to 500 LEDs supported

## Setup

Built with [PlatformIO](https://platformio.org/). Install the [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html) (or the VS Code extension) before continuing.

### 1. Configure the LED data pin

Open `src/config.h` and set `LED_DATA_PIN` to the GPIO your strip's data line is wired to:

```c
#define LED_DATA_PIN  4   // change to match your wiring
```

### 2. Configure the board (if needed)

The default board in `platformio.ini` is `esp32-c3-devkitm-1`. If you're using a different ESP32 variant (classic ESP32, ESP32-S3, ESP32-S2, etc.), update the `board` line:

```ini
[env:esp32dev]
platform  = espressif32
board     = esp32dev          ; or esp32-s3-devkitc-1, etc.
framework = arduino
```

A list of supported boards is available in the [PlatformIO board index](https://docs.platformio.org/en/latest/boards/index.html#espressif-32).

### 3. Build and flash

Connect the board over USB, then:

```sh
pio run -t upload            # build + flash firmware
pio run -t uploadfs          # upload web UI files to LittleFS (run once, and again whenever data/ changes)
pio device monitor           # optional: watch serial logs
```

### 4. First boot — WiFi provisioning

On first boot (or after `/api/wifi/reset`) the device has no saved WiFi credentials and starts a captive portal:

1. LED 0 blinks blue — slow (500 ms) while it tries any saved credentials, fast (150 ms) once the AP portal is up.
2. On your phone or laptop, connect to the WiFi network **`F1-Sensor-Setup`**.
3. The captive portal should open automatically. If it doesn't, browse to `http://192.168.4.1`.
4. Pick your home network and enter its password. The ESP32 saves the credentials and reboots into normal mode.

### 5. Open the dashboard

Once connected, browse to **`http://f1sensor.local`** (or the IP printed over serial) to see live flag status and tweak LED count, brightness, and broadcast delay. Settings are stored in NVS and persist across reboots.

## API

| Endpoint            | Method | Description                  |
|----------------------|--------|------------------------------|
| `/api/status`        | GET    | Current flag/session state   |
| `/api/config`        | GET    | Read LED count, brightness, delay |
| `/api/config`        | POST   | Update config (JSON body)    |
| `/api/restart`       | POST   | Reboot the ESP32             |
| `/api/wifi/reset`    | POST   | Clear WiFi credentials and reboot |
| `/events`            | SSE    | Real-time status stream      |

## How It Works

1. **First boot** — the ESP32 creates a WiFi access point called `F1-Sensor-Setup`. While setup is in progress LED 0 blinks blue: a slow blink (500 ms) means it's trying saved credentials; a fast blink (150 ms) means the AP portal is up and waiting for you. Connect to it with your phone or laptop and pick your home network from the captive portal. Credentials are saved for future boots.

2. **Connects to F1 Live Timing** — once online, the firmware negotiates a SignalR connection to `livetiming.formula1.com` and subscribes to the live data streams. The connection pill in the web UI shows the current state (Connecting / Connected / Reconnecting).

3. **Tracks session state** — incoming `SessionStatus` and `TrackStatus` messages are parsed into a flag (Green, Yellow, VSC, SC, Red, Chequered). Each flag change is pushed into a delay queue so the LEDs stay in sync with your broadcast feed.

4. **Drives the LED strip** — the `LedController` runs a per-flag animation loop at ~30-200 fps depending on the effect:
   | Flag | Animation |
   |------|-----------|
   | Idle | Dim red base + red comet |
   | Green (Clear) | Green pulse effect for ~10 seconds, then back to idle |
   | Yellow | Yellow comet chase |
   | VSC | Slow alternating yellow/off segments |
   | SC | Fast alternating yellow/off segments |
   | Red | Alternating bright/dark red segments |
   | Chequered | Scrolling black/white segments + white flash |

5. **Web dashboard** — browse to `http://f1sensor.local` (or the device IP) to see live flag status, session info, and SignalR connection state via SSE. The Settings card lets you adjust LED count, brightness, and broadcast delay. The Device card has restart and WiFi reset buttons.

## TODO

- [ ] Configurable LED setup — a config file (or web UI section) where you can define FastLED animations, color palettes, and strip segments per flag state instead of hardcoded effects

## Project Structure

```
src/
  main.cpp            — setup/loop, SignalR callback, flag queue logic
  config.h            — hardware pins, timing constants, NVS config struct
  f1_state.h/cpp      — F1Flag/SessionStatus enums, state machine, delay queue
  signalr_client.h/cpp — SignalR negotiation, WebSocket, auto-reconnect
  led_controller.h/cpp — FastLED animations per flag state
  web_server.h/cpp     — async HTTP server, REST API, SSE
data/
  index.html, style.css, app.js — web dashboard (served from LittleFS)
```
