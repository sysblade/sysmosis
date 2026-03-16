# Krosmosis Firmware (Arduino C++)

ESP32 controller for a reverse osmosis water filtration system, written in C++ using the Arduino framework and PlatformIO. This is a rewrite of the MicroPython version in the parent directory.

## Hardware

| Component | GPIO |
|---|---|
| I2C LCD 20×4 | SCL=22, SDA=21 (addr 0x27) |
| Pump relay | 12 |
| Inlet valve relay | 13 |
| Flush valve relay | 14 |
| Low pressure sensor (NPN opto) | 25 |
| High pressure sensor (NPN opto) | 26 |
| Leak sensor (NPN opto) | 27 |
| TDS sensor (analog) | 34 |
| Buzzer (active) | 23 |
| Status LED | 2 |

All sensor inputs are active-low with internal pull-up. All relay outputs are active-high, defaulting to LOW on boot.

## Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) CLI or IDE extension

### First flash (USB)

```bash
cd firmware

# Build
pio run

# Flash firmware
pio run --target upload

# Flash filesystem (static web files) — required on first flash, and after any change to data/
pio run --target uploadfs

# Monitor serial output
pio device monitor
```

### Subsequent updates (OTA, no USB required)

```bash
pio run --target upload    --upload-port krosmosis.local
pio run --target uploadfs  --upload-port krosmosis.local
```

## Configuration

All settings live in `include/config.h`. Edit and recompile to apply — no runtime config file.

### Required settings

```cpp
constexpr char WIFI_SSID[]     = "your_network";
constexpr char WIFI_PASSWORD[] = "your_password";
```

### Key settings

| Setting | Default | Description |
|---|---|---|
| `WIFI_SSID` | `""` | WiFi network name |
| `WIFI_PASSWORD` | `""` | WiFi password |
| `WEB_AUTH_PASSWORD` | `""` | Web UI password — empty disables auth |
| `OTA_HOSTNAME` | `"krosmosis"` | mDNS hostname (`krosmosis.local`) |
| `OTA_PASSWORD` | `""` | OTA password — empty disables OTA auth |
| `TDS_THRESHOLD` | `100` | PPM above which TDS alarm triggers |
| `TDS_THRESHOLD_CLEAR` | `90` | PPM below which TDS alarm clears (hysteresis) |
| `TDS_SAMPLES` | `16` | ADC samples averaged per TDS reading |
| `FLUSH_STARTUP_DURATION_MS` | `20000` | Startup flush duration (ms) |
| `FLUSH_INACTIVITY_DURATION_MS` | `60000` | Inactivity flush duration (ms) |
| `FLUSH_INACTIVITY_MIN_MS` | `86400000` | Minimum standby time before inactivity flush (24 h) |
| `FLUSH_INACTIVITY_MAX_MS` | `172800000` | Maximum standby time before inactivity flush (48 h) |
| `WATCHDOG_TIMEOUT_S` | `30` | Hardware watchdog timeout |
| `WEB_PORT` | `80` | Web UI port |
| `METRICS_PORT` | `8080` | Prometheus metrics port |

## System states

```
STANDBY ──→ RUNNING      source water pressure + faucet open
RUNNING ──→ STANDBY      faucet closed or pressure lost
STANDBY ──→ FLUSHING     startup / inactivity / production-interval / manual trigger
FLUSHING──→ STANDBY      flush timer elapsed
Any     ──→ EMERGENCY    leak sensor triggered — latching, requires power cycle
Any     ──→ MAINTENANCE  web UI toggle
MAINTENANCE──→ STANDBY   web UI toggle
```

### Flush schedule

| Trigger | Condition | Duration |
|---|---|---|
| Startup | Once per boot, when source water first detected | `FLUSH_STARTUP_DURATION_MS` |
| Inactivity | After randomised 24–48 h in STANDBY | `FLUSH_INACTIVITY_DURATION_MS` |
| Production interval | Optional — disabled when `FLUSH_PRODUCTION_INTERVAL_MS = 0` | `FLUSH_PRODUCTION_DURATION_MS` |
| Manual | Web UI button, STANDBY only | `FLUSH_MANUAL_DURATION_MS` |

## Web interface

Navigate to `http://krosmosis.local/` (or the device IP).

If `WEB_AUTH_PASSWORD` is set you will be prompted to log in. The session cookie persists until the device reboots.

### Control actions (POST `/control`, parameter `action=...`)

