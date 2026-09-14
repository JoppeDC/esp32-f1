#include "web_server.h"
#include "config.h"
#include "f1_state.h"
#include "led_controller.h"
#include "relay_client.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>

F1WebServer webServer;

// ── Helpers ───────────────────────────────────────────────────────────────────

String F1WebServer::buildStatusJson() {
    JsonDocument doc;
    doc["flag"]          = flagName(f1State.flags.current());
    doc["session"]       = f1State.sessionType.length() ? f1State.sessionType : "—";
    doc["sessionStatus"] = sessionStatusName(f1State.sessionStatus);
    doc["trackRaw"]      = f1State.trackStatusRaw;
    doc["relayState"]    = relay.getStateStr();
    doc["stale"]         = relay.isStale();
    doc["freshAgeMs"]    = relay.getFreshAgeMs();
    doc["ip"]            = WiFi.localIP().toString();
    doc["rssi"]          = WiFi.RSSI();
    doc["delayMs"]       = config.delay_ms;
    doc["queueDepth"]    = f1State.flags.depth();
    // Next flag waiting in the delay queue and how long until it shows, so the
    // dashboard can count down instead of just sitting on the old flag.
    if (f1State.flags.hasPending()) {
        doc["pendingFlag"] = flagName(f1State.flags.pendingFlag());
        doc["pendingInMs"] = f1State.flags.pendingInMs(millis(), config.delay_ms);
    }

    String out;
    serializeJson(doc, out);
    return out;
}

String F1WebServer::buildDebugSystemJson() {
    JsonDocument doc;

    doc["freeHeap"]    = ESP.getFreeHeap();
    doc["minFreeHeap"] = ESP.getMinFreeHeap();
    doc["chipModel"]   = ESP.getChipModel();
    doc["cpuFreqMHz"]  = ESP.getCpuFreqMHz();
    doc["sdkVersion"]  = ESP.getSdkVersion();
    doc["uptimeMs"]    = millis();

    JsonObject wifi    = doc["wifi"].to<JsonObject>();
    wifi["ssid"]       = WiFi.SSID();
    wifi["bssid"]      = WiFi.BSSIDstr();
    wifi["channel"]    = WiFi.channel();
    wifi["ip"]         = WiFi.localIP().toString();
    wifi["gateway"]    = WiFi.gatewayIP().toString();
    wifi["subnet"]     = WiFi.subnetMask().toString();
    wifi["dns"]        = WiFi.dnsIP().toString();
    wifi["rssi"]       = WiFi.RSSI();
    wifi["txPower"]    = WiFi.getTxPower();

    String out;
    serializeJson(doc, out);
    return out;
}

String F1WebServer::buildDebugLiveJson() {
    JsonDocument doc;

    // Relay link. reconnectDelayMs is the current backoff interval, which
    // escalates while disconnected; freshAgeMs is the age of the last message
    // the relay vouched for (stale == false).
    JsonObject rl           = doc["relay"].to<JsonObject>();
    rl["state"]             = relay.getStateStr();
    rl["reconnectDelayMs"]  = relay.getReconnectDelayMs();
    rl["lastMessageAgoMs"]  = relay.getLastMessageAgoMs();
    rl["freshAgeMs"]        = relay.getFreshAgeMs();
    rl["stale"]             = relay.isStale();

    // F1 State
    JsonObject f1         = doc["f1"].to<JsonObject>();
    f1["flag"]            = flagName(f1State.flags.current());
    f1["queueDepth"]      = f1State.flags.depth();
    f1["trackStatusRaw"]  = f1State.trackStatusRaw;
    f1["sessionType"]     = f1State.sessionType.length() ? f1State.sessionType : "—";
    f1["sessionStatus"]   = sessionStatusName(f1State.sessionStatus);
    f1["lastQueuedFlag"]  = flagName(f1State.flags.lastQueued());

    // LED
    JsonObject led        = doc["led"].to<JsonObject>();
    led["animation"]      = flagName(leds.getState());
    led["brightness"]     = config.brightness;
    led["ledCount"]       = config.led_count;
    led["dataPin"]        = LED_DATA_PIN;

    // Override
    JsonObject ovr        = doc["override"].to<JsonObject>();
    ovr["active"]         = _flagOverride;
    ovr["flag"]           = _flagOverride ? flagName(_overrideFlag) : "";

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
        doc["relay_url"]  = config.relay_url;
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

            // Validate the relay URL before applying anything, so a rejected
            // request can't leave other fields half-applied to the hardware.
            const bool hasRelayUrl = doc["relay_url"].is<const char*>();
            String relayUrl;
            if (hasRelayUrl) {
                relayUrl = String(doc["relay_url"].as<const char*>());
                relayUrl.trim();
                RelayUrl parsed;
                if (relayUrl.length() > 0 && !parseRelayUrl(relayUrl.c_str(), parsed)) {
                    req->send(400, "application/json",
                              "{\"error\":\"relay_url must be ws://host[:port][/path] "
                              "or wss://host[:port][/path]\"}");
                    return;
                }
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
            if (hasRelayUrl && relayUrl != config.relay_url) {
                config.relay_url = relayUrl;
                changed = true;
                extern void onRelayConfigChanged();
                onRelayConfigChanged();
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

    // ── GET /api/debug/system — static system & WiFi info (called once) ─────
    _server.on("/api/debug/system", HTTP_GET, [this](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildDebugSystemJson());
    });

    // ── GET /api/debug/live — fast-changing state (polled) ───────────────────
    _server.on("/api/debug/live", HTTP_GET, [this](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildDebugLiveJson());
    });

    // ── POST /api/flag/override — manual animation trigger ───────────────────
    _server.on("/api/flag/override", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len,
               size_t index, size_t total) {

            JsonDocument doc;
            if (deserializeJson(doc, (const char*)data, len)) {
                req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
                return;
            }

            const char* flag = doc["flag"] | "";

            // "LIVE" or empty → release override
            if (strcmp(flag, "LIVE") == 0 || strlen(flag) == 0) {
                _flagOverride = false;
                req->send(200, "application/json", "{\"ok\":true,\"override\":false}");
                return;
            }

            _overrideFlag = flagFromName(flag);
            _flagOverride = true;

            JsonDocument resp;
            resp["ok"]       = true;
            resp["override"] = true;
            resp["flag"]     = flagName(_overrideFlag);
            String out;
            serializeJson(resp, out);
            req->send(200, "application/json", out);
        }
    );

    // ── Silence favicon requests (browsers always request this) ─────────────
    _server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(204);
    });

    // ── Static files from LittleFS (after API routes to avoid unnecessary lookups)
    _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

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
