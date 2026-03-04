from machine import Pin, ADC, SoftI2C, WDT, PWM, Timer
from esp8266_i2c_lcd import I2cLcd
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

# ==========================================
# 2. STATE
# ==========================================
class State:
    STANDBY     = 0
    RUNNING     = 1
    FLUSHING    = 2
    EMERGENCY   = 3
    MAINTENANCE = 4

_STATE_NAMES = {
    State.STANDBY:     "STANDBY",
    State.RUNNING:     "RUNNING",
    State.FLUSHING:    "FLUSHING",
    State.EMERGENCY:   "EMERGENCY",
    State.MAINTENANCE: "MAINTENANCE",
}

# ==========================================
# 3. CONTROLLER
# ==========================================
class ROController:
    def __init__(self):
        # State fields
        self.leak_detected = False
        self.wifi_connected = False
        self.last_wifi_check = 0
        self.production_time = 0
        self.last_flush_time = 0
        self.system_state = State.STANDBY
        self.metrics_production_total = 0
        self.metrics_flush_cycles = 0
        self.metrics_wifi_reconnects = 0
        self.metrics_server = None
        self.start_time = time.time()
        self.last_loop_time = time.time()
        self.startup_flush_done = False
        self.flush_reason = ""
        self.flush_start_time = 0
        self.flush_duration = 0
        self.last_standby_start = time.time()
        self.next_inactivity_flush_in = 0
        self.web_server = None
        self.wifi_ip = ""
        self.source_water = False
        self.faucet_open = False
        self.current_tds = 0
        self.lcd_lines = ["", "", "", ""]
        self.wdt = None

        # Hardware init
        i2c = SoftI2C(
            scl=Pin(get_config("PIN_LCD_SCL", 22)),
            sda=Pin(get_config("PIN_LCD_SDA", 21)),
            freq=400000,
        )
        self.lcd = I2cLcd(i2c, get_config("LCD_I2C_ADDR", 0x27), 4, 20)
        self.pump    = Pin(get_config("PIN_PUMP", 12),          Pin.OUT, value=0)
        self.inlet_v = Pin(get_config("PIN_INLET_VALVE", 13),   Pin.OUT, value=0)
        self.flush_v = Pin(get_config("PIN_FLUSH_VALVE", 14),   Pin.OUT, value=0)
        self.lps     = Pin(get_config("PIN_LOW_PRESSURE", 25),  Pin.IN, Pin.PULL_UP)
        self.hps     = Pin(get_config("PIN_HIGH_PRESSURE", 26), Pin.IN, Pin.PULL_UP)
        self.leak    = Pin(get_config("PIN_LEAK_SENSOR", 27),   Pin.IN, Pin.PULL_UP)
        self.tds_sensor = ADC(Pin(get_config("PIN_TDS_SENSOR", 34)))
        self.tds_sensor.atten(ADC.ATTN_11DB)
        self.leak.irq(trigger=Pin.IRQ_FALLING, handler=self._emergency_shutdown)
        self._schedule_next_inactivity_flush()

    # ==========================================
    # IRQ & HELPERS
    # ==========================================
    def _emergency_shutdown(self, pin):
        self.pump.value(0)
        self.inlet_v.value(0)
        self.flush_v.value(0)
        self.leak_detected = True
        self.system_state = State.EMERGENCY

    def update_display(self, l1="", l2="", l3="", l4=""):
        self.lcd_lines = [l1.ljust(20), l2.ljust(20), l3.ljust(20), l4.ljust(20)]
        self.lcd.move_to(0, 0)
        self.lcd.putstr(self.lcd_lines[0])
        self.lcd.move_to(0, 1)
        self.lcd.putstr(self.lcd_lines[1])
        self.lcd.move_to(0, 2)
        self.lcd.putstr(self.lcd_lines[2])
        self.lcd.move_to(0, 3)
        self.lcd.putstr(self.lcd_lines[3])

    def get_tds(self):
        val = self.tds_sensor.read()
        return int(val * get_config("TDS_FACTOR", 0.5) + get_config("TDS_OFFSET", 0))

    # ==========================================
    # FLUSH HELPERS
    # ==========================================
    def _schedule_next_inactivity_flush(self):
        """Pick a random interval between min and max for the next inactivity flush."""
        min_i = get_config("FLUSH_INACTIVITY_INTERVAL_MIN", 24 * 3600)
        max_i = get_config("FLUSH_INACTIVITY_INTERVAL_MAX", 48 * 3600)
        self.next_inactivity_flush_in = random.randint(min_i, max_i)

    def start_flush(self, reason, duration):
        """Stop any active production and start a flush cycle."""
        self.pump.value(0)
        self.inlet_v.value(0)
        time.sleep_ms(500)
        self.flush_v.value(1)
        self.pump.value(1)
        self.flush_reason = reason
        self.flush_start_time = time.time()
        self.flush_duration = duration
        self.last_flush_time = self.flush_start_time
        self.system_state = State.FLUSHING

    def stop_flush(self):
        """End the current flush cycle and return to standby."""
        self.pump.value(0)
        self.flush_v.value(0)
        self.system_state = State.STANDBY
        self.metrics_flush_cycles += 1
        self.last_standby_start = time.time()
        self._schedule_next_inactivity_flush()

    def enter_maintenance(self):
        """Stop all automated hardware and enter maintenance mode for manual control."""
        if self.system_state == State.RUNNING:
            self.pump.value(0)
            time.sleep(1)
            self.inlet_v.value(0)
            self.metrics_production_total += self.production_time
            self.production_time = 0
        elif self.system_state == State.FLUSHING:
            self.pump.value(0)
            self.flush_v.value(0)
        self.system_state = State.MAINTENANCE

    def exit_maintenance(self):
        """Return to standby from maintenance mode."""
        if self.system_state == State.MAINTENANCE:
            self.system_state = State.STANDBY
            self.last_standby_start = time.time()  # Reset inactivity timer

    # ==========================================
    # WEB CALLBACKS
    # ==========================================
    def _get_web_status(self):
        now = time.time()
        remaining = 0
        if self.system_state == State.FLUSHING:
            remaining = max(0, int(self.flush_duration - (now - self.flush_start_time)))
        return {
            "state": _STATE_NAMES.get(self.system_state, "UNKNOWN"),
            "state_id": self.system_state,
            "source_water": self.source_water,
            "faucet_open": self.faucet_open,
            "tds_ppm": self.current_tds,
            "wifi_connected": self.wifi_connected,
            "uptime_s": int(now - self.start_time),
            "production_time_s": int(self.production_time),
            "flush_reason": self.flush_reason,
            "flush_remaining_s": remaining,
            "flush_duration_s": self.flush_duration,
            "pump": bool(self.pump.value()),
            "inlet_valve": bool(self.inlet_v.value()),
            "flush_valve": bool(self.flush_v.value()),
            "lps": self.lps.value() == 1,
            "hps": self.hps.value() == 1,
            "leak_detected": self.leak_detected,
            "flush_cycles_total": self.metrics_flush_cycles,
            "production_total_s": int(self.metrics_production_total + self.production_time),
            "ip": self.wifi_ip,
            "lcd": self.lcd_lines,
        }

    def _do_web_control(self, action, params):
        if action == "maintenance_toggle":
            if self.system_state == State.EMERGENCY:
                return (False, "Cannot toggle maintenance during emergency")
            if self.system_state == State.MAINTENANCE:
                self.exit_maintenance()
            else:
                self.enter_maintenance()
            return (True, "OK")
        if action == "flush_start":
            if self.system_state != State.STANDBY:
                return (False, "Flush only allowed from STANDBY state")
            self.start_flush("Manual", get_config("FLUSH_MANUAL_DURATION", 30))
            return (True, "OK")
        if action == "reset":
            import machine
            machine.reset()
        if action == "pump_on":
            if self.system_state != State.MAINTENANCE:
                return (False, "Relay control only in MAINTENANCE mode")
            self.pump.value(1)
            return (True, "OK")
        if action == "pump_off":
            if self.system_state != State.MAINTENANCE:
                return (False, "Relay control only in MAINTENANCE mode")
            self.pump.value(0)
            return (True, "OK")
        if action == "inlet_on":
            if self.system_state != State.MAINTENANCE:
                return (False, "Relay control only in MAINTENANCE mode")
            self.inlet_v.value(1)
            return (True, "OK")
        if action == "inlet_off":
            if self.system_state != State.MAINTENANCE:
                return (False, "Relay control only in MAINTENANCE mode")
            self.inlet_v.value(0)
            return (True, "OK")
        if action == "flush_valve_on":
            if self.system_state != State.MAINTENANCE:
                return (False, "Relay control only in MAINTENANCE mode")
            self.flush_v.value(1)
            return (True, "OK")
        if action == "flush_valve_off":
            if self.system_state != State.MAINTENANCE:
                return (False, "Relay control only in MAINTENANCE mode")
            self.flush_v.value(0)
            return (True, "OK")
        return (False, "Unknown action: " + action)

    # ==========================================
    # WIFI / SERVER INIT
    # ==========================================
    def connect_wifi(self):
        ssid = get_config("WIFI_SSID", None)
        if not ssid:
            return
        import network
        from metrics import create_system_collector, MetricsServer
        from webserver import WebServer
        wlan = network.WLAN(network.STA_IF)
        wlan.active(True)
        wlan.connect(ssid, get_config("WIFI_PASSWORD", ""))
        timeout = get_config("WIFI_TIMEOUT", 10)
        t = time.time()
        while not wlan.isconnected():
            self.wdt.feed()
            time.sleep_ms(200)
            if time.time() - t > timeout:
                print("WiFi: Connection timed out")
                return
        self.wifi_connected = True
        self.wifi_ip = wlan.ifconfig()[0]
        print("WiFi: Connected, IP=" + self.wifi_ip)
        if get_config("METRICS_ENABLED", True):
            self.metrics_server = MetricsServer(port=get_config("METRICS_PORT", 8080))
            collector = create_system_collector(
                get_state=lambda: {
                    "system_state": self.system_state,
                    "production_time": self.production_time,
                    "wifi_connected": self.wifi_connected,
                    "time_to_flush": max(
                        0,
                        self.next_inactivity_flush_in - int(time.time() - self.last_standby_start),
                    ),
                },
                get_tds=lambda: self.current_tds,
                get_sensors=lambda: {
                    "lps": self.lps.value(),
                    "hps": self.hps.value(),
                    "leak": 1 if self.leak_detected else 0,
                },
                get_relays=lambda: {
                    "pump": self.pump.value(),
                    "inlet_v": self.inlet_v.value(),
                    "flush_v": self.flush_v.value(),
                },
                get_counters=lambda: {
                    "production_total": self.metrics_production_total + self.production_time,
                    "flush_cycles": self.metrics_flush_cycles,
                    "wifi_reconnects": self.metrics_wifi_reconnects,
                },
                start_time=self.start_time,
            )
            self.metrics_server.register_collector(collector)
            self.metrics_server.start()
        if get_config("WEB_ENABLED", True):
            _web_port = get_config("WEB_PORT", 443 if get_config("WEB_HTTPS", False) else 80)
            self.web_server = WebServer(port=_web_port)
            self.web_server.register_callbacks(self._get_web_status, self._do_web_control)

            _auth_pw = get_config("WEB_AUTH_PASSWORD", None)
            if _auth_pw:
                self.web_server.configure_auth(_auth_pw)

            if get_config("WEB_HTTPS", False):
                self.web_server.configure_https(
                    get_config("WEB_CERT_FILE", "cert.pem"),
                    get_config("WEB_KEY_FILE", "key.pem"),
                )

            self.web_server.start()
            _scheme = "https" if get_config("WEB_HTTPS", False) else "http"
            print("WebUI: " + _scheme + "://" + self.wifi_ip + "/")

    def check_wifi_reconnect(self):
        ssid = get_config("WIFI_SSID", None)
        if not ssid:
            return
        now = time.time()
        if now - self.last_wifi_check < get_config("WIFI_RECONNECT_INTERVAL", 60):
            return
        self.last_wifi_check = now
        import network
        wlan = network.WLAN(network.STA_IF)
        if not wlan.isconnected():
            self.wifi_connected = False
            self.metrics_wifi_reconnects += 1
            print("WiFi: Reconnecting...")
            self.connect_wifi()

    # ==========================================
    # MAIN LOOP
    # ==========================================
    def run(self):
        self.update_display("RO SYSTEM v1.0", "Initializing...", "Sensors: OK", "Ready.")
        self.wdt = WDT(timeout=get_config("WATCHDOG_TIMEOUT", 30000))
        self.connect_wifi()
        self.last_wifi_check = time.time()

        while True:
            self.wdt.feed()
            update_alarm_async()
            self.check_wifi_reconnect()
            if self.metrics_server:
                self.metrics_server.handle_request()
            if self.web_server:
                self.web_server.handle_request()

            # 1. LEAK LOGIC (Critical)
            if self.leak_detected or self.leak.value() == 0:
                trigger_alarm('LEAK')
                while True:
                    self.wdt.feed()
                    update_alarm_async()
                    time.sleep_ms(10)

            # 2. SOURCE WATER LOGIC
            if self.lps.value() == 1:  # No pressure
                trigger_alarm('LOW_PRESSURE')
            else:
                clear_alarm('LOW_PRESSURE')

            # 3. TDS LOGIC
            if self.get_tds() > get_config("TDS_THRESHOLD", 100):
                trigger_alarm('TDS_HIGH')
            else:
                clear_alarm('TDS_HIGH')

            # 4. TIMING
            now = time.time()
            delta = now - self.last_loop_time
            self.last_loop_time = now

            self.source_water = (self.lps.value() == 0)
            self.faucet_open = (self.hps.value() == 0)
            self.current_tds = self.get_tds()

            # 5. MAINTENANCE MODE: bypass all automation
            if self.system_state == State.MAINTENANCE:
                p  = "ON " if self.pump.value()    else "OFF"
                iv = "ON " if self.inlet_v.value() else "OFF"
                fv = "ON " if self.flush_v.value() else "OFF"
                self.update_display(
                    "STATUS: MAINTENANCE",
                    f"P:{p} IV:{iv} FV:{fv}",
                    f"TDS: {self.current_tds} PPM",
                    "WiFi: " + ("ON" if self.wifi_connected else "OFF"),
                )
                continue

            # 6. FLUSH LOGIC

            # 6a. Trigger startup flush (deferred until source water is available)
            if not self.startup_flush_done and self.source_water:
                self.startup_flush_done = True
                self.start_flush("Startup", get_config("FLUSH_STARTUP_DURATION", 20))

            # 6b. Handle active flush: update display and check for completion
            if self.system_state == State.FLUSHING:
                elapsed = now - self.flush_start_time
                remaining = max(0, self.flush_duration - elapsed)
                self.update_display(
                    "STATUS: FLUSHING",
                    f"Reason: {self.flush_reason}",
                    f"Rem:{int(remaining):3d}s Tot:{self.flush_duration:3d}s",
                    "WiFi: " + ("ON" if self.wifi_connected else "OFF"),
                )
                if remaining <= 0:
                    self.stop_flush()
                continue  # Skip production logic while flushing

            # 6c. Trigger inactivity flush (standby only, source water required)
            if self.system_state == State.STANDBY and self.source_water:
                standby_secs = now - self.last_standby_start
                if standby_secs >= self.next_inactivity_flush_in:
                    self.start_flush("Inactivity", get_config("FLUSH_INACTIVITY_DURATION", 60))
                    continue

            # 6d. Trigger production-interval flush (optional, disabled when interval=0)
            production_flush_interval = get_config("FLUSH_PRODUCTION_INTERVAL", 0)
            if (production_flush_interval > 0
                    and self.system_state == State.STANDBY
                    and self.source_water
                    and (now - self.last_flush_time) >= production_flush_interval):
                self.start_flush("Production", get_config("FLUSH_PRODUCTION_DURATION", 30))
                continue

            # 7. PRODUCTION LOGIC
            if self.source_water and self.faucet_open:
                if self.system_state != State.RUNNING:
                    self.update_display("STATUS: STARTING", "Opening Inlet...", "", "")
                    self.inlet_v.value(1)
                    time.sleep(1)
                    self.pump.value(1)
                    self.system_state = State.RUNNING
                self.production_time += delta

            elif not self.faucet_open or not self.source_water:
                if self.system_state == State.RUNNING:
                    self.pump.value(0)
                    time.sleep(1)
                    self.inlet_v.value(0)
                    self.system_state = State.STANDBY
                    self.metrics_production_total += self.production_time
                    self.production_time = 0
                    # Reset inactivity timer on production stop
                    self.last_standby_start = time.time()

            # 8. DISPLAY REFRESH
            if self.system_state == State.RUNNING:
                self.update_display(
                    "STATUS: RUNNING",
                    f"TDS: {self.current_tds} PPM",
                    f"Time: {int(self.production_time//60)}m {int(self.production_time%60)}s",
                    "WiFi: " + ("ON" if self.wifi_connected else "OFF")
                )
            else:
                self.update_display(
                    "STATUS: STANDBY",
                    "System Ready",
                    "Waiting for Faucet",
                    f"Last TDS: {self.current_tds}PPM"
                )


ROController().run()
