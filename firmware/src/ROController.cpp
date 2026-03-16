#include "ROController.h"
#include "config.h"
#include "esp_task_wdt.h"

// =============================================================================
// ISR — runs on any core, must be in IRAM, minimal work only.
// Uses a file-scope volatile so the ISR needs no class pointer.
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
{
    memset(_lcdLines, ' ', sizeof(_lcdLines));
    for (int i = 0; i < 4; i++) _lcdLines[i][20] = '\0';
    _flushReason[0] = '\0';
    _wifiIp[0]      = '\0';
}

// =============================================================================
// begin() — hardware init, called once from setup()
// =============================================================================
void ROController::begin() {
    _initHardware();
    _initWdt();

    _startTime        = millis();
    _lastLoopTime     = millis();
    _lastStandbyStart = millis();
    _scheduleNextInactivityFlush();

    _updateDisplay("RO SYSTEM v2.0", "Initializing...", "Sensors: OK", "Ready.");
    Serial.println("[ROC] Initialized");
}

void ROController::_initHardware() {
    // Relay outputs — safe default: all off
    pinMode(Config::PIN_PUMP,        OUTPUT); digitalWrite(Config::PIN_PUMP,        LOW);
    pinMode(Config::PIN_INLET_VALVE, OUTPUT); digitalWrite(Config::PIN_INLET_VALVE, LOW);
    pinMode(Config::PIN_FLUSH_VALVE, OUTPUT); digitalWrite(Config::PIN_FLUSH_VALVE, LOW);

    // Opto-isolated sensor inputs — sensors pull the line LOW when active
    pinMode(Config::PIN_LOW_PRESSURE,  INPUT_PULLUP);
    pinMode(Config::PIN_HIGH_PRESSURE, INPUT_PULLUP);
    pinMode(Config::PIN_LEAK_SENSOR,   INPUT_PULLUP);

    // ADC — 11dB attenuation gives 0-3.3V range on ESP32
    analogSetAttenuation(ADC_11db);

    // I2C + LCD
    Wire.begin(Config::PIN_LCD_SDA, Config::PIN_LCD_SCL);
    _lcd.init();
    _lcd.backlight();

    // Alarm subsystem (Phase 2 will activate buzzer/LED)
    _alarms.begin();

    // Leak sensor ISR — latching, FALLING edge (sensor pulls LOW on leak)
    attachInterrupt(digitalPinToInterrupt(Config::PIN_LEAK_SENSOR), leakISR, FALLING);

    Serial.println("[ROC] Hardware initialized");
}

void ROController::_initWdt() {
    esp_task_wdt_init(Config::WATCHDOG_TIMEOUT_S, /*panic=*/true);
    esp_task_wdt_add(NULL);  // subscribe current task
    Serial.printf("[ROC] WDT set to %us\n", Config::WATCHDOG_TIMEOUT_S);
}

