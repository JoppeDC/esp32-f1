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
#define RED_FRAME_MS       25    // step length for breathing pulse
#define CHEQ_FRAME_MS      60    // chequered sweep
#define GREEN_PULSE_MS     12    // fade step speed for green pulses

void LedController::begin(uint16_t count, uint8_t brightness) {
    _count = constrain(count, 1, MAX_LEDS);
    // Register the full buffer (the template needs a compile-time ceiling),
    // then narrow the controller to the configured count so each show()
    // costs ~30 µs per real LED rather than ~15 ms for all 500.
    _controller = &FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(ledBuffer, MAX_LEDS);
    _controller->setLeds(ledBuffer, _count);
    FastLED.setBrightness(brightness);
    FastLED.clear(true);
}

void LedController::setBrightness(uint8_t b) {
    FastLED.setBrightness(b);
}

void LedController::setCount(uint16_t count) {
    uint16_t newCount = constrain(count, 1, MAX_LEDS);
    if (newCount == _count) return;
    if (newCount < _count) {
        // WS2812s latch their last value, so blank the LEDs being removed
        // while the controller still reaches them, then narrow it.
        for (uint16_t i = newCount; i < _count; i++) ledBuffer[i] = CRGB::Black;
        FastLED.show();
    }
    _count = newCount;
    _controller->setLeds(ledBuffer, _count);
    _lastTick = 0;   // redraw at the new size on the next tick
}

// ── Boot indicator ────────────────────────────────────────────────────────────
// Blinks LED 0 blue while WiFi setup is in progress. setup() blocks inside
// WiFiManager::autoConnect(), so loop() (and tick()) can't drive the strip;
// a dedicated FreeRTOS task gives us a heartbeat during that window.
//
// The task ends itself when asked rather than being vTaskDelete()d from
// outside: killing it mid-FastLED.show() would leave the RMT driver's state
// (and its semaphore) wherever the transmission was, and the next show()
// from loop() could hang on it.

void LedController::bootIndicatorTask(void* arg) {
    LedController* self = static_cast<LedController*>(arg);
    bool on = false;
    while (!self->_bootStop) {
        ledBuffer[0] = on ? CRGB::Blue : CRGB::Black;
        FastLED.show();
        on = !on;
        // Sleep in short slices so a stop request is honoured promptly.
        const uint32_t periodMs = self->_bootApMode ? 150 : 500;
        for (uint32_t slept = 0; slept < periodMs && !self->_bootStop; slept += 50) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    ledBuffer[0] = CRGB::Black;
    FastLED.show();
    self->_bootTask = nullptr;   // signals stopBootIndicator()
    vTaskDelete(nullptr);
}

void LedController::startBootIndicator() {
    if (_bootTask) return;
    _bootApMode = false;
    _bootStop   = false;
    TaskHandle_t handle = nullptr;
    xTaskCreatePinnedToCore(bootIndicatorTask, "bootBlink", 2048, this,
                            /*priority=*/1, &handle, /*core=*/0);
    _bootTask = handle;
}

void LedController::stopBootIndicator() {
    if (!_bootTask) return;
    _bootStop = true;
    // Wait for the task to finish its frame and exit, so the strip is ours
    // again before loop() starts drawing.
    while (_bootTask) vTaskDelay(1);
}

void LedController::setBootIndicatorApMode(bool apActive) {
    _bootApMode = apActive;
}

void LedController::setState(F1Flag flag) {
    if (_state == flag) return;
    _state       = flag;
    _frame       = 0;
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

// ── CLEAR — green pulses ──────────────────────────────────────────────────────
// Pulses for as long as the state is CLEAR. How long that is (the 10 s
// window before falling back to IDLE) is policy and lives in main.cpp; this
// renderer never changes its own state.

void LedController::tickClear() {
    if (millis() - _lastTick < GREEN_PULSE_MS) return;
    _lastTick = millis();

    // _frame goes 0→255 (rising) then 255→0 (falling)
    if (_pulseRising) {
        _frame = min((int)_frame + 5, 255);
        if (_frame >= 255) _pulseRising = false;
    } else {
        _frame = (_frame > 5) ? _frame - 5 : 0;
        if (_frame == 0) _pulseRising = true;
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

// ── RED FLAG — smooth red breathing pulse ────────────────────────────────────

void LedController::tickRed() {
    if (millis() - _lastTick < RED_FRAME_MS) return;
    _lastTick = millis();

    // Whole strip breathes between dim and full red on a sine envelope.
    // Never reaches black (floor of 30), so no hard on/off — calmer than a
    // strobe but still reads as "alert". Period ≈ 1.3 s.
    uint8_t v = sin8((uint8_t)(_frame * 5));   // 0..255, wraps every ~51 frames
    uint8_t r = 30 + scale8(v, 225);           // map to 30..255
    fillSolid(CRGB(r, 0, 0));
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
