#include "AlarmManager.h"
#include "config.h"

// =============================================================================
// Alarm definitions — match alarms.py exactly.
// Notes are {freqHz, durationMs}; freqHz == 0 is a rest.
// =============================================================================

struct Note {
    uint16_t freqHz;
    uint16_t durationMs;
};

struct AlarmDef {
    Note     notes[4];
    uint8_t  noteCount;
    uint16_t blinkRateMs;
    uint8_t  priority;
};

static const AlarmDef DEFS[ALARM_COUNT] = {
    // LEAK: A5/E5 alternating, very fast blink
    { {{880,100},{659,100},{880,100},{659,100}}, 4,  100, 0 },
    // LOW_PRESSURE: descending G5→E5→C5→G4
    { {{784,150},{659,150},{523,150},{392,300}}, 4,  500, 2 },
    // TDS_HIGH: A4/A#4 buzz
    { {{440, 70},{466, 70},{440, 70},{466, 70}}, 4, 1000, 3 },
};

// =============================================================================
// Constructor / begin
// =============================================================================

AlarmManager::AlarmManager()
    : _activeMask(0)
    , _queueIdx(0)
    , _noteIdx(0)
    , _nextNoteTick(0)
    , _ledBlinkRateMs(0)
    , _ledLastToggle(0)
    , _ledState(false)
{}

void AlarmManager::begin() {
    pinMode(Config::PIN_LED,    OUTPUT);
    pinMode(Config::PIN_BUZZER, OUTPUT);
    digitalWrite(Config::PIN_LED,    LOW);
    digitalWrite(Config::PIN_BUZZER, LOW);
    noTone(Config::PIN_BUZZER);
}

// =============================================================================
// Public API
// =============================================================================

void AlarmManager::triggerAlarm(AlarmId id) {
    uint8_t bit = 1u << static_cast<uint8_t>(id);
    if (_activeMask & bit) return;  // already active
    _activeMask |= bit;
    _refreshLed();
}

void AlarmManager::clearAlarm(AlarmId id) {
    uint8_t bit = 1u << static_cast<uint8_t>(id);
    if (!(_activeMask & bit)) return;  // not active
    _activeMask &= ~bit;
    // Reset sequence state — matches Python clear_alarm() behaviour
    _queueIdx = 0;
    _noteIdx  = 0;
    _refreshLed();
    if (_activeMask == 0) {
        noTone(Config::PIN_BUZZER);
    }
}

bool AlarmManager::isActive(AlarmId id) const {
    return (_activeMask >> static_cast<uint8_t>(id)) & 1u;
}

// =============================================================================
// update() — non-blocking tick, call every loop()
// =============================================================================

void AlarmManager::update() {
    // ---- LED blink (millis()-based toggle) ----------------------------------
    if (_ledBlinkRateMs == 0) {
        // No active alarms — LED off (already set in _refreshLed / clearAlarm)
    } else {
        uint32_t now = millis();
        uint32_t halfPeriod = _ledBlinkRateMs / 2;
        if (now - _ledLastToggle >= halfPeriod) {
            _ledLastToggle = now;
            _ledState = !_ledState;
            digitalWrite(Config::PIN_LED, _ledState ? HIGH : LOW);
        }
    }

    // ---- Buzzer note sequencing ---------------------------------------------
    if (_activeMask == 0) {
        return;
    }

    uint32_t now = millis();
    if (now < _nextNoteTick) {
        return;  // still within current note's duration
    }

    // Get the alarm at the current rotation index
    int8_t alarmIdx = _activeAtIndex(_queueIdx);
    if (alarmIdx < 0) {
        // Rotation index stale after a clear — reset
        _queueIdx = 0;
        _noteIdx  = 0;
        alarmIdx  = _activeAtIndex(0);
        if (alarmIdx < 0) return;
    }

    const AlarmDef& def  = DEFS[alarmIdx];
    const Note&     note = def.notes[_noteIdx];

    // Play or silence
    if (note.freqHz == 0) {
        noTone(Config::PIN_BUZZER);
    } else {
        tone(Config::PIN_BUZZER, note.freqHz);
    }

    _nextNoteTick = now + note.durationMs;
    _noteIdx++;

    // Finished this alarm's sequence — advance rotation
    if (_noteIdx >= def.noteCount) {
        _noteIdx   = 0;
        uint8_t n  = _activeCount();
        _queueIdx  = (n > 0) ? ((_queueIdx + 1) % n) : 0;
    }
}

// =============================================================================
// Private helpers
// =============================================================================

void AlarmManager::_refreshLed() {
    if (_activeMask == 0) {
        _ledBlinkRateMs = 0;
        _ledState       = false;
        digitalWrite(Config::PIN_LED, LOW);
        return;
    }

    // Fastest blink (lowest ms value) belongs to the highest-priority alarm
    uint16_t fastest = 0xFFFF;
    for (uint8_t i = 0; i < ALARM_COUNT; i++) {
        if ((_activeMask >> i) & 1u) {
            if (DEFS[i].blinkRateMs < fastest) {
                fastest = DEFS[i].blinkRateMs;
            }
        }
    }
    _ledBlinkRateMs = fastest;
    // Reset toggle timer so the new rate takes effect immediately
    _ledLastToggle = millis();
}

uint8_t AlarmManager::_activeCount() const {
    // Popcount over 3 bits
    uint8_t n = 0;
    for (uint8_t i = 0; i < ALARM_COUNT; i++) {
        if ((_activeMask >> i) & 1u) n++;
    }
    return n;
}

// Returns the AlarmDef index of the idx-th active alarm (insertion order = bit order).
int8_t AlarmManager::_activeAtIndex(uint8_t idx) const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < ALARM_COUNT; i++) {
        if ((_activeMask >> i) & 1u) {
            if (count == idx) return static_cast<int8_t>(i);
            count++;
        }
    }
    return -1;
}