// =============================================================================
// update() — full state machine tick, called every loop()
// =============================================================================
void ROController::update() {
    esp_task_wdt_reset();
    _alarms.update();

    // ------------------------------------------------------------------
    // 1. LEAK CHECK — latching, highest priority
    //    ISR sets s_leakIrqFired; polling is the backup.
    // ------------------------------------------------------------------
    if (s_leakIrqFired || digitalRead(Config::PIN_LEAK_SENSOR) == LOW) {
        if (!_leakDetected) {
            _leakDetected = true;
            _state = SystemState::EMERGENCY;
            // Immediately cut all flow
            digitalWrite(Config::PIN_PUMP,        LOW);
            digitalWrite(Config::PIN_INLET_VALVE, LOW);
            digitalWrite(Config::PIN_FLUSH_VALVE, LOW);
            Serial.println("[ROC] LEAK DETECTED — emergency shutdown");
        }
    }

    if (_state == SystemState::EMERGENCY) {
        _alarms.triggerAlarm(AlarmId::LEAK);
        _updateDisplay(
            "!! LEAK DETECTED !!",
            "ALL FLOW STOPPED",
            "Check system and",
            "power-cycle to reset"
        );
        return;  // nothing else runs until power-cycle
    }

    // ------------------------------------------------------------------
    // 2. TIMING
    // ------------------------------------------------------------------
    uint32_t now   = millis();
    uint32_t delta = now - _lastLoopTime;
    _lastLoopTime  = now;

    // ------------------------------------------------------------------
    // 3. SENSOR READS
    // ------------------------------------------------------------------
    _sourceWater = (digitalRead(Config::PIN_LOW_PRESSURE)  == LOW);  // LOW = pressure present
    _faucetOpen  = (digitalRead(Config::PIN_HIGH_PRESSURE) == LOW);  // LOW = faucet drawing water
    _currentTds  = _readTds();

    // ------------------------------------------------------------------
    // 4. PRESSURE / TDS ALARMS
    // ------------------------------------------------------------------
    if (!_sourceWater) {
        _alarms.triggerAlarm(AlarmId::LOW_PRESSURE);
    } else {
        _alarms.clearAlarm(AlarmId::LOW_PRESSURE);
    }

    if (_currentTds > Config::TDS_THRESHOLD) {
        _alarms.triggerAlarm(AlarmId::TDS_HIGH);
    } else {
        _alarms.clearAlarm(AlarmId::TDS_HIGH);
    }

    // ------------------------------------------------------------------
    // 5. MAINTENANCE MODE — bypass all automation
    // ------------------------------------------------------------------
    if (_state == SystemState::MAINTENANCE) {
        char l2[21], l3[21];
        snprintf(l2, sizeof(l2), "P:%s IV:%s FV:%s",
            digitalRead(Config::PIN_PUMP)        ? "ON " : "OFF",
            digitalRead(Config::PIN_INLET_VALVE) ? "ON " : "OFF",
            digitalRead(Config::PIN_FLUSH_VALVE) ? "ON " : "OFF");
        snprintf(l3, sizeof(l3), "TDS: %d PPM", _currentTds);
        _updateDisplay(
            "STATUS: MAINTENANCE",
            l2, l3,
            _wifiConnected ? "WiFi: ON" : "WiFi: OFF"
        );
        return;
    }

    // ------------------------------------------------------------------
    // 6. FLUSH LOGIC
    // ------------------------------------------------------------------

    // 6a. Startup flush — deferred until source water is available
    if (!_startupFlushDone && _sourceWater) {
        _startupFlushDone = true;
        _startFlush("Startup", Config::FLUSH_STARTUP_DURATION_MS);
        return;
    }

    // 6b. Active flush — update display, check completion
    if (_state == SystemState::FLUSHING) {
        uint32_t elapsed   = now - _flushStartTime;
        uint32_t remaining = (elapsed < _flushDuration) ? (_flushDuration - elapsed) : 0;
        char l3[21];
        snprintf(l3, sizeof(l3), "Rem:%3us Tot:%3us",
                 remaining / 1000, _flushDuration / 1000);
        char l2[21];
        snprintf(l2, sizeof(l2), "Reason: %.13s", _flushReason);
        _updateDisplay("STATUS: FLUSHING", l2, l3,
                       _wifiConnected ? "WiFi: ON" : "WiFi: OFF");
        if (remaining == 0) {
            _stopFlush();
        }
        return;
    }

    // 6c. Inactivity flush — triggered from standby when source water present
    if (_state == SystemState::STANDBY && _sourceWater) {
        uint32_t standbyMs = now - _lastStandbyStart;
        if (standbyMs >= _nextInactivityFlushMs) {
            _startFlush("Inactivity", Config::FLUSH_INACTIVITY_DURATION_MS);
            return;
        }
    }

    // 6d. Production-interval flush (optional, disabled when interval = 0)
    if (Config::FLUSH_PRODUCTION_INTERVAL_MS > 0
            && _state == SystemState::STANDBY
            && _sourceWater
            && (now - _lastFlushTime) >= Config::FLUSH_PRODUCTION_INTERVAL_MS) {
        _startFlush("Production", Config::FLUSH_PRODUCTION_DURATION_MS);
        return;
    }

    // ------------------------------------------------------------------
    // 7. PRODUCTION LOGIC
    // ------------------------------------------------------------------
    if (_sourceWater && _faucetOpen) {
        if (_state != SystemState::RUNNING) {
            Serial.println("[ROC] Starting production");
            _updateDisplay("STATUS: STARTING", "Opening Inlet...", "", "");
            digitalWrite(Config::PIN_INLET_VALVE, HIGH);
            delay(1000);  // allow inlet valve to open before starting pump
            digitalWrite(Config::PIN_PUMP, HIGH);
            _state = SystemState::RUNNING;
        }
        _productionTime += delta;

    } else {
        if (_state == SystemState::RUNNING) {
            Serial.println("[ROC] Stopping production");
            digitalWrite(Config::PIN_PUMP, LOW);
            delay(1000);  // let pressure equalise before closing valve
            digitalWrite(Config::PIN_INLET_VALVE, LOW);
            _productionTotal += _productionTime;
            _productionTime   = 0;
            _state            = SystemState::STANDBY;
            _lastStandbyStart = millis();
        }
    }

    // ------------------------------------------------------------------
    // 8. DISPLAY REFRESH
    // ------------------------------------------------------------------
    if (_state == SystemState::RUNNING) {
        uint32_t secs = _productionTime / 1000;
        char l3[21];
        snprintf(l3, sizeof(l3), "Time: %um %02us",
                 (unsigned)(secs / 60), (unsigned)(secs % 60));
        char l2[21];
        snprintf(l2, sizeof(l2), "TDS: %d PPM", _currentTds);
        _updateDisplay("STATUS: RUNNING", l2, l3,
                       _wifiConnected ? "WiFi: ON" : "WiFi: OFF");
    } else {
        char l4[21];
        snprintf(l4, sizeof(l4), "Last TDS: %dPPM", _currentTds);
        _updateDisplay("STATUS: STANDBY", "System Ready",
                       "Waiting for Faucet", l4);
    }
}

