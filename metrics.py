"""Prometheus metrics server for Krosmosis RO controller."""

import socket
import time


class MetricsServer:
    """Non-blocking HTTP server for Prometheus metrics."""

    def __init__(self, port: int = 8080):
        self.port = port
        self.server: socket.socket | None = None
        self._collectors: list[callable] = []

    def register_collector(self, collector: callable) -> None:
        """Register a metrics collector function.

        Collectors should return a dict with metric data.
        """
        self._collectors.append(collector)

    def start(self) -> bool:
        """Start the metrics server. Returns True if successful."""
        try:
            self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server.bind(("0.0.0.0", self.port))
            self.server.listen(1)
            self.server.setblocking(False)
            print(f"Metrics: Server started on port {self.port}")
            return True
        except Exception as e:
            print(f"Metrics: Failed to start server - {e}")
            self.server = None
            return False

    def stop(self) -> None:
        """Stop the metrics server."""
        if self.server:
            try:
                self.server.close()
            except Exception:
                pass
            self.server = None

    def handle_request(self) -> None:
        """Handle incoming metrics requests (non-blocking)."""
        if self.server is None:
            return

        try:
            client, addr = self.server.accept()
            client.setblocking(True)
            client.settimeout(1.0)

            try:
                request = client.recv(1024).decode("utf-8")

                # Only respond to GET /metrics or GET /
                if "GET /metrics" in request or "GET / " in request:
                    metrics = self._generate_metrics()
                    response = (
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        f"Content-Length: {len(metrics)}\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        f"{metrics}"
                    )
                    client.send(response.encode("utf-8"))
                else:
                    response = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n"
                    client.send(response.encode("utf-8"))
            finally:
                client.close()
        except OSError:
            # No connection waiting (non-blocking socket)
            pass
        except Exception as e:
            print(f"Metrics: Request error - {e}")

    def _generate_metrics(self) -> str:
        """Generate Prometheus-formatted metrics from all collectors."""
        lines = []

        for collector in self._collectors:
            try:
                data = collector()
                if data:
                    lines.extend(self._format_metrics(data))
            except Exception as e:
                print(f"Metrics: Collector error - {e}")

        return "\n".join(lines) + "\n"

    def _format_metrics(self, data: dict) -> list[str]:
        """Format metrics data into Prometheus text format.

        Expected data format:
        {
            "metric_name": {
                "help": "Description",
                "type": "gauge|counter",
                "value": 123,
                # OR for labeled metrics:
                "values": [
                    {"labels": {"state": "running"}, "value": 1},
                    {"labels": {"state": "standby"}, "value": 0},
                ]
            }
        }
        """
        lines = []

        for name, metric in data.items():
            help_text = metric.get("help", "")
            metric_type = metric.get("type", "gauge")

            lines.append(f"# HELP {name} {help_text}")
            lines.append(f"# TYPE {name} {metric_type}")

            if "values" in metric:
                # Labeled metric
                for item in metric["values"]:
                    labels = item.get("labels", {})
                    label_str = ",".join(f'{k}="{v}"' for k, v in labels.items())
                    value = item.get("value", 0)
                    lines.append(f"{name}{{{label_str}}} {value}")
            else:
                # Simple metric
                value = metric.get("value", 0)
                lines.append(f"{name} {value}")

        return lines


def create_system_collector(
    get_state: callable,
    get_tds: callable,
    get_sensors: callable,
    get_relays: callable,
    get_counters: callable,
    start_time: float,
) -> callable:
    """Create a collector function for system metrics.

    Args:
        get_state: Returns dict with system_state, production_time, wifi_connected
        get_tds: Returns current TDS reading
        get_sensors: Returns dict with lps, hps, leak values
        get_relays: Returns dict with pump, inlet_v, flush_v values
        get_counters: Returns dict with production_total, flush_cycles, wifi_reconnects
        start_time: System start timestamp
    """

    def collector() -> dict:
        state = get_state()
        sensors = get_sensors()
        relays = get_relays()
        counters = get_counters()

        system_state = state.get("system_state", 0)

        return {
            "krosmosis_info": {
                "help": "System information",
                "type": "gauge",
                "values": [{"labels": {"version": "1.0"}, "value": 1}],
            },
            "krosmosis_uptime_seconds": {
                "help": "System uptime in seconds",
                "type": "counter",
                "value": int(time.time() - start_time),
            },
            "krosmosis_system_state": {
                "help": "Current system state",
                "type": "gauge",
                "values": [
                    {"labels": {"state": "standby"}, "value": 1 if system_state == 0 else 0},
                    {"labels": {"state": "running"}, "value": 1 if system_state == 1 else 0},
                    {"labels": {"state": "flushing"}, "value": 1 if system_state == 2 else 0},
                    {"labels": {"state": "emergency"}, "value": 1 if system_state == 3 else 0},
                ],
            },
            "krosmosis_tds_ppm": {
                "help": "Current TDS reading in PPM",
                "type": "gauge",
                "value": get_tds(),
            },
            "krosmosis_pressure_low": {
                "help": "Low pressure sensor state",
                "type": "gauge",
                "value": sensors.get("lps", 0),
            },
            "krosmosis_pressure_high": {
                "help": "High pressure sensor state",
                "type": "gauge",
                "value": sensors.get("hps", 0),
            },
            "krosmosis_leak_detected": {
                "help": "Leak sensor state",
                "type": "gauge",
                "value": sensors.get("leak", 0),
            },
            "krosmosis_pump_active": {
                "help": "Pump relay state",
                "type": "gauge",
                "value": relays.get("pump", 0),
            },
            "krosmosis_inlet_valve_active": {
                "help": "Inlet valve state",
                "type": "gauge",
                "value": relays.get("inlet_v", 0),
            },
            "krosmosis_flush_valve_active": {
                "help": "Flush valve state",
                "type": "gauge",
                "value": relays.get("flush_v", 0),
            },
            "krosmosis_production_seconds": {
                "help": "Current cycle production time",
                "type": "gauge",
                "value": int(state.get("production_time", 0)),
            },
            "krosmosis_production_total_seconds": {
                "help": "Total cumulative production time",
                "type": "counter",
                "value": int(counters.get("production_total", 0)),
            },
            "krosmosis_flush_cycles_total": {
                "help": "Number of flush cycles completed",
                "type": "counter",
                "value": counters.get("flush_cycles", 0),
            },
            "krosmosis_time_to_flush_seconds": {
                "help": "Time until next flush",
                "type": "gauge",
                "value": int(state.get("time_to_flush", 0)),
            },
            "krosmosis_wifi_connected": {
                "help": "WiFi connection state",
                "type": "gauge",
                "value": 1 if state.get("wifi_connected", False) else 0,
            },
            "krosmosis_wifi_reconnects_total": {
                "help": "Number of WiFi reconnections",
                "type": "counter",
                "value": counters.get("wifi_reconnects", 0),
            },
        }

    return collector
