from machine import Pin, PWM, Timer
import time

# --- Hardware Setup ---
BUZZER_PIN = 23
LED_PIN = 2
BUZZER_VOLUME = 512 # 50% Duty Cycle

# --- Frequencies (Standard Pitch) ---
NOTE_REST = 0
NOTE_C4  = 262
NOTE_CS4 = 277
NOTE_D4  = 294
NOTE_DS4 = 311
NOTE_E4  = 330
NOTE_F4  = 349
NOTE_FS4 = 370
NOTE_G4  = 392
NOTE_GS4 = 415
NOTE_A4  = 440
NOTE_AS4 = 466
NOTE_B4  = 494
NOTE_C5  = 523
NOTE_CS5 = 554
NOTE_D5  = 587
NOTE_DS5 = 622
NOTE_E5  = 659
NOTE_F5  = 698
NOTE_FS5 = 740
NOTE_G5  = 784
NOTE_GS5 = 831
NOTE_A5  = 880
NOTE_AS5 = 932
NOTE_B5  = 988
NOTE_C6  = 1047

# --- Global Alarm State ---
current_alarm = None
note_index = 0
next_tick = 0
alarm_active = False

# Hardware Setup
buzzer = PWM(Pin(23))
led = Pin(2, Pin.OUT)
led_timer = Timer(0)

# Alarm Definitions (Sound + Blink Rate)
ALARMS = {
    'FLOODING': {'notes': [(NOTE_A5, 100), (NOTE_E5, 100)], 'blink': 100},
    'LEAK':     {'notes': [(NOTE_D5, 100), (NOTE_REST, 800)], 'blink': 1500},
    'TDS_HIGH': {'notes': [(NOTE_A4, 70), (NOTE_AS4, 70)], 'blink': 1000},
    'OFF':      {'notes': [], 'blink': 0}
}

def set_alarm(name):
    """Call this to change the current alarm state"""
    global current_alarm, note_index, next_tick, alarm_active
    if name == 'OFF':
        alarm_active = False
        buzzer.duty(0)
        led_timer.deinit()
        led.value(0)
    else:
        current_alarm = ALARMS[name]
        note_index = 0
        next_tick = time.ticks_ms()
        alarm_active = True
        # Start LED Background Timer
        led_timer.init(period=current_alarm['blink']//2, mode=Timer.PERIODIC,
                       callback=lambda t: led.value(not led.value()))

def update_alarm_async():
    """Call this ONCE in your main loop. Non-blocking."""
    global note_index, next_tick

    if not alarm_active or time.ticks_ms() < next_tick:
        return

    notes = current_alarm['notes']
    freq, duration = notes[note_index]

    if freq == NOTE_REST:
        buzzer.duty(0)
    else:
        buzzer.freq(freq)
        buzzer.duty(512)

    # Schedule next note change
    next_tick = time.ticks_ms() + duration
    note_index = (note_index + 1) % len(notes)
