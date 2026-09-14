#include "relay_client.h"
#include <ArduinoJson.h>

RelayClient relay;

// ── Loop-task side ────────────────────────────────────────────────────────────

void RelayClient::begin(const String& url) {
    if (_task) return;

    _urlQueue      = xQueueCreate(1, sizeof(UrlCommand));
    _snapshotQueue = xQueueCreate(SNAPSHOT_QUEUE_LEN, sizeof(RelaySnapshot));

    _ws.onEvent([](WStype_t type, uint8_t* payload, size_t length) {
        relay.onWsEvent(type, payload, length);
    });

    requestReconfigure(url);

    // Core 0 on dual-core parts — alongside WiFi, away from the LED loop on
    // core 1 — and the only core on the C3. Same priority as the loop task;
    // the connect path blocks in the socket layer, so the loop keeps running
    // through it either way.
    xTaskCreatePinnedToCore(taskEntry, "relay", TASK_STACK_BYTES, this,
                            /*priority=*/1, &_task, /*core=*/0);
}

void RelayClient::requestReconfigure(const String& url) {
    if (!_urlQueue) return;
    UrlCommand cmd;
    strlcpy(cmd.url, url.c_str(), sizeof(cmd.url));
    xQueueOverwrite(_urlQueue, &cmd);
}

void RelayClient::tick() {
    if (!_snapshotQueue) return;
    RelaySnapshot snap;
    while (xQueueReceive(_snapshotQueue, &snap, 0) == pdTRUE) {
        if (_callback) _callback(snap);
    }
}

const char* RelayClient::getStateStr() const {
    if (!_configured)       return "unconfigured";
    if (!_urlValid)         return "invalid";
    if (_protocolMismatch)  return "incompatible";
    return _connected ? "connected" : "reconnecting";
}

// ── Relay task ────────────────────────────────────────────────────────────────

void RelayClient::taskEntry(void* arg) {
    static_cast<RelayClient*>(arg)->run();
}

void RelayClient::run() {
    for (;;) {
        UrlCommand cmd;
        if (xQueueReceive(_urlQueue, &cmd, 0) == pdTRUE) configure(cmd.url);

        if (usable()) {
            _ws.loop();

            // The relay sends state only on connect and on change, so silence
            // on a healthy link means "unchanged", not "lost". Keep the
            // freshness clock running while connected and the relay vouches
            // for its state; it only starts ageing after a disconnect or a
            // stale:true message.
            if (_connected && !_stale) _lastFreshMs = millis();
        }

        vTaskDelay(1);   // cede the core; message latency stays within a tick
    }
}

void RelayClient::configure(const char* url) {
    _ws.disconnect();
    _connected        = false;
    _stale            = false;
    _protocolMismatch = false;
    _urlValid         = false;
    _backoffMs        = RECONNECT_INTERVAL_MS;
    _lastDisconnectReason[0] = '\0';
    _configured       = url[0] != '\0';

    if (!_configured) {
        Serial.println("[Relay] No relay URL configured");
        return;
    }

    RelayUrl u;
    if (!parseRelayUrl(url, u)) {
        Serial.printf("[Relay] Invalid relay URL: %s\n", url);
        return;
    }
    _urlValid = true;

    // TLS without certificate validation: beginSSL's default empty fingerprint
    // resolves to WiFiClientSecure::setInsecure(). The relay carries public
    // race-flag state and the lamp sends no credentials, so a MITM gains only
    // the ability to show a wrong colour — not worth pinning a CA that would
    // brick every deployed lamp if the issuer ever changed.
    if (u.tls) {
        _ws.beginSSL(u.host, u.port, u.path);
    } else {
        _ws.begin(u.host, u.port, u.path);
    }

    _ws.setReconnectInterval(_backoffMs);
    // Client-side liveness check, no stricter than the relay's own policy
    // (it pings every 30 s and drops after two missed pongs). A dead link is
    // noticed within ~80 s worst case, well inside the 5-minute freshness
    // window; anything tighter tore down healthy sockets on 2.4 GHz hiccups
    // and paid a blocking TLS reconnect plus a backoff step each time.
    _ws.enableHeartbeat(30000, 10000, 2);
    _lastMessageMs = millis();
    _lastFreshMs   = millis();
    Serial.printf("[Relay] Connecting to %s://%s:%u%s\n",
                  u.tls ? "wss" : "ws", u.host, u.port, u.path);
}

