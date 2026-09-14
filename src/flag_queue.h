#pragma once
// Delay queue between the relay's flag and the flag the LEDs show.
//
// Each flag change is pushed with a timestamp and applied in order once the
// configured delay has elapsed, so a short-lived flag (a brief yellow) is
// never swallowed by the CLEAR that follows it. Time is passed in rather than
// read from millis() so the queue is testable on the host; all comparisons
// use uint32_t subtraction and survive the 49-day wrap.
//
// Pure C++ (no Arduino).
#include <stdint.h>
#include "f1_flags.h"

class FlagQueue {
public:
    static constexpr uint8_t CAPACITY = 16;

    // Queue a change to `flag`. A repeat of the most recently queued target
    // is ignored (the relay resends full state on every message), so callers
    // can push every message unconditionally. Returns true if queued.
    bool push(F1Flag flag, uint32_t nowMs) {
        if (flag == _lastQueued) return false;
        _lastQueued = flag;

        if (_count >= CAPACITY) {
            // Full — drop the oldest so the newest state always lands.
            _head = (_head + 1) % CAPACITY;
            _count--;
        }
        _slots[_tail] = { flag, nowMs };
        _tail = (_tail + 1) % CAPACITY;
        _count++;
        return true;
    }

    // Apply every queued flag whose delay has elapsed. Returns true if the
    // current flag changed.
    bool tick(uint32_t nowMs, uint32_t delayMs) {
        const F1Flag before = _current;
        while (_count > 0) {
            const Entry& front = _slots[_head];
            if (static_cast<uint32_t>(nowMs - front.queuedAt) < delayMs) break;
            _current = front.flag;
            _head = (_head + 1) % CAPACITY;
            _count--;
        }
        return _current != before;
    }

    // Forget everything and show IDLE. Used when no fresh state is available
    // any more: whatever was queued or displayed may be hours old, and the
    // next message from the relay must queue from a clean slate rather than
    // be deduplicated against a target that was never shown.
    void reset() {
        _head = _tail = _count = 0;
        _current = _lastQueued = F1Flag::IDLE;
    }

    F1Flag  current()    const { return _current; }
    F1Flag  lastQueued() const { return _lastQueued; }
    uint8_t depth()      const { return _count; }
    bool    hasPending() const { return _count > 0; }

    // Next flag to be applied (only meaningful while hasPending()).
    F1Flag pendingFlag() const { return _count ? _slots[_head].flag : _current; }

    // Milliseconds until the next flag applies, clamped at 0.
    uint32_t pendingInMs(uint32_t nowMs, uint32_t delayMs) const {
        if (!_count) return 0;
        const uint32_t waited = nowMs - _slots[_head].queuedAt;
        return waited >= delayMs ? 0 : delayMs - waited;
    }

private:
    struct Entry {
        F1Flag   flag;
        uint32_t queuedAt;
    };

    Entry   _slots[CAPACITY] = {};
    uint8_t _head  = 0;   // next slot to read
    uint8_t _tail  = 0;   // next slot to write
    uint8_t _count = 0;

    F1Flag _current    = F1Flag::IDLE;   // what the LEDs should show
    F1Flag _lastQueued = F1Flag::IDLE;   // most recent push target, for dedup
};
