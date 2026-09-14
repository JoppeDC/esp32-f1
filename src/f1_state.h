#pragma once
#include <Arduino.h>
#include "f1_flags.h"
#include "flag_queue.h"

// Everything the lamp knows about the current F1 session. `flags` is the
// delayed flag pipeline that drives the LEDs; the rest is informational and
// only shown in the web UI.
struct F1State {
    FlagQueue     flags;

    SessionStatus sessionStatus  = SessionStatus::UNKNOWN;
    String        sessionType;      // "Race", "Sprint", "Qualifying", etc.
    String        trackStatusRaw;   // CLEAR / YELLOW / VSC / SC / RED
};

extern F1State f1State;
