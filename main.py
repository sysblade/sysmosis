from machine import Pin, ADC, SoftI2C, WDT
from esp8266_i2c_lcd import I2cLcd  # Common library for 20x4 I2C LCD
import network
import time

# ==========================================
# 1. CONFIGURATION WITH DEFAULTS
# ==========================================

# Try to load config, use defaults if not present
try:
    import config as cfg
except ImportError:
    cfg = None  # type: ignore[assignment]


def get_config(name: str, default: any) -> any:
    """Get config value with fallback to default."""
    if cfg is None:
        return default
    return getattr(cfg, name, default)


# Hardware pins (from config or defaults)
PIN_LCD_SCL = get_config("PIN_LCD_SCL", 22)
PIN_LCD_SDA = get_config("PIN_LCD_SDA", 21)
LCD_I2C_ADDR = get_config("LCD_I2C_ADDR", 0x27)
LCD_ROWS = get_config("LCD_ROWS", 4)
LCD_COLS = get_config("LCD_COLS", 20)

PIN_PUMP = get_config("PIN_PUMP", 12)
PIN_INLET_VALVE = get_config("PIN_INLET_VALVE", 13)
PIN_FLUSH_VALVE = get_config("PIN_FLUSH_VALVE", 14)
PIN_BUZZER = get_config("PIN_BUZZER", 32)

PIN_LOW_PRESSURE = get_config("PIN_LOW_PRESSURE", 25)
PIN_HIGH_PRESSURE = get_config("PIN_HIGH_PRESSURE", 26)
PIN_LEAK_SENSOR = get_config("PIN_LEAK_SENSOR", 27)
PIN_TDS_SENSOR = get_config("PIN_TDS_SENSOR", 34)

# TDS calibration
TDS_FACTOR = get_config("TDS_FACTOR", 0.5)
TDS_OFFSET = get_config("TDS_OFFSET", 0)

# Flush cycle settings
FLUSH_INTERVAL = get_config("FLUSH_INTERVAL", 3600)  # seconds of production
FLUSH_DURATION = get_config("FLUSH_DURATION", 30)  # seconds

# WiFi settings
WIFI_RECONNECT_INTERVAL = get_config("WIFI_RECONNECT_INTERVAL", 60)

# Watchdog
WATCHDOG_TIMEOUT = get_config("WATCHDOG_TIMEOUT", 30000)  # ms

# Metrics server
METRICS_ENABLED = get_config("METRICS_ENABLED", True)
METRICS_PORT = get_config("METRICS_PORT", 8080)

# ==========================================
# 2. GLOBAL STATE
# ==========================================

# System state
leak_detected = False  # Set by IRQ, read by main loop
wifi_connected = False
last_wifi_check = 0
production_time = 0  # Current cycle production time in seconds
last_flush_time = 0  # Time of last flush cycle
system_state = 0  # 0=standby, 1=running, 2=flushing, 3=emergency
start_time = time.time()  # For uptime tracking

# Metrics counters (cumulative)
metrics_production_total = 0  # Total production time (all cycles)
metrics_flush_cycles = 0  # Number of flush cycles
metrics_wifi_reconnects = 0  # WiFi reconnection count

# Metrics server instance
metrics_server = None

# ==========================================
# 3. WIFI & OTA
# ==========================================


def connect_wifi() -> bool:
    """Attempt WiFi connection. Returns True if connected, False otherwise."""
    if cfg is None:
        print("WiFi: No config.py found, skipping WiFi setup")
        return False

    ssid = getattr(cfg, "WIFI_SSID", None)
    password = getattr(cfg, "WIFI_PASSWORD", None)
    if not ssid or not password:
        print("WiFi: SSID or password not configured")
        return False

    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)

    if wlan.isconnected():
        print(f"WiFi: Already connected - {wlan.ifconfig()[0]}")
        return True

    print(f"WiFi: Connecting to {ssid}...")
    wlan.connect(ssid, password)

    timeout = getattr(cfg, "WIFI_TIMEOUT", 10)
    start = time.time()
    while not wlan.isconnected():
        if time.time() - start > timeout:
            print("WiFi: Connection timeout, continuing without WiFi")
            return False
        time.sleep(0.5)

    print(f"WiFi: Connected - {wlan.ifconfig()[0]}")
    return True


