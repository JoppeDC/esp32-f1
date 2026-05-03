#include <Arduino.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

#include "config.h"
#include "f1_state.h"
#include "led_controller.h"
#include "signalr_client.h"
#include "web_server.h"

Config config;

// ── DEBUG: Built-in LED blink pattern to visualise flag state without a strip ─
// Remove this once external LEDs are connected.
#define BUILTIN_LED_PIN 2
static uint32_t _blinkLastToggle = 0;
static bool     _blinkOn         = false;
// Duration to display CLEAR before falling back to IDLE while track remains CLEAR.
static constexpr uint32_t CLEAR_DISPLAY_MS = 10000;
static F1Flag   _lastAppliedFlag = F1Flag::IDLE;
static uint32_t _clearAppliedAt  = 0;
static bool     _clearTimerActive = false;

// Wrap-safe elapsed-time check for millis()-based timers (uint32_t subtraction).
static inline bool elapsedMs(uint32_t startMs, uint32_t durationMs, uint32_t nowMs) {
    return static_cast<uint32_t>(nowMs - startMs) >= durationMs;
}

static uint16_t blinkIntervalForFlag(F1Flag flag) {
    switch (flag) {
        case F1Flag::CLEAR:    return 0;      // solid on
        case F1Flag::YELLOW:   return 800;
        case F1Flag::VSC:      return 400;
        case F1Flag::SC:       return 200;
        case F1Flag::RED_FLAG: return 80;
        case F1Flag::CHEQ:     return 50;
        default:               return 0;      // off for IDLE
    }
}

static void tickBuiltinLed(F1Flag flag) {
    if (flag == F1Flag::IDLE) {
        digitalWrite(BUILTIN_LED_PIN, LOW);
        return;
    }

    uint16_t interval = blinkIntervalForFlag(flag);
    if (interval == 0) {
        // Solid on (CLEAR)
        digitalWrite(BUILTIN_LED_PIN, HIGH);
        return;
    }

    if (millis() - _blinkLastToggle >= interval) {
        _blinkLastToggle = millis();
        _blinkOn = !_blinkOn;
        digitalWrite(BUILTIN_LED_PIN, _blinkOn ? HIGH : LOW);
    }
}

// ── LED update hooks (called from web_server.cpp on config change) ────────────

void onLedCountChanged(uint16_t count) {
    leds.setCount(count);
}

void onBrightnessChanged(uint8_t brightness) {
    leds.setBrightness(brightness);
}

// ── SignalR message handler ───────────────────────────────────────────────────

F1Flag lastQueuedFlag = F1Flag::IDLE;

