#include "relay_client.h"

RelayClient relay;

void RelayClient::begin(const String& host, uint16_t port) {
    _ws.disconnect();
    _connected  = false;
    _configured = host.length() > 0;
    if (!_configured) {
        Serial.println("[Relay] No relay host configured");
        return;
    }

    _ws.onEvent([](WStype_t type, uint8_t* payload, size_t length) {
        relay.onWsEvent(type, payload, length);
    });
    _ws.begin(host.c_str(), port, "/ws");
    _ws.setReconnectInterval(RECONNECT_INTERVAL_MS);
    // Protocol-level ping every 15 s; treat 2 missed pongs (3 s timeout) as dead.
    _ws.enableHeartbeat(15000, 3000, 2);
    _lastMessageMs = millis();
    Serial.printf("[Relay] Connecting to ws://%s:%u/ws\n", host.c_str(), port);
}

void RelayClient::requestReconfigure(const String& host, uint16_t port) {
    _pendingHost = host;
    _pendingPort = port;
    _reconfigurePending = true;
}

void RelayClient::tick() {
    if (_reconfigurePending) {
        _reconfigurePending = false;
        begin(_pendingHost, _pendingPort);
    }
    if (_configured) _ws.loop();
}

const char* RelayClient::getStateStr() const {
    if (!_configured) return "unconfigured";
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
            Serial.println("[Relay] Connected");
            break;

        case WStype_DISCONNECTED:
            if (_connected) Serial.println("[Relay] Disconnected");
            _connected = false;
            break;

        case WStype_TEXT: {
            _lastMessageMs = millis();
            JsonDocument doc;
            if (deserializeJson(doc, payload, length)) {
                Serial.println("[Relay] JSON parse error");
                return;
            }
            if (!doc["display"].is<const char*>()) return;
            // doc.as<JsonObject>() is a lightweight view into `doc`; it is
            // only valid for the duration of this callback invocation.
            if (_callback) _callback(flagFromDisplay(doc["display"]), doc.as<JsonObject>());
            break;
        }

        default:
            break;
    }
}
