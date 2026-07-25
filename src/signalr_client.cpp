#include "signalr_client.h"
#include "config.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

SignalRClient signalr;

static const char RS = '\x1e';   // SignalR Core record separator

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

            // Note: SignalR Core has its own ping/pong (type 6) handled in
            // processCoreSegment(), which is what actually keeps the server-side
            // connection alive. We no longer need to periodically re-Subscribe
            // the way the classic protocol required — this heartbeat now just
            // logs liveness and gives us a cheap secondary safety net.
            if (millis() - _lastHeartbeatMs > HEARTBEAT_INTERVAL_MS) {
                _lastHeartbeatMs = millis();
                Serial.println("[SignalR] Heartbeat check (Core ping/pong handles keepalive)");
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

// ── Step 1: HTTP negotiate (SignalR Core: POST, negotiateVersion=1) ───────────

void SignalRClient::doNegotiate() {
    Serial.println("[SignalR] Negotiating (Core)…");
    transitionTo(SignalRState::IDLE);   // prevent re-entry

    WiFiClientSecure tls;
    tls.setInsecure();   // skip cert verification (acceptable for live data)

    HTTPClient http;
    String url = String("https://") + F1_HOST + F1_NEGOTIATE_PATH;
    http.begin(tls, url);

    // ESP32 HTTPClient silently discards any response header not explicitly
    // registered here — without this call, http.header("Set-Cookie") below
    // always returns "" with no error, even though the server did send one.
    const char* headerKeys[] = { "Set-Cookie" };
    http.collectHeaders(headerKeys, 1);

    http.addHeader("User-Agent", "BestHTTP");
    http.addHeader("Content-Length", "0");

    int code = http.POST("");   // Core negotiate is POST with an empty body
    if (code != 200) {
        Serial.printf("[SignalR] Negotiate failed, HTTP %d\n", code);
        http.end();
        scheduleReconnect();
        return;
    }

    // Optional: capture load-balancer cookie if F1 sets one (best effort)
    _cookie = http.header("Set-Cookie");

    String body = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err || !doc["connectionToken"].is<const char*>()) {
        Serial.println("[SignalR] Negotiate JSON parse failed");
        Serial.printf("[SignalR] Raw body: %.200s\n", body.c_str());
        scheduleReconnect();
        return;
    }

    _token = doc["connectionToken"].as<String>();
    _handshakeDone = false;
    Serial.println("[SignalR] Negotiate OK, connecting WebSocket…");

    // Reset backoff on successful negotiate
    _reconnectDelay = RECONNECT_INITIAL_MS;

    transitionTo(SignalRState::CONNECTING);
}

// ── Step 2: WebSocket connect (SignalR Core: /signalrcore?id=<token>) ─────────

void SignalRClient::doConnect() {
    transitionTo(SignalRState::IDLE);   // prevent re-entry while connecting

    String path = String(F1_CONNECT_PATH) + "?id=" + urlEncode(_token);

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
        case WStype_CONNECTED: {
            Serial.println("[SignalR] WebSocket connected, sending protocol handshake…");
            // SignalR Core requires a JSON protocol handshake before anything else
            String hs = "{\"protocol\":\"json\",\"version\":1}";
            hs += RS;
            _ws.sendTXT(hs);
            _lastMessageMs   = millis();
            _lastHeartbeatMs = millis();
            break;
        }

        case WStype_TEXT:
            _lastMessageMs = millis();
            handleCoreFrame((const char*)payload, length);
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

// ── Subscribe message (SignalR Core invocation, public streams only) ──────────

void SignalRClient::sendSubscribe() {
    // Public, unauthenticated streams only — exactly what the lamp needs.
    // (CarData/Position/TeamRadio etc. are auth-gated on Core and deliberately omitted.)
    String msg = String("{\"type\":1,\"target\":\"Subscribe\",\"arguments\":")
                 + "[[\"TrackStatus\",\"SessionStatus\",\"SessionInfo\",\"Heartbeat\"]],"
                 + "\"invocationId\":\"" + String(_msgId++) + "\"}";
    msg += RS;
    _ws.sendTXT(msg);
}

// ── Incoming frame handling ────────────────────────────────────────────────────

// A single WebSocket TEXT frame may contain one or more RS-delimited JSON
// messages concatenated together. Split first, then process each on its own.
void SignalRClient::handleCoreFrame(const char* raw, size_t length) {
    size_t start = 0;
    for (size_t i = 0; i <= length; i++) {
        if (i == length || raw[i] == RS) {
            if (i > start) {
                String segment(raw + start, i - start);
                processCoreSegment(segment);
            }
            start = i + 1;
        }
    }
}

void SignalRClient::processCoreSegment(const String& segment) {
    // The very first message we ever receive is the handshake response.
    // Success looks like "{}"; failure includes an "error" field.
    if (!_handshakeDone) {
        JsonDocument hs;
        DeserializationError err = deserializeJson(hs, segment);
        if (!err && !hs["error"].is<const char*>()) {
            _handshakeDone = true;
            Serial.println("[SignalR] Handshake OK, subscribing…");
            sendSubscribe();
        } else {
            Serial.printf("[SignalR] Handshake error: %s\n", segment.c_str());
            scheduleReconnect();
        }
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, segment);
    if (err) {
        Serial.printf("[SignalR] JSON parse error: %s\n", err.c_str());
        Serial.printf("[SignalR] Raw segment (%d bytes): %.200s\n",
                      (int)segment.length(), segment.c_str());
        return;
    }

    int msgType = doc["type"] | -1;
    switch (msgType) {
        case 1: {  // Invocation — a live push update (feed)
            if (!doc["target"].is<const char*>()) break;
            String stream = doc["target"].as<String>();
            JsonArray args = doc["arguments"].as<JsonArray>();
            if (args.size() > 0 && _callback) {
                _callback(stream, args[0].as<JsonObject>());
            }
            break;
        }

        case 3: {  // Completion — initial snapshot, keyed by stream name
            JsonObject result = doc["result"].as<JsonObject>();
            for (JsonPair kv : result) {
                if (!kv.value().is<JsonObject>()) continue;
                if (_callback) _callback(kv.key().c_str(), kv.value().as<JsonObject>());
            }
            break;
        }

        case 6:  // Ping — must echo back or the server will close the connection
            _ws.sendTXT(String("{\"type\":6}") + RS);
            break;

        case 7:  // Close
            Serial.printf("[SignalR] Server closed connection: %s\n",
                          doc["error"] | "(no reason given)");
            scheduleReconnect();
            break;

        default:
            break;
    }
}
