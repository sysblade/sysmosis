#pragma once

// =============================================================================
// WebInterface — WiFi management + async web server.
//
// Runs entirely on Core 0 in the network task.
// Reads controller state via ROController::getStatusSnapshot() (mutex-protected).
// Sends control actions via ROController::postCommand() (queue-based).
//
// Routes (match the MicroPython webserver.py exactly):
//   GET  /           → index.html (requires auth)
//   GET  /status     → JSON status (requires auth)
//   POST /control    → control action (requires auth)
//   GET  /login      → login.html
//   POST /login      → password check, set session cookie
//   GET  /static/*   → static files from LittleFS
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>

#include "ROController.h"
#include "config.h"

class WebInterface {
public:
    explicit WebInterface(ROController& ctrl);

    void begin();   // connect WiFi, mount LittleFS, start servers — call from task
    void loop();    // periodic reconnect check — call from task loop

private:
    ROController&  _ctrl;
    AsyncWebServer _server;
    AsyncWebServer _metricsServer;

    char     _sessionToken[33];   // 32 hex chars + NUL, empty = no session
    bool     _authEnabled;
    uint32_t _lastReconnectCheck;

    void _connectWifi();
    void _setupRoutes();
    void _setupMetricsRoutes();
    void _setupOta();
    void _generateToken();

    bool   _checkAuth(AsyncWebServerRequest* req) const;
    void   _sendJson(AsyncWebServerRequest* req, int code,
                     bool ok, const char* msg) const;
    String _generateMetrics(const StatusSnapshot& s) const;

    // Web UI route handlers
    void _handleRoot(AsyncWebServerRequest* req);
    void _handleStatus(AsyncWebServerRequest* req);
    void _handleControl(AsyncWebServerRequest* req);
    void _handleLoginGet(AsyncWebServerRequest* req);
    void _handleLoginPost(AsyncWebServerRequest* req);
};
