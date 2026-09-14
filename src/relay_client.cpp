#include "relay_client.h"

RelayClient relay;

void RelayClient::begin(const String& url) {
    _ws.disconnect();
    _connected        = false;
    _stale            = false;
    _protocolMismatch = false;
    _urlValid         = false;
    _backoffMs        = RECONNECT_INTERVAL_MS;
    _configured       = url.length() > 0;

    if (!_configured) {
        Serial.println("[Relay] No relay URL configured");
        return;
    }

    RelayUrl u;
    if (!parseRelayUrl(url.c_str(), u)) {
        Serial.printf("[Relay] Invalid relay URL: %s\n", url.c_str());
        return;
    }
    _urlValid = true;

    _ws.onEvent([](WStype_t type, uint8_t* payload, size_t length) {
        relay.onWsEvent(type, payload, length);
    });

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
    // Protocol-level ping every 15 s; treat 2 missed pongs (3 s timeout) as dead.
    _ws.enableHeartbeat(15000, 3000, 2);
    _lastMessageMs = millis();
    _lastFreshMs   = millis();
    Serial.printf("[Relay] Connecting to %s://%s:%u%s\n",
                  u.tls ? "wss" : "ws", u.host, u.port, u.path);
}

void RelayClient::requestReconfigure(const String& url) {
    _pendingUrl = url;
    _reconfigurePending = true;
}

void RelayClient::tick() {
    if (_reconfigurePending) {
        _reconfigurePending = false;
        // Copy first: the async_tcp task may reassign _pendingUrl while
        // begin() is still reading it (begin blocks in _ws.disconnect()).
        const String url = _pendingUrl;
        begin(url);
    }
    if (!usable()) return;
    _ws.loop();

    // The relay sends state only on connect and on change, so silence on a
    // healthy link means "unchanged", not "lost". Keep the freshness clock
    // running while connected and the relay vouches for its state; it only
    // starts ageing on a disconnect or a stale:true message.
    if (_connected && !_stale) _lastFreshMs = millis();
}

const char* RelayClient::getStateStr() const {
    if (!_configured)       return "unconfigured";
    if (!_urlValid)         return "invalid";
    if (_protocolMismatch)  return "incompatible";
    return _connected ? "connected" : "reconnecting";
}

void RelayClient::onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            _connected     = true;
            _lastMessageMs = millis();
            _backoffMs     = RECONNECT_INTERVAL_MS;
            _ws.setReconnectInterval(_backoffMs);
            Serial.println("[Relay] Connected");
            break;

        case WStype_DISCONNECTED:
            if (_connected) Serial.println("[Relay] Disconnected");
            _connected = false;
            // Fires on every failed retry as well as on a real disconnect, so
            // the interval escalates on its own. The library parses the close
            // code only for a debug print and never surfaces it, so a 1013
            // "subscriber limit" close is indistinguishable from any other
            // drop — backing off is the only response available to us.
            if (_backoffMs < RECONNECT_INTERVAL_MAX_MS) {
                const uint32_t next = _backoffMs * 2;
                _backoffMs = (next > RECONNECT_INTERVAL_MAX_MS) ? RECONNECT_INTERVAL_MAX_MS : next;
                _ws.setReconnectInterval(_backoffMs);
                Serial.printf("[Relay] Reconnecting in %lu ms\n", _backoffMs);
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

            _stale = doc["stale"] | false;
            if (!_stale) _lastFreshMs = millis();

            // doc.as<JsonObject>() is a lightweight view into `doc`; it is
            // only valid for the duration of this callback invocation.
            if (_callback) _callback(flagFromName(doc["display"]), doc.as<JsonObject>());
            break;
        }

        default:
            break;
    }
}
