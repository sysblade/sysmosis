#include "WebInterface.h"
#include "esp_random.h"

// =============================================================================
// Constructor
// =============================================================================
WebInterface::WebInterface(ROController& ctrl)
    : _ctrl(ctrl)
    , _server(Config::WEB_PORT)
    , _metricsServer(Config::METRICS_PORT)
    , _authEnabled(strlen(Config::WEB_AUTH_PASSWORD) > 0)
    , _serversStarted(false)
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

    if (strlen(Config::WIFI_SSID) == 0) {
        Serial.println("[Net] No WIFI_SSID configured — network disabled");
        return;
    }

    _connectWifi();

    if (WiFi.status() == WL_CONNECTED) {
        _startServers();
    } else {
        // Servers will be started by loop() on the first successful reconnect
        Serial.println("[Net] Initial WiFi connect failed — will retry");
    }
}

// =============================================================================
// loop() — periodic reconnect check
// =============================================================================
void WebInterface::loop() {
    if (strlen(Config::WIFI_SSID) == 0) return;

    // OTA must be polled every iteration — blocks during an active upload
    if (Config::OTA_ENABLED && _serversStarted) {
        ArduinoOTA.handle();
    }

    uint32_t now = millis();
    if (now - _lastReconnectCheck < Config::WIFI_RECONNECT_INTERVAL_MS) return;
    _lastReconnectCheck = now;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Net] WiFi lost — reconnecting");
        _ctrl.incWifiReconnects();
        _ctrl.setWifiConnected(false, "");
        _connectWifi();

        // Start servers on the first ever successful connect (initial attempt
        // may have timed out — this is the recovery path)
        if (!_serversStarted && WiFi.status() == WL_CONNECTED) {
            _startServers();
        }
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
// Server startup (called once after first successful WiFi connect)
// =============================================================================
void WebInterface::_startServers() {
    if (Config::WEB_ENABLED)     _setupRoutes();
    if (Config::METRICS_ENABLED) _setupMetricsRoutes();
    if (Config::OTA_ENABLED)     _setupOta();
    _serversStarted = true;
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
    doc["time_to_flush_s"]    = s.timeToFlushS;
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

// =============================================================================
// OTA
// =============================================================================
void WebInterface::_setupOta() {
    ArduinoOTA.setHostname(Config::OTA_HOSTNAME);

    if (strlen(Config::OTA_PASSWORD) > 0) {
        ArduinoOTA.setPassword(Config::OTA_PASSWORD);
    }

    // Before flashing: cut all flow immediately.
    // We're about to reboot so the control task's relay management is moot;
    // this ensures valves are closed even if the update only takes a few seconds.
    ArduinoOTA.onStart([this]() {
        const char* kind = (ArduinoOTA.getCommand() == U_FLASH)
                               ? "firmware" : "filesystem";
        Serial.printf("[OTA] Starting %s update\n", kind);

        // Direct GPIO writes — safe from any task, no delay needed
        digitalWrite(Config::PIN_PUMP,        LOW);
        digitalWrite(Config::PIN_INLET_VALVE, LOW);
        digitalWrite(Config::PIN_FLUSH_VALVE, LOW);
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("[OTA] Complete — rebooting");
    });

    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        // Print only at 10% increments to avoid flooding serial
        static unsigned int lastPct = 0;
        unsigned int pct = (done * 100) / total;
        if (pct / 10 != lastPct / 10) {
            lastPct = pct;
            Serial.printf("[OTA] %u%%\n", pct);
        }
    });

    ArduinoOTA.onError([](ota_error_t err) {
        const char* reason = "unknown";
        switch (err) {
            case OTA_AUTH_ERROR:    reason = "auth failed";      break;
            case OTA_BEGIN_ERROR:   reason = "begin failed";     break;
            case OTA_CONNECT_ERROR: reason = "connect failed";   break;
            case OTA_RECEIVE_ERROR: reason = "receive failed";   break;
            case OTA_END_ERROR:     reason = "end failed";       break;
        }
        Serial.printf("[OTA] Error: %s\n", reason);
    });

    ArduinoOTA.begin();
    Serial.printf("[Net] OTA ready — hostname \"%s\"\n", Config::OTA_HOSTNAME);
}

