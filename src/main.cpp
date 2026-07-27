#include <Arduino.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

#include "config.h"
#include "f1_state.h"
#include "led_controller.h"
#include "relay_client.h"
#include "web_server.h"

Config config;

// ── DEBUG: Built-in LED blink pattern to visualise flag state without a strip ─
// Remove this once external LEDs are connected.
#ifdef CONFIG_IDF_TARGET_ESP32C3
#define BUILTIN_LED_PIN 8   // most C3 dev boards; often active-low (blink inverted)
#else
#define BUILTIN_LED_PIN 2
#endif
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

void onRelayConfigChanged() {
    relay.requestReconfigure(config.relay_host, config.relay_port);
}

// ── Relay message handler ─────────────────────────────────────────────────────

F1Flag lastQueuedFlag = F1Flag::IDLE;

static void onRelayMessage(F1Flag display, JsonObject msg) {
    // Informational fields for the lamp web UI
    f1State.trackStatusRaw = String(msg["track"]   | "");
    f1State.sessionType    = String(msg["type"]    | "");
    f1State.sessionStatus  = F1State::sessionStatusFromString(msg["session"] | "");

    if (display != lastQueuedFlag) {
        F1Flag prev = lastQueuedFlag;
        lastQueuedFlag = display;
        f1State.queueFlag(display, config.delay_ms);
        Serial.printf("[F1] queued: %s → %s (delay %lu ms, queue %d)\n",
                      F1State::flagNameFor(prev), F1State::flagNameFor(display),
                      config.delay_ms, f1State._queueCount);
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

    // Wire up the relay client and start connecting
    relay.setCallback(onRelayMessage);
    relay.begin(config.relay_host, config.relay_port);

    Serial.println("[F1 Sensor] Ready.");
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
    relay.tick();

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
    const bool overrideActive = webServer.isOverrideActive();
    F1Flag liveTarget = f1State.currentFlag;
    uint32_t nowMs = millis();
    if (!overrideActive &&
        liveTarget == F1Flag::CLEAR &&
        _clearTimerActive &&
        elapsedMs(_clearAppliedAt, CLEAR_DISPLAY_MS, nowMs)) {
        liveTarget = F1Flag::IDLE;
    }

    F1Flag target = overrideActive
                        ? webServer.getOverrideFlag()
                        : liveTarget;
    if (leds.getState() != target) leds.setState(target);

    // Update LED animation frame
    leds.tick();

    // DEBUG: mirror flag state to built-in LED
    tickBuiltinLed(leds.getState());
}
