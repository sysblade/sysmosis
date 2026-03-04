from machine import Pin, ADC, SoftI2C, WDT, PWM, Timer
from esp8266_i2c_lcd import I2cLcd
from metrics import handle_metrics
import network
import time
import random

# Ensure alarms.py exists on your ESP32 root with the non-blocking logic
from alarms import trigger_alarm, clear_alarm, update_alarm_async, ALARMS

# ==========================================
# 1. CONFIGURATION
# ==========================================
try:
    import config as cfg
except ImportError:
    cfg = None

def get_config(name, default):
    if cfg is None:
        return default
    return getattr(cfg, name, default)

PIN_LCD_SCL = get_config("PIN_LCD_SCL", 22)
PIN_LCD_SDA = get_config("PIN_LCD_SDA", 21)
PIN_PUMP = get_config("PIN_PUMP", 12)
PIN_INLET_VALVE = get_config("PIN_INLET_VALVE", 13)
PIN_FLUSH_VALVE = get_config("PIN_FLUSH_VALVE", 14)
PIN_LOW_PRESSURE = get_config("PIN_LOW_PRESSURE", 25)
PIN_HIGH_PRESSURE = get_config("PIN_HIGH_PRESSURE", 26)
PIN_LEAK_SENSOR = get_config("PIN_LEAK_SENSOR", 27)
PIN_TDS_SENSOR = get_config("PIN_TDS_SENSOR", 34)

# ==========================================
# 2. STATE
# ==========================================
class State:
    STANDBY     = 0
    RUNNING     = 1
    FLUSHING    = 2
    EMERGENCY   = 3
    MAINTENANCE = 4

# ==========================================
# 3. GLOBAL STATE
# ==========================================
leak_detected = False
wifi_connected = False
last_wifi_check = 0
production_time = 0
last_flush_time = 0
system_state = State.STANDBY
metrics_production_total = 0
metrics_flush_cycles = 0
metrics_wifi_reconnects = 0
metrics_server = None
start_time = time.time()
last_loop_time = time.time()

# Flush state
startup_flush_done = False
flush_reason = ""
flush_start_time = 0
flush_duration = 0
last_standby_start = time.time()
next_inactivity_flush_in = 0  # seconds until next inactivity flush (set at boot)

# ==========================================
# 4. HARDWARE INIT
# ==========================================
i2c = SoftI2C(scl=Pin(PIN_LCD_SCL), sda=Pin(PIN_LCD_SDA), freq=400000)
lcd = I2cLcd(i2c, get_config("LCD_I2C_ADDR", 0x27), 4, 20)

pump = Pin(PIN_PUMP, Pin.OUT, value=0)
inlet_v = Pin(PIN_INLET_VALVE, Pin.OUT, value=0)
flush_v = Pin(PIN_FLUSH_VALVE, Pin.OUT, value=0)

lps = Pin(PIN_LOW_PRESSURE, Pin.IN, Pin.PULL_UP)
hps = Pin(PIN_HIGH_PRESSURE, Pin.IN, Pin.PULL_UP)
leak = Pin(PIN_LEAK_SENSOR, Pin.IN, Pin.PULL_UP)
tds_sensor = ADC(Pin(PIN_TDS_SENSOR))
tds_sensor.atten(ADC.ATTN_11DB)

# ==========================================
# 5. IRQ & HELPERS
# ==========================================
def _emergency_shutdown(pin):
    global leak_detected, system_state
    pump.value(0)
    inlet_v.value(0)
    flush_v.value(0)
    leak_detected = True
    system_state = State.EMERGENCY

leak.irq(trigger=Pin.IRQ_FALLING, handler=_emergency_shutdown)

def update_display(l1="", l2="", l3="", l4=""):
    lcd.move_to(0, 0)
    lcd.putstr(l1.ljust(20))
    lcd.move_to(0, 1)
    lcd.putstr(l2.ljust(20))
    lcd.move_to(0, 2)
    lcd.putstr(l3.ljust(20))
    lcd.move_to(0, 3)
    lcd.putstr(l4.ljust(20))

def get_tds():
    val = tds_sensor.read()
    return int(val * get_config("TDS_FACTOR", 0.5) + get_config("TDS_OFFSET", 0))

# ==========================================
# 6. FLUSH HELPERS
# ==========================================
def _schedule_next_inactivity_flush():
    """Pick a random interval between min and max for the next inactivity flush."""
    global next_inactivity_flush_in
    min_i = get_config("FLUSH_INACTIVITY_INTERVAL_MIN", 24 * 3600)
    max_i = get_config("FLUSH_INACTIVITY_INTERVAL_MAX", 48 * 3600)
    next_inactivity_flush_in = random.randint(min_i, max_i)

def start_flush(reason, duration):
    """Stop any active production and start a flush cycle."""
    global system_state, flush_reason, flush_start_time, flush_duration, last_flush_time
    pump.value(0)
    inlet_v.value(0)
    time.sleep_ms(500)
    flush_v.value(1)
    pump.value(1)
    flush_reason = reason
    flush_start_time = time.time()
    flush_duration = duration
    last_flush_time = flush_start_time
    system_state = State.FLUSHING

def stop_flush():
    """End the current flush cycle and return to standby."""
    global system_state, metrics_flush_cycles, last_standby_start
    pump.value(0)
    flush_v.value(0)
    system_state = State.STANDBY
    metrics_flush_cycles += 1
    last_standby_start = time.time()
    _schedule_next_inactivity_flush()

