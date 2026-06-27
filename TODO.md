# TODO

Future improvements for the Sysmosis RO controller.

## Web Interface

Add a lightweight web UI served directly from the ESP32 for local management.

### Features

- [ ] **Dashboard**: Real-time display of system status, sensor readings, and relay states
- [ ] **Manual Controls**: Buttons to trigger flush cycles, toggle pump, open/close valves
- [ ] **Metrics View**: Live TDS readings, production time, uptime stats
- [ ] **Configuration**: Edit settings without re-uploading config.py (WiFi, flush intervals, etc.)
- [ ] **Logs**: View recent system events and errors

### Technical Considerations

- Use `microdot` or raw sockets for HTTP server (already have socket code in metrics.py)
- Serve static HTML/CSS/JS or use server-side templating
- WebSocket support for real-time updates (or polling fallback)
- Keep memory footprint low - ESP32 has ~520KB SRAM

## Filter Replacement Tracking

Track filter lifespan and replacement schedules.

### Features

- [ ] **Filter Registry**: Track multiple filter types (sediment, carbon, RO membrane, post-carbon)
- [ ] **Replacement Dates**: Record when each filter was last replaced
- [ ] **Due Date Alerts**: Calculate next replacement based on:
  - Time-based (e.g., every 6 months)
  - Usage-based (e.g., after X liters or Y hours of production)
- [ ] **Web UI Integration**: Display filter status, allow marking filters as replaced
- [ ] **Prometheus Metrics**: Export filter age and days until replacement

### Technical Considerations

- [ ] **RTC (Real-Time Clock)**: Add DS3231 or similar RTC module for accurate timekeeping
  - ESP32 has internal RTC but loses time on power loss
  - DS3231 has battery backup and high accuracy
  - Connect via I2C (can share bus with LCD)
- [ ] **Persistent Storage**: Save filter data to flash using `btree` or JSON file
- [ ] **NTP Sync**: Sync time from internet when WiFi is available (fallback for RTC)

### Hardware Addition

```
DS3231 RTC Module:
- VCC → 3.3V
- GND → GND
- SDA → GPIO 21 (shared I2C bus)
- SCL → GPIO 22 (shared I2C bus)
```

### Data Model

```python
filters = {
    "sediment": {
        "name": "Sediment Filter (5 micron)",
        "replaced_at": 1704067200,  # Unix timestamp
        "lifespan_days": 180,
        "lifespan_liters": 10000,  # Optional
    },
    "carbon_pre": {
        "name": "Carbon Pre-Filter",
        "replaced_at": 1704067200,
        "lifespan_days": 180,
    },
    "ro_membrane": {
        "name": "RO Membrane",
        "replaced_at": 1704067200,
        "lifespan_days": 730,  # 2 years
    },
    "carbon_post": {
        "name": "Carbon Post-Filter",
        "replaced_at": 1704067200,
        "lifespan_days": 365,
    },
}
```

## Future Ideas

- [ ] **Flow Meter Integration**: Measure actual water production volume
- [ ] **MQTT Support**: Alternative to Prometheus for Home Assistant integration
- [ ] **Telegram/Push Notifications**: Alert on leak, filter due, high TDS
- [ ] **Power Monitoring**: Track pump power consumption
- [ ] **Multi-unit Support**: Manage multiple RO systems from one interface
