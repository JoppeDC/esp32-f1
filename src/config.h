#pragma once
#include <Arduino.h>
#include <Preferences.h>

// ── Hardware ──────────────────────────────────────────────────────────────────
// Change LED_DATA_PIN to match your wiring before flashing.
#define LED_DATA_PIN  4
#define MAX_LEDS      500   // buffer ceiling; actual count is runtime-configurable

// ── F1 Live Timing endpoints (SignalR Core, unauthenticated public streams) ───
#define F1_HOST            "livetiming.formula1.com"
#define F1_NEGOTIATE_PATH  "/signalrcore/negotiate?negotiateVersion=1"
#define F1_CONNECT_PATH    "/signalrcore"
#define F1_WS_PORT         443

// ── Timing constants ──────────────────────────────────────────────────────────
#define HEARTBEAT_INTERVAL_MS   300000UL   // periodic liveness log
#define INACTIVITY_TIMEOUT_MS    45000UL   // reconnect if no message for 45 s
#define RECONNECT_INITIAL_MS      5000UL   // first retry after 5 s
#define RECONNECT_MAX_MS         60000UL   // cap at 60 s

// ── Runtime configuration (persisted to NVS) ─────────────────────────────────
// Tunable from the web UI; values below are first-boot defaults used when
// no value is present in NVS (fresh flash, after NVS erase, etc.).
struct Config {
    uint16_t led_count  = 60;
    uint8_t  brightness = 128;
    uint32_t delay_ms   = 45000;   // delay between F1 event and LED change

    void load() {
        Preferences prefs;
        prefs.begin("f1sensor", /*readOnly=*/true);
        led_count  = prefs.getUShort("led_count",  led_count);
        brightness = prefs.getUChar ("brightness", brightness);
        delay_ms   = prefs.getULong ("delay_ms",   delay_ms);
        prefs.end();
    }

    void save() const {
        Preferences prefs;
        prefs.begin("f1sensor", /*readOnly=*/false);
        prefs.putUShort("led_count",  led_count);
        prefs.putUChar ("brightness", brightness);
        prefs.putULong ("delay_ms",   delay_ms);
        prefs.end();
    }
};

extern Config config;