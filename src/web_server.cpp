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
    rl["lastDisconnect"]    = relay.getLastDisconnectReason();

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

// Body handlers are called once per TCP chunk. Every body this API accepts
// fits in one, so anything larger is refused rather than parsed piecemeal —
// parsing each chunk separately would send one response per chunk.
// Returns true when `data` holds the complete body and parsing may proceed.
static bool wholeBody(AsyncWebServerRequest* req, size_t index, size_t len, size_t total) {
    if (index + len != total) return false;          // more chunks coming
    if (index != 0) {                                 // this is the last of several
        req->send(413, "application/json", "{\"error\":\"body too large\"}");
        return false;
    }
    return true;
}

// ── Startup ───────────────────────────────────────────────────────────────────

void F1WebServer::begin() {
    // A missing or unmounted filesystem (e.g. after a partition-table change
    // without re-running uploadfs) must not take the API down with it — the
    // API is the only way to reconfigure the lamp.
    _fsMounted = LittleFS.begin();
    if (!_fsMounted) Serial.println("[Web] LittleFS mount failed — API only, no dashboard");

    setupRoutes();
    _server.addHandler(&_events);
    _server.begin();
    Serial.println("[Web] Server started on port 80");
}

// State-changing endpoints require a custom request header. A cross-origin
// page can fire a plain POST at http://f1sensor.local without asking, but a
// custom header forces a CORS preflight, which this server never answers.
// Cheap insurance against a random web page rebooting the lamp or wiping
// its WiFi credentials.
static bool requireApiHeader(AsyncWebServerRequest* req) {
    if (req->hasHeader("X-F1-Sensor")) return true;
    req->send(403, "application/json", "{\"error\":\"missing X-F1-Sensor header\"}");
    return false;
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
    // Runs on the async_tcp task. Validates everything up front so a rejected
    // request applies nothing, then stages the accepted fields for loop() to
    // apply (see Config::stage). Nothing here touches hardware, NVS or the
    // live Config fields.
    _server.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest* req) {},   // final handler (unused for body)
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t index, size_t total) {
            if (!wholeBody(req, index, len, total)) return;
            if (!requireApiHeader(req)) return;

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, (const char*)data, len);
            if (err) {
                req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
                return;
            }

            Config::Staged staged;

            if (doc["relay_url"].is<const char*>()) {
                String url = doc["relay_url"].as<const char*>();
                url.trim();
                RelayUrl parsed;
                if (url.length() > 0 && !parseRelayUrl(url.c_str(), parsed)) {
                    req->send(400, "application/json",
                              "{\"error\":\"relay_url must be ws://host[:port][/path] "
                              "or wss://host[:port][/path]\"}");
                    return;
                }
                strlcpy(staged.relay_url, url.c_str(), sizeof(staged.relay_url));
                staged.mask |= Config::RELAY_URL;
            }
            if (doc["led_count"].is<uint16_t>()) {
                staged.led_count = constrain((uint16_t)doc["led_count"], 1, MAX_LEDS);
                staged.mask |= Config::LED_COUNT;
            }
            if (doc["brightness"].is<uint8_t>()) {
                staged.brightness = doc["brightness"];
                staged.mask |= Config::BRIGHTNESS;
            }
            if (doc["delay_ms"].is<uint32_t>()) {
                staged.delay_ms = constrain((uint32_t)doc["delay_ms"], 0UL, Config::DELAY_MS_MAX);
                staged.mask |= Config::DELAY_MS;
            }

            if (staged.mask) config.stage(staged);
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    // ── POST /api/restart ─────────────────────────────────────────────────────
    // Reboot once the response has left, rather than delay()ing on the
    // network task and hoping it got out.
    _server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!requireApiHeader(req)) return;
        req->onDisconnect([]() { ESP.restart(); });
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // ── POST /api/wifi/reset — clears WiFiManager credentials ────────────────
    _server.on("/api/wifi/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!requireApiHeader(req)) return;
        req->onDisconnect([]() {
            WiFi.disconnect(true, true);   // erase saved credentials
            ESP.restart();
        });
        req->send(200, "application/json", "{\"ok\":true}");
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
            if (!wholeBody(req, index, len, total)) return;
            if (!requireApiHeader(req)) return;

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
    if (_fsMounted) {
        _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    }

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
