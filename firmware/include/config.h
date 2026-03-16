#pragma once

// =============================================================================
// Krosmosis Configuration
// Edit this file and recompile to apply changes.
// =============================================================================

#include <stdint.h>

namespace Config {

    // -------------------------------------------------------------------------
    // LCD
    // -------------------------------------------------------------------------
    constexpr uint8_t  PIN_LCD_SCL  = 22;
    constexpr uint8_t  PIN_LCD_SDA  = 21;
    constexpr uint8_t  LCD_I2C_ADDR = 0x27;  // 0x27 or 0x3F are common

    // -------------------------------------------------------------------------
    // Relay outputs (active-high, all off at boot)
    // -------------------------------------------------------------------------
    constexpr uint8_t  PIN_PUMP        = 12;
    constexpr uint8_t  PIN_INLET_VALVE = 13;
    constexpr uint8_t  PIN_FLUSH_VALVE = 14;

    // -------------------------------------------------------------------------
    // Sensor inputs (NPN opto-isolated, active-low — use INPUT_PULLUP)
    // -------------------------------------------------------------------------
    constexpr uint8_t  PIN_LOW_PRESSURE  = 25;
    constexpr uint8_t  PIN_HIGH_PRESSURE = 26;
    constexpr uint8_t  PIN_LEAK_SENSOR   = 27;
    constexpr uint8_t  PIN_TDS_SENSOR    = 34;   // ADC1 channel, input-only pin

    // -------------------------------------------------------------------------
    // Alarm hardware
    // -------------------------------------------------------------------------
    constexpr uint8_t  PIN_BUZZER = 23;
    constexpr uint8_t  PIN_LED    = 2;   // on-board blue LED

    // -------------------------------------------------------------------------
    // TDS calibration:  PPM = ADC_raw * TDS_FACTOR + TDS_OFFSET
    // ADC_raw range is 0-4095 (12-bit, 11dB attenuation, 0-3.3V)
    // -------------------------------------------------------------------------
    constexpr float    TDS_FACTOR    = 0.5f;
    constexpr float    TDS_OFFSET    = 0.0f;
    constexpr int      TDS_THRESHOLD       = 100;  // PPM — above this triggers TDS_HIGH alarm
    constexpr int      TDS_THRESHOLD_CLEAR =  90;  // PPM — below this clears TDS_HIGH alarm
    constexpr int      TDS_SAMPLES         =  16;  // ADC samples averaged per reading

    // -------------------------------------------------------------------------
    // Flush timing (all values in milliseconds)
    // -------------------------------------------------------------------------
    constexpr uint32_t FLUSH_STARTUP_DURATION_MS    =  20000UL;   // on every boot
    constexpr uint32_t FLUSH_MANUAL_DURATION_MS     =  30000UL;   // web UI trigger
    constexpr uint32_t FLUSH_INACTIVITY_DURATION_MS =  60000UL;   // periodic standby flush
    constexpr uint32_t FLUSH_INACTIVITY_MIN_MS      =  86400000UL;  // 24 h
    constexpr uint32_t FLUSH_INACTIVITY_MAX_MS      = 172800000UL;  // 48 h
    constexpr uint32_t FLUSH_PRODUCTION_INTERVAL_MS =      0UL;   // 0 = disabled
    constexpr uint32_t FLUSH_PRODUCTION_DURATION_MS =  30000UL;

    // -------------------------------------------------------------------------
    // Watchdog
    // Only the control task (Core 1 / loopTask) subscribes to the WDT.
    // The network task intentionally does NOT subscribe: WiFi reconnects and
    // OTA flashing can legitimately stall for several seconds without meaning
    // the system is hung. If the control task hangs, the WDT fires regardless.
    // -------------------------------------------------------------------------
    constexpr uint32_t WATCHDOG_TIMEOUT_S = 30;   // esp_task_wdt_init takes seconds

    // -------------------------------------------------------------------------
    // OTA (over-the-air firmware update via Arduino IDE / PlatformIO)
    // -------------------------------------------------------------------------
    constexpr bool     OTA_ENABLED    = true;
    constexpr char     OTA_HOSTNAME[] = "krosmosis";
    constexpr char     OTA_PASSWORD[] = "";        // empty = no password required

    // -------------------------------------------------------------------------
    // WiFi
    // -------------------------------------------------------------------------
    constexpr char     WIFI_SSID[]                = "";
    constexpr char     WIFI_PASSWORD[]            = "";
    constexpr uint32_t WIFI_TIMEOUT_MS            = 10000UL;
    constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 60000UL;

    // -------------------------------------------------------------------------
    // Web UI  (Phase 3)
    // -------------------------------------------------------------------------
    constexpr bool     WEB_ENABLED        = true;
    constexpr uint16_t WEB_PORT           = 80;
    constexpr char     WEB_AUTH_PASSWORD[] = "";   // empty = no auth

    // -------------------------------------------------------------------------
    // Prometheus metrics  (Phase 4)
    // -------------------------------------------------------------------------
    constexpr bool     METRICS_ENABLED = true;
    constexpr uint16_t METRICS_PORT    = 8080;

}  // namespace Config
