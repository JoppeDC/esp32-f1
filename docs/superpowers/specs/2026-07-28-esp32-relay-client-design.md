# ESP32 Relay Client — Finalisation Design

**Date:** 2026-07-28
**Status:** Approved, ready for implementation
**Branch:** `feat/relay-client`

## Context

The `feat/relay-client` branch already replaced the lamp's direct SignalR
connection to `livetiming.formula1.com` with a WebSocket client for the F1
relay. That relay is now deployed at `https://f1-relay.joppe.dev` and serving
live state.

The branch cannot reach it. `RelayClient::begin()` calls
`_ws.begin(host, port, "/ws")` — plain `ws://` only — and `config.h` stores a
`relay_host` + `relay_port` pair defaulting to port 8000. The public instance is
HTTPS-only (`:80` returns `301` to `:443`). As it stands the firmware only works
against a LAN-hosted relay.

This design closes that gap and finishes the remaining work on the branch:
protocol conformance against the relay's published client contract, the
SignalR-era naming still scattered through the web layer, and the README, which
still documents an architecture the code no longer has.

The relay's contract at `https://f1-relay.joppe.dev/llms.txt` is canonical and
is treated as the authority throughout.

## Platform findings

These were verified against the installed toolchain before designing, and
several of them narrowed the options materially.

| Finding | Where | Consequence |
|---|---|---|
| Arduino core is **2.0.17** | `framework-arduinoespressif32/package.json` | `WiFiClientSecure` with `setCACert` / `setCACertBundle` / `setInsecure` |
| `CONFIG_MBEDTLS_HAVE_TIME_DATE` **is not set** | `tools/sdk/esp32c3/sdkconfig:1500` | mbedTLS does not check certificate validity dates — **no NTP sync needed before TLS** |
| Arduino's `esp_crt_bundle.c` has **no default bundle** | `libraries/WiFiClientSecure/src/esp_crt_bundle.c:181` | `arduino_esp_crt_bundle_attach()` logs `"Failed to attach bundle"` and bails unless `setCACertBundle()` was given a blob. There is no free ride to the Mozilla root store |
| `HAS_SSL` is defined for ESP32 | `WebSockets.h:308` | `beginSSL` / `beginSslWithCA` / `beginSslWithBundle` all available, no library fork |
| `SSL_FINGERPRINT_IS_SET` is `(_fingerprint.length())`, default `""` | `WebSocketsClient.h:134` | A bare `beginSSL(host, port, path)` resolves to `setInsecure()` |
| Server pings are auto-answered | `WebSockets.cpp:493` | `WSop_ping` → `WSop_pong` automatically; the relay's 30 s keepalive is satisfied |
| **Close codes are invisible to the application** | `WebSockets.cpp:503` | The code is parsed only inside `#ifndef NODEBUG_WEBSOCKETS` for a debug print, then `clientDisconnect(client, 1000)` runs unconditionally and `WStype_DISCONNECTED` arrives with a NULL payload. The contract's `1013` subscriber-limit close **cannot** be distinguished from any other disconnect |

## Decisions

### 1. Connection config: a single URL field

`config.h` drops `relay_host` and `relay_port` in favour of one field:

```cpp
String relay_url = "wss://f1-relay.joppe.dev/ws";   // empty = disabled
```

Rejected: keeping host + port and adding a `relay_tls` bool (three controls for
one concept, and the user has to know that 443 pairs with the checkbox);
inferring TLS from port 443 (a hidden rule that breaks for TLS on a
non-standard port).

A single URL is what the relay's own documentation gives users to paste, and it
carries the path, so a relay behind a reverse proxy on a non-`/ws` route works
without a firmware change.

**No NVS migration.** The relay branch was never released, so no deployed device
has `relay_host` set; devices still running the SignalR firmware have neither
key and will take the new default. Stale `relay_host` / `relay_port` entries
left by development flashes remain in NVS unused, which is harmless. The NVS key
name limit is 15 characters; `relay_url` is 9.

### 2. Default target: the public relay

First boot (and post-NVS-erase) defaults to `wss://f1-relay.joppe.dev/ws`, so
the lamp works out of the box: flash, join WiFi, LEDs go live.

The accepted cost is that every lamp anyone builds points at the public instance
by default, making `MAX_SUBSCRIBERS` (256) and bandwidth the ceiling on how many
lamps can exist. Users can repoint at their own relay in Settings.

