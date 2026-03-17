#!/usr/bin/env python3
"""Deploy all files to the ESP32 via mpremote."""
import hashlib
import os
import subprocess
import sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"


def run(*args):
    subprocess.run(["mpremote", "connect", PORT, *args], check=True)


def local_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(512):
            h.update(chunk)
    return h.hexdigest()


def remote_sha256(remote_path):
    # Returns the sha256 hex digest, or None if the file does not exist
    script = (
        "import hashlib, binascii, os\n"
        "try:\n"
        "    os.stat('{p}')\n"
        "    h = hashlib.sha256()\n"
        "    f = open('{p}', 'rb')\n"
        "    b = f.read(512)\n"
        "    while b:\n"
        "        h.update(b)\n"
        "        b = f.read(512)\n"
        "    f.close()\n"
        "    print(binascii.hexlify(h.digest()).decode())\n"
        "except OSError:\n"
        "    print('MISSING')\n"
    ).format(p=remote_path)
    result = subprocess.run(
        ["mpremote", "connect", PORT, "exec", script],
        capture_output=True, text=True, check=True,
    )
    value = result.stdout.strip()
    return None if value == "MISSING" else value


def upload(local_path, remote_path, retries=3):
    local_crc  = local_sha256(local_path)
    local_size = os.path.getsize(local_path)

    # Skip upload if remote already matches
    remote_crc = remote_sha256(remote_path)
    if remote_crc == local_crc:
        print(f"  SKIP {local_path} (unchanged, {local_crc[:12]}...)  [{local_size}B]")
        return

    for attempt in range(1, retries + 1):
        run("cp", local_path, ":" + remote_path)

        remote_crc = remote_sha256(remote_path)

        if remote_crc == local_crc:
            print(f"  OK  {local_path} ({local_crc[:12]}...)  [{local_size}B]")
            return

        print(f"  attempt {attempt}/{retries} MISMATCH {local_path}")
        print(f"    local  sha256: {local_crc}")
        print(f"    remote sha256: {remote_crc}")

    print(f"ERROR: {local_path} failed to transfer correctly after {retries} attempts")
    sys.exit(1)


# Create static dir on device (ignore error if already exists)
try:
    run("mkdir", ":static")
except subprocess.CalledProcessError:
    pass

FILES = [
    ("main.py",             "main.py"),
    ("webserver.py",        "webserver.py"),
    ("metrics.py",          "metrics.py"),
    ("alarms.py",           "alarms.py"),
    ("static/style.css",    "static/style.css"),
    ("static/app.js",       "static/app.js"),
    ("static/index.html",   "static/index.html"),
    ("static/login.html",   "static/login.html"),
]

# config.py is optional — not committed to git
if os.path.exists("config.py"):
    FILES.append(("config.py", "config.py"))

for local, remote in FILES:
    print(f"Uploading {local}...")
    upload(local, remote)

print("Resetting device...")
run("reset")
print("Done.")
