#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#include "state.h"
#include "AlarmManager.h"

// =============================================================================
// StatusSnapshot — point-in-time copy of controller state for the network task.
// Fields match the Python _get_web_status() dict exactly.
// =============================================================================
struct StatusSnapshot {
    SystemState state;
    bool        sourceWater;
    bool        faucetOpen;
    int         tdsPpm;
    bool        wifiConnected;
    char        wifiIp[16];
    uint32_t    uptimeS;
    uint32_t    productionS;
    char        flushReason[16];
    uint32_t    flushRemainingS;
    uint32_t    flushDurationS;
    bool        pump;
    bool        inletValve;
    bool        flushValve;
    bool        lps;
    bool        hps;
    bool        leakDetected;
    uint32_t    flushCyclesTotal;
    uint32_t    productionTotalS;
    uint32_t    wifiReconnects;
    uint32_t    timeToFlushS;     // seconds until next inactivity flush
    char        lcdLines[4][21];
};

// =============================================================================
// ControlCmd — posted by the network task, executed by the control task.
// Using a queue avoids calling delay() or GPIO writes from web callbacks.
// =============================================================================
struct ControlCmd {
    enum class Type : uint8_t {
        MAINTENANCE_TOGGLE,
        MANUAL_FLUSH,
        RELAY,
        RESET,
    } type;
    char action[24];   // relay action string (pump_on, inlet_off, …)
};

// =============================================================================
// ROController
// =============================================================================
class ROController {
public:
    ROController();

    void begin();    // call once from setup()
    void update();   // call every loop() — drives state machine + WDT

    // -------------------------------------------------------------------------
    // Thread-safe interface for the network task (Core 0)
    // -------------------------------------------------------------------------
    StatusSnapshot getStatusSnapshot();
    bool           postCommand(const ControlCmd& cmd);   // false = queue full

    // Validate a command against a snapshot before posting.
    // Returns false and writes a reason into err[errLen].
    static bool validateMaintenanceToggle(const StatusSnapshot& s,
                                          char* err, size_t errLen);
    static bool validateManualFlush(const StatusSnapshot& s,
                                    char* err, size_t errLen);
    static bool validateRelay(const StatusSnapshot& s, const char* action,
                               char* err, size_t errLen);

    // Called by the network task after WiFi connects / drops
    void setWifiConnected(bool connected, const char* ip);
    void incWifiReconnects() { _wifiReconnects++; }

private:
    // Hardware
    LiquidCrystal_I2C _lcd;
    AlarmManager      _alarms;

    // State
    SystemState   _state;
    volatile bool _leakDetected;

    // Display buffer
    char _lcdLines[4][21];

    // Timing (all in ms)
    uint32_t _startTime;
    uint32_t _lastLoopTime;
    uint32_t _productionTime;
    uint32_t _productionTotal;
    uint32_t _lastFlushTime;
    uint32_t _flushStartTime;
    uint32_t _flushDuration;
    uint32_t _lastStandbyStart;
    uint32_t _nextInactivityFlushMs;

    // Flush metadata
    char _flushReason[16];
    bool _startupFlushDone;

    // Sensor cache
    bool _sourceWater;
    bool _faucetOpen;
    int  _currentTds;

    // Counters
    uint32_t _flushCycles;
    uint32_t _wifiReconnects;

    // WiFi (written by network task, read by control task for display only)
    bool _wifiConnected;
    char _wifiIp[16];

    // Cross-task communication
    SemaphoreHandle_t _snapshotMutex;
    StatusSnapshot    _snapshot;
    QueueHandle_t     _cmdQueue;

    // Helpers
    void    _initHardware();
    void    _initWdt();
    void    _updateDisplay(const char* l1, const char* l2,
                           const char* l3, const char* l4);
    int     _readTds() const;
    uint32_t _flushRemainMs() const;

    void _startFlush(const char* reason, uint32_t durationMs);
    void _stopFlush();
    void _enterMaintenance();
    void _exitMaintenance();
    void _scheduleNextInactivityFlush();

    void _updateSnapshot();
    void _processCommand(const ControlCmd& cmd);
};
