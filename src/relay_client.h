#pragma once
#include <Arduino.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <functional>
#include "f1_flags.h"
#include "relay_url.h"

// One relay message reduced to what the lamp uses. Plain data with fixed
// buffers so it can cross the task boundary through a FreeRTOS queue.
struct RelaySnapshot {
    F1Flag        display = F1Flag::IDLE;
    bool          stale   = false;
    SessionStatus session = SessionStatus::UNKNOWN;
    char          track[8]  = {};   // CLEAR / YELLOW / VSC / SC / RED
    char          type[32]  = {};   // "Race", "Sprint Qualifying", …
};

// Client for the self-hosted F1 relay (lamp protocol v1). Every incoming
// message is the full state; `display` is the only field that drives LEDs.
//
// Threading: the WebSocket is serviced by a dedicated FreeRTOS task, because
// the library connects synchronously — a TCP connect can block for 5 s and a
// TLS handshake for a second or two — and doing that on the Arduino loop task
// froze the LED animation on every reconnect attempt. The task parses each
// message into a RelaySnapshot and queues it; tick() drains the queue on the
// loop task and invokes the callback there, so everything downstream
// (F1State, the web server's SSE push) stays single-threaded.
class RelayClient {
public:
    using MessageCallback = std::function<void(const RelaySnapshot&)>;

    // Reconnect backoff bounds. The interval doubles on every disconnect and
    // resets once the relay delivers a message.
    static constexpr uint32_t RECONNECT_INTERVAL_MS     = 5000;
    static constexpr uint32_t RECONNECT_INTERVAL_MAX_MS = 60000;

    // Starts the relay task and connects to `url`. Empty or unparseable =
    // stay idle. Call once from setup().
    void begin(const String& url);

    // Call from loop(). Delivers received snapshots to the callback, in
    // order, on the calling task.
    void tick();

    // Any task. Hands a new URL to the relay task; the latest one wins. Empty
    // disconnects and stays idle, so a cleared URL doesn't keep
    // auto-reconnecting to the previous one.
    void requestReconfigure(const String& url);

    void setCallback(MessageCallback cb) { _callback = cb; }

    // One of: unconfigured, invalid, incompatible, connected, reconnecting.
    const char* getStateStr() const;

    // True while the relay reports its own upstream F1 link is down.
    bool isStale() const { return _stale; }

    // Current reconnect interval, for the debug UI.
    uint32_t getReconnectDelayMs() const { return _backoffMs; }

    // Text the library attached to the last disconnect ("HTTP 301",
    // "WebSocket handshake failed - HTTP 404", …), or "" if none. A plain
    // TCP connect failure carries no text.
    const char* getLastDisconnectReason() const { return _lastDisconnectReason; }

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
    // The TLS handshake runs on this stack. Arduino's own loop task is 8 KB
    // and carried it before; a little headroom on top of that.
    static constexpr uint32_t TASK_STACK_BYTES   = 10240;
    static constexpr uint8_t  SNAPSHOT_QUEUE_LEN = 8;

    struct UrlCommand {
        char url[RELAY_URL_MAX_LEN + 1];
    };

    WebSocketsClient _ws;
    TaskHandle_t     _task          = nullptr;
    QueueHandle_t    _urlQueue      = nullptr;   // length 1, overwrite: latest wins
    QueueHandle_t    _snapshotQueue = nullptr;   // relay task → loop task
    MessageCallback  _callback;

    // Written by the relay task, read by any task. Each is a single aligned
    // word, so a read is atomic on both Xtensa and RISC-V; volatile keeps the
    // compiler from caching a value across a poll.
    volatile bool     _configured       = false;   // URL non-empty
    volatile bool     _urlValid         = false;   // …and it parsed
    volatile bool     _connected        = false;
    volatile bool     _stale            = false;
    volatile bool     _protocolMismatch = false;
    volatile uint32_t _lastMessageMs    = 0;
    volatile uint32_t _lastFreshMs      = 0;
    volatile uint32_t _backoffMs        = RECONNECT_INTERVAL_MS;
    char              _lastDisconnectReason[48] = {};

    bool usable() const { return _configured && _urlValid; }

    // Relay task only.
    static void taskEntry(void* arg);
    void run();
    void configure(const char* url);
    void onWsEvent(WStype_t type, uint8_t* payload, size_t length);
};

extern RelayClient relay;