### 3. TLS: encrypt, do not validate

```cpp
u.tls ? _ws.beginSSL(u.host.c_str(), u.port, u.path.c_str())
      : _ws.begin   (u.host.c_str(), u.port, u.path.c_str());
```

`beginSSL` with the default empty fingerprint resolves to `setInsecure()` —
TLS for confidentiality and to speak to `:443` at all, without certificate chain
validation.

Rejected: pinning ISRG Root X1/X2 (~3 KB flash); embedding the Mozilla root
bundle (~65 KB flash plus a generated blob in the repo).

The reasoning is threat model, not convenience. The relay carries public
race-flag state, the lamp sends no credentials, and the only capability an
attacker gains from a successful MITM is showing the wrong LED colour — while
needing a position on the network path to do it. Against that, pinning is the
only option that can *break a working lamp*: if `f1-relay.joppe.dev` ever moves
to a different issuer, every deployed lamp silently stops connecting, and with
no OTA in this firmware each one needs a USB reflash. That is an asymmetric
maintenance liability bought with no meaningful security.

A side benefit: anyone self-hosting with a self-signed certificate can use
`wss://` on their LAN with no special casing.

**Revisit if** the relay protocol ever grows something the lamp *acts on* rather
than displays — a command channel, an OTA URL, a firmware pointer. At that point
`beginSslWithCA` is a two-line swap, and `beginSslWithBundle` already exists in
the pinned library version.

### 4. URL parsing

A free function in `relay_client.h/cpp`, shared by `RelayClient::begin()` and
the `/api/config` validator so the UI and the connection cannot disagree about
what is valid:

```cpp
struct RelayUrl {
    bool     tls  = false;
    String   host;
    uint16_t port = 0;
    String   path;
};

bool parseRelayUrl(const String& url, RelayUrl& out);
```

Rules:

- `wss://` → `tls = true`, default port 443. `ws://` → `tls = false`, default port 80.
- **Scheme is required**; anything else is rejected. Guessing fails in both
  directions — assume `wss` and a LAN user typing `192.168.1.5:8000` fails,
  assume `ws` and anyone typing a public hostname fails. A rejection with a
  visible reason beats a silent wrong guess.
- Host runs from after `://` to the first `:`, `/`, or end of string.
- Optional `:port`, validated 1–65535.
- Path runs from the first `/` after the host to the end. Empty → `/ws`. An
  explicit path (including a bare `/`) is used as given.
- Empty URL is not a parse error — it means "disabled", as today.

**IPv6 literals (`ws://[fe80::1]:8000/ws`) are not supported.** Deliberate
scope cut; mDNS and IPv4 cover the LAN case.

Validation runs in `/api/config` (rejecting with HTTP 400 and a reason the UI
displays) and again defensively in `begin()`.

### 5. Reconnect backoff

5 s → 10 → 20 → 40 → 60 s cap, doubling on each disconnect, reset to 5 s on a
successful connect and on every `begin()`. Implemented by re-calling
`_ws.setReconnectInterval()` from the event callback; `WStype_DISCONNECTED`
fires on every failed retry as well as on a real disconnect, so escalation is
automatic. Bounds are `RECONNECT_INTERVAL_MS` (existing, 5 s) and a new
`RECONNECT_INTERVAL_MAX_MS` (60 s).

This satisfies the contract's "reconnect with backoff", and is the only
available mitigation for a `1013` subscriber-limit close, since the close code
is unreadable (see platform findings). It matters more than it otherwise would
because the public relay is now the default target for every lamp.

### 6. Staleness and connection loss: one timer

The relay keeps serving last-known state with `stale: true` when its upstream F1
link drops, rather than disconnecting. The contract leaves the trust window to
the client.

There is a second failure with identical symptoms: if the **connection** drops
mid-session under SC, the lamp holds SC forever. That is the more likely of the
two, since it includes the lamp's own WiFi dropping.

Both are the same condition — *no fresh authoritative state* — so they get one
mechanism, not two. `RelayClient` tracks `_lastFreshMs`, bumped on every message
with `stale == false`, and exposes it as `getFreshAgeMs()`. Past
`STALE_IDLE_TIMEOUT_MS` (5 min), LEDs fall back to IDLE. A dead connection
delivers no messages at all, so it is covered by the same timer for free.

Display priority in `loop()`:

