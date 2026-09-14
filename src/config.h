#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include "relay_url.h"

// ── Hardware ──────────────────────────────────────────────────────────────────
// Change LED_DATA_PIN to match your wiring before flashing.
// On ESP32-C3 GPIO 11-17 are wired to flash, so the classic pin 16 is unusable.
#ifdef CONFIG_IDF_TARGET_ESP32C3
#define LED_DATA_PIN  4
#else
#define LED_DATA_PIN  16
#endif
#define MAX_LEDS      500   // buffer ceiling; actual count is runtime-configurable

// ── Runtime configuration (persisted to NVS) ─────────────────────────────────
// Tunable from the web UI; values below are first-boot defaults used when
// no value is present in NVS (fresh flash, after NVS erase, etc.).
//
// Ownership: the fields are read and written by the Arduino loop task only.
// The web handler runs on the async_tcp task and must not touch them — it
// stages validated values with stage(), and loop() applies them with
// takeStaged(). That keeps FastLED, the relay client and NVS flash writes on
// one task, and means a String field is never reassigned under a reader.
struct Config {
    uint16_t led_count  = 60;
    uint8_t  brightness = 128;
    uint32_t delay_ms   = 45000;   // delay between F1 event and LED change

    // Full WebSocket URL of the relay; empty disables the connection. Defaults
    // to the public instance so a freshly flashed lamp works without setup.
    String   relay_url  = "wss://f1-relay.joppe.dev/ws";

    static constexpr uint32_t DELAY_MS_MAX = 120000;

    void load() {
        Preferences prefs;
        prefs.begin("f1sensor", /*readOnly=*/true);
        led_count  = prefs.getUShort("led_count",  led_count);
        brightness = prefs.getUChar ("brightness", brightness);
        delay_ms   = prefs.getULong ("delay_ms",   delay_ms);
        relay_url  = prefs.getString("relay_url",  relay_url);
        prefs.end();
    }

    void save() const {
        Preferences prefs;
        prefs.begin("f1sensor", /*readOnly=*/false);
        prefs.putUShort("led_count",  led_count);
        prefs.putUChar ("brightness", brightness);
        prefs.putULong ("delay_ms",   delay_ms);
        prefs.putString("relay_url",  relay_url);
        prefs.end();
    }

    // ── Cross-task handoff ───────────────────────────────────────────────────

    enum Field : uint8_t {
        LED_COUNT  = 1 << 0,
        BRIGHTNESS = 1 << 1,
        DELAY_MS   = 1 << 2,
        RELAY_URL  = 1 << 3,
    };

    // A partial update: only the fields named in `mask` are meaningful.
    // Fixed buffers so it can be copied under a spinlock.
    struct Staged {
        uint8_t  mask       = 0;
        uint16_t led_count  = 0;
        uint8_t  brightness = 0;
        uint32_t delay_ms   = 0;
        char     relay_url[RELAY_URL_MAX_LEN + 1] = {};
    };

    // Any task. Merges `s` into the pending update; a later stage() of the
    // same field wins.
    void stage(const Staged& s) {
        portENTER_CRITICAL(&_lock);
        if (s.mask & LED_COUNT)  _staged.led_count  = s.led_count;
        if (s.mask & BRIGHTNESS) _staged.brightness = s.brightness;
        if (s.mask & DELAY_MS)   _staged.delay_ms   = s.delay_ms;
        if (s.mask & RELAY_URL)  strlcpy(_staged.relay_url, s.relay_url, sizeof(_staged.relay_url));
        _staged.mask |= s.mask;
        portEXIT_CRITICAL(&_lock);
    }

    // Loop task. Returns true and fills `out` if anything was staged.
    bool takeStaged(Staged& out) {
        portENTER_CRITICAL(&_lock);
        const bool any = _staged.mask != 0;
        if (any) {
            out = _staged;
            _staged.mask = 0;
        }
        portEXIT_CRITICAL(&_lock);
        return any;
    }

private:
    Staged        _staged;
    portMUX_TYPE  _lock = portMUX_INITIALIZER_UNLOCKED;
};

extern Config config;
