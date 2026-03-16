#include "ROController.h"
#include "config.h"
#include "esp_task_wdt.h"
#include <Preferences.h>

// =============================================================================
// ISR — IRAM, minimal work only.
// =============================================================================
static volatile bool s_leakIrqFired = false;

void IRAM_ATTR leakISR() {
    s_leakIrqFired = true;
}

// =============================================================================
// Constructor
// =============================================================================
ROController::ROController()
    : _lcd(Config::LCD_I2C_ADDR, 20, 4)
    , _state(SystemState::STANDBY)
    , _leakDetected(false)
    , _startTime(0)
    , _lastLoopTime(0)
    , _productionTime(0)
    , _productionTotal(0)
    , _lastFlushTime(0)
    , _flushStartTime(0)
    , _flushDuration(0)
    , _lastStandbyStart(0)
    , _nextInactivityFlushMs(0)
    , _startupFlushDone(false)
    , _sourceWater(false)
    , _faucetOpen(false)
    , _currentTds(0)
    , _flushCycles(0)
    , _wifiReconnects(0)
    , _wifiConnected(false)
    , _snapshotMutex(nullptr)
    , _cmdQueue(nullptr)
{
    memset(_lcdLines,    ' ', sizeof(_lcdLines));
    memset(&_snapshot,    0,   sizeof(_snapshot));
    for (int i = 0; i < 4; i++) _lcdLines[i][20] = '\0';
    _flushReason[0] = '\0';
    _wifiIp[0]      = '\0';
}

// =============================================================================
// begin()
// =============================================================================
void ROController::begin() {
    _snapshotMutex = xSemaphoreCreateMutex();
    _cmdQueue      = xQueueCreate(8, sizeof(ControlCmd));

    _initHardware();
    _initWdt();
    _loadCounters();

    _startTime        = millis();
    _lastLoopTime     = millis();
    _lastStandbyStart = millis();
    _scheduleNextInactivityFlush();

    _updateDisplay("RO SYSTEM v2.0", "Initializing...", "Sensors: OK", "Ready.");
    Serial.println("[ROC] Initialized");
}

void ROController::_initHardware() {
    pinMode(Config::PIN_PUMP,        OUTPUT); digitalWrite(Config::PIN_PUMP,        LOW);
    pinMode(Config::PIN_INLET_VALVE, OUTPUT); digitalWrite(Config::PIN_INLET_VALVE, LOW);
    pinMode(Config::PIN_FLUSH_VALVE, OUTPUT); digitalWrite(Config::PIN_FLUSH_VALVE, LOW);

    pinMode(Config::PIN_LOW_PRESSURE,  INPUT_PULLUP);
    pinMode(Config::PIN_HIGH_PRESSURE, INPUT_PULLUP);
    pinMode(Config::PIN_LEAK_SENSOR,   INPUT_PULLUP);

    analogSetAttenuation(ADC_11db);

    Wire.begin(Config::PIN_LCD_SDA, Config::PIN_LCD_SCL);
    _lcd.init();
    _lcd.backlight();

    _alarms.begin();

    attachInterrupt(digitalPinToInterrupt(Config::PIN_LEAK_SENSOR), leakISR, FALLING);
    Serial.println("[ROC] Hardware initialized");
}

void ROController::_initWdt() {
    esp_task_wdt_init(Config::WATCHDOG_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);
    Serial.printf("[ROC] WDT set to %us\n", Config::WATCHDOG_TIMEOUT_S);
}