| Action | Description | Required state |
|---|---|---|
| `maintenance_toggle` | Enter / exit manual relay control mode | Any except EMERGENCY |
| `flush_start` | Trigger a manual flush | STANDBY |
| `pump_on` / `pump_off` | Direct pump control | MAINTENANCE |
| `inlet_on` / `inlet_off` | Direct inlet valve control | MAINTENANCE |
| `flush_valve_on` / `flush_valve_off` | Direct flush valve control | MAINTENANCE |
| `reset` | Reboot the device | Any |

### Status (GET `/status`)

Returns a JSON object with the full system state, suitable for dashboards and automation. Key fields:

```json
{
  "state": "STANDBY",
  "state_id": 0,
  "tds_ppm": 42,
  "source_water": true,
  "faucet_open": false,
  "pump": false,
  "inlet_valve": false,
  "flush_valve": false,
  "lps": false,
  "hps": false,
  "leak_detected": false,
  "uptime_s": 3600,
  "production_time_s": 0,
  "production_total_s": 7200,
  "flush_cycles_total": 3,
  "time_to_flush_s": 71400,
  "wifi_connected": true,
  "ip": "192.168.1.42",
  "lcd": ["STATUS: STANDBY     ", ...]
}
```

## Prometheus metrics

```
http://krosmosis.local:8080/metrics
```

Example `prometheus.yml` scrape config:

```yaml
scrape_configs:
  - job_name: krosmosis
    static_configs:
      - targets: ['krosmosis.local:8080']
    scrape_interval: 10s
```

Available metrics: `krosmosis_tds_ppm`, `krosmosis_system_state{state=...}`, `krosmosis_uptime_seconds`, `krosmosis_production_total_seconds`, `krosmosis_flush_cycles_total`, `krosmosis_leak_detected`, `krosmosis_wifi_reconnects_total`, and more.

## Alarms

| Alarm | Trigger | Buzzer | LED blink |
|---|---|---|---|
| `LEAK` | Leak sensor (latching — power cycle to reset) | A5/E5 rapid alternation | 100 ms |
| `LOW_PRESSURE` | No source water pressure | G5→E5→C5→G4 descending | 500 ms |
| `TDS_HIGH` | TDS above threshold | A4/A#4 buzz | 1000 ms |

Multiple simultaneous alarms: buzzer cycles round-robin through each active alarm's sequence; LED blinks at the rate of the highest-priority alarm.

## Architecture

### Two-core design

| Core | Task | Responsibilities |
|---|---|---|
| Core 1 | `loopTask` (Arduino `loop()`) | State machine, sensors, relays, LCD, WDT |
| Core 0 | `net` | WiFi, reconnect, web server, metrics, OTA |

The control task is the only one subscribed to the hardware watchdog. The network task intentionally is not — WiFi reconnects and OTA uploads can stall without indicating a system fault.

### Thread safety

- `ROController` publishes a `StatusSnapshot` struct under a FreeRTOS mutex at the end of every `update()` tick.
- Web handler callbacks read the snapshot under the same mutex.
- Web control actions are posted to a FreeRTOS queue (capacity 8) and executed by the control task at the top of the next tick. No `delay()` or GPIO writes occur in web callbacks.

### Persistent storage

`production_total_s` and `flush_cycles_total` are saved to ESP32 NVS (non-volatile storage) at the end of every production cycle and every flush. Counters survive reboots and power loss, ensuring Prometheus `rate()` calculations remain valid across restarts.

NVS write endurance: ~10,000 cycles per key. At one flush per day and a few production stops per day, this exceeds 10 years.

## File structure

```
firmware/
├── platformio.ini          PlatformIO project config
├── README.md               This file
├── include/
│   ├── config.h            All compile-time settings
│   └── state.h             SystemState enum + stateName()
├── src/
│   ├── main.cpp            setup() / loop() / FreeRTOS task creation
│   ├── ROController.h/.cpp State machine and hardware abstraction
│   ├── AlarmManager.h/.cpp Non-blocking buzzer melody + LED blink
│   └── WebInterface.h/.cpp WiFi, web server, Prometheus metrics, OTA
└── data/
    └── static/             Web UI static files (LittleFS)
        ├── index.html
        ├── login.html
        ├── app.js
        └── style.css
```

## Differences from the MicroPython version

| | MicroPython | Arduino C++ |
|---|---|---|
| Firmware updates | USB + mpremote | USB or OTA over WiFi |
| Type safety | Runtime | Compile-time enforced |
| Threading | Single-threaded cooperative | FreeRTOS dual-core |
| Web commands | Executed directly in callback (blocking) | FreeRTOS queue, executed by control task |
| Counter persistence | Lost on reboot | NVS (survives power loss) |
| Emergency state | Infinite spin loop | Drains queue, returns normally |
| HTTPS | Supported | Not implemented — use a reverse proxy |
| mDNS | Not supported | `krosmosis.local` via ESPmDNS |
