#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include "f1_state.h"

class LedController {
public:
    void begin(uint16_t count, uint8_t brightness);
    void tick();

    void setState(F1Flag flag);
    F1Flag getState() const { return _state; }

    void setBrightness(uint8_t b);
    void setCount(uint16_t count);

private:
    // ── Animation state ───────────────────────────────────────────────────────
    F1Flag   _state       = F1Flag::IDLE;
    uint32_t _lastTick    = 0;
    uint16_t _frame       = 0;   // general frame counter per animation
    uint8_t  _pulseCount  = 0;   // for GREEN: counts completed pulses
    bool     _pulseRising = true; // for GREEN: fade direction

    uint16_t _count       = 60;

    // ── Per-effect tick handlers ──────────────────────────────────────────────
    void tickIdle();
    void tickClear();
    void tickYellow();
    void tickVSC();
    void tickSC();
    void tickRed();
    void tickCheq();

    // ── Helpers ───────────────────────────────────────────────────────────────
    void clearAll();
    void fillSolid(CRGB color);
    void setComet(uint16_t pos, CRGB cometColor, CRGB bgColor, uint8_t trailLen);
    void setAlternatingSegments(uint8_t segLen, uint16_t offset,
                                 CRGB colorA, CRGB colorB);
};

extern LedController leds;
