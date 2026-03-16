#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#include "state.h"
#include "AlarmManager.h"

// =============================================================================
// ROController — core state machine for the Krosmosis RO system.
//
// Ownership model: one singleton, created in main.cpp and driven by
//   controller.begin()  — hardware init, called once in setup()
//   controller.update() — state machine tick, called every loop()
//
// Phase 3 will add getStatus() / doControl() for the network task.
// =============================================================================

class ROController {
public:
    ROController();

    void begin();
    void update();

    // -------------------------------------------------------------------------
    // Status accessors — used by the network task in Phase 3.
    // All reads are safe from Core 0 because the values are either atomic
    // (bool/int) or only read, never written, outside the control task.
    // A mutex will be added in Phase 3 for the struct snapshot.
    // -------------------------------------------------------------------------
    SystemState getState()         const { return _state; }
    int         getTds()           const { return _currentTds; }
    bool        hasSourceWater()   const { return _sourceWater; }
    bool        isFaucetOpen()     const { return _faucetOpen; }
    bool        isLeakDetected()   const { return _leakDetected; }
    bool        isWifiConnected()  const { return _wifiConnected; }
    uint32_t    getProductionMs()  const { return _productionTime; }
    uint32_t    getUptimeMs()      const { return millis() - _startTime; }
    uint32_t    getFlushCycles()   const { return _flushCycles; }
    uint32_t    getProductionTotalMs() const { return _productionTotal + _productionTime; }
    const char* getFlushReason()   const { return _flushReason; }
    uint32_t    getFlushRemainMs() const;
    uint32_t    getFlushDurationMs() const { return _flushDuration; }
    const char* getLcdLine(int row) const { return (row >= 0 && row < 4) ? _lcdLines[row] : ""; }
    const char* getWifiIp()        const { return _wifiIp; }

    // Network task sets these after WiFi connects (Phase 3)
    void setWifiConnected(bool connected, const char* ip);
    void incWifiReconnects() { _wifiReconnects++; }
    uint32_t getWifiReconnects() const { return _wifiReconnects; }

    // Web control actions (Phase 3)
    bool doMaintenanceToggle(char* errOut, size_t errLen);
    bool doManualFlush(char* errOut, size_t errLen);
    bool doRelayControl(const char* action, char* errOut, size_t errLen);
    void doReset();

private:
    // Hardware
    LiquidCrystal_I2C _lcd;
    AlarmManager   _alarms;

    // State
    SystemState _state;
    volatile bool _leakDetected;  // set by ISR, cleared never (latching)

    // Display buffer (also served to web UI)
    char _lcdLines[4][21];  // 20 chars + NUL

    // Timing
    uint32_t _startTime;
    uint32_t _lastLoopTime;
    uint32_t _productionTime;    // ms, current cycle
    uint32_t _productionTotal;   // ms, cumulative across cycles
    uint32_t _lastFlushTime;     // millis() at last flush end
    uint32_t _flushStartTime;    // millis() at current flush start
    uint32_t _flushDuration;     // ms, current flush target duration
    uint32_t _lastStandbyStart;  // millis() when we last entered standby
    uint32_t _nextInactivityFlushMs; // randomised interval until next inactivity flush

    // Flush metadata
    char _flushReason[16];
    bool _startupFlushDone;

    // Sensor cache (updated each tick)
    bool _sourceWater;
    bool _faucetOpen;
    int  _currentTds;

    // Counters
    uint32_t _flushCycles;
    uint32_t _wifiReconnects;

    // WiFi state (set by network task)
    bool _wifiConnected;
    char _wifiIp[16];  // "xxx.xxx.xxx.xxx\0"

    // Helpers
    void _initHardware();
    void _initWdt();
    void _updateDisplay(const char* l1, const char* l2,
                        const char* l3, const char* l4);
    int  _readTds() const;

    void _startFlush(const char* reason, uint32_t durationMs);
    void _stopFlush();
    void _enterMaintenance();
    void _exitMaintenance();
    void _scheduleNextInactivityFlush();
};
