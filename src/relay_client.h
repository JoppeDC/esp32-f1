#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <functional>
#include "f1_flags.h"
#include "relay_url.h"

// Client for the self-hosted F1 relay (lamp protocol v1). Every incoming
// message is the full state; `display` is the only field that drives LEDs.
class RelayClient {
public:
    using MessageCallback = std::function<void(F1Flag display, JsonObject msg)>;

    // Reconnect backoff bounds. The interval doubles on every disconnect and
    // resets on a successful connect.
    static constexpr uint32_t RECONNECT_INTERVAL_MS     = 5000;
    static constexpr uint32_t RECONNECT_INTERVAL_MAX_MS = 60000;

    // Connect to the given relay URL. Empty or unparseable = stay idle.
    // Safe to call again after a config change.
    void begin(const String& url);

    // Call from loop(). Applies any pending reconfiguration, then services
    // the WebSocket — but only while usably configured, so a cleared or
    // invalid URL doesn't keep auto-reconnecting to the previous one.
    void tick();

    // Defer a URL change to the next tick() (Arduino loop task). Config
    // changes arrive from the async_tcp task via the /api/config handler;
    // applying them there would tear down/rebuild the WebSocket while loop()
    // may concurrently be inside _ws.loop().
    void requestReconfigure(const String& url);

    void setCallback(MessageCallback cb) { _callback = cb; }

    // One of: unconfigured, invalid, incompatible, connected, reconnecting.
    const char* getStateStr() const;

    // True while the relay reports its own upstream F1 link is down.
    bool isStale() const { return _stale; }

    // Current reconnect interval, for the debug UI.
    uint32_t getReconnectDelayMs() const { return _backoffMs; }

    // Age of the last message of any kind; UINT32_MAX while not usably
    // configured, which the debug UI renders as "—" rather than "0.0s ago".
    uint32_t getLastMessageAgoMs() const { return usable() ? millis() - _lastMessageMs : UINT32_MAX; }

    // How long the relay has failed to vouch for its state: time since the
    // link dropped, or since it last said stale:false. 0 while connected and
    // fresh (the relay only sends on change, so silence there is not age).
    // UINT32_MAX while not usably configured — nothing fresh is ever coming.
    uint32_t getFreshAgeMs() const { return usable() ? millis() - _lastFreshMs : UINT32_MAX; }

    // True when there is no state worth displaying: no usable relay
    // configured, or nothing fresh for maxAgeMs.
    bool isStateExpired(uint32_t maxAgeMs) const { return getFreshAgeMs() >= maxAgeMs; }

private:
    WebSocketsClient _ws;
    bool     _configured    = false;   // URL non-empty
    bool     _urlValid      = false;   // …and it parsed
    bool     _connected     = false;
    bool     _stale         = false;
    bool     _protocolMismatch = false;
    uint32_t _lastMessageMs = 0;
    uint32_t _lastFreshMs   = 0;
    uint32_t _backoffMs     = RECONNECT_INTERVAL_MS;
    MessageCallback _callback;

    // Reconfiguration handoff: single-writer (async_tcp task) / single-reader
    // (loop task). The pending URL is written before the flag is flipped.
    volatile bool _reconfigurePending = false;
    String        _pendingUrl;

    bool usable() const { return _configured && _urlValid; }

    void onWsEvent(WStype_t type, uint8_t* payload, size_t length);
};

extern RelayClient relay;
