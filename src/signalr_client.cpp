#include "signalr_client.h"
#include "config.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

SignalRClient signalr;

// SignalR Core frames are JSON documents separated by a 0x1e record separator.
static const char RECORD_SEP = '\x1e';

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

            // Inactivity watchdog — the server pings every ~15 s, so silence
            // for 45 s means the connection is dead.
            if (millis() - _lastMessageMs > INACTIVITY_TIMEOUT_MS) {
                Serial.println("[SignalR] Inactivity timeout, reconnecting…");
                _ws.disconnect();
                scheduleReconnect();
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
    String url = String("https://") + F1_HOST + F1_CORE_NEGOTIATE_PATH;
    http.begin(tls, url);

    // HTTPClient only exposes headers registered before the request.
    const char* headerKeys[] = { "Set-Cookie" };
    http.collectHeaders(headerKeys, 1);

    int code = http.POST("");
    if (code != 200) {
        Serial.printf("[SignalR] Negotiate failed, HTTP %d\n", code);
        http.end();
        scheduleReconnect();
        return;
    }

    // Extract the AWS ALB sticky-session cookie so the WebSocket lands on the
    // same backend that minted our connection token.
    String setCookie = http.header("Set-Cookie");
    _cookie = "";
    for (const char* name : { "AWSALBCORS=", "AWSALB=" }) {
        int idx = setCookie.indexOf(name);
        if (idx < 0) continue;
        int vStart = idx + strlen(name);
        int vEnd = setCookie.indexOf(';', vStart);
        if (vEnd < 0) vEnd = setCookie.length();
        _cookie = String(name) + setCookie.substring(vStart, vEnd);
        break;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    const char* token = doc["connectionToken"] | doc["ConnectionToken"] | "";
    if (err || strlen(token) == 0) {
        Serial.println("[SignalR] Negotiate JSON parse failed");
        scheduleReconnect();
        return;
    }

    _token = token;
    Serial.println("[SignalR] Negotiate OK, connecting WebSocket…");

    // Reset backoff on successful negotiate
    _reconnectDelay = RECONNECT_INITIAL_MS;

    transitionTo(SignalRState::CONNECTING);
}

// ── Step 2: WebSocket connect ─────────────────────────────────────────────────

void SignalRClient::doConnect() {
    transitionTo(SignalRState::IDLE);   // prevent re-entry while connecting

    String path = String(F1_CORE_WS_PATH) + "?id=" + urlEncode(_token);

    String extraHeaders;
    if (_cookie.length() > 0) {
        extraHeaders = "Cookie: " + _cookie;
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
            Serial.println("[SignalR] WebSocket connected, handshaking…");
            _ws.sendTXT("{\"protocol\":\"json\",\"version\":1}\x1e");
            sendSubscribe();
            _lastMessageMs   = millis();
            _lastHeartbeatMs = millis();
            break;

        case WStype_TEXT:
            _lastMessageMs = millis();
            parseFrame((char*)payload, length);
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
    String msg = String("{\"type\":1,\"target\":\"Subscribe\","
                        "\"arguments\":[[\"TrackStatus\",\"SessionStatus\","
                        "\"SessionInfo\",\"Heartbeat\"]],\"invocationId\":\"")
                 + _msgId++ + "\"}\x1e";
    _ws.sendTXT(msg);
}

// ── Incoming frame parser ─────────────────────────────────────────────────────

void SignalRClient::parseFrame(char* payload, size_t length) {
    // A single WebSocket frame can carry several 0x1e-separated JSON documents.
    size_t start = 0;
    for (size_t i = 0; i <= length; i++) {
        if (i == length || payload[i] == RECORD_SEP) {
            if (i > start) {
                payload[i] = '\0';
                parseSegment(payload + start);
            }
            start = i + 1;
        }
    }
}

void SignalRClient::parseSegment(const char* json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[SignalR] JSON parse error: %s\n", err.c_str());
        Serial.printf("[SignalR] Raw segment (%d bytes): %.200s\n",
                      (int)strlen(json), json);
        return;
    }

    // Handshake response is "{}" on success, {"error": "..."} on failure.
    if (doc["error"].is<const char*>()) {
        Serial.printf("[SignalR] Handshake error: %s\n",
                      doc["error"].as<const char*>());
        _ws.disconnect();
        scheduleReconnect();
        return;
    }

    switch (doc["type"] | 0) {
        case 1: {   // Invocation — live feed push
            if (String(doc["target"] | "") != "feed") break;
            JsonArray args = doc["arguments"].as<JsonArray>();
            if (args.size() < 2) break;
            String stream = args[0].as<String>();
            JsonObject data = args[1].as<JsonObject>();
            if (_callback) _callback(stream, data);
            break;
        }

        case 3: {   // Completion — initial snapshot as a stream→data map
            if (!doc["result"].is<JsonObject>()) break;
            for (JsonPair kv : doc["result"].as<JsonObject>()) {
                if (!kv.value().is<JsonObject>()) continue;
                String stream = kv.key().c_str();
                JsonObject data = kv.value().as<JsonObject>();
                if (_callback) _callback(stream, data);
            }
            break;
        }

        case 6:     // Ping — must answer or the server drops us
            _lastHeartbeatMs = millis();
            _ws.sendTXT("{\"type\":6}\x1e");
            break;

        case 7:     // Server-initiated close
            Serial.printf("[SignalR] Server closed connection: %s\n",
                          doc["error"] | "");
            _ws.disconnect();
            scheduleReconnect();
            break;

        default:
            break;
    }
}
