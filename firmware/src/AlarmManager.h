#pragma once

// =============================================================================
// AlarmManager — Phase 2 placeholder
//
// Manages the buzzer melody queue and LED blink rate based on active alarms.
// Priority scheme mirrors the Python alarms.py:  lower number = higher priority.
//
// Phase 1: stubs compile and do nothing.
// Phase 2: full implementation with tone() + millis()-based non-blocking sequencing.
// =============================================================================

#include <Arduino.h>

enum class AlarmId : uint8_t {
    LEAK         = 0,   // priority 0 (highest) — immediate shutdown
    LOW_PRESSURE = 1,   // priority 2
    TDS_HIGH     = 2,   // priority 3
};

class AlarmManager {
public:
    AlarmManager();

    // Call once in setup()
    void begin();

    // Idempotent — safe to call every loop iteration
    void triggerAlarm(AlarmId id);
    void clearAlarm(AlarmId id);

    // Non-blocking tick — call every loop iteration
    void update();

    bool isActive(AlarmId id) const;
    bool hasAnyAlarm() const;
};