def check_wifi_reconnect() -> bool:
    """Check WiFi status and reconnect if needed. Returns True if connected."""
    global wifi_connected, last_wifi_check, metrics_wifi_reconnects

    now = time.time()
    if now - last_wifi_check < WIFI_RECONNECT_INTERVAL:
        return wifi_connected

    last_wifi_check = now
    wlan = network.WLAN(network.STA_IF)

    if wlan.isconnected():
        if not wifi_connected:
            print(f"WiFi: Reconnected - {wlan.ifconfig()[0]}")
            wifi_connected = True
            metrics_wifi_reconnects += 1
        return True

    # Not connected, try to reconnect
    if wifi_connected:
        print("WiFi: Connection lost, attempting reconnect...")
    wifi_connected = connect_wifi()
    if wifi_connected:
        metrics_wifi_reconnects += 1
    return wifi_connected


def start_webrepl() -> bool:
    """Start WebREPL for OTA updates if WiFi is connected."""
    try:
        import webrepl

        password = get_config("WEBREPL_PASSWORD", "micropython")
        webrepl.start(password=password)
        print("WebREPL: Started - OTA updates enabled")
        return True
    except ImportError:
        print("WebREPL: Module not available")
        return False
    except Exception as e:
        print(f"WebREPL: Failed to start - {e}")
        return False


# ==========================================
# 4. METRICS SETUP
# ==========================================


def init_metrics_server():
    """Initialize the Prometheus metrics server."""
    global metrics_server

    if not METRICS_ENABLED:
        return None

    try:
        from metrics import MetricsServer, create_system_collector

        metrics_server = MetricsServer(port=METRICS_PORT)

        # Create collector with callbacks to get current state
        collector = create_system_collector(
            get_state=lambda: {
                "system_state": system_state,
                "production_time": production_time,
                "wifi_connected": wifi_connected,
                "time_to_flush": max(0, FLUSH_INTERVAL - production_time)
                if FLUSH_INTERVAL > 0
                else 0,
            },
            get_tds=get_tds,
            get_sensors=lambda: {
                "lps": 1 if lps.value() == 0 else 0,
                "hps": 1 if hps.value() == 0 else 0,
                "leak": 1 if leak_detected or leak.value() == 0 else 0,
            },
            get_relays=lambda: {
                "pump": pump.value(),
                "inlet_v": inlet_v.value(),
                "flush_v": flush_v.value(),
            },
            get_counters=lambda: {
                "production_total": metrics_production_total,
                "flush_cycles": metrics_flush_cycles,
                "wifi_reconnects": metrics_wifi_reconnects,
            },
            start_time=start_time,
        )

        metrics_server.register_collector(collector)

        if metrics_server.start():
            return metrics_server
        return None
    except ImportError:
        print("Metrics: Module not available")
        return None
    except Exception as e:
        print(f"Metrics: Failed to initialize - {e}")
        return None


def handle_metrics():
    """Handle metrics server requests (non-blocking)."""
    if metrics_server:
        metrics_server.handle_request()


# ==========================================
# 5. HARDWARE INITIALIZATION
# ==========================================

# I2C for LCD
i2c = SoftI2C(scl=Pin(PIN_LCD_SCL), sda=Pin(PIN_LCD_SDA), freq=400000)
lcd = I2cLcd(i2c, LCD_I2C_ADDR, LCD_ROWS, LCD_COLS)

