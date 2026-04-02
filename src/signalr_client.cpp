#include "signalr_client.h"
#include "config.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

SignalRClient signalr;

// ── URL encoding ──────────────────────────────────────────────────────────────

String urlEncode(const String& str) {
    String out;
    out.reserve(str.length() * 3);
    for (size_t i = 0; i < str.length(); i++) {
        char c = str.charAt(i);
        if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            out += hex;
        }
    }
    return out;
}

// ── Public API ────────────────────────────────────────────────────────────────

void SignalRClient::begin() {
    _reconnectDelay = RECONNECT_INITIAL_MS;
    transitionTo(SignalRState::NEGOTIATING);
}

void SignalRClient::tick() {
    switch (_state) {
        case SignalRState::IDLE:
            break;

        case SignalRState::NEGOTIATING:
            doNegotiate();
            break;

        case SignalRState::CONNECTING:
            doConnect();
            break;

        case SignalRState::SUBSCRIBED:
            _ws.loop();

            // Inactivity watchdog — reconnect if no message for 45 s
            if (millis() - _lastMessageMs > INACTIVITY_TIMEOUT_MS) {
                Serial.println("[SignalR] Inactivity timeout, reconnecting…");
                _ws.disconnect();
                scheduleReconnect();
            }

            // Heartbeat — re-subscribe every 5 min to keep SignalR alive
            if (millis() - _lastHeartbeatMs > HEARTBEAT_INTERVAL_MS) {
                sendSubscribe();
                _lastHeartbeatMs = millis();
                Serial.println("[SignalR] Heartbeat: subscriptions renewed");
            }
            break;

        case SignalRState::RECONNECT_WAIT:
            if (millis() >= _reconnectAfter) {
                transitionTo(SignalRState::NEGOTIATING);
            }
            break;
    }
}

const char* SignalRClient::getStateStr() const {
    switch (_state) {
        case SignalRState::IDLE:           return "idle";
        case SignalRState::NEGOTIATING:    return "negotiating";
        case SignalRState::CONNECTING:     return "connecting";
        case SignalRState::SUBSCRIBED:     return "connected";
        case SignalRState::RECONNECT_WAIT: return "reconnecting";
    }
    return "idle";
}

// ── State machine transitions ─────────────────────────────────────────────────

void SignalRClient::transitionTo(SignalRState next) {
    _state = next;
}

void SignalRClient::scheduleReconnect() {
    Serial.printf("[SignalR] Reconnecting in %lu ms\n", _reconnectDelay);
    _reconnectAfter = millis() + _reconnectDelay;
    _reconnectDelay = min(_reconnectDelay * 2, (uint32_t)RECONNECT_MAX_MS);
    transitionTo(SignalRState::RECONNECT_WAIT);
}

// ── Step 1: HTTP negotiate ────────────────────────────────────────────────────

void SignalRClient::doNegotiate() {
    Serial.println("[SignalR] Negotiating…");
    transitionTo(SignalRState::IDLE);   // prevent re-entry

    WiFiClientSecure tls;
    tls.setInsecure();   // skip cert verification (acceptable for live data)

    HTTPClient http;
    String url = String("https://") + F1_HOST + F1_NEGOTIATE_PATH;
    http.begin(tls, url);
    http.addHeader("User-Agent", "BestHTTP");

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[SignalR] Negotiate failed, HTTP %d\n", code);
        http.end();
        scheduleReconnect();
        return;
    }

    // Extract Set-Cookie header before parsing body
    _cookie = http.header("Set-Cookie");

    String body = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err || !doc["ConnectionToken"].is<const char*>()) {
        Serial.println("[SignalR] Negotiate JSON parse failed");
        scheduleReconnect();
        return;
    }

    _token = doc["ConnectionToken"].as<String>();
    Serial.println("[SignalR] Negotiate OK, connecting WebSocket…");

    // Reset backoff on successful negotiate
    _reconnectDelay = RECONNECT_INITIAL_MS;

    transitionTo(SignalRState::CONNECTING);
}

