from machine import Pin, PWM, Timer
import time

# --- 1. Full Note Scale (1 per line) ---
NOTE_REST = 0
NOTE_C4   = 262
NOTE_CS4  = 277
NOTE_D4   = 294
NOTE_DS4  = 311
NOTE_E4   = 330
NOTE_F4   = 349
NOTE_FS4  = 370
NOTE_G4   = 392
NOTE_GS4  = 415
NOTE_A4   = 440
NOTE_AS4  = 466
NOTE_B4   = 494
NOTE_C5   = 523
NOTE_CS5  = 554
NOTE_D5   = 587
NOTE_DS5  = 622
NOTE_E5   = 659
NOTE_F5   = 698
NOTE_FS5  = 740
NOTE_G5   = 784
NOTE_GS5  = 831
NOTE_A5   = 880
NOTE_AS5  = 932
NOTE_B5   = 988
NOTE_C6   = 1047

# --- 2. Alarm Definitions ---
# Priority: Lower number = Higher priority (0 is highest)
ALARMS = {
    'FLOODING': {
        'notes': [(NOTE_A5, 100), (NOTE_E5, 100), (NOTE_A5, 100), (NOTE_E5, 100)],
        'blink': 100,
        'priority': 0
    },
    'LEAK': {
        'notes': [(NOTE_D5, 100), (NOTE_REST, 800)],
        'blink': 1500,
        'priority': 1
    },
    'LOW_PRESSURE': {
        'notes': [(NOTE_G5, 150), (NOTE_E5, 150), (NOTE_C5, 150), (NOTE_G4, 300)],
        'blink': 500,
        'priority': 2
    },
    'TDS_HIGH': {
        'notes': [(NOTE_A4, 70), (NOTE_AS4, 70), (NOTE_A4, 70), (NOTE_AS4, 70)],
        'blink': 1000,
        'priority': 3
    }
}

# --- 3. System State ---
active_alarms = []  # List of names like ['LEAK', 'TDS_HIGH']
current_queue_idx = 0
note_index = 0
next_tick = 0

buzzer = PWM(Pin(23))
led = Pin(2, Pin.OUT)
led_timer = Timer(0)
current_blink_rate = 0

def trigger_alarm(name):
    """Add an alarm to the active list if not already there."""
    if name in ALARMS and name not in active_alarms:
        active_alarms.append(name)
        _refresh_led_logic()

def clear_alarm(name):
    """Remove an alarm from the active list."""
    global current_queue_idx, note_index
    if name in active_alarms:
        active_alarms.remove(name)
        current_queue_idx = 0
        note_index = 0
        _refresh_led_logic()

def _refresh_led_logic():
    """Sets LED to the blink rate of the highest priority active alarm."""
    global current_blink_rate
    led_timer.deinit()
    if not active_alarms:
        led.value(0)
        current_blink_rate = 0
        return

    # Find highest priority (lowest number)
    highest_prio_rate = min([ALARMS[a]['blink'] for a in active_alarms])

    if highest_prio_rate != current_blink_rate:
        current_blink_rate = highest_prio_rate
        led_timer.init(period=current_blink_rate // 2, mode=Timer.PERIODIC,
                       callback=lambda t: led.value(not led.value()))

def update_alarm_async():
    """Alternates between all active alarm sounds. Non-blocking."""
    global current_queue_idx, note_index, next_tick

    if not active_alarms:
        buzzer.duty(0)
        return

    if time.ticks_ms() < next_tick:
        return

    # Get notes for the alarm currently being played in the rotation
    alarm_name = active_alarms[current_queue_idx]
    notes = ALARMS[alarm_name]['notes']

    freq, duration = notes[note_index]

    if freq == 0:
        buzzer.duty(0)
    else:
        buzzer.freq(freq)
        buzzer.duty(512)

    # Schedule next note
    next_tick = time.ticks_ms() + duration
    note_index += 1

    # If we finished this alarm's sequence, move to the next alarm in the queue
    if note_index >= len(notes):
        note_index = 0
        current_queue_idx = (current_queue_idx + 1) % len(active_alarms)