static void onF1Message(const String& stream, JsonObject data) {
    bool stateChanged = false;

    // Log all incoming stream data for debugging
    String rawPayload;
    serializeJson(data, rawPayload);
    Serial.printf("[F1 RX] %s: %s\n", stream.c_str(), rawPayload.c_str());

    if (stream == "TrackStatus") {
        const char* statusCode = data["Status"] | "";
        const char* message    = data["Message"] | "";
        String parsed = parseTrackStatus(statusCode, message);
        if (parsed.length() > 0 && parsed != f1State.trackStatusRaw) {
            f1State.trackStatusRaw = parsed;
            stateChanged = true;
            Serial.printf("[F1] TrackStatus parsed: %s (session: %s)\n",
                          parsed.c_str(), f1State.sessionStatusName());
        }
    }
    else if (stream == "SessionStatus") {
        const char* status  = data["Status"]  | "";
        const char* started = data["Started"] | "";
        SessionStatus ss = parseSessionStatus(status, started, f1State.trackStatusRaw);
        Serial.printf("[F1] SessionStatus parsed: %s → %s\n",
                      status, f1State.sessionStatusName());
        if (ss != SessionStatus::UNKNOWN && ss != f1State.sessionStatus) {
            f1State.sessionStatus = ss;
            f1State.sessionRaw    = status;
            stateChanged = true;
        }
    }
    else if (stream == "SessionInfo") {
        const char* type = data["Type"] | "";
        const char* name = data["Name"] | "";

        // Resolve label (mirrors HA plugin logic)
        String label = "";
        String t = type; t.toLowerCase();
        String n = name; n.toLowerCase();

        if (t == "practice") {
            int num = data["Number"] | 0;
            label = "Practice " + String(num);
        } else if (t == "qualifying") {
            label = (n.indexOf("sprint") >= 0) ? "Sprint Qualifying" : "Qualifying";
        } else if (t == "race") {
            label = (n.indexOf("sprint") >= 0) ? "Sprint" : "Race";
        } else {
            label = name;  // fallback
        }

        if (label != f1State.sessionType) {
            f1State.sessionType = label;
            stateChanged = true;
        }
    }
    else if (stream == "Heartbeat") {
        // Just touching _lastMessageMs via the client is enough — no action needed
        return;
    }

    if (!stateChanged) return;

    // ── Resolve new target flag and queue it ───────────────────────────────────
    F1Flag target = f1State.resolveDisplayFlag();
    if (target != lastQueuedFlag) {
        F1Flag prev = lastQueuedFlag;
        lastQueuedFlag = target;
        f1State.queueFlag(target, config.delay_ms);
        Serial.printf("[F1] %s queued: %s → %s (delay %lu ms, queue %d)\n",
                      stream.c_str(), f1State.flagNameFor(prev),
                      f1State.flagNameFor(target), config.delay_ms,
                      f1State._queueCount);
    }

    webServer.sendStatus();
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("\n[F1 Sensor] Booting…");

    // DEBUG: built-in LED for testing without strip
    pinMode(BUILTIN_LED_PIN, OUTPUT);

    // Load config from NVS
    config.load();

    // Start LEDs immediately so there's visual feedback during WiFi setup
    leds.begin(config.led_count, config.brightness);
    leds.setState(F1Flag::IDLE);

    // Blink LED 0 while WiFi setup is in progress (autoConnect blocks setup).
    leds.startBootIndicator();

    // WiFiManager — blocks until WiFi connects.
    // On first boot (or after /api/wifi/reset) opens AP "F1-Sensor-Setup".
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);   // 3-min portal timeout, then retry
    wm.setConnectTimeout(15);
    wm.setAPCallback([](WiFiManager*) { leds.setBootIndicatorApMode(true); });

    if (!wm.autoConnect("F1-Sensor-Setup")) {
        Serial.println("[WiFi] Connect failed, restarting…");
        ESP.restart();
    }

    leds.stopBootIndicator();

    Serial.printf("[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());

    if (MDNS.begin("f1sensor")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[mDNS] http://f1sensor.local");
    }

    // Start web server (serves UI + SSE)
    webServer.begin();

    // Wire up SignalR and start connecting
    signalr.setCallback(onF1Message);
    signalr.begin();

    Serial.println("[F1 Sensor] Ready.");
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
    // Tick SignalR state machine (negotiate / ws.loop / heartbeat / reconnect)
    signalr.tick();

    // Promote pending flag once delay has elapsed
    F1Flag before = f1State.currentFlag;
    f1State.tick(config.delay_ms);
    if (f1State.currentFlag != before) {
        Serial.printf("[F1] Flag applied: %s\n", f1State.flagName());
        webServer.sendStatus();
    }

    if (f1State.currentFlag != _lastAppliedFlag) {
        _lastAppliedFlag = f1State.currentFlag;
        if (_lastAppliedFlag == F1Flag::CLEAR) {
            _clearAppliedAt = millis();
            _clearTimerActive = true;
        } else {
            // Clear timer when leaving CLEAR so a future CLEAR starts a fresh window.
            _clearAppliedAt = 0;
            _clearTimerActive = false;
        }
    }

    // LED state: manual override takes priority, otherwise mirror live flag.
    // When the override is released, this snaps back to the current live flag
    // (IDLE if none), so testing CHEQ and releasing returns to IDLE immediately.
    F1Flag liveTarget = f1State.currentFlag;
    uint32_t nowMs = millis();
    if (!webServer.isOverrideActive() &&
        liveTarget == F1Flag::CLEAR &&
        _clearTimerActive &&
        elapsedMs(_clearAppliedAt, CLEAR_DISPLAY_MS, nowMs)) {
        liveTarget = F1Flag::IDLE;
    }

    F1Flag target = webServer.isOverrideActive()
                        ? webServer.getOverrideFlag()
                        : liveTarget;
    if (leds.getState() != target) leds.setState(target);

    // Update LED animation frame
    leds.tick();

    // DEBUG: mirror flag state to built-in LED
    tickBuiltinLed(leds.getState());
}
