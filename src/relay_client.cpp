#include "relay_client.h"

RelayClient relay;

// Hostname / IPv4 characters. Anything else — including the brackets of an
// IPv6 literal, which is unsupported — is rejected so the UI can say so.
static bool isHostChar(char c) {
    return isAlphaNumeric(c) || c == '-' || c == '.' || c == '_';
}

bool parseRelayUrl(const String& url, RelayUrl& out) {
    String s = url;
    s.trim();

    RelayUrl u;
    int hostStart;
    if (s.startsWith("wss://")) {
        u.tls  = true;
        u.port = 443;
        hostStart = 6;
    } else if (s.startsWith("ws://")) {
        u.tls  = false;
        u.port = 80;
        hostStart = 5;
    } else {
        return false;   // scheme is required
    }

    // Authority runs from the scheme to the first '/', which starts the path.
    int pathStart = s.indexOf('/', hostStart);
    int authEnd   = (pathStart >= 0) ? pathStart : (int)s.length();

    // A colon after the authority belongs to the path, not to a port.
    int colon = s.indexOf(':', hostStart);
    if (colon >= authEnd) colon = -1;

    u.host = s.substring(hostStart, (colon >= 0) ? colon : authEnd);
    if (u.host.length() == 0) return false;
    for (unsigned i = 0; i < u.host.length(); i++) {
        if (!isHostChar(u.host[i])) return false;
    }

    if (colon >= 0) {
        String portStr = s.substring(colon + 1, authEnd);
        if (portStr.length() == 0) return false;
        for (unsigned i = 0; i < portStr.length(); i++) {
            if (!isDigit(portStr[i])) return false;
        }
        long p = portStr.toInt();
        if (p < 1 || p > 65535) return false;
        u.port = (uint16_t)p;
    }

    u.path = (pathStart >= 0) ? s.substring(pathStart) : String("");
    if (u.path.length() == 0) u.path = "/ws";

    out = u;
    return true;
}

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
    if (!parseRelayUrl(url, u)) {
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
        _ws.beginSSL(u.host.c_str(), u.port, u.path.c_str());
    } else {
        _ws.begin(u.host.c_str(), u.port, u.path.c_str());
    }

    _ws.setReconnectInterval(_backoffMs);
    // Protocol-level ping every 15 s; treat 2 missed pongs (3 s timeout) as dead.
    _ws.enableHeartbeat(15000, 3000, 2);
    _lastMessageMs = millis();
    _lastFreshMs   = millis();
    Serial.printf("[Relay] Connecting to %s://%s:%u%s\n",
                  u.tls ? "wss" : "ws", u.host.c_str(), u.port, u.path.c_str());
}

void RelayClient::requestReconfigure(const String& url) {
    _pendingUrl = url;
    _reconfigurePending = true;
}

void RelayClient::tick() {
    if (_reconfigurePending) {
        _reconfigurePending = false;
        begin(_pendingUrl);
    }
    if (usable()) _ws.loop();
}

const char* RelayClient::getStateStr() const {
    if (!_configured)       return "unconfigured";
    if (!_urlValid)         return "invalid";
    if (_protocolMismatch)  return "incompatible";
    return _connected ? "connected" : "reconnecting";
}

F1Flag RelayClient::flagFromDisplay(const char* d) {
    if (strcmp(d, "RED")    == 0) return F1Flag::RED_FLAG;
    if (strcmp(d, "SC")     == 0) return F1Flag::SC;
    if (strcmp(d, "VSC")    == 0) return F1Flag::VSC;
    if (strcmp(d, "YELLOW") == 0) return F1Flag::YELLOW;
    if (strcmp(d, "CLEAR")  == 0) return F1Flag::CLEAR;
    if (strcmp(d, "CHEQ")   == 0) return F1Flag::CHEQ;
    return F1Flag::IDLE;
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
            if (_callback) _callback(flagFromDisplay(doc["display"]), doc.as<JsonObject>());
            break;
        }

        default:
            break;
    }
}
