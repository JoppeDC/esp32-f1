#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <functional>
#include "f1_state.h"

// Client for the self-hosted F1 relay (lamp protocol v1). Every incoming
// message is the full state; `display` is the only field that drives LEDs.
class RelayClient {
public:
    using MessageCallback = std::function<void(F1Flag display, JsonObject msg)>;

    // Fixed WebSocket reconnect interval, also reported to the web UI.
    static constexpr uint32_t RECONNECT_INTERVAL_MS = 5000;

    // Connect to ws://host:port/ws. Empty host = stay idle ("unconfigured").
    // Safe to call again after a config change.
    void begin(const String& host, uint16_t port);

    // Call from loop(). Applies any pending reconfiguration, then services
    // the WebSocket — but only while configured, so a cleared host doesn't
    // keep auto-reconnecting to the previous one.
    void tick();

    // Defer a host/port change to the next tick() (Arduino loop task).
    // Config changes arrive from the async_tcp task via the /api/config
    // handler; applying them there would tear down/rebuild the WebSocket
    // while loop() may concurrently be inside _ws.loop().
    void requestReconfigure(const String& host, uint16_t port);

    void setCallback(MessageCallback cb) { _callback = cb; }

    const char* getStateStr() const;
    uint32_t getLastMessageAgoMs() const { return _configured ? millis() - _lastMessageMs : 0; }

private:
    WebSocketsClient _ws;
    bool     _configured    = false;
    bool     _connected     = false;
    uint32_t _lastMessageMs = 0;
    MessageCallback _callback;

    // Reconfiguration handoff: single-writer (async_tcp task) / single-reader
    // (loop task). Pending host/port are written before the flag is flipped.
    volatile bool _reconfigurePending = false;
    String        _pendingHost;
    uint16_t      _pendingPort = 0;

    void onWsEvent(WStype_t type, uint8_t* payload, size_t length);
    static F1Flag flagFromDisplay(const char* display);
};

extern RelayClient relay;
