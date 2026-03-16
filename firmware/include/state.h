#pragma once

#include <stdint.h>

enum class SystemState : uint8_t {
    STANDBY     = 0,
    RUNNING     = 1,
    FLUSHING    = 2,
    EMERGENCY   = 3,
    MAINTENANCE = 4,
};

inline const char* stateName(SystemState s) {
    switch (s) {
        case SystemState::STANDBY:     return "STANDBY";
        case SystemState::RUNNING:     return "RUNNING";
        case SystemState::FLUSHING:    return "FLUSHING";
        case SystemState::EMERGENCY:   return "EMERGENCY";
        case SystemState::MAINTENANCE: return "MAINTENANCE";
        default:                       return "UNKNOWN";
    }
}