// ── Step 2: WebSocket connect ─────────────────────────────────────────────────

void SignalRClient::doConnect() {
    transitionTo(SignalRState::IDLE);   // prevent re-entry while connecting

    String path = String("/signalr/connect?transport=webSockets&clientProtocol=1.5")
                  + "&connectionToken=" + urlEncode(_token)
                  + "&connectionData=" + F1_HUB_DATA;

    // Build extra headers string
    String extraHeaders = "User-Agent: BestHTTP";
    if (_cookie.length() > 0) {
        extraHeaders += "\r\nCookie: " + _cookie;
    }

    _ws.setExtraHeaders(extraHeaders.c_str());

    // Register event handler using a lambda (captures `this`)
    _ws.onEvent([](WStype_t type, uint8_t* payload, size_t length) {
        signalr.onWsEvent(type, payload, length);
    });

    _ws.beginSSL(F1_HOST, F1_WS_PORT, path);
    _ws.setReconnectInterval(0);   // we handle reconnects ourselves

    _lastMessageMs   = millis();
    _lastHeartbeatMs = millis();

    transitionTo(SignalRState::SUBSCRIBED);  // assume connected; ws.loop() will correct
    Serial.println("[SignalR] WebSocket connecting…");
}

// ── WebSocket event handler ───────────────────────────────────────────────────

void SignalRClient::onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            Serial.println("[SignalR] WebSocket connected, subscribing…");
            sendSubscribe();
            _lastMessageMs   = millis();
            _lastHeartbeatMs = millis();
            break;

        case WStype_TEXT:
            _lastMessageMs = millis();
            Serial.printf("[SignalR] Frame received (%d bytes)\n", (int)length);
            parseFrame((const char*)payload);
            break;

        case WStype_DISCONNECTED:
            Serial.println("[SignalR] WebSocket disconnected");
            scheduleReconnect();
            break;

        case WStype_ERROR:
            Serial.println("[SignalR] WebSocket error");
            scheduleReconnect();
            break;

        default:
            break;
    }
}

// ── Subscribe message ─────────────────────────────────────────────────────────

void SignalRClient::sendSubscribe() {
    // Subscribe to exactly the streams we need + Heartbeat for watchdog
    String msg = String("{\"H\":\"Streaming\",\"M\":\"Subscribe\","
                        "\"A\":[[\"TrackStatus\",\"SessionStatus\","
                        "\"SessionInfo\",\"Heartbeat\"]],\"I\":")
                 + _msgId++ + "}";
    _ws.sendTXT(msg);
}

// ── Incoming frame parser ─────────────────────────────────────────────────────

void SignalRClient::parseFrame(const char* json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[SignalR] JSON parse error: %s\n", err.c_str());
        Serial.printf("[SignalR] Raw frame (%d bytes): %.200s\n",
                      (int)strlen(json), json);
        return;
    }

    // Push messages arrive under the "M" key
    if (doc["M"].is<JsonArray>()) {
        for (JsonObject hubMsg : doc["M"].as<JsonArray>()) {
            if (!hubMsg["M"].is<const char*>()) continue;
            String method = hubMsg["M"].as<String>();
            if (method != "feed") continue;

            JsonArray args = hubMsg["A"].as<JsonArray>();
            if (args.size() < 2) continue;

            String stream = args[0].as<String>();
            JsonObject data = args[1].as<JsonObject>();

            if (_callback) _callback(stream, data);
        }
    }

    // Initial snapshot arrives under the "R" key as a map of stream→data
    if (doc["R"].is<JsonObject>()) {
        for (JsonPair kv : doc["R"].as<JsonObject>()) {
            String stream = kv.key().c_str();
            if (!kv.value().is<JsonObject>()) continue;
            JsonObject data = kv.value().as<JsonObject>();
            if (_callback) _callback(stream, data);
        }
    }
}
