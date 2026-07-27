#pragma once
#include <Arduino.h>
#include <Preferences.h>

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
struct Config {
    uint16_t led_count  = 60;
    uint8_t  brightness = 128;
    uint32_t delay_ms   = 45000;   // delay between F1 event and LED change
    String   relay_host = "";      // empty = relay not configured
    uint16_t relay_port = 8000;

    void load() {
        Preferences prefs;
        prefs.begin("f1sensor", /*readOnly=*/true);
        led_count  = prefs.getUShort("led_count",  led_count);
        brightness = prefs.getUChar ("brightness", brightness);
        delay_ms   = prefs.getULong ("delay_ms",   delay_ms);
        relay_host = prefs.getString("relay_host", relay_host);
        relay_port = prefs.getUShort("relay_port", relay_port);
        prefs.end();
    }

    void save() const {
        Preferences prefs;
        prefs.begin("f1sensor", /*readOnly=*/false);
        prefs.putUShort("led_count",  led_count);
        prefs.putUChar ("brightness", brightness);
        prefs.putULong ("delay_ms",   delay_ms);
        prefs.putString("relay_host", relay_host);
        prefs.putUShort("relay_port", relay_port);
        prefs.end();
    }
};

extern Config config;