void RelayClient::onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            _connected     = true;
            _lastMessageMs = millis();
            _lastDisconnectReason[0] = '\0';
            // Backoff is deliberately not reset here: a relay at its
            // subscriber limit accepts the handshake and then closes with
            // 1013, and resetting on the handshake kept every over-limit
            // lamp hammering it at 5–10 s forever. It resets on the first
            // message instead (see WStype_TEXT).
            Serial.println("[Relay] Connected");
            break;

        case WStype_DISCONNECTED:
            // The library attaches a reason for handshake-level failures
            // ("HTTP 301", "WebSocket handshake failed - HTTP 404", "Header
            // response timeout", "Connection lost"); a plain TCP connect
            // failure and a WebSocket close frame arrive with no payload, so
            // a 1013 subscriber-limit close is indistinguishable from any
            // other drop — backing off is the only response available.
            if (payload && length) {
                snprintf(_lastDisconnectReason, sizeof(_lastDisconnectReason),
                         "%.*s", (int)length, (const char*)payload);
                Serial.printf("[Relay] Disconnected: %s\n", _lastDisconnectReason);
            } else if (_connected) {
                Serial.println("[Relay] Disconnected");
            }
            _connected = false;
            // Fires on every failed retry as well as on a real disconnect, so
            // the interval escalates on its own.
            if (_backoffMs < RECONNECT_INTERVAL_MAX_MS) {
                const uint32_t next = _backoffMs * 2;
                _backoffMs = (next > RECONNECT_INTERVAL_MAX_MS) ? RECONNECT_INTERVAL_MAX_MS : next;
                _ws.setReconnectInterval(_backoffMs);
                Serial.printf("[Relay] Reconnecting in %lu ms\n", (unsigned long)_backoffMs);
            }
            break;

        case WStype_TEXT: {
            _lastMessageMs = millis();
            JsonDocument doc;
            if (deserializeJson(doc, payload, length)) {
                Serial.println("[Relay] JSON parse error");
                return;
            }

            // Protocol version. Absent or 1 is fine; the contract adds new
            // fields within v1 and bumps only on breaking changes, so an
            // unknown version means ignoring the message beats driving LEDs
            // from a shape we don't understand.
            if (doc["v"].is<int>() && doc["v"].as<int>() != 1) {
                if (!_protocolMismatch) {
                    Serial.printf("[Relay] Unsupported protocol v%d — ignoring messages\n",
                                  doc["v"].as<int>());
                }
                _protocolMismatch = true;
                return;
            }
            _protocolMismatch = false;

            if (!doc["display"].is<const char*>()) return;

            // A real message on a live socket: the connection is good, so
            // the next drop starts the backoff from the bottom again.
            if (_backoffMs != RECONNECT_INTERVAL_MS) {
                _backoffMs = RECONNECT_INTERVAL_MS;
                _ws.setReconnectInterval(_backoffMs);
            }

            _stale = doc["stale"] | false;
            if (!_stale) _lastFreshMs = millis();

            RelaySnapshot snap;
            snap.display = flagFromName(doc["display"]);
            snap.stale   = _stale;
            snap.session = sessionStatusFromName(doc["session"] | "");
            strlcpy(snap.track, doc["track"] | "", sizeof(snap.track));
            strlcpy(snap.type,  doc["type"]  | "", sizeof(snap.type));

            if (xQueueSend(_snapshotQueue, &snap, 0) != pdTRUE) {
                // Only if loop() has stalled for eight state changes; the
                // relay coalesces to one message per change, so this is a
                // symptom of something else being wrong.
                Serial.println("[Relay] Snapshot queue full — message dropped");
            }
            break;
        }

        default:
            break;
    }
}
