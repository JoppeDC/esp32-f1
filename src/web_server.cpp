#include "web_server.h"
#include "config.h"
#include "f1_state.h"
#include "signalr_client.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>

F1WebServer webServer;

// ── Helpers ───────────────────────────────────────────────────────────────────

String F1WebServer::buildStatusJson() {
    JsonDocument doc;
    doc["flag"]          = f1State.flagName();
    doc["session"]       = f1State.sessionType.length() ? f1State.sessionType : "—";
    doc["sessionStatus"] = f1State.sessionStatusName();
    doc["trackRaw"]      = f1State.trackStatusRaw;
    doc["sigRState"]     = signalr.getStateStr();
    doc["ip"]            = WiFi.localIP().toString();
    doc["rssi"]          = WiFi.RSSI();
    doc["delayMs"]       = config.delay_ms;
    doc["queueDepth"]    = f1State._queueCount;

    String out;
    serializeJson(doc, out);
    return out;
}

// ── Startup ───────────────────────────────────────────────────────────────────

void F1WebServer::begin() {
    if (!LittleFS.begin()) {
        Serial.println("[Web] LittleFS mount failed");
        return;
    }

    setupRoutes();
    _server.addHandler(&_events);
    _server.begin();
    Serial.println("[Web] Server started on port 80");
}

// ── Routes ────────────────────────────────────────────────────────────────────

void F1WebServer::setupRoutes() {
    // ── Static files from LittleFS ────────────────────────────────────────────
    _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // ── GET /api/status — one-shot status snapshot ────────────────────────────
    _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildStatusJson());
    });

    // ── GET /api/config ───────────────────────────────────────────────────────
    _server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["led_count"]  = config.led_count;
        doc["brightness"] = config.brightness;
        doc["delay_ms"]   = config.delay_ms;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ── POST /api/config ──────────────────────────────────────────────────────
    _server.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest* req) {},   // final handler (unused for body)
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t index, size_t total) {

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, (const char*)data, len);
            if (err) {
                req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
                return;
            }

            bool changed = false;

            if (doc["led_count"].is<uint16_t>()) {
                uint16_t v = constrain((uint16_t)doc["led_count"], 1, MAX_LEDS);
                if (v != config.led_count) {
                    config.led_count = v;
                    changed = true;
                    // notify led_controller via extern — handled in main
                    extern void onLedCountChanged(uint16_t);
                    onLedCountChanged(v);
                }
            }
            if (doc["brightness"].is<uint8_t>()) {
                uint8_t v = doc["brightness"];
                if (v != config.brightness) {
                    config.brightness = v;
                    changed = true;
                    extern void onBrightnessChanged(uint8_t);
                    onBrightnessChanged(v);
                }
            }
            if (doc["delay_ms"].is<uint32_t>()) {
                uint32_t v = constrain((uint32_t)doc["delay_ms"], 0UL, 120000UL);
                config.delay_ms = v;
                changed = true;
            }

            if (changed) config.save();
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    // ── POST /api/restart ─────────────────────────────────────────────────────
    _server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", "{\"ok\":true}");
        delay(500);
        ESP.restart();
    });

    // ── POST /api/wifi/reset — clears WiFiManager credentials ────────────────
    _server.on("/api/wifi/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", "{\"ok\":true}");
        delay(500);
        // Erase WiFiManager NVS partition then restart
        WiFi.disconnect(true, true);
        ESP.restart();
    });

    // 404
    _server.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not found");
    });
}

// ── SSE push ──────────────────────────────────────────────────────────────────

void F1WebServer::sendStatus() {
    if (_events.count() == 0) return;   // no clients connected
    _events.send(buildStatusJson().c_str(), "status", millis());
}
