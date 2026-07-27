#pragma once
#include <Arduino.h>

// ── Enums ─────────────────────────────────────────────────────────────────────

enum class F1Flag {
    IDLE,
    CLEAR,      // green — track clear
    YELLOW,
    VSC,        // virtual safety car
    SC,         // safety car
    RED_FLAG,
    CHEQ        // chequered — session finished
};

enum class SessionStatus {
    UNKNOWN,
    PRE,
    LIVE,
    SUSPENDED,
    BREAK,
    FINISHED,
    FINALISED,
    ENDED
};

// ── State struct ──────────────────────────────────────────────────────────────

struct F1State {
    // Current displayed flag (post-delay)
    F1Flag        currentFlag     = F1Flag::IDLE;

    // Session
    SessionStatus sessionStatus   = SessionStatus::UNKNOWN;
    String        sessionType     = "";   // "Race", "Sprint", "Qualifying", etc.

    // Raw strings for web UI
    String        trackStatusRaw  = "";   // CLEAR / YELLOW / VSC / SC / RED

    // ── Delay queue (FIFO) ─────────────────────────────────────────────────────
    // Each flag change is pushed with a timestamp. They apply in order after
    // the configured delay elapses, so short-lived flags (e.g. a brief yellow)
    // are never swallowed by a subsequent CLEAR.

    static constexpr uint8_t QUEUE_SIZE = 16;

    struct QueueEntry {
        F1Flag   flag;
        uint32_t queuedAt;   // millis() when queued
    };

    QueueEntry  _queue[QUEUE_SIZE] = {};
    uint8_t     _queueHead  = 0;   // next slot to read
    uint8_t     _queueTail  = 0;   // next slot to write
    uint8_t     _queueCount = 0;

    void queueFlag(F1Flag flag, uint32_t delay_ms) {
        if (delay_ms == 0) {
            currentFlag = flag;
            return;
        }

        if (_queueCount >= QUEUE_SIZE) {
            // Queue full — drop oldest entry
            _queueHead = (_queueHead + 1) % QUEUE_SIZE;
            _queueCount--;
        }

        _queue[_queueTail] = { flag, millis() };
        _queueTail = (_queueTail + 1) % QUEUE_SIZE;
        _queueCount++;
    }

    // Call from loop() — applies queued flags whose delay has elapsed.
    void tick(uint32_t delay_ms) {
        while (_queueCount > 0) {
            QueueEntry& front = _queue[_queueHead];
            if (millis() - front.queuedAt < delay_ms) break;

            currentFlag = front.flag;
            _queueHead = (_queueHead + 1) % QUEUE_SIZE;
            _queueCount--;
        }
    }

    // ── String helpers for web UI / logging ──────────────────────────────────
    static const char* flagNameFor(F1Flag f) {
        switch (f) {
            case F1Flag::IDLE:     return "IDLE";
            case F1Flag::CLEAR:    return "CLEAR";
            case F1Flag::YELLOW:   return "YELLOW";
            case F1Flag::VSC:      return "VSC";
            case F1Flag::SC:       return "SC";
            case F1Flag::RED_FLAG: return "RED";
            case F1Flag::CHEQ:     return "CHEQ";
        }
        return "IDLE";
    }

    // Maps a relay "session" string (lamp protocol v1) to SessionStatus,
    // for the lamp web UI.
    static SessionStatus sessionStatusFromString(const char* s) {
        if (strcmp(s, "pre")       == 0) return SessionStatus::PRE;
        if (strcmp(s, "live")      == 0) return SessionStatus::LIVE;
        if (strcmp(s, "suspended") == 0) return SessionStatus::SUSPENDED;
        if (strcmp(s, "break")     == 0) return SessionStatus::BREAK;
        if (strcmp(s, "finished")  == 0) return SessionStatus::FINISHED;
        if (strcmp(s, "finalised") == 0) return SessionStatus::FINALISED;
        if (strcmp(s, "ended")     == 0) return SessionStatus::ENDED;
        return SessionStatus::UNKNOWN;
    }

    const char* flagName() const {
        switch (currentFlag) {
            case F1Flag::IDLE:     return "IDLE";
            case F1Flag::CLEAR:    return "CLEAR";
            case F1Flag::YELLOW:   return "YELLOW";
            case F1Flag::VSC:      return "VSC";
            case F1Flag::SC:       return "SC";
            case F1Flag::RED_FLAG: return "RED";
            case F1Flag::CHEQ:     return "CHEQ";
        }
        return "IDLE";
    }

    const char* sessionStatusName() const {
        switch (sessionStatus) {
            case SessionStatus::UNKNOWN:   return "unknown";
            case SessionStatus::PRE:       return "pre";
            case SessionStatus::LIVE:      return "live";
            case SessionStatus::SUSPENDED: return "suspended";
            case SessionStatus::BREAK:     return "break";
            case SessionStatus::FINISHED:  return "finished";
            case SessionStatus::FINALISED: return "finalised";
            case SessionStatus::ENDED:     return "ended";
        }
        return "unknown";
    }
};

extern F1State f1State;