def enter_maintenance():
    """Stop all automated hardware and enter maintenance mode for manual control."""
    global system_state, metrics_production_total, production_time, last_standby_start
    if system_state == State.RUNNING:
        pump.value(0)
        time.sleep(1)
        inlet_v.value(0)
        metrics_production_total += production_time
        production_time = 0
    elif system_state == State.FLUSHING:
        pump.value(0)
        flush_v.value(0)
    system_state = State.MAINTENANCE

def exit_maintenance():
    """Return to standby from maintenance mode."""
    global system_state, last_standby_start
    if system_state == State.MAINTENANCE:
        system_state = State.STANDBY
        last_standby_start = time.time()  # Reset inactivity timer

# Schedule the first inactivity flush interval
_schedule_next_inactivity_flush()

# ==========================================
# 7. MAIN LOGIC
# ==========================================
update_display("RO SYSTEM v1.0", "Initializing...", "Sensors: OK", "Ready.")

wdt = WDT(timeout=get_config("WATCHDOG_TIMEOUT", 30000))

while True:
    wdt.feed()
    update_alarm_async()
    handle_metrics()

    # 1. LEAK LOGIC (Critical)
    if leak_detected or leak.value() == 0:
        trigger_alarm('LEAK')
        while True:
            wdt.feed()
            update_alarm_async()
            time.sleep_ms(10)

    # 2. SOURCE WATER LOGIC
    if lps.value() == 1:  # No pressure
        trigger_alarm('LOW_PRESSURE')
    else:
        clear_alarm('LOW_PRESSURE')

    # 3. TDS LOGIC
    if get_tds() > get_config("TDS_THRESHOLD", 100):
        trigger_alarm('TDS_HIGH')
    else:
        clear_alarm('TDS_HIGH')

    # 4. TIMING
    now = time.time()
    delta = now - last_loop_time
    last_loop_time = now

    source_water = (lps.value() == 0)
    faucet_open = (hps.value() == 0)
    current_tds = get_tds()

    # 5. MAINTENANCE MODE: bypass all automation
    if system_state == State.MAINTENANCE:
        p  = "ON " if pump.value()    else "OFF"
        iv = "ON " if inlet_v.value() else "OFF"
        fv = "ON " if flush_v.value() else "OFF"
        update_display(
            "STATUS: MAINTENANCE",
            f"P:{p} IV:{iv} FV:{fv}",
            f"TDS: {current_tds} PPM",
            "WiFi: " + ("ON" if wifi_connected else "OFF"),
        )
        continue

    # 6. FLUSH LOGIC

    # 6a. Trigger startup flush (deferred until source water is available)
    if not startup_flush_done and source_water:
        startup_flush_done = True
        start_flush("Startup", get_config("FLUSH_STARTUP_DURATION", 20))

    # 6b. Handle active flush: update display and check for completion
    if system_state == State.FLUSHING:
        elapsed = now - flush_start_time
        remaining = max(0, flush_duration - elapsed)
        update_display(
            "STATUS: FLUSHING",
            f"Reason: {flush_reason}",
            f"Rem:{int(remaining):3d}s Tot:{flush_duration:3d}s",
            "WiFi: " + ("ON" if wifi_connected else "OFF"),
        )
        if remaining <= 0:
            stop_flush()
        continue  # Skip production logic while flushing

    # 6c. Trigger inactivity flush (standby only, source water required)
    if system_state == State.STANDBY and source_water:
        standby_secs = now - last_standby_start
        if standby_secs >= next_inactivity_flush_in:
            start_flush("Inactivity", get_config("FLUSH_INACTIVITY_DURATION", 60))
            continue

    # 6d. Trigger production-interval flush (optional, disabled when interval=0)
    production_flush_interval = get_config("FLUSH_PRODUCTION_INTERVAL", 0)
    if (production_flush_interval > 0
            and system_state == State.STANDBY
            and source_water
            and (now - last_flush_time) >= production_flush_interval):
        start_flush("Production", get_config("FLUSH_PRODUCTION_DURATION", 30))
        continue

    # 7. PRODUCTION LOGIC
    if source_water and faucet_open:
        if system_state != State.RUNNING:
            update_display("STATUS: STARTING", "Opening Inlet...", "", "")
            inlet_v.value(1)
            time.sleep(1)
            pump.value(1)
            system_state = State.RUNNING
        production_time += delta

    elif not faucet_open or not source_water:
        if system_state == State.RUNNING:
            pump.value(0)
            time.sleep(1)
            inlet_v.value(0)
            system_state = State.STANDBY
            metrics_production_total += production_time
            production_time = 0
            last_standby_start = time.time()  # Reset inactivity timer on production stop

    # 8. DISPLAY REFRESH
    if system_state == State.RUNNING:
        update_display(
            "STATUS: RUNNING",
            f"TDS: {current_tds} PPM",
            f"Time: {int(production_time//60)}m {int(production_time%60)}s",
            "WiFi: " + ("ON" if wifi_connected else "OFF")
        )
    else:
        update_display(
            "STATUS: STANDBY",
            "System Ready",
            "Waiting for Faucet",
            f"Last TDS: {current_tds}PPM"
        )
