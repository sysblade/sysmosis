"""HTTP web interface for Krosmosis RO controller."""
# ruff: noqa: E501  — HTML_PAGE contains intentionally minified HTML/CSS/JS

import socket
import ujson

LOGIN_PAGE = """\
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Krosmosis Login</title>
<style>*{box-sizing:border-box;margin:0;padding:0}body{background:#111;color:#ccc;font-family:sans-serif;font-size:14px;display:flex;align-items:center;justify-content:center;height:100vh}.box{background:#1a1a1a;border-radius:8px;padding:24px;min-width:220px}h1{font-size:16px;margin-bottom:16px}input{width:100%;padding:7px;background:#222;border:1px solid #444;border-radius:4px;color:#ccc;font-size:14px;margin-bottom:10px}button{width:100%;padding:8px;background:#2a7;border:none;border-radius:4px;color:#fff;cursor:pointer;font-size:14px}.err{color:#c44;font-size:12px;margin-bottom:8px}</style></head><body>
<div class="box"><h1>Krosmosis</h1>
<form method="POST" action="/login"><input type="password" name="password" placeholder="Password" autofocus>
{ERR}<button type="submit">Login</button></form></div></body></html>"""

HTML_PAGE = """\
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Krosmosis</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#111;color:#ccc;font-family:sans-serif;font-size:14px;padding:10px}
h1{font-size:18px;display:flex;align-items:center;gap:8px;margin-bottom:10px}
.badge{padding:2px 8px;border-radius:4px;font-size:12px;font-weight:bold}
.s0{background:#555}.s1{background:#2a7}.s2{background:#27a}
.s3{background:#a22;animation:blink 0.5s step-end infinite}.s4{background:#a70}
@keyframes blink{50%{opacity:0}}
section{background:#1a1a1a;border-radius:6px;padding:10px;margin-bottom:8px}
h2{font-size:12px;color:#888;margin-bottom:6px;text-transform:uppercase;letter-spacing:.05em}
.row{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:4px}
.lbl{color:#888;margin-right:3px}
.ok{color:#4c4}.warn{color:#cc4}.err{color:#c44}
#lcd-widget{background:#0a1f0a;color:#33ff44;font-family:monospace;letter-spacing:.1em;padding:6px 10px;border-radius:4px;border:1px solid #1a4a1a}
#lcd-display{white-space:pre;font-size:13px}
button{padding:5px 11px;border:none;border-radius:4px;cursor:pointer;font-size:13px;background:#333;color:#ccc;margin:3px}
button:hover{background:#444}
.btn-danger{background:#722}.btn-danger:hover{background:#944}
hr{border:none;border-top:1px solid #333;margin:7px 0}
.ip{font-size:11px;color:#666;margin-left:auto}
a{color:#5af}
</style></head><body>
<h1>Krosmosis <span id="badge" class="badge s0">...</span><span id="ip-hdr" class="ip"></span></h1>
<section><h2>LCD Display</h2>
<div id="lcd-widget"><pre id="lcd-display">                    \n                    \n                    \n                    </pre></div>
</section>
<section><h2>Sensors</h2>
<div class="row">
<span><span class="lbl">Source:</span><span id="sw">-</span></span>
<span><span class="lbl">Faucet:</span><span id="fo">-</span></span>
<span><span class="lbl">TDS:</span><span id="tds">-</span> PPM</span>
</div>
<div class="row">
<span><span class="lbl">LPS:</span><span id="lps">-</span></span>
<span><span class="lbl">HPS:</span><span id="hps">-</span></span>
<span><span class="lbl">Leak:</span><span id="leak">-</span></span>
</div></section>
<section><h2>Relays</h2>
<div class="row">
<span><span class="lbl">Pump:</span><span id="pump">-</span></span>
<span><span class="lbl">Inlet:</span><span id="inlet">-</span></span>
<span><span class="lbl">Flush valve:</span><span id="fv">-</span></span>
</div></section>
<section><h2>Timing</h2>
<div class="row">
<span><span class="lbl">Uptime:</span><span id="upt">-</span></span>
<span><span class="lbl">Production:</span><span id="prod">-</span></span>
</div>
<div class="row">
<span><span class="lbl">Flush cycles:</span><span id="fcyc">-</span></span>
<span><span class="lbl">Total prod:</span><span id="ptot">-</span></span>
</div>
<div id="flush-info" style="display:none" class="row">
<span><span class="lbl">Flush reason:</span><span id="freason">-</span></span>
<span><span class="lbl">Remaining:</span><span id="frem">-</span>s</span>
</div></section>
<section><h2>Controls</h2>
<div>
<button onclick="postAction('maintenance_toggle')">Toggle Maintenance (M)</button>
<button onclick="postAction('flush_start')">Manual Flush (F)</button>
<button class="btn-danger" onclick="confirmReset()">Reset System (R)</button>
</div>
<div id="maint-controls" style="display:none">
<hr><small style="color:#888">Maintenance relay controls</small><br>
<button onclick="postAction('pump_on')">Pump ON</button>
<button onclick="postAction('pump_off')">Pump OFF</button>
<button onclick="postAction('inlet_on')">Inlet ON</button>
<button onclick="postAction('inlet_off')">Inlet OFF</button>
<button onclick="postAction('flush_valve_on')">Flush ON</button>
<button onclick="postAction('flush_valve_off')">Flush OFF</button>
</div></section>
<script>
function fmt(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60),ss=s%60;return h>0?h+'h'+m+'m':m>0?m+'m'+ss+'s':ss+'s';}
function yn(v){return v?'<span class="ok">YES</span>':'<span class="err">NO</span>';}
function relay(v){return v?'<span class="ok">ON</span>':'<span class="lbl">OFF</span>';}
var BC=['s0','s1','s2','s3','s4'];
function refresh(){
fetch('/status').then(function(r){if(r.status===401){location.href='/login';return null;}return r.json();}).then(function(d){if(!d)return;
var b=document.getElementById('badge');
b.textContent=d.state||'?';b.className='badge '+(BC[d.state_id]||'s0');
document.getElementById('ip-hdr').textContent=d.ip||'';
if(d.lcd)document.getElementById('lcd-display').textContent=d.lcd.join('\\n');
document.getElementById('sw').innerHTML=yn(d.source_water);
document.getElementById('fo').innerHTML=yn(d.faucet_open);
document.getElementById('tds').textContent=d.tds_ppm;
document.getElementById('lps').innerHTML=d.lps?'<span class="warn">LOW</span>':'<span class="ok">OK</span>';
document.getElementById('hps').innerHTML=d.hps?'<span class="warn">HIGH</span>':'<span class="ok">OK</span>';
document.getElementById('leak').innerHTML=d.leak_detected?'<span class="err">LEAK!</span>':'<span class="ok">NONE</span>';
document.getElementById('pump').innerHTML=relay(d.pump);
document.getElementById('inlet').innerHTML=relay(d.inlet_valve);
document.getElementById('fv').innerHTML=relay(d.flush_valve);
document.getElementById('upt').textContent=fmt(d.uptime_s||0);
document.getElementById('prod').textContent=fmt(d.production_time_s||0);
document.getElementById('fcyc').textContent=d.flush_cycles_total||0;
document.getElementById('ptot').textContent=fmt(d.production_total_s||0);
var fi=document.getElementById('flush-info');
if(d.flush_remaining_s>0){fi.style.display='';document.getElementById('freason').textContent=d.flush_reason;document.getElementById('frem').textContent=d.flush_remaining_s;}
else fi.style.display='none';
document.getElementById('maint-controls').style.display=d.state_id===4?'block':'none';
}).catch(function(){});}
function postAction(a){
fetch('/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action='+a})
.then(function(r){if(r.status===401){location.href='/login';return null;}return r.json();}).then(function(d){if(!d)return;if(!d.ok)alert(d.message);else refresh();}).catch(function(){});}
function confirmReset(){if(confirm('Reset the device?'))postAction('reset');}
document.addEventListener('keydown',function(e){
if(e.target.tagName==='INPUT'||e.target.tagName==='TEXTAREA')return;
if(e.key==='m'||e.key==='M')postAction('maintenance_toggle');
else if(e.key==='f'||e.key==='F')postAction('flush_start');
else if(e.key==='r'||e.key==='R')confirmReset();});
refresh();setInterval(refresh,2000);
</script></body></html>"""


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
            self._send(client, "200 OK", "text/html", HTML_PAGE)
        elif method == "GET" and path == "/status":
            self._handle_status(client)
        elif method == "POST" and path == "/control":
            self._handle_control(client, body)
        elif method == "GET" and path == "/login":
            self._send(client, "200 OK", "text/html", LOGIN_PAGE.replace("{ERR}", ""))
        elif method == "POST" and path == "/login":
            self._handle_login_post(client, body)
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
            self._send(client, "401 Unauthorized", "text/html",
                       LOGIN_PAGE.replace("{ERR}", '<p class="err">Wrong password</p>'))

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