// =============================================================================
// Hardware helpers
// =============================================================================
int ROController::_readTds() const {
    int raw = analogRead(Config::PIN_TDS_SENSOR);
    return (int)(raw * Config::TDS_FACTOR + Config::TDS_OFFSET);
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
    delay(500);  // let system depressurise before opening flush valve
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
// Status accessors
// =============================================================================
uint32_t ROController::getFlushRemainMs() const {
    if (_state != SystemState::FLUSHING) return 0;
    uint32_t elapsed = millis() - _flushStartTime;
    return (elapsed < _flushDuration) ? (_flushDuration - elapsed) : 0;
}

void ROController::setWifiConnected(bool connected, const char* ip) {
    _wifiConnected = connected;
    if (ip) strlcpy(_wifiIp, ip, sizeof(_wifiIp));
    else     _wifiIp[0] = '\0';
}

// =============================================================================
// Web control actions (called from network task — Phase 3)
// =============================================================================
bool ROController::doMaintenanceToggle(char* errOut, size_t errLen) {
    if (_state == SystemState::EMERGENCY) {
        strlcpy(errOut, "Cannot toggle maintenance during emergency", errLen);
        return false;
    }
    if (_state == SystemState::MAINTENANCE) _exitMaintenance();
    else                                    _enterMaintenance();
    return true;
}

bool ROController::doManualFlush(char* errOut, size_t errLen) {
    if (_state != SystemState::STANDBY) {
        strlcpy(errOut, "Flush only allowed from STANDBY state", errLen);
        return false;
    }
    _startFlush("Manual", Config::FLUSH_MANUAL_DURATION_MS);
    return true;
}

bool ROController::doRelayControl(const char* action, char* errOut, size_t errLen) {
    if (_state != SystemState::MAINTENANCE) {
        strlcpy(errOut, "Relay control only in MAINTENANCE mode", errLen);
        return false;
    }
    if      (strcmp(action, "pump_on")        == 0) digitalWrite(Config::PIN_PUMP,        HIGH);
    else if (strcmp(action, "pump_off")       == 0) digitalWrite(Config::PIN_PUMP,        LOW);
    else if (strcmp(action, "inlet_on")       == 0) digitalWrite(Config::PIN_INLET_VALVE, HIGH);
    else if (strcmp(action, "inlet_off")      == 0) digitalWrite(Config::PIN_INLET_VALVE, LOW);
    else if (strcmp(action, "flush_valve_on") == 0) digitalWrite(Config::PIN_FLUSH_VALVE, HIGH);
    else if (strcmp(action, "flush_valve_off")== 0) digitalWrite(Config::PIN_FLUSH_VALVE, LOW);
    else {
        snprintf(errOut, errLen, "Unknown action: %s", action);
        return false;
    }
    return true;
}

void ROController::doReset() {
    esp_restart();
}