# Relays (Outputs)
pump = Pin(PIN_PUMP, Pin.OUT, value=0)
inlet_v = Pin(PIN_INLET_VALVE, Pin.OUT, value=0)
flush_v = Pin(PIN_FLUSH_VALVE, Pin.OUT, value=0)

# Active Buzzer (Output)
buzzer = Pin(PIN_BUZZER, Pin.OUT, value=0)

# Opto-Inputs (NPN Active Low)
lps = Pin(PIN_LOW_PRESSURE, Pin.IN, Pin.PULL_UP)  # Low Pressure
hps = Pin(PIN_HIGH_PRESSURE, Pin.IN, Pin.PULL_UP)  # High Pressure
leak = Pin(PIN_LEAK_SENSOR, Pin.IN, Pin.PULL_UP)  # Leak Sensor

# Analog TDS Sensor
tds_sensor = ADC(Pin(PIN_TDS_SENSOR))
tds_sensor.atten(ADC.ATTN_11DB)


# ==========================================
# 6. IRQ HANDLERS
# ==========================================


def _emergency_shutdown(pin: Pin) -> None:
    """IRQ handler for leak detection - immediately cuts all water flow.

    This runs at interrupt level, so it must be fast and minimal.
    LCD updates are handled in the main loop via the leak_detected flag.
    """
    global leak_detected, system_state
    # Immediately cut power to all water control outputs
    pump.value(0)
    inlet_v.value(0)
    flush_v.value(0)
    # Start alarm buzzer
    buzzer.value(1)
    leak_detected = True
    system_state = 3  # Emergency


# Register IRQ for leak detection (falling edge = leak sensor activated)
leak.irq(trigger=Pin.IRQ_FALLING, handler=_emergency_shutdown)


# ==========================================
# 7. HELPER FUNCTIONS
# ==========================================


def update_display(line1: str = "", line2: str = "", line3: str = "", line4: str = "") -> None:
    """Update the 20x4 LCD efficiently."""
    lcd.move_to(0, 0)
    lcd.putstr(line1.ljust(LCD_COLS))
    lcd.move_to(0, 1)
    lcd.putstr(line2.ljust(LCD_COLS))
    lcd.move_to(0, 2)
    lcd.putstr(line3.ljust(LCD_COLS))
    lcd.move_to(0, 3)
    lcd.putstr(line4.ljust(LCD_COLS))


def get_tds() -> int:
    """Read TDS sensor and convert to PPM using calibration values."""
    val = tds_sensor.read()
    ppm = int(val * TDS_FACTOR + TDS_OFFSET)
    return ppm


def beep(duration_ms: int = 100, count: int = 1, interval_ms: int = 100) -> None:
    """Sound the buzzer with specified pattern."""
    for i in range(count):
        buzzer.value(1)
        time.sleep_ms(duration_ms)
        buzzer.value(0)
        if i < count - 1:
            time.sleep_ms(interval_ms)


def run_flush_cycle(wdt: WDT | None = None) -> None:
    """Run membrane flush cycle to extend RO membrane life."""
    global last_flush_time, metrics_flush_cycles, system_state

    print("Flush: Starting membrane flush cycle")
    system_state = 2  # Flushing
    update_display("STATUS: FLUSHING", "Membrane Flush", f"Duration: {FLUSH_DURATION}s", "")

    # Open flush valve, keep pump running
    flush_v.value(1)

    # Wait for flush duration, feeding watchdog and handling metrics
    for _ in range(FLUSH_DURATION):
        if wdt:
            wdt.feed()
        handle_metrics()
        time.sleep(1)

    # Close flush valve
    flush_v.value(0)
    last_flush_time = time.time()
    metrics_flush_cycles += 1
    print("Flush: Cycle complete")


def should_flush() -> bool:
    """Check if it's time for a flush cycle based on production time."""
    if FLUSH_INTERVAL <= 0:
        return False
    return production_time >= FLUSH_INTERVAL and (time.time() - last_flush_time) > FLUSH_INTERVAL


