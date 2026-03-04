"""HTTP web interface for Krosmosis RO controller."""

import socket
import ujson

STATIC_DIR = "static"
CONTENT_TYPES = {"html": "text/html", "css": "text/css", "js": "application/javascript"}


class WebServer:
    """Non-blocking HTTP server for the Krosmosis web UI."""

    def __init__(self, port=80):
        self.port = port
        self.server = None
        self._get_status = None
        self._do_control = None
        self._auth_password = None
        self._session_token = None
        self._https = False
        self._certfile = None
        self._keyfile = None

    def register_callbacks(self, get_status, do_control):
        self._get_status = get_status
        self._do_control = do_control

    def configure_auth(self, password):
        """Enable password authentication. Call before start()."""
        self._auth_password = password

    def configure_https(self, certfile, keyfile):
        """Enable HTTPS. certfile/keyfile are paths on the device filesystem."""
        try:
            import ssl as _ssl  # noqa — just verify module exists
            self._certfile = certfile
            self._keyfile = keyfile
            self._https = True
            print("WebUI: HTTPS enabled (cert=" + certfile + ")")
        except ImportError:
            print("WebUI: ssl module unavailable, HTTPS disabled")

    def _generate_token(self):
        import os
        return ''.join('{:02x}'.format(b) for b in os.urandom(16))

    def _check_auth(self, raw):
        """Returns True if request carries a valid session cookie (or auth is off)."""
        if not self._auth_password:
            return True
        if not self._session_token:
            return False
        for line in raw.split("\r\n"):
            if line.lower().startswith("cookie:"):
                if "krosmosis_token=" + self._session_token in line:
                    return True
        return False

    def _url_decode(self, s):
        """Minimal application/x-www-form-urlencoded decoder."""
        s = s.replace('+', ' ')
        out = []
        i = 0
        while i < len(s):
            if s[i] == '%' and i + 2 < len(s):
                out.append(chr(int(s[i+1:i+3], 16)))
                i += 3
            else:
                out.append(s[i])
                i += 1
        return ''.join(out)

    def _send_redirect(self, client, location):
        client.send((
            "HTTP/1.1 303 See Other\r\n"
            "Location: " + location + "\r\n"
            "Connection: close\r\n\r\n"
        ).encode("utf-8"))

    def start(self):
        """Start the web server. Returns True if successful."""
        try:
            self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server.bind(("0.0.0.0", self.port))
            self.server.listen(1)
            self.server.setblocking(False)
            print("WebUI: Server started on port " + str(self.port))
            return True
        except Exception as e:
            print("WebUI: Failed to start - " + str(e))
            self.server = None
            return False

    def stop(self):
        """Stop the web server."""
        if self.server:
            try:
                self.server.close()
            except Exception:
                pass
            self.server = None

    def handle_request(self):
        """Handle one pending request if any (non-blocking)."""
        if self.server is None:
            return
        try:
            raw_client, addr = self.server.accept()
            raw_client.setblocking(True)
            raw_client.settimeout(5.0)
            client = raw_client
            if self._https:
                import ssl
                try:
                    client = ssl.wrap_socket(raw_client, server_side=True,
                                             certfile=self._certfile,
                                             keyfile=self._keyfile)
                except Exception as e:
                    print("WebUI: TLS error - " + str(e))
                    raw_client.close()
                    return
            try:
                raw = client.recv(2048).decode("utf-8")
                first_line = raw.split("\r\n")[0]
                parts = first_line.split(" ")
                method = parts[0] if len(parts) > 0 else "GET"
                path = parts[1] if len(parts) > 1 else "/"
                body = ""
                if "\r\n\r\n" in raw:
                    body = raw.split("\r\n\r\n", 1)[1]
                self._dispatch(method, path, body, raw, client)
            finally:
                client.close()
        except OSError:
            # No connection waiting (non-blocking socket returns EAGAIN)
            pass
        except Exception as e:
            print("WebUI: Request error - " + str(e))

    def _serve_file(self, client, filepath, content_type):
        try:
            with open(filepath, "rb") as f:
                body = f.read()
            self._send(client, "200 OK", content_type, body)
        except OSError:
            self._send(client, "404 Not Found", "text/plain", "Not Found")

    def _dispatch(self, method, path, body, raw, client):
        if path != "/login":
            if not self._check_auth(raw):
                if method == "GET":
                    self._send_redirect(client, "/login")
                else:
                    self._send(client, "401 Unauthorized", "application/json",
                               ujson.dumps({"ok": False, "message": "Unauthorized"}))
                return
        if method == "GET" and path == "/":
            self._serve_file(client, STATIC_DIR + "/index.html", "text/html")
        elif method == "GET" and path == "/status":
            self._handle_status(client)
        elif method == "POST" and path == "/control":
            self._handle_control(client, body)
        elif method == "GET" and path == "/login":
            self._serve_file(client, STATIC_DIR + "/login.html", "text/html")
        elif method == "POST" and path == "/login":
            self._handle_login_post(client, body)
        elif method == "GET" and path.startswith("/static/"):
            filename = path[len("/static/"):]
            if ".." in filename or "/" in filename:
                self._send(client, "404 Not Found", "text/plain", "Not Found")
                return
            ext = filename.rsplit(".", 1)[-1] if "." in filename else ""
            content_type = CONTENT_TYPES.get(ext, "application/octet-stream")
            self._serve_file(client, STATIC_DIR + "/" + filename, content_type)
        else:
            self._send(client, "404 Not Found", "text/plain", "Not Found")

    def _handle_login_post(self, client, body):
        password = ""
        for part in body.split("&"):
            if "=" in part:
                k, v = part.split("=", 1)
                if k == "password":
                    password = self._url_decode(v)
        if password == self._auth_password:
            self._session_token = self._generate_token()
            secure = "; Secure" if self._https else ""
            client.send((
                "HTTP/1.1 303 See Other\r\n"
                "Location: /\r\n"
                "Set-Cookie: krosmosis_token=" + self._session_token
                + "; HttpOnly; SameSite=Strict" + secure + "\r\n"
                "Connection: close\r\n\r\n"
            ).encode("utf-8"))
        else:
            self._send_redirect(client, "/login?error=1")

    def _handle_status(self, client):
        if self._get_status:
            try:
                data = self._get_status()
                self._send(client, "200 OK", "application/json", ujson.dumps(data))
            except Exception as e:
                self._send(client, "500 Internal Server Error", "application/json",
                           ujson.dumps({"error": str(e)}))
        else:
            self._send(client, "503 Service Unavailable", "application/json",
                       ujson.dumps({"error": "No status callback"}))

    def _handle_control(self, client, body):
        action = ""
        params = {}
        for part in body.split("&"):
            if "=" in part:
                k, v = part.split("=", 1)
                if k == "action":
                    action = v
                else:
                    params[k] = v
        if not action:
            self._send(client, "400 Bad Request", "application/json",
                       ujson.dumps({"ok": False, "message": "Missing action"}))
            return
        if self._do_control:
            try:
                ok, message = self._do_control(action, params)
                self._send(client, "200 OK", "application/json",
                           ujson.dumps({"ok": ok, "message": message}))
            except Exception as e:
                self._send(client, "500 Internal Server Error", "application/json",
                           ujson.dumps({"ok": False, "message": str(e)}))
        else:
            self._send(client, "503 Service Unavailable", "application/json",
                       ujson.dumps({"ok": False, "message": "No control callback"}))

    def _send(self, client, status, content_type, body):
        if isinstance(body, str):
            body_bytes = body.encode("utf-8")
        else:
            body_bytes = body
        header = (
            "HTTP/1.1 " + status + "\r\n"
            "Content-Type: " + content_type + "\r\n"
            "Content-Length: " + str(len(body_bytes)) + "\r\n"
            "Connection: close\r\n"
            "\r\n"
        ).encode("utf-8")
        client.send(header + body_bytes)
