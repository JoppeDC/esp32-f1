#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

class F1WebServer {
public:
    void begin();

    // Call whenever connection state or flag changes to push SSE update.
    void sendStatus();

private:
    AsyncWebServer   _server{80};
    AsyncEventSource _events{"/events"};

    void setupRoutes();
    String buildStatusJson();
};

extern F1WebServer webServer;
