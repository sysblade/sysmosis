#pragma once

// =============================================================================
// AlarmManager — non-blocking buzzer melody queue + LED blink.
//
// Mirrors alarms.py exactly:
//   - Three alarms ranked by priority (0 = highest).
//   - LED blinks at the rate of the highest-priority active alarm.
//   - Buzzer cycles through each active alarm's note sequence in round-robin.
//   - All timing is millis()-based; no hardware timers or interrupts.
// =============================================================================

#include <Arduino.h>

enum class AlarmId : uint8_t {
    LEAK         = 0,   // priority 0 — blink 100 ms
    LOW_PRESSURE = 1,   // priority 2 — blink 500 ms
    TDS_HIGH     = 2,   // priority 3 — blink 1000 ms
};

static constexpr uint8_t ALARM_COUNT = 3;

class AlarmManager {
public:
    AlarmManager();

    void begin();                       // call once in setup()
    void triggerAlarm(AlarmId id);      // idempotent
    void clearAlarm(AlarmId id);        // idempotent, resets sequence
    void update();                      // non-blocking tick — call every loop()

    bool isActive(AlarmId id) const;
    bool hasAnyAlarm()        const { return _activeMask != 0; }

private:
    // Active alarm bitmask — bit N set ↔ AlarmId(N) is active
    uint8_t  _activeMask;

    // Buzzer sequence state
    uint8_t  _queueIdx;       // index into the active-alarm rotation
    uint8_t  _noteIdx;        // note index within current alarm's sequence
    uint32_t _nextNoteTick;   // millis() when to play the next note

    // LED blink state (millis()-based, no timer ISR)
    uint16_t _ledBlinkRateMs; // half-period for toggle; 0 = off
    uint32_t _ledLastToggle;
    bool     _ledState;

    // Helpers
    void    _refreshLed();
    uint8_t _activeCount()              const;
    int8_t  _activeAtIndex(uint8_t idx) const; // -1 if out of range
};
