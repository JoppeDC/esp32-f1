#pragma once
// Flag and session enums plus their wire names. Pure C++ (no Arduino) so the
// native test env can include it.
#include <stdint.h>
#include <string.h>

enum class F1Flag : uint8_t {
    IDLE,
    CLEAR,      // green — track clear
    YELLOW,
    VSC,        // virtual safety car
    SC,         // safety car
    RED_FLAG,
    CHEQ        // chequered — session finished
};

enum class SessionStatus : uint8_t {
    UNKNOWN,
    PRE,
    LIVE,
    SUSPENDED,
    BREAK,
    FINISHED,
    FINALISED,
    ENDED
};

// Wire name shared by the relay's `display` field, the web API and logs.
inline const char* flagName(F1Flag f) {
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

// Inverse of flagName(). Unknown or null → IDLE.
inline F1Flag flagFromName(const char* s) {
    if (!s) return F1Flag::IDLE;
    if (strcmp(s, "RED")    == 0) return F1Flag::RED_FLAG;
    if (strcmp(s, "SC")     == 0) return F1Flag::SC;
    if (strcmp(s, "VSC")    == 0) return F1Flag::VSC;
    if (strcmp(s, "YELLOW") == 0) return F1Flag::YELLOW;
    if (strcmp(s, "CLEAR")  == 0) return F1Flag::CLEAR;
    if (strcmp(s, "CHEQ")   == 0) return F1Flag::CHEQ;
    return F1Flag::IDLE;
}

inline const char* sessionStatusName(SessionStatus s) {
    switch (s) {
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

// Maps a relay "session" string (lamp protocol v1) to SessionStatus.
inline SessionStatus sessionStatusFromName(const char* s) {
    if (!s) return SessionStatus::UNKNOWN;
    if (strcmp(s, "pre")       == 0) return SessionStatus::PRE;
    if (strcmp(s, "live")      == 0) return SessionStatus::LIVE;
    if (strcmp(s, "suspended") == 0) return SessionStatus::SUSPENDED;
    if (strcmp(s, "break")     == 0) return SessionStatus::BREAK;
    if (strcmp(s, "finished")  == 0) return SessionStatus::FINISHED;
    if (strcmp(s, "finalised") == 0) return SessionStatus::FINALISED;
    if (strcmp(s, "ended")     == 0) return SessionStatus::ENDED;
    return SessionStatus::UNKNOWN;
}
