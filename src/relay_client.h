#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <functional>
#include "f1_state.h"

// A relay endpoint, parsed out of a user-supplied URL.
struct RelayUrl {
    bool     tls  = false;
    String   host;
    uint16_t port = 0;
    String   path;
};

// Parses "ws://host[:port][/path]" or "wss://host[:port][/path]" and leaves
// `out` untouched on failure.
//
// The scheme is required. Guessing it fails in both directions — assume wss and
// a LAN user typing "192.168.1.5:8000" breaks, assume ws and anyone typing a
// public hostname breaks — so a rejection the UI can show beats a silent wrong
// guess. Default port is 443 for wss and 80 for ws; an omitted path becomes
// "/ws". IPv6 literals are not supported.
bool parseRelayUrl(const String& url, RelayUrl& out);

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

    uint32_t getLastMessageAgoMs() const { return usable() ? millis() - _lastMessageMs : 0; }

    // Age of the last message the relay vouched for (stale == false). Covers
    // both a stale relay and a dead link: a dead link delivers no messages at
    // all, so this keeps climbing either way. 0 while not usably configured.
    uint32_t getFreshAgeMs() const { return usable() ? millis() - _lastFreshMs : 0; }

    // True when there is no state worth displaying: no usable relay
    // configured (so none is ever coming), or nothing fresh for maxAgeMs.
    bool isStateExpired(uint32_t maxAgeMs) const {
        if (!usable()) return true;
        return (millis() - _lastFreshMs) >= maxAgeMs;
    }

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
    static F1Flag flagFromDisplay(const char* display);
};

extern RelayClient relay;
