# Krosmosis

ESP32 MicroPython controller for a reverse osmosis (RO) water filtration system.

## Features

- Automatic pump and valve control based on pressure sensors
- Real-time TDS (Total Dissolved Solids) monitoring with configurable calibration
- 20x4 LCD status display
- Emergency leak detection with IRQ-based instant shutoff and audible alarm
- Low/high pressure sensor monitoring
- Automatic membrane flush cycles to extend RO membrane life
- Audio feedback (buzzer) for system status
- Optional WiFi with OTA (Over-The-Air) updates via WebREPL
- WiFi auto-reconnect on connection loss
- Hardware watchdog timer for automatic recovery from hangs
- Prometheus metrics endpoint for monitoring and alerting
- Fully configurable via `config.py` (GPIO pins, timings, calibration)

## Hardware Requirements

Default GPIO assignments (configurable in `config.py`):

| Component | GPIO | Description |
|-----------|------|-------------|
| LCD (I2C) | SCL=22, SDA=21 | 20x4 character display (I2C address 0x27) |
| Pump Relay | 12 | Booster pump control |
| Inlet Valve | 13 | Source water inlet solenoid |
| Flush Valve | 14 | Membrane flush solenoid |
| Low Pressure Sensor | 25 | Source water pressure (opto-isolated, active low) |
| High Pressure Sensor | 26 | Tank/faucet pressure (opto-isolated, active low) |
| Leak Sensor | 27 | Water leak detection (opto-isolated, active low) |
| Buzzer | 32 | Active buzzer for audio alerts |
| TDS Sensor | 34 | Analog water quality measurement |

## Setup

### Prerequisites

- ESP32 with MicroPython firmware installed
- Python 3.12+ on your development machine
- [uv](https://github.com/astral-sh/uv) package manager

### Install Development Tools

```bash
uv sync --extra dev
```

### Install MicroPython Libraries on ESP32

Connect your ESP32 via USB, then install the LCD library:

```bash
mpremote connect /dev/ttyUSB0 mip install github:dhylands/python_lcd/lcd/esp8266_i2c_lcd.py
```

## Local Development

### Linting

```bash
uv run ruff check .
uv run ruff check --fix .  # Auto-fix issues
```

### Pre-compile to Bytecode (Optional)

MicroPython can run `.py` files directly, but you can optionally pre-compile to `.mpy` bytecode for faster loading and reduced memory usage:

```bash
# Install mpy-cross compiler
pip install mpy-cross

# Compile Python files to bytecode
mpy-cross main.py -o main.mpy
mpy-cross metrics.py -o metrics.mpy

# Upload compiled files instead
mpremote connect /dev/ttyUSB0 cp main.mpy :main.mpy
mpremote connect /dev/ttyUSB0 cp metrics.mpy :metrics.mpy
```

**Note:** When using `.mpy` files, remove the corresponding `.py` files from the device to avoid conflicts.

### Type Checking

The project includes MicroPython type stubs for IDE support:

```bash
# Type check with pyright (used by VS Code Pylance)
uv run pyright main.py metrics.py
```

## Deployment

### USB Upload

Upload the scripts to the ESP32:

```bash
# Upload all Python files
mpremote connect /dev/ttyUSB0 cp main.py :main.py
mpremote connect /dev/ttyUSB0 cp metrics.py :metrics.py

# Restart the device
mpremote connect /dev/ttyUSB0 reset
```

### OTA Upload (WebREPL)

WiFi and WebREPL are built into `main.py` and start automatically if configured.

**1. Create configuration file:**

```bash
cp config.py.example config.py
```

Edit `config.py` with your WiFi credentials:

```python
WIFI_SSID = "YourNetworkName"
WIFI_PASSWORD = "YourPassword"
WEBREPL_PASSWORD = "micropython"  # 4-9 characters
WIFI_TIMEOUT = 10
```

**2. Initial upload via USB (one-time):**

```bash
mpremote connect /dev/ttyUSB0 cp config.py :config.py
mpremote connect /dev/ttyUSB0 cp main.py :main.py
mpremote connect /dev/ttyUSB0 cp metrics.py :metrics.py
mpremote connect /dev/ttyUSB0 reset
```

**3. Subsequent uploads via WebREPL:**

Find the device IP in serial output or your router's DHCP table, then:

```bash
# Using webrepl_cli (install with: pip install webrepl)
webrepl_cli.py -p YOUR_WEBREPL_PASSWORD main.py 192.168.1.100:/main.py
```

Or use the WebREPL web client at http://micropython.org/webrepl/

**Note:** WiFi is optional. If `config.py` is missing or WiFi fails to connect, the RO system continues to operate normally without network features.

## Monitoring (Prometheus)

When WiFi is connected, the device exposes a Prometheus-compatible metrics endpoint:

```
http://<device-ip>:8080/metrics
```

### Available Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `krosmosis_tds_ppm` | gauge | Current TDS reading in PPM |
| `krosmosis_system_state` | gauge | System state (standby/running/flushing/emergency) |
| `krosmosis_pressure_low` | gauge | Low pressure sensor state |
| `krosmosis_pressure_high` | gauge | High pressure sensor state |
| `krosmosis_leak_detected` | gauge | Leak detection state |
| `krosmosis_pump_active` | gauge | Pump relay state |
| `krosmosis_production_seconds` | gauge | Current cycle production time |
| `krosmosis_production_total_seconds` | counter | Total cumulative production time |
| `krosmosis_flush_cycles_total` | counter | Number of flush cycles completed |
| `krosmosis_uptime_seconds` | counter | System uptime |
| `krosmosis_wifi_connected` | gauge | WiFi connection state |

### Example Prometheus scrape config

```yaml
scrape_configs:
  - job_name: 'krosmosis'
    static_configs:
      - targets: ['192.168.1.100:8080']
    scrape_interval: 15s
```

### Alerting Examples

```yaml
groups:
  - name: krosmosis
    rules:
      - alert: ROLeakDetected
        expr: krosmosis_leak_detected == 1
        for: 0m
        labels:
          severity: critical
        annotations:
          summary: "Water leak detected!"

      - alert: ROHighTDS
        expr: krosmosis_tds_ppm > 50
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "TDS reading above threshold - check membrane"
```

## Debugging

### Interactive REPL

Connect to the ESP32's Python REPL for interactive debugging:

```bash
mpremote connect /dev/ttyUSB0
```

You can now run Python commands directly on the device.

### View Serial Output

Monitor print statements and errors:

```bash
# Using mpremote
mpremote connect /dev/ttyUSB0 repl

# Or using screen
screen /dev/ttyUSB0 115200

# Or using minicom
minicom -D /dev/ttyUSB0 -b 115200
```

### Soft Reset

Reset the device without power cycling:

```bash
mpremote connect /dev/ttyUSB0 reset
```

Or in the REPL, press `Ctrl+D`.

### Debug Tips

- **Import errors**: Ensure libraries are installed on the device with `mip`
- **Pin conflicts**: Verify no GPIO pins are shared between components
- **I2C issues**: Run `i2c.scan()` in REPL to verify LCD address (should return `[39]` for 0x27)
- **Sensor readings**: Test individual sensors in REPL before running main loop

```python
# Example: Test TDS sensor in REPL
from machine import Pin, ADC
tds = ADC(Pin(34))
tds.atten(ADC.ATTN_11DB)
print(tds.read())
```

## License

MIT
