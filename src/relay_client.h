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

    // Connect to ws://host:port/ws. Empty host = stay idle ("unconfigured").
    // Safe to call again after a config change.
    void begin(const String& host, uint16_t port);
    void tick() { _ws.loop(); }

    void setCallback(MessageCallback cb) { _callback = cb; }

    const char* getStateStr() const;
    uint32_t getLastMessageAgoMs() const { return millis() - _lastMessageMs; }

private:
    WebSocketsClient _ws;
    bool     _configured    = false;
    bool     _connected     = false;
    uint32_t _lastMessageMs = 0;
    MessageCallback _callback;

    void onWsEvent(WStype_t type, uint8_t* payload, size_t length);
    static F1Flag flagFromDisplay(const char* display);
};

extern RelayClient relay;
