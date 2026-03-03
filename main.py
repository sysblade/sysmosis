from machine import Pin, ADC, SoftI2C, WDT, PWM, Timer
from esp8266_i2c_lcd import I2cLcd
from metrics import handle_metrics
import network
import time

# Ensure alarms.py exists on your ESP32 root with the non-blocking logic
from alarms import trigger_alarm, clear_alarm, update_alarm_async, ALARMS

# ==========================================
# 1. CONFIGURATION (Restored from your code)
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
# 2. GLOBAL STATE (Fully Restored)
# ==========================================
leak_detected = False
wifi_connected = False
last_wifi_check = 0
production_time = 0
last_flush_time = 0
system_state = 0 # 0=standby, 1=running, 2=flushing, 3=emergency
metrics_production_total = 0
metrics_flush_cycles = 0
metrics_wifi_reconnects = 0
metrics_server = None
start_time = time.time()
last_loop_time = time.time()

# ==========================================
# 3. HARDWARE INIT
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
# 4. IRQ & HELPERS
# ==========================================
def _emergency_shutdown(pin):
    global leak_detected, system_state
    pump.value(0)
    inlet_v.value(0)
    flush_v.value(0)
    leak_detected = True
    system_state = 3

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
# 5. MAIN LOGIC
# ==========================================
update_display("RO SYSTEM v1.0", "Initializing...", "Sensors: OK", "Ready.")

wdt = WDT(timeout=get_config("WATCHDOG_TIMEOUT", 30000))
system_active = False

while True:
    wdt.feed()
    update_alarm_async() # Handles the alternating sounds and LED
    handle_metrics()

    # 1. LEAK LOGIC (Critical)
    if leak_detected or leak.value() == 0:
        trigger_alarm('LEAK')
        # Emergency Shutdown logic...
        while True:
            wdt.feed()
            update_alarm_async()
            time.sleep_ms(10)

    # 2. SOURCE WATER LOGIC
    if lps.value() == 1: # No pressure
        trigger_alarm('LOW_PRESSURE')
    else:
        clear_alarm('LOW_PRESSURE')

    # 3. TDS LOGIC
    if get_tds() > get_config("TDS_THRESHOLD", 100):
        trigger_alarm('TDS_HIGH')
    else:
        clear_alarm('TDS_HIGH')

    # 5.3 Sensor Reading & Production Tracking
    now = time.time()
    delta = now - last_loop_time
    last_loop_time = now

    source_water = (lps.value() == 0)
    faucet_open = (hps.value() == 0)
    current_tds = get_tds()

    # 5.4 Production Logic
    if source_water and faucet_open:
        if not system_active:
            update_display("STATUS: STARTING", "Opening Inlet...", "", "")
            inlet_v.value(1)
            time.sleep(1)
            pump.value(1)
            system_active = True
            system_state = 1
        production_time += delta

    elif not faucet_open or not source_water:
        if system_active:
            pump.value(0)
            time.sleep(1)
            inlet_v.value(0)
            system_active = False
            system_state = 0
            metrics_production_total += production_time
            production_time = 0

    # 5.6 Display Refresh
    if system_active:
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
