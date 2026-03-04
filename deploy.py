#!/usr/bin/env python3
"""Deploy all files to the ESP32 via mpremote."""
import subprocess
import sys
import os

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"


def run(*args):
    subprocess.run(["mpremote", "connect", PORT, *args], check=True)


# Create static dir on device (ignore error if already exists)
try:
    run("mkdir", ":static")
except subprocess.CalledProcessError:
    pass

FILES = [
    "main.py",
    "webserver.py",
    "metrics.py",
    "alarms.py",
    "static/style.css",
    "static/app.js",
    "static/index.html",
    "static/login.html",
]

# config.py is optional — not committed to git
if os.path.exists("config.py"):
    FILES.append("config.py")

for f in FILES:
    print(f"Uploading {f}...")
    run("cp", f, ":" + f)

print("Resetting device...")
run("reset")
print("Done.")
