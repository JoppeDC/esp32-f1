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
    String        sessionRaw      = "";   // Started / Finished / etc.

    // ── Derived helpers ───────────────────────────────────────────────────────
    bool sessionActive()   const { return sessionStatus == SessionStatus::LIVE; }
    bool sessionFinished() const {
        return sessionStatus == SessionStatus::FINISHED
            || sessionStatus == SessionStatus::FINALISED;
    }
    bool isRaceOrSprint() const {
        return sessionType == "Race" || sessionType == "Sprint";
    }

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

    // ── Automation logic ──────────────────────────────────────────────────────
    // Maps current state to the flag that should drive the LEDs,
    // using the same priority order as the Home Assistant automation.
    F1Flag resolveDisplayFlag() const {
        const bool scActive = (trackStatusRaw == "SC" || trackStatusRaw == "VSC");

        if (sessionActive() && trackStatusRaw == "RED")    return F1Flag::RED_FLAG;
        if (sessionActive() && scActive && trackStatusRaw == "SC")  return F1Flag::SC;
        if (sessionActive() && scActive && trackStatusRaw == "VSC") return F1Flag::VSC;
        if (sessionActive() && trackStatusRaw == "YELLOW") return F1Flag::YELLOW;
        if (sessionActive() && trackStatusRaw == "CLEAR")  return F1Flag::CLEAR;
        if (sessionFinished() && isRaceOrSprint())          return F1Flag::CHEQ;
        return F1Flag::IDLE;
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

// ── Parsing helpers ───────────────────────────────────────────────────────────

// Parse TrackStatus payload → normalised string (CLEAR/YELLOW/VSC/SC/RED).
String parseTrackStatus(const char* statusCode, const char* message);

// Parse SessionStatus payload → SessionStatus enum.
SessionStatus parseSessionStatus(const char* status, const char* started,
                                  const String& trackStatusRaw);

extern F1State f1State;