// =============================================================================
// Prometheus metrics
// =============================================================================
void WebInterface::_setupMetricsRoutes() {
    _metricsServer.on("/metrics", HTTP_GET, [this](AsyncWebServerRequest* req) {
        StatusSnapshot s = _ctrl.getStatusSnapshot();
        String body = _generateMetrics(s);
        req->send(200, "text/plain; version=0.0.4; charset=utf-8", body);
    });

    // Redirect bare "/" to /metrics for convenience
    _metricsServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->redirect("/metrics");
    });

    _metricsServer.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not Found");
    });

    _metricsServer.begin();
    Serial.printf("[Net] Metrics server started on port %u\n", Config::METRICS_PORT);
}

// Builds Prometheus text format — mirrors metrics.py _format_metrics() exactly.
String WebInterface::_generateMetrics(const StatusSnapshot& s) const {
    String out;
    out.reserve(1400);  // avoid reallocations; typical payload ~1 KB

    // Helper lambdas for the two metric shapes
    auto gauge = [&](const char* name, const char* help, uint32_t value) {
        out += "# HELP "; out += name; out += ' '; out += help; out += '\n';
        out += "# TYPE "; out += name; out += " gauge\n";
        out += name; out += ' '; out += value; out += '\n';
    };
    auto counter = [&](const char* name, const char* help, uint32_t value) {
        out += "# HELP "; out += name; out += ' '; out += help; out += '\n';
        out += "# TYPE "; out += name; out += " counter\n";
        out += name; out += ' '; out += value; out += '\n';
    };
    auto labeled = [&](const char* name, const char* label,
                        const char* lvalue, uint32_t value) {
        out += name;
        out += '{'; out += label; out += "=\""; out += lvalue; out += "\"} ";
        out += value; out += '\n';
    };

    // krosmosis_info
    out += "# HELP krosmosis_info System information\n";
    out += "# TYPE krosmosis_info gauge\n";
    out += "krosmosis_info{version=\"2.0\"} 1\n";

    // krosmosis_uptime_seconds
    counter("krosmosis_uptime_seconds", "System uptime in seconds", s.uptimeS);

    // krosmosis_system_state (labeled)
    out += "# HELP krosmosis_system_state Current system state\n";
    out += "# TYPE krosmosis_system_state gauge\n";
    labeled("krosmosis_system_state", "state", "standby",
            s.state == SystemState::STANDBY     ? 1 : 0);
    labeled("krosmosis_system_state", "state", "running",
            s.state == SystemState::RUNNING     ? 1 : 0);
    labeled("krosmosis_system_state", "state", "flushing",
            s.state == SystemState::FLUSHING    ? 1 : 0);
    labeled("krosmosis_system_state", "state", "emergency",
            s.state == SystemState::EMERGENCY   ? 1 : 0);
    labeled("krosmosis_system_state", "state", "maintenance",
            s.state == SystemState::MAINTENANCE ? 1 : 0);

    // Sensors
    gauge("krosmosis_tds_ppm",        "Current TDS reading in PPM",  (uint32_t)s.tdsPpm);
    gauge("krosmosis_pressure_low",   "Low pressure sensor state",   s.lps  ? 1 : 0);
    gauge("krosmosis_pressure_high",  "High pressure sensor state",  s.hps  ? 1 : 0);
    gauge("krosmosis_leak_detected",  "Leak sensor state",           s.leakDetected ? 1 : 0);

    // Relays
    gauge("krosmosis_pump_active",         "Pump relay state",         s.pump       ? 1 : 0);
    gauge("krosmosis_inlet_valve_active",  "Inlet valve relay state",  s.inletValve ? 1 : 0);
    gauge("krosmosis_flush_valve_active",  "Flush valve relay state",  s.flushValve ? 1 : 0);

    // Production
    gauge("krosmosis_production_seconds",
          "Current cycle production time in seconds", s.productionS);
    counter("krosmosis_production_total_seconds",
            "Total cumulative production time in seconds", s.productionTotalS);

    // Flush
    counter("krosmosis_flush_cycles_total",
            "Number of flush cycles completed", s.flushCyclesTotal);
    gauge("krosmosis_time_to_flush_seconds",
          "Seconds until next inactivity flush", s.timeToFlushS);

    // WiFi
    gauge("krosmosis_wifi_connected",
          "WiFi connection state", s.wifiConnected ? 1 : 0);
    counter("krosmosis_wifi_reconnects_total",
            "Number of WiFi reconnections", s.wifiReconnects);

    return out;
}
