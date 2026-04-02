#include "led_controller.h"
#include "config.h"

// ── Static LED buffer (MAX_LEDS ceiling, runtime count used) ──────────────────
static CRGB ledBuffer[MAX_LEDS];

LedController leds;

// ── Animation timing (ms per frame) ──────────────────────────────────────────
#define IDLE_FRAME_MS      30    // comet speed
#define YELLOW_FRAME_MS    25    // yellow chase — slightly faster
#define VSC_FRAME_MS      200    // slow alternating segments
#define SC_FRAME_MS        80    // fast alternating segments
#define RED_FRAME_MS      150    // alternating red flash
#define CHEQ_FRAME_MS      60    // chequered sweep
#define GREEN_PULSE_MS     12    // fade step speed for green pulses

#define GREEN_PULSES        3    // number of pulses before returning to IDLE

void LedController::begin(uint16_t count, uint8_t brightness) {
    _count = constrain(count, 1, MAX_LEDS);
    FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(ledBuffer, MAX_LEDS);
    FastLED.setBrightness(brightness);
    FastLED.clear(true);
}

void LedController::setBrightness(uint8_t b) {
    FastLED.setBrightness(b);
}

void LedController::setCount(uint16_t count) {
    uint16_t newCount = constrain(count, 1, MAX_LEDS);
    // Zero out any LEDs that are being removed
    if (newCount < _count) {
        for (uint16_t i = newCount; i < _count; i++) ledBuffer[i] = CRGB::Black;
        FastLED.show();
    }
    _count = newCount;
}

void LedController::setState(F1Flag flag) {
    if (_state == flag) return;
    _state       = flag;
    _frame       = 0;
    _pulseCount  = 0;
    _pulseRising = true;
    _lastTick    = 0;   // force immediate first frame
}

// ── Main tick ─────────────────────────────────────────────────────────────────

void LedController::tick() {
    switch (_state) {
        case F1Flag::IDLE:     tickIdle();   break;
        case F1Flag::CLEAR:    tickClear();  break;
        case F1Flag::YELLOW:   tickYellow(); break;
        case F1Flag::VSC:      tickVSC();    break;
        case F1Flag::SC:       tickSC();     break;
        case F1Flag::RED_FLAG: tickRed();    break;
        case F1Flag::CHEQ:     tickCheq();   break;
    }
}

// ── IDLE — dim red base with a brighter red comet ─────────────────────────────

void LedController::tickIdle() {
    if (millis() - _lastTick < IDLE_FRAME_MS) return;
    _lastTick = millis();

    CRGB bg    = CRGB(35, 0, 0);
    CRGB comet = CRGB(220, 0, 0);
    fillSolid(bg);
    setComet(_frame % _count, comet, bg, 6);
    FastLED.show();
    _frame++;
}

// ── CLEAR — 3 green pulses, then back to IDLE ─────────────────────────────────

void LedController::tickClear() {
    if (millis() - _lastTick < GREEN_PULSE_MS) return;
    _lastTick = millis();

    if (_pulseCount >= GREEN_PULSES) {
        setState(F1Flag::IDLE);
        return;
    }

    // _frame goes 0→255 (rising) then 255→0 (falling)
    if (_pulseRising) {
        _frame = min((int)_frame + 5, 255);
        if (_frame >= 255) _pulseRising = false;
    } else {
        _frame = (_frame > 5) ? _frame - 5 : 0;
        if (_frame == 0) {
            _pulseRising = true;
            _pulseCount++;
        }
    }

    CRGB color = CRGB(0, (uint8_t)_frame, 0);
    fillSolid(color);
    FastLED.show();
}

// ── YELLOW — yellow comet chase ───────────────────────────────────────────────

void LedController::tickYellow() {
    if (millis() - _lastTick < YELLOW_FRAME_MS) return;
    _lastTick = millis();

    CRGB bg    = CRGB(45, 36, 0);
    CRGB comet = CRGB(255, 200, 0);
    fillSolid(bg);
    setComet(_frame % _count, comet, bg, 6);
    FastLED.show();
    _frame++;
}

// ── VSC — slow alternating yellow/off segments ───────────────────────────────

void LedController::tickVSC() {
    if (millis() - _lastTick < VSC_FRAME_MS) return;
    _lastTick = millis();

    setAlternatingSegments(5, _frame % 10, CRGB(255, 180, 0), CRGB::Black);
    FastLED.show();
    _frame++;
}

// ── SC — fast alternating yellow/off segments ────────────────────────────────

void LedController::tickSC() {
    if (millis() - _lastTick < SC_FRAME_MS) return;
    _lastTick = millis();

    setAlternatingSegments(4, _frame % 8, CRGB(255, 180, 0), CRGB::Black);
    FastLED.show();
    _frame++;
}

// ── RED FLAG — alternating bright red / dark red segments ────────────────────

void LedController::tickRed() {
    if (millis() - _lastTick < RED_FRAME_MS) return;
    _lastTick = millis();

    // Alternate between two reds for a pulsing danger feel
    CRGB bright = CRGB(255, 0, 0);
    CRGB dark   = CRGB(60, 0, 0);
    setAlternatingSegments(5, _frame % 10, bright, dark);
    FastLED.show();
    _frame++;
}

// ── CHEQ — chequered sweep with full-white flash ──────────────────────────────

void LedController::tickCheq() {
    if (millis() - _lastTick < CHEQ_FRAME_MS) return;
    _lastTick = millis();

    uint16_t cycle = _frame % 30;

    if (cycle < 20) {
        // Scrolling chequered segments
        setAlternatingSegments(3, _frame % 6, CRGB::White, CRGB::Black);
    } else {
        // Full white flash every 20 frames
        fillSolid(CRGB::White);
    }

    FastLED.show();
    _frame++;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void LedController::clearAll() {
    for (uint16_t i = 0; i < _count; i++) ledBuffer[i] = CRGB::Black;
}

void LedController::fillSolid(CRGB color) {
    for (uint16_t i = 0; i < _count; i++) ledBuffer[i] = color;
}

// Draw a comet at `pos` with a fading trail behind it.
void LedController::setComet(uint16_t pos, CRGB cometColor, CRGB bgColor,
                               uint8_t trailLen) {
    ledBuffer[pos] = cometColor;
    for (uint8_t t = 1; t <= trailLen; t++) {
        uint16_t trailPos = (pos + _count - t) % _count;
        uint8_t  fade     = 255 - (t * (255 / (trailLen + 1)));
        ledBuffer[trailPos] = blend(bgColor, cometColor, fade);
    }
}

// Fill strip with alternating segments of colorA and colorB.
// `offset` scrolls the pattern one LED per call.
void LedController::setAlternatingSegments(uint8_t segLen, uint16_t offset,
                                            CRGB colorA, CRGB colorB) {
    for (uint16_t i = 0; i < _count; i++) {
        uint16_t pos = (i + offset) % (segLen * 2);
        ledBuffer[i] = (pos < segLen) ? colorA : colorB;
    }
}
