# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Krosmosis is a MicroPython-based controller for a reverse osmosis (RO) water filtration system running on an ESP32 microcontroller.

## Development Commands

```bash
# Install dev tools (linting, mpremote, MicroPython stubs for IDE)
uv sync --extra dev

# Lint code
uv run ruff check .

# Connect to ESP32 REPL
mpremote connect /dev/ttyUSB0

# Upload code to ESP32
mpremote connect /dev/ttyUSB0 cp main.py :main.py
mpremote connect /dev/ttyUSB0 cp metrics.py :metrics.py

# Install MicroPython libraries on the device
# lcd_api.py must be installed first — it is a required dependency of esp8266_i2c_lcd.py
mpremote connect /dev/ttyUSB0 mip install github:dhylands/python_lcd/lcd/lcd_api.py
mpremote connect /dev/ttyUSB0 mip install github:dhylands/python_lcd/lcd/esp8266_i2c_lcd.py

# Upload all Python source files
mpremote connect /dev/ttyUSB0 cp main.py :main.py + cp alarms.py :alarms.py
```

## Architecture

### Hardware Components

- **ESP32 microcontroller** running MicroPython
- **20x4 I2C LCD display** (address 0x27, pins SCL=22, SDA=21)
- **Relays** (GPIO 12, 13, 14): pump, inlet valve, flush valve
- **Active buzzer** (GPIO 32): audio alerts for status and alarms
- **Opto-isolated inputs** (GPIO 25, 26, 27): low pressure sensor, high pressure sensor, leak sensor
- **Analog TDS sensor** (GPIO 34): measures water purity in PPM

### Control Logic

The system operates as a state machine:
1. **Emergency state**: Leak detection immediately cuts all water flow and locks the system
2. **Production state**: Activates when source water pressure exists AND faucet is open
3. **Flushing state**: Periodic membrane flush to extend RO membrane life
4. **Standby state**: Waits for faucet to open while monitoring sensors

### Configuration

All hardware pins, timing, and calibration values are configurable via `config.py`:
- Hardware pins (GPIO assignments)
- TDS calibration (factor and offset)
- Flush cycle timing (interval and duration)
- WiFi settings and reconnect interval
- Watchdog timeout

### IRQ-Based Leak Detection

Leak detection uses a hardware interrupt (`Pin.IRQ_FALLING`) for immediate response:
- The `_emergency_shutdown()` IRQ handler runs instantly when the leak sensor triggers
- IRQ handlers must be minimal (no I2C/LCD operations) - only GPIO writes
- The handler immediately: cuts pump/valves, activates buzzer, sets `leak_detected` flag
- The main loop updates the display and continues alarm beeping pattern
- Polling is retained as a backup safety check

### Safety Features

- **Watchdog Timer (WDT)**: Auto-resets ESP32 if system hangs (default 30s timeout)
- **WiFi Auto-Reconnect**: Periodically checks connection and reconnects if dropped
- **Flush Cycle**: Automatic membrane flush based on production time to extend membrane life

### Prometheus Metrics

The `metrics.py` module provides a lightweight HTTP server exposing Prometheus-formatted metrics on port 8080:
- `MetricsServer`: Non-blocking HTTP server class
- `create_system_collector()`: Factory function to create metrics collector with callbacks
- Metrics include: TDS, system state, sensor states, relay states, production time, counters

### MicroPython Dependencies

Dependencies are installed directly on the ESP32, not via pip:

| Library | Source | Install Command |
|---------|--------|-----------------|
| `machine` | Built-in | (included in MicroPython firmware) |
| `lcd_api` | [dhylands/python_lcd](https://github.com/dhylands/python_lcd) | `mpremote mip install github:dhylands/python_lcd/lcd/lcd_api.py` |
| `esp8266_i2c_lcd` | [dhylands/python_lcd](https://github.com/dhylands/python_lcd) | `mpremote mip install github:dhylands/python_lcd/lcd/esp8266_i2c_lcd.py` |

## IDE Support

MicroPython stubs (`micropython-esp32-stubs`) are installed as dev dependencies, providing autocompletion and type hints for MicroPython modules like `machine`, `network`, etc. The `pyrightconfig.json` configures Pylance/Pyright to use these stubs.
