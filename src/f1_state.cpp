#include "f1_state.h"

F1State f1State;

// ── TrackStatus parsing ───────────────────────────────────────────────────────
// Mirrors normalize_track_status() from the HA plugin.

String parseTrackStatus(const char* statusCode, const char* message) {
    // Numeric status codes take priority
    if (statusCode && *statusCode) {
        String s = String(statusCode);
        s.trim();
        if (s == "1" || s == "8") return "CLEAR";
        if (s == "2")             return "YELLOW";
        if (s == "4")             return "SC";
        if (s == "5")             return "RED";
        if (s == "6" || s == "7") return "VSC";
    }

    // Fall back to message string
    if (message && *message) {
        String m = String(message);
        m.toUpperCase();
        m.trim();

        if (m == "ALLCLEAR" || m == "ALL CLEAR")               return "CLEAR";
        if (m == "YELLOW" || m == "DOUBLE YELLOW"
                          || m == "DOUBLEYELLOW")               return "YELLOW";
        if (m == "SAFETY CAR" || m == "SAFETYCAR"
                              || m == "SC DEPLOYED")            return "SC";
        if (m == "RED FLAG" || m == "REDFLAG")                  return "RED";
        if (m == "VSC" || m == "VSC DEPLOYED" || m == "VSC ENDING") return "VSC";
    }

    return "";  // unknown — keep previous value
}

// ── SessionStatus parsing ─────────────────────────────────────────────────────
// Mirrors the mapping in the HA plugin's sensor.py.

SessionStatus parseSessionStatus(const char* status, const char* started,
                                  const String& trackStatusRaw) {
    if (!status || !*status) return SessionStatus::UNKNOWN;

    String s = String(status);
    s.trim();

    if (s.equalsIgnoreCase("Started"))   return SessionStatus::LIVE;
    if (s.equalsIgnoreCase("Finished"))  return SessionStatus::FINISHED;
    if (s.equalsIgnoreCase("Finalised")) return SessionStatus::FINALISED;
    if (s.equalsIgnoreCase("Ends"))      return SessionStatus::ENDED;

    if (s.equalsIgnoreCase("Inactive") || s.equalsIgnoreCase("Aborted")) {
        String st = started ? String(started) : String("");
        if (st.equalsIgnoreCase("Finished")) return SessionStatus::BREAK;
        if (st.equalsIgnoreCase("Started")) {
            // Red flag suspension vs normal inactive-while-live
            return (trackStatusRaw == "RED") ? SessionStatus::SUSPENDED
                                             : SessionStatus::LIVE;
        }
        return SessionStatus::PRE;
    }

    return SessionStatus::PRE;
}