// =============================================================================
// update() — state machine tick
// =============================================================================
void ROController::update() {
    esp_task_wdt_reset();
    _alarms.update();

    // ------------------------------------------------------------------
    // 1. LEAK CHECK
    // ------------------------------------------------------------------
    if (s_leakIrqFired || digitalRead(Config::PIN_LEAK_SENSOR) == LOW) {
        if (!_leakDetected) {
            _leakDetected = true;
            _state = SystemState::EMERGENCY;
            digitalWrite(Config::PIN_PUMP,        LOW);
            digitalWrite(Config::PIN_INLET_VALVE, LOW);
            digitalWrite(Config::PIN_FLUSH_VALVE, LOW);
            Serial.println("[ROC] LEAK DETECTED — emergency shutdown");
        }
    }

    if (_state == SystemState::EMERGENCY) {
        // Drain queue — discard all pending commands
        ControlCmd discard;
        while (xQueueReceive(_cmdQueue, &discard, 0) == pdTRUE) {}

        _alarms.triggerAlarm(AlarmId::LEAK);
        _updateDisplay("!! LEAK DETECTED !!",
                       "ALL FLOW STOPPED",
                       "Check system and",
                       "power-cycle to reset");
        _updateSnapshot();
        return;
    }

    // ------------------------------------------------------------------
    // 2. PROCESS PENDING WEB COMMANDS
    // ------------------------------------------------------------------
    ControlCmd cmd;
    while (xQueueReceive(_cmdQueue, &cmd, 0) == pdTRUE) {
        _processCommand(cmd);
    }

    // ------------------------------------------------------------------
    // 3. TIMING
    // ------------------------------------------------------------------
    uint32_t now   = millis();
    uint32_t delta = now - _lastLoopTime;
    _lastLoopTime  = now;

    // ------------------------------------------------------------------
    // 4. SENSOR READS
    // ------------------------------------------------------------------
    _sourceWater = (digitalRead(Config::PIN_LOW_PRESSURE)  == LOW);
    _faucetOpen  = (digitalRead(Config::PIN_HIGH_PRESSURE) == LOW);
    _currentTds  = _readTds();

    // ------------------------------------------------------------------
    // 5. PRESSURE / TDS ALARMS
    // ------------------------------------------------------------------
    if (!_sourceWater) _alarms.triggerAlarm(AlarmId::LOW_PRESSURE);
    else               _alarms.clearAlarm(AlarmId::LOW_PRESSURE);

    if      (_currentTds > Config::TDS_THRESHOLD)       _alarms.triggerAlarm(AlarmId::TDS_HIGH);
    else if (_currentTds < Config::TDS_THRESHOLD_CLEAR) _alarms.clearAlarm(AlarmId::TDS_HIGH);
    // else: within the dead band — hold current alarm state

    // ------------------------------------------------------------------
    // 6. MAINTENANCE MODE
    // ------------------------------------------------------------------
    if (_state == SystemState::MAINTENANCE) {
        char l2[21], l3[21];
        snprintf(l2, sizeof(l2), "P:%s IV:%s FV:%s",
            digitalRead(Config::PIN_PUMP)        ? "ON " : "OFF",
            digitalRead(Config::PIN_INLET_VALVE) ? "ON " : "OFF",
            digitalRead(Config::PIN_FLUSH_VALVE) ? "ON " : "OFF");
        snprintf(l3, sizeof(l3), "TDS: %d PPM", _currentTds);
        _updateDisplay("STATUS: MAINTENANCE", l2, l3,
                       _wifiConnected ? "WiFi: ON" : "WiFi: OFF");
        _updateSnapshot();
        return;
    }

    // ------------------------------------------------------------------
    // 7. FLUSH LOGIC
    // ------------------------------------------------------------------

    // 7a. Startup flush
    if (!_startupFlushDone && _sourceWater) {
        _startupFlushDone = true;
        _startFlush("Startup", Config::FLUSH_STARTUP_DURATION_MS);
        _updateSnapshot();
        return;
    }

    // 7b. Active flush
    if (_state == SystemState::FLUSHING) {
        uint32_t remaining = _flushRemainMs();
        char l2[21], l3[21];
        snprintf(l2, sizeof(l2), "Reason: %.13s", _flushReason);
        snprintf(l3, sizeof(l3), "Rem:%3us Tot:%3us",
                 remaining / 1000, _flushDuration / 1000);
        _updateDisplay("STATUS: FLUSHING", l2, l3,
                       _wifiConnected ? "WiFi: ON" : "WiFi: OFF");
        if (remaining == 0) _stopFlush();
        _updateSnapshot();
        return;
    }

    // 7c. Inactivity flush
    if (_state == SystemState::STANDBY && _sourceWater) {
        if ((now - _lastStandbyStart) >= _nextInactivityFlushMs) {
            _startFlush("Inactivity", Config::FLUSH_INACTIVITY_DURATION_MS);
            _updateSnapshot();
            return;
        }
    }

    // 7d. Production-interval flush
    if (Config::FLUSH_PRODUCTION_INTERVAL_MS > 0
            && _state == SystemState::STANDBY
            && _sourceWater
            && (now - _lastFlushTime) >= Config::FLUSH_PRODUCTION_INTERVAL_MS) {
        _startFlush("Production", Config::FLUSH_PRODUCTION_DURATION_MS);
        _updateSnapshot();
        return;
    }

    // ------------------------------------------------------------------
    // 8. PRODUCTION LOGIC
    // ------------------------------------------------------------------
    if (_sourceWater && _faucetOpen) {
        if (_state != SystemState::RUNNING) {
            Serial.println("[ROC] Starting production");
            _updateDisplay("STATUS: STARTING", "Opening Inlet...", "", "");
            digitalWrite(Config::PIN_INLET_VALVE, HIGH);
            delay(1000);
            digitalWrite(Config::PIN_PUMP, HIGH);
            _state = SystemState::RUNNING;
        }
        _productionTime += delta;
    } else {
        if (_state == SystemState::RUNNING) {
            Serial.println("[ROC] Stopping production");
            digitalWrite(Config::PIN_PUMP, LOW);
            delay(1000);
            digitalWrite(Config::PIN_INLET_VALVE, LOW);
            _productionTotal    += _productionTime;
            _productionTime      = 0;
            _state               = SystemState::STANDBY;
            _lastStandbyStart    = millis();
            _saveCounters();
        }
    }

    // ------------------------------------------------------------------
    // 9. DISPLAY REFRESH
    // ------------------------------------------------------------------
    if (_state == SystemState::RUNNING) {
        uint32_t secs = _productionTime / 1000;
        char l2[21], l3[21];
        snprintf(l2, sizeof(l2), "TDS: %d PPM", _currentTds);
        snprintf(l3, sizeof(l3), "Time: %um %02us",
                 (unsigned)(secs / 60), (unsigned)(secs % 60));
        _updateDisplay("STATUS: RUNNING", l2, l3,
                       _wifiConnected ? "WiFi: ON" : "WiFi: OFF");
    } else {
        char l4[21];
        snprintf(l4, sizeof(l4), "Last TDS: %dPPM", _currentTds);
        _updateDisplay("STATUS: STANDBY", "System Ready",
                       "Waiting for Faucet", l4);
    }

    _updateSnapshot();
}

