#include "WebInterface.h"
#include "esp_random.h"

// =============================================================================
// Constructor
// =============================================================================
WebInterface::WebInterface(ROController& ctrl)
    : _ctrl(ctrl)
    , _server(Config::WEB_PORT)
    , _authEnabled(strlen(Config::WEB_AUTH_PASSWORD) > 0)
    , _lastReconnectCheck(0)
{
    _sessionToken[0] = '\0';
}

// =============================================================================
// begin() — called once at network task startup
// =============================================================================
void WebInterface::begin() {
    if (!LittleFS.begin(true)) {
        Serial.println("[Net] LittleFS mount failed — static files unavailable");
    } else {
        Serial.println("[Net] LittleFS mounted");
    }

    if (strlen(Config::WIFI_SSID) > 0) {
        _connectWifi();
    } else {
        Serial.println("[Net] No WIFI_SSID configured — network disabled");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Net] WiFi not connected — web server not started");
        return;
    }

    if (Config::WEB_ENABLED) {
        _setupRoutes();
    }
}

// =============================================================================
// loop() — periodic reconnect check
// =============================================================================
void WebInterface::loop() {
    if (strlen(Config::WIFI_SSID) == 0) return;

    uint32_t now = millis();
    if (now - _lastReconnectCheck < Config::WIFI_RECONNECT_INTERVAL_MS) return;
    _lastReconnectCheck = now;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Net] WiFi lost — reconnecting");
        _ctrl.incWifiReconnects();
        _ctrl.setWifiConnected(false, "");
        _connectWifi();
    }
}

// =============================================================================
// WiFi
// =============================================================================
void WebInterface::_connectWifi() {
    WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);
    Serial.printf("[Net] Connecting to \"%s\"...\n", Config::WIFI_SSID);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > Config::WIFI_TIMEOUT_MS) {
            Serial.println("[Net] WiFi connection timed out");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    String ip = WiFi.localIP().toString();
    Serial.printf("[Net] WiFi connected — IP %s\n", ip.c_str());
    _ctrl.setWifiConnected(true, ip.c_str());
}

// =============================================================================
// Route setup
// =============================================================================
void WebInterface::_setupRoutes() {
    // Static files served directly from LittleFS (no auth — CSS/JS are safe)
    _server.serveStatic("/static/", LittleFS, "/static/");

    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleRoot(req);
    });

    _server.on("/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleStatus(req);
    });

    // POST /control — body is application/x-www-form-urlencoded
    _server.on("/control", HTTP_POST, [this](AsyncWebServerRequest* req) {
        _handleControl(req);
    });

    _server.on("/login", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleLoginGet(req);
    });

    _server.on("/login", HTTP_POST, [this](AsyncWebServerRequest* req) {
        _handleLoginPost(req);
    });

    _server.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not Found");
    });

    _server.begin();
    Serial.printf("[Net] Web server started on port %u\n", Config::WEB_PORT);
    if (_authEnabled) {
        Serial.println("[Net] Password auth enabled");
    }
}

// =============================================================================
// Auth helpers
// =============================================================================
void WebInterface::_generateToken() {
    uint32_t r[4];
    for (int i = 0; i < 4; i++) r[i] = esp_random();
    snprintf(_sessionToken, sizeof(_sessionToken),
             "%08x%08x%08x%08x", r[0], r[1], r[2], r[3]);
}

bool WebInterface::_checkAuth(AsyncWebServerRequest* req) const {
    if (!_authEnabled) return true;
    if (_sessionToken[0] == '\0') return false;
    if (!req->hasHeader("Cookie")) return false;
    String cookie = req->header("Cookie");
    return cookie.indexOf(String("krosmosis_token=") + _sessionToken) >= 0;
}

void WebInterface::_sendJson(AsyncWebServerRequest* req, int code,
                              bool ok, const char* msg) const {
    // Small fixed JSON — avoid heap allocation via ArduinoJson for this case
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"message\":\"%s\"}",
             ok ? "true" : "false", msg);
    req->send(code, "application/json", buf);
}

// =============================================================================
// Route handlers
// =============================================================================
void WebInterface::_handleRoot(AsyncWebServerRequest* req) {
    if (!_checkAuth(req)) { req->redirect("/login"); return; }
    req->send(LittleFS, "/index.html", "text/html");
}