```
override active         → override flag
else no fresh state 5m  → IDLE
else CLEAR window > 10s → IDLE
else                    → f1State.currentFlag
```

This is display-side only and never touches the 45 s delay queue. The web UI
surfaces `stale` and the age of the last fresh message regardless of the
timeout, so a frozen lamp is diagnosable without a serial console.

The timeout comparison lives in `loop()` alongside the CLEAR window and uses the
existing wrap-safe `elapsedMs()` helper in `main.cpp`. `getFreshAgeMs()` returns
a `millis()` difference, which is wrap-safe by unsigned arithmetic, matching how
`getLastMessageAgoMs()` already works.

### 7. Protocol version handling

`v` absent, or `v == 1` → accept. Any other value → ignore the message and
report `incompatible` through `getStateStr()` for the UI, rather than driving
LEDs from a shape we do not understand. The contract states `v` increments only
on breaking changes, and that clients must ignore unknown fields — new fields
within `v: 1` stay compatible.

### 8. SignalR-era naming sweep

| Location | Change |
|---|---|
| `web_server.cpp:22` | `doc["sigRState"]` → `doc["relayState"]` |
| `web_server.cpp:65` | `doc["signalr"]` object → `doc["relay"]` |
| `web_server.cpp:68-69` | Drop the duplicated `lastHeartbeatAgoMs` / `lastMessageAgoMs` pair, keep one |
| `web_server.cpp:137-138,184-194` | `relay_host`/`relay_port` → `relay_url` in GET and POST `/api/config` |
| `data/index.html:49` | `stat-signalr` → `stat-relay` |
| `data/index.html:100-108` | Host + port inputs → single URL input, updated hint text |
| `data/app.js:9,53` | `statSignalR` → `statRelay` |
| `data/app.js:100-101,132-133` | `relay_host`/`relay_port` → `relay_url` |
| `data/debug.js:90-91` | `d.signalr` → `d.relay` |

### 9. README

Rewritten. It currently describes a direct SignalR connection to
`livetiming.formula1.com` and lists a `signalr_client.h/cpp` that no longer
exists in the tree. The architecture is now lamp → relay. Covers the public
instance, pointing at a self-hosted one, and the URL format.

`ESP32_F1_CLIENT.md` at the repository root is left untouched — it remains an
accurate reference for the upstream F1 protocol, which is now the relay's
concern rather than the lamp's.

## Files touched

```
src/config.h          relay_url replaces relay_host/relay_port
src/relay_client.h    RelayUrl, parseRelayUrl, backoff/stale/version state
src/relay_client.cpp  URL parsing, TLS branch, backoff, stale + version handling
src/main.cpp          relay.begin(config.relay_url); stale timeout in display priority
src/web_server.cpp    relay_url in config API + validation; signalr → relay renames
data/index.html       single URL input; stat-relay rename
data/app.js           relay_url field; statRelay rename
data/debug.js         d.relay rename
README.md             rewrite
```

## Verification

Two stages, because neither target exercises everything.

**Stage 1 — local relay, plain `ws://`.** Run the relay locally
(`npm run dev`), point the lamp at `ws://<laptop-ip>:8000/ws`. Drive every
`display` value through `/override` on the admin port `:8001` and confirm each
animation, the 45 s delay queue, and the CLEAR 10 s window. This is the only way
to test flag transitions on demand — F1 is not running.

**Stage 2 — public relay, `wss://`.** Point the lamp at
`wss://f1-relay.joppe.dev/ws`. Confirms TLS end to end and the
out-of-box default path.

Also verified during bring-up:

- **Heap headroom during the TLS handshake.** `WiFiClientSecure` wants roughly
  30–40 KB transient, alongside AsyncWebServer and FastLED on an ESP32-C3. This
  is the main unknown in the design. Log `ESP.getFreeHeap()` around connect. If
  headroom is tight the fallback is trimming the async server's buffers, but
  measure before designing around it.
- Rejection of a malformed `relay_url` surfaces in the UI rather than bricking
  the connection.
- An empty `relay_url` leaves the lamp in `unconfigured` without reconnect
  attempts.
- Backoff escalation, by pointing at a dead host and watching the serial log.
- Recovery after the relay is restarted underneath a connected lamp.

## Out of scope

- OTA firmware updates.
- IPv6 literal URLs.
- Per-flag configurable animations and palettes (the existing README TODO).
- Any change to the relay itself.
