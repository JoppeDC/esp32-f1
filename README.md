# F1 Sensor - ESP32 Client

ESP32 firmware that drives a WS2812B LED strip from live Formula 1 track status
— flags, safety car, session state. It gets that state from an
[F1 relay](https://f1-relay.joppe.dev) over a single WebSocket, so the lamp
never has to speak F1's own protocol and doesn't break when that protocol
changes.

Out of the box it points at the public relay at `wss://f1-relay.joppe.dev/ws`.
Flash it, join your WiFi, and it's live — no account, no key, no configuration.

## Features

- **Live F1 data** — one WebSocket to the relay; every message is the complete
  state, so there's nothing to resync after a reconnect
- **LED animations** — distinct effects per flag state: comet chase
  (idle/yellow), alternating segments (VSC/SC/red), timed green pulses
  (10-second clear window), chequered sweep (finish)
- **Configurable delay** — queues flag changes with a configurable delay
  (default 45 s) so LED state matches your broadcast feed
- **Web UI** — a local dashboard over HTTP with real-time SSE updates for status
  and configuration
- **WiFi provisioning** — WiFiManager captive portal on first boot
  (AP: `F1-Sensor-Setup`); LED 0 blinks blue while setup is in progress
  (slow = connecting, fast = AP portal open)
- **mDNS** — reachable at `http://f1sensor.local`
- **Persistent config** — LED count, brightness, delay, and relay URL in NVS

## Hardware

- ESP32 dev board (defaults target ESP32-C3 DevKitM-1)
- WS2812B LED strip on GPIO 4 (configurable in `src/config.h`)
- Up to 500 LEDs supported

## Setup

Built with [PlatformIO](https://platformio.org/). Install the
[PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
(or the VS Code extension) before continuing.

### 1. Configure the LED data pin

Open `src/config.h` and set `LED_DATA_PIN` to the GPIO your strip's data line is
wired to:

```c
#define LED_DATA_PIN  4   // change to match your wiring
```

### 2. Configure the board (if needed)

The default board in `platformio.ini` is `esp32-c3-devkitm-1`. For a different
ESP32 variant (classic ESP32, ESP32-S3, ESP32-S2, etc.), update the `board`
line:

```ini
[env:esp32dev]
platform  = espressif32
board     = esp32dev          ; or esp32-s3-devkitc-1, etc.
framework = arduino
```

Supported boards are listed in the
[PlatformIO board index](https://docs.platformio.org/en/latest/boards/index.html#espressif-32).

### 3. Build and flash

Connect the board over USB, then:

```sh
pio run -t upload            # build + flash firmware
pio run -t uploadfs          # upload web UI files to LittleFS (once, and again whenever data/ changes)
pio device monitor           # optional: watch serial logs
```

### 4. First boot — WiFi provisioning

On first boot (or after `/api/wifi/reset`) the device has no saved credentials
and starts a captive portal:

1. LED 0 blinks blue — slow (500 ms) while trying saved credentials, fast
   (150 ms) once the AP portal is up.
2. On your phone or laptop, join the WiFi network **`F1-Sensor-Setup`**.
3. The captive portal should open automatically. If it doesn't, browse to
   `http://192.168.4.1`.
4. Pick your home network and enter its password. The ESP32 saves the
   credentials and reboots into normal mode.

### 5. Open the dashboard

Browse to **`http://f1sensor.local`** (or the IP printed over serial) for live
flag status and to tweak LED count, brightness, broadcast delay, and the relay
URL. Settings persist in NVS across reboots.

## Choosing a relay

The lamp needs a relay to talk to. The default is the public instance, and for
most people that's the end of it.

| Relay URL | Notes |
|---|---|
| `wss://f1-relay.joppe.dev/ws` | Public instance. The default. |
| `ws://192.168.1.50:8000/ws` | Your own, on the LAN. |
| *(empty)* | Disables the connection; the lamp sits idle. |

To run your own, see the [F1 relay repository](https://github.com/JoppeDC/f1-relay).
Point the lamp at port `8000` — the public feed. Never at `8001`, which is the
admin listener.

The URL is parsed on-device and must include the scheme. `wss://` defaults to
port 443, `ws://` to port 80, and an omitted path becomes `/ws`. A malformed URL
is rejected with an explanation in the web UI rather than silently ignored.
IPv6 literals are not supported.

**On TLS:** `wss://` connections are encrypted but the certificate chain is not
validated. The relay carries public race-flag data and the lamp sends no
credentials, so the only thing a successful MITM buys is a wrong LED colour —
not worth pinning a CA that would brick every deployed lamp if the issuer ever
changed. A side effect is that self-hosted relays with self-signed certificates
work over `wss://` with no extra setup.

## API

| Endpoint | Method | Description |
|---|---|---|
| `/api/status` | GET | Current flag/session state |
| `/api/config` | GET | Read LED count, brightness, delay, relay URL |
| `/api/config` | POST | Update config (JSON body) |
| `/api/restart` | POST | Reboot the ESP32 |
| `/api/wifi/reset` | POST | Clear WiFi credentials and reboot |
| `/api/flag/override` | POST | Force an animation for testing |
| `/api/debug/system` | GET | Static system and WiFi info |
| `/api/debug/live` | GET | Fast-changing relay/LED/state info |
| `/events` | SSE | Real-time status stream |

## How It Works

1. **First boot** — the ESP32 creates a WiFi access point called
   `F1-Sensor-Setup`. Connect to it and pick your home network from the captive
   portal; credentials are saved for future boots.

2. **Connects to the relay** — once online, the firmware opens a WebSocket to
   the configured relay URL. The connection pill in the web UI shows the current
   state (Connected / Reconnecting / No relay / Bad relay URL).

3. **Tracks flag state** — every relay message carries the full state. The
   lamp reads `display` — one of `RED`, `SC`, `VSC`, `YELLOW`, `CLEAR`, `CHEQ`,
   `IDLE` — and pushes each change into a delay queue so the LEDs stay in sync
   with your broadcast feed.

4. **Drives the LED strip** — `LedController` runs a per-flag animation loop at
   ~30–200 fps depending on the effect:

   | Flag | Animation |
   |---|---|
   | Idle | Dim red base + red comet |
   | Green (Clear) | Green pulse effect for ~10 seconds, then back to idle |
   | Yellow | Yellow comet chase |
   | VSC | Slow alternating yellow/off segments |
   | SC | Fast alternating yellow/off segments |
   | Red | Alternating bright/dark red segments |
   | Chequered | Scrolling black/white segments + white flash |

5. **Web dashboard** — `http://f1sensor.local` shows live flag status, session
   info, and relay connection state via SSE. The Settings card adjusts LED
   count, brightness, delay, and relay URL. The Device card has restart and WiFi
   reset buttons.

### Staying honest when the data stops

Two things can leave the lamp showing a flag that is no longer true: the relay
losing its own upstream F1 link (it keeps serving last-known state, marked
`stale`), or the lamp losing the relay entirely.

Both are the same condition — no fresh state — so both are handled by one
timer. After **5 minutes** without a message the relay vouched for, the LEDs
fall back to idle rather than sitting on a safety car that ended an hour ago.
The web UI shows `stale` and the age of the last fresh message throughout, so
you can tell a quiet session from a broken link.

Reconnection uses exponential backoff, 5 s doubling to a 60 s cap, reset on a
successful connect.

## TODO

- [ ] Configurable LED setup — a config file (or web UI section) to define
  FastLED animations, color palettes, and strip segments per flag state instead
  of hardcoded effects

## Project Structure

```
src/
  main.cpp             — setup/loop, relay message handler, flag queue logic
  config.h             — hardware pins, NVS config struct
  f1_state.h/cpp       — F1Flag/SessionStatus enums, state machine, delay queue
  relay_client.h/cpp   — relay URL parsing, WebSocket client, auto-reconnect
  led_controller.h/cpp — FastLED animations per flag state
  web_server.h/cpp     — async HTTP server, REST API, SSE
data/
  index.html, style.css, app.js — web dashboard (served from LittleFS)
  debug.html, debug.js          — debug page
```