// =============================================================================
// Snapshot helpers
// =============================================================================
void ROController::_updateSnapshot() {
    if (xSemaphoreTake(_snapshotMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    _snapshot.state           = _state;
    _snapshot.sourceWater     = _sourceWater;
    _snapshot.faucetOpen      = _faucetOpen;
    _snapshot.tdsPpm          = _currentTds;
    _snapshot.wifiConnected   = _wifiConnected;
    strlcpy(_snapshot.wifiIp, _wifiIp, sizeof(_snapshot.wifiIp));
    _snapshot.uptimeS         = (millis() - _startTime) / 1000;
    _snapshot.productionS     = _productionTime / 1000;
    strlcpy(_snapshot.flushReason, _flushReason, sizeof(_snapshot.flushReason));
    _snapshot.flushRemainingS = _flushRemainMs() / 1000;
    _snapshot.flushDurationS  = _flushDuration / 1000;
    _snapshot.pump            = digitalRead(Config::PIN_PUMP);
    _snapshot.inletValve      = digitalRead(Config::PIN_INLET_VALVE);
    _snapshot.flushValve      = digitalRead(Config::PIN_FLUSH_VALVE);
    _snapshot.lps             = digitalRead(Config::PIN_LOW_PRESSURE);
    _snapshot.hps             = digitalRead(Config::PIN_HIGH_PRESSURE);
    _snapshot.leakDetected    = _leakDetected;
    _snapshot.flushCyclesTotal = _flushCycles;
    _snapshot.productionTotalS = (_productionTotal + _productionTime) / 1000;
    _snapshot.wifiReconnects  = _wifiReconnects;
    {
        uint32_t standbyMs = millis() - _lastStandbyStart;
        _snapshot.timeToFlushS = (_nextInactivityFlushMs > standbyMs)
                                     ? (_nextInactivityFlushMs - standbyMs) / 1000
                                     : 0;
    }
    for (int i = 0; i < 4; i++) {
        strlcpy(_snapshot.lcdLines[i], _lcdLines[i], sizeof(_snapshot.lcdLines[i]));
    }

    xSemaphoreGive(_snapshotMutex);
}

StatusSnapshot ROController::getStatusSnapshot() {
    StatusSnapshot snap{};
    if (xSemaphoreTake(_snapshotMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snap = _snapshot;
        xSemaphoreGive(_snapshotMutex);
    }
    return snap;
}

// =============================================================================
// Command queue
// =============================================================================
bool ROController::postCommand(const ControlCmd& cmd) {
    return xQueueSend(_cmdQueue, &cmd, 0) == pdTRUE;
}

void ROController::_processCommand(const ControlCmd& cmd) {
    switch (cmd.type) {
        case ControlCmd::Type::MAINTENANCE_TOGGLE:
            if (_state == SystemState::MAINTENANCE) _exitMaintenance();
            else                                    _enterMaintenance();
            break;

        case ControlCmd::Type::MANUAL_FLUSH:
            if (_state == SystemState::STANDBY) {
                _startFlush("Manual", Config::FLUSH_MANUAL_DURATION_MS);
            }
            break;

        case ControlCmd::Type::RELAY:
            if (_state == SystemState::MAINTENANCE) {
                if      (strcmp(cmd.action, "pump_on")         == 0) digitalWrite(Config::PIN_PUMP,        HIGH);
                else if (strcmp(cmd.action, "pump_off")        == 0) digitalWrite(Config::PIN_PUMP,        LOW);
                else if (strcmp(cmd.action, "inlet_on")        == 0) digitalWrite(Config::PIN_INLET_VALVE, HIGH);
                else if (strcmp(cmd.action, "inlet_off")       == 0) digitalWrite(Config::PIN_INLET_VALVE, LOW);
                else if (strcmp(cmd.action, "flush_valve_on")  == 0) digitalWrite(Config::PIN_FLUSH_VALVE, HIGH);
                else if (strcmp(cmd.action, "flush_valve_off") == 0) digitalWrite(Config::PIN_FLUSH_VALVE, LOW);
            }
            break;

        case ControlCmd::Type::RESET:
            esp_restart();
            break;
    }
}

// =============================================================================
// Validation helpers (static — safe to call from network task)
// =============================================================================
bool ROController::validateMaintenanceToggle(const StatusSnapshot& s,
                                              char* err, size_t n) {
    if (s.state == SystemState::EMERGENCY) {
        strlcpy(err, "Cannot toggle maintenance during emergency", n);
        return false;
    }
    return true;
}

bool ROController::validateManualFlush(const StatusSnapshot& s,
                                        char* err, size_t n) {
    if (s.state != SystemState::STANDBY) {
        strlcpy(err, "Flush only allowed from STANDBY state", n);
        return false;
    }
    return true;
}

bool ROController::validateRelay(const StatusSnapshot& s, const char* action,
                                  char* err, size_t n) {
    if (s.state != SystemState::MAINTENANCE) {
        strlcpy(err, "Relay control only in MAINTENANCE mode", n);
        return false;
    }
    static const char* valid[] = {
        "pump_on", "pump_off", "inlet_on", "inlet_off",
        "flush_valve_on", "flush_valve_off"
    };
    for (auto a : valid) {
        if (strcmp(action, a) == 0) return true;
    }
    snprintf(err, n, "Unknown relay action: %s", action);
    return false;
}

// =============================================================================
// Hardware helpers
// =============================================================================
int ROController::_readTds() const {
    int32_t sum = 0;
    for (int i = 0; i < Config::TDS_SAMPLES; i++) {
        sum += analogRead(Config::PIN_TDS_SENSOR);
    }
    return (int)((sum / Config::TDS_SAMPLES) * Config::TDS_FACTOR
                 + Config::TDS_OFFSET);
}

uint32_t ROController::_flushRemainMs() const {
    if (_state != SystemState::FLUSHING) return 0;
    uint32_t elapsed = millis() - _flushStartTime;
    return (elapsed < _flushDuration) ? (_flushDuration - elapsed) : 0;
}

void ROController::_updateDisplay(const char* l1, const char* l2,
                                   const char* l3, const char* l4) {
    const char* lines[4] = {l1, l2, l3, l4};
    for (int i = 0; i < 4; i++) {
        snprintf(_lcdLines[i], sizeof(_lcdLines[i]), "%-20s", lines[i]);
        _lcd.setCursor(0, i);
        _lcd.print(_lcdLines[i]);
    }
}

// =============================================================================
// NVS persistence
// =============================================================================
void ROController::_loadCounters() {
    Preferences prefs;
    prefs.begin("krosmosis", /*readOnly=*/true);
    _productionTotal = prefs.getULong("prod_ms",  0);
    _flushCycles     = prefs.getULong("flush_n",  0);
    prefs.end();
    Serial.printf("[ROC] Loaded counters — prod: %ums  flushes: %u\n",
                  _productionTotal, _flushCycles);
}

void ROController::_saveCounters() {
    Preferences prefs;
    prefs.begin("krosmosis", /*readOnly=*/false);
    prefs.putULong("prod_ms", _productionTotal);
    prefs.putULong("flush_n", _flushCycles);
    prefs.end();
}

// =============================================================================
// Flush helpers
// =============================================================================
void ROController::_scheduleNextInactivityFlush() {
    _nextInactivityFlushMs = (uint32_t)random(
        (long)Config::FLUSH_INACTIVITY_MIN_MS,
        (long)Config::FLUSH_INACTIVITY_MAX_MS
    );
}

void ROController::_startFlush(const char* reason, uint32_t durationMs) {
    Serial.printf("[ROC] Flush start: %s (%us)\n", reason, durationMs / 1000);
    digitalWrite(Config::PIN_PUMP,        LOW);
    digitalWrite(Config::PIN_INLET_VALVE, LOW);
    delay(500);
    digitalWrite(Config::PIN_FLUSH_VALVE, HIGH);
    digitalWrite(Config::PIN_PUMP,        HIGH);
    strlcpy(_flushReason, reason, sizeof(_flushReason));
    _flushStartTime = millis();
    _flushDuration  = durationMs;
    _lastFlushTime  = _flushStartTime;
    _state          = SystemState::FLUSHING;
}

void ROController::_stopFlush() {
    Serial.println("[ROC] Flush complete");
    digitalWrite(Config::PIN_PUMP,        LOW);
    digitalWrite(Config::PIN_FLUSH_VALVE, LOW);
    _flushCycles++;
    _state            = SystemState::STANDBY;
    _lastStandbyStart = millis();
    _scheduleNextInactivityFlush();
    _saveCounters();
}

// =============================================================================
// Maintenance helpers
// =============================================================================
void ROController::_enterMaintenance() {
    if (_state == SystemState::RUNNING) {
        digitalWrite(Config::PIN_PUMP, LOW);
        delay(1000);
        digitalWrite(Config::PIN_INLET_VALVE, LOW);
        _productionTotal += _productionTime;
        _productionTime   = 0;
        _saveCounters();
    } else if (_state == SystemState::FLUSHING) {
        digitalWrite(Config::PIN_PUMP,        LOW);
        digitalWrite(Config::PIN_FLUSH_VALVE, LOW);
    }
    _state = SystemState::MAINTENANCE;
    Serial.println("[ROC] Entered maintenance");
}

void ROController::_exitMaintenance() {
    _state            = SystemState::STANDBY;
    _lastStandbyStart = millis();
    Serial.println("[ROC] Exited maintenance");
}

// =============================================================================
// Called by network task
// =============================================================================
void ROController::setWifiConnected(bool connected, const char* ip) {
    _wifiConnected = connected;
    if (ip) strlcpy(_wifiIp, ip, sizeof(_wifiIp));
    else     _wifiIp[0] = '\0';
}