# ==========================================
# 8. MAIN LOGIC
# ==========================================

print("System Initializing...")

# Initialize WiFi
wifi_connected = connect_wifi()
if wifi_connected:
    start_webrepl()
    metrics_server = init_metrics_server()

# Initialize display
lcd.backlight_on()
update_display("RO SYSTEM v1.0", "Initializing...", "Check Sensors: OK", "Ready.")
time.sleep(2)

# Ready beep (two short beeps)
beep(duration_ms=100, count=2, interval_ms=100)

# Initialize watchdog timer (will reset device if not fed)
wdt = WDT(timeout=WATCHDOG_TIMEOUT)
print(f"Watchdog: Enabled with {WATCHDOG_TIMEOUT}ms timeout")

system_active = False
last_loop_time = time.time()

while True:
    # Feed the watchdog to prevent reset
    wdt.feed()

    # Handle metrics requests (non-blocking)
    handle_metrics()

    # Calculate loop delta time for production tracking
    now = time.time()
    delta = now - last_loop_time
    last_loop_time = now

    # Check for leak (IRQ handles immediate shutdown, we handle display/halt)
    if leak_detected or leak.value() == 0:
        system_state = 3  # Emergency
        # Ensure outputs are off (IRQ should have done this, but be safe)
        pump.value(0)
        inlet_v.value(0)
        flush_v.value(0)
        update_display(
            "!!! EMERGENCY !!!",
            "LEAK DETECTED!",
            "Water Cut Off.",
            "Please check unit."
        )
        # Continuous alarm beeping until manual reset
        while True:
            wdt.feed()  # Keep feeding watchdog during alarm
            handle_metrics()  # Keep metrics available
            buzzer.value(1)
            time.sleep_ms(500)
            buzzer.value(0)
            time.sleep_ms(500)

    # Periodic WiFi reconnect check
    if check_wifi_reconnect() and metrics_server is None:
        # WiFi reconnected, try to start metrics server
        metrics_server = init_metrics_server()

    # Read sensor states
    source_water = lps.value() == 0  # True if pressure exists
    faucet_open = hps.value() == 0  # True if pressure dropped (tap open)

    # Logic: Start Production
    if source_water and faucet_open:
        if not system_active:
            update_display("STATUS: STARTING", "Opening Inlet...", "", "")
            inlet_v.value(1)
            time.sleep(1)
            wdt.feed()
            update_display("STATUS: RUNNING", "Pump: ON", "Source: OK", "")
            pump.value(1)
            system_active = True
            system_state = 1  # Running

    # Logic: Stop Production
    elif not faucet_open or not source_water:
        if system_active:
            # Check if flush is needed before stopping
            if should_flush():
                run_flush_cycle(wdt)

            reason = "Tank Full" if not faucet_open else "No Source"
            update_display("STATUS: STOPPING", f"Reason: {reason}", "", "")
            pump.value(0)
            time.sleep(1)
            wdt.feed()
            inlet_v.value(0)
            system_active = False
            system_state = 0  # Standby
            metrics_production_total += production_time  # Add to total
            production_time = 0  # Reset current cycle counter

    # Track production time and check for flush
    if system_active:
        production_time += delta

        # Mid-production flush check (for very long production cycles)
        if should_flush():
            run_flush_cycle(wdt)
            system_state = 1  # Back to running after flush

        ppm = get_tds()
        mins = int(production_time // 60)
        update_display(
            "STATUS: RUNNING",
            f"Pure Water: {ppm} PPM",
            f"Run Time: {mins} min",
            f"Next Flush: {int((FLUSH_INTERVAL - production_time) // 60)}m"
        )
    else:
        update_display(
            "STATUS: STANDBY",
            "System Ready",
            "Waiting for Faucet",
            f"Last TDS: {get_tds()}PPM"
        )

    time.sleep(0.5)
