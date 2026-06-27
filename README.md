# Sysmosis

ESP32 MicroPython controller for a reverse osmosis (RO) water filtration system.

## Features

- Automatic pump and valve control based on pressure sensors
- Real-time TDS (Total Dissolved Solids) monitoring with configurable calibration
- 20x4 LCD status display
- Emergency leak detection with IRQ-based instant shutoff and audible alarm
- Low/high pressure sensor monitoring
- Automatic membrane flush cycles to extend RO membrane life
- Audio feedback (buzzer) for system status
- Web interface for real-time monitoring and control (port 80)
- Password-protected web UI with cookie-based session authentication
- Optional HTTPS for the web interface (self-signed certificate)
- WiFi auto-reconnect on connection loss
- Hardware watchdog timer for automatic recovery from hangs
- Prometheus metrics endpoint for monitoring and alerting
- Fully configurable via `config.py` (GPIO pins, timings, calibration)

## Hardware Requirements

Default GPIO assignments (configurable in `config.py`):

| Component | GPIO | Description |
|-----------|------|-------------|
| LCD (I2C) | SCL=23, SDA=21 | 20x4 character display (I2C address 0x27) |
| Pump Relay | 12 | Booster pump control |
| Inlet Valve | 13 | Source water inlet solenoid |
| Flush Valve | 14 | Membrane flush solenoid |
| Low Pressure Sensor | 33 | Source water pressure (opto-isolated, active low) |
| High Pressure Sensor | 32 | Tank/faucet pressure (opto-isolated, active low) |
| Leak Sensor | 15 | Water leak detection (opto-isolated, active low) |
| Buzzer | 16 | Active buzzer for audio alerts |
| TDS Sensor | 2 | Analog water quality measurement |

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

Connect your ESP32 via USB, then install the LCD libraries (`lcd_api` must be installed first as it is a dependency):

```bash
mpremote connect /dev/ttyUSB0 mip install github:dhylands/python_lcd/lcd/lcd_api.py
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

Upload all files to the ESP32 using the deploy script:

```bash
uv run deploy.py
# or specify a different port:
uv run deploy.py /dev/ttyACM0
```

The script uploads `main.py`, `webserver.py`, `metrics.py`, `alarms.py`, and all files in `static/`. If `config.py` exists locally it is included automatically.

### WiFi / OTA Setup

WiFi starts automatically if configured. WebREPL has been removed — use `mpremote` for all file uploads.

**1. Create configuration file:**

```bash
cp config.py.example config.py
```

Edit `config.py` with your WiFi credentials:

```python
WIFI_SSID = "YourNetworkName"
WIFI_PASSWORD = "YourPassword"
WIFI_TIMEOUT = 10
```

**2. Upload all files via USB:**

```bash
uv run deploy.py
```

If `config.py` exists it is included automatically. To use a different port: `uv run deploy.py /dev/ttyACM0`.

**3. Subsequent uploads over WiFi (mpremote):**

```bash
# mpremote also works over network via mDNS or IP
mpremote connect /dev/ttyUSB0 cp main.py :main.py
mpremote connect /dev/ttyUSB0 reset
```

**Note:** WiFi is optional. If `config.py` is missing or WiFi fails to connect, the RO system continues to operate normally without network features.

## Web Interface

When WiFi is connected, a web UI is available at `http://<device-ip>/` (default port 80).

The interface shows:
- Current system state (STANDBY / RUNNING / FLUSHING / EMERGENCY / MAINTENANCE) with live updates
- LCD display mirror
- Sensor readings (source water, faucet, TDS, LPS, HPS, leak)
- Relay states (pump, inlet valve, flush valve)
- Timing info (uptime, production time, flush cycles)

Controls available from the UI:
- Toggle maintenance mode (also bindable to key `M`)
- Trigger a manual flush (key `F`)
- Reset the device (key `R`)
- Individual relay control (pump, inlet valve, flush valve) when in maintenance mode

### Authentication

Set `WEB_AUTH_PASSWORD` in `config.py` to protect the UI with a password. A login page is shown to unauthenticated users; a session cookie is issued on success. Leave the key unset (or empty) to disable authentication.

```python
WEB_AUTH_PASSWORD = "changeme"
```

### HTTPS (optional)

Generate a self-signed certificate on your PC and upload it to the device:

```bash
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
            -days 3650 -nodes -subj "/CN=sysmosis"

mpremote connect /dev/ttyUSB0 cp cert.pem :cert.pem
mpremote connect /dev/ttyUSB0 cp key.pem  :key.pem
```

Then enable it in `config.py`:

```python
WEB_HTTPS = True
WEB_PORT  = 443
```

The browser will show a certificate warning for self-signed certs — accept it to proceed. The Prometheus metrics endpoint (port 8080) is unaffected and remains plain HTTP.

## Monitoring (Prometheus)

When WiFi is connected, the device exposes a Prometheus-compatible metrics endpoint:

```
http://<device-ip>:8080/metrics
```

### Available Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `sysmosis_tds_ppm` | gauge | Current TDS reading in PPM |
| `sysmosis_system_state` | gauge | System state (standby/running/flushing/emergency) |
| `sysmosis_pressure_low` | gauge | Low pressure sensor state |
| `sysmosis_pressure_high` | gauge | High pressure sensor state |
| `sysmosis_leak_detected` | gauge | Leak detection state |
| `sysmosis_pump_active` | gauge | Pump relay state |
| `sysmosis_production_seconds` | gauge | Current cycle production time |
| `sysmosis_production_total_seconds` | counter | Total cumulative production time |
| `sysmosis_flush_cycles_total` | counter | Number of flush cycles completed |
| `sysmosis_uptime_seconds` | counter | System uptime |
| `sysmosis_wifi_connected` | gauge | WiFi connection state |

### Example Prometheus scrape config

```yaml
scrape_configs:
  - job_name: 'sysmosis'
    static_configs:
      - targets: ['192.168.1.100:8080']
    scrape_interval: 15s
```

### Alerting Examples

```yaml
groups:
  - name: sysmosis
    rules:
      - alert: ROLeakDetected
        expr: sysmosis_leak_detected == 1
        for: 0m
        labels:
          severity: critical
        annotations:
          summary: "Water leak detected!"

      - alert: ROHighTDS
        expr: sysmosis_tds_ppm > 50
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
tds = ADC(Pin(2))
tds.atten(ADC.ATTN_11DB)
print(tds.read())
```

## License

MIT
