#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "f1_state.h"

class F1WebServer {
public:
    void begin();

    // Call whenever connection state or flag changes to push SSE update.
    void sendStatus();

    // Debug flag override
    bool   isOverrideActive() const { return _flagOverride; }
    F1Flag getOverrideFlag()  const { return _overrideFlag; }

private:
    AsyncWebServer   _server{80};
    AsyncEventSource _events{"/events"};

    bool _fsMounted = false;

    // Debug override state (RAM only, cleared on reboot)
    bool   _flagOverride = false;
    F1Flag _overrideFlag = F1Flag::IDLE;

    void setupRoutes();
    String buildStatusJson();
    String buildDebugSystemJson();
    String buildDebugLiveJson();
};

extern F1WebServer webServer;
