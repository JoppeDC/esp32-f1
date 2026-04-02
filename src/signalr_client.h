#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <functional>

enum class SignalRState {
    IDLE,
    NEGOTIATING,
    CONNECTING,
    SUBSCRIBED,
    RECONNECT_WAIT
};

class SignalRClient {
public:
    using MessageCallback = std::function<void(const String& stream, JsonObject data)>;

    void begin();
    void tick();

    void setCallback(MessageCallback cb) { _callback = cb; }

    SignalRState getState()    const { return _state; }
    const char*  getStateStr() const;

private:
    // ── State machine ─────────────────────────────────────────────────────────
    SignalRState _state = SignalRState::IDLE;

    void transitionTo(SignalRState next);
    void doNegotiate();
    void doConnect();
    void scheduleReconnect();

    // ── WebSocket ─────────────────────────────────────────────────────────────
    WebSocketsClient _ws;
    String           _token;
    String           _cookie;
    uint8_t          _msgId = 1;

    void onWsEvent(WStype_t type, uint8_t* payload, size_t length);
    void parseFrame(const char* json);
    void sendSubscribe();

    // ── Timers ────────────────────────────────────────────────────────────────
    uint32_t _lastMessageMs   = 0;
    uint32_t _lastHeartbeatMs = 0;
    uint32_t _reconnectAfter  = 0;   // millis() target for next connect attempt
    uint32_t _reconnectDelay  = 0;   // current backoff value

    // ── Callback ──────────────────────────────────────────────────────────────
    MessageCallback _callback;
};

// URL-encode a string for embedding in a query parameter.
String urlEncode(const String& str);

extern SignalRClient signalr;