void WebInterface::_handleLoginGet(AsyncWebServerRequest* req) {
    req->send(LittleFS, "/login.html", "text/html");
}

void WebInterface::_handleLoginPost(AsyncWebServerRequest* req) {
    String pw = req->hasParam("password", true)
                    ? req->getParam("password", true)->value()
                    : "";

    if (_authEnabled && pw == Config::WEB_AUTH_PASSWORD) {
        _generateToken();
        AsyncWebServerResponse* resp = req->beginResponse(303);
        resp->addHeader("Location", "/");
        resp->addHeader("Set-Cookie",
                        String("krosmosis_token=") + _sessionToken
                        + "; HttpOnly; SameSite=Strict");
        req->send(resp);
    } else if (!_authEnabled) {
        req->redirect("/");
    } else {
        req->redirect("/login?error=1");
    }
}

void WebInterface::_handleStatus(AsyncWebServerRequest* req) {
    if (!_checkAuth(req)) {
        req->send(401, "application/json",
                  "{\"ok\":false,\"message\":\"Unauthorized\"}");
        return;
    }

    StatusSnapshot s = _ctrl.getStatusSnapshot();

    JsonDocument doc;
    doc["state"]              = stateName(s.state);
    doc["state_id"]           = (int)s.state;
    doc["source_water"]       = s.sourceWater;
    doc["faucet_open"]        = s.faucetOpen;
    doc["tds_ppm"]            = s.tdsPpm;
    doc["wifi_connected"]     = s.wifiConnected;
    doc["uptime_s"]           = s.uptimeS;
    doc["production_time_s"]  = s.productionS;
    doc["flush_reason"]       = s.flushReason;
    doc["flush_remaining_s"]  = s.flushRemainingS;
    doc["flush_duration_s"]   = s.flushDurationS;
    doc["pump"]               = s.pump;
    doc["inlet_valve"]        = s.inletValve;
    doc["flush_valve"]        = s.flushValve;
    doc["lps"]                = s.lps;
    doc["hps"]                = s.hps;
    doc["leak_detected"]      = s.leakDetected;
    doc["flush_cycles_total"] = s.flushCyclesTotal;
    doc["production_total_s"] = s.productionTotalS;
    doc["ip"]                 = s.wifiIp;

    JsonArray lcd = doc["lcd"].to<JsonArray>();
    for (int i = 0; i < 4; i++) lcd.add(s.lcdLines[i]);

    String body;
    serializeJson(doc, body);
    req->send(200, "application/json", body);
}

void WebInterface::_handleControl(AsyncWebServerRequest* req) {
    if (!_checkAuth(req)) {
        _sendJson(req, 401, false, "Unauthorized");
        return;
    }

    if (!req->hasParam("action", true)) {
        _sendJson(req, 400, false, "Missing action");
        return;
    }

    String action = req->getParam("action", true)->value();
    StatusSnapshot snap = _ctrl.getStatusSnapshot();
    char err[64] = "";
    ControlCmd cmd{};

    if (action == "maintenance_toggle") {
        if (!ROController::validateMaintenanceToggle(snap, err, sizeof(err))) {
            _sendJson(req, 400, false, err);
            return;
        }
        cmd.type = ControlCmd::Type::MAINTENANCE_TOGGLE;

    } else if (action == "flush_start") {
        if (!ROController::validateManualFlush(snap, err, sizeof(err))) {
            _sendJson(req, 400, false, err);
            return;
        }
        cmd.type = ControlCmd::Type::MANUAL_FLUSH;

    } else if (action == "reset") {
        cmd.type = ControlCmd::Type::RESET;

    } else {
        // Relay actions: pump_on/off, inlet_on/off, flush_valve_on/off
        if (!ROController::validateRelay(snap, action.c_str(), err, sizeof(err))) {
            _sendJson(req, 400, false, err);
            return;
        }
        cmd.type = ControlCmd::Type::RELAY;
        strlcpy(cmd.action, action.c_str(), sizeof(cmd.action));
    }

    if (!_ctrl.postCommand(cmd)) {
        _sendJson(req, 503, false, "Command queue full");
        return;
    }

    _sendJson(req, 200, true, "OK");
}
