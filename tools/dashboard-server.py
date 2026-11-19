#!/usr/bin/env python3
"""dashboard-server: the computer side of Inkwell's Dashboard.

Run it on your computer. The Kindle's Dashboard asks it every few minutes for
some lines of information, shows them, and shows its ACTIONS as buttons;
tapping one runs that command here.

    python3 tools/dashboard-server.py                 # port 8765
    python3 tools/dashboard-server.py --port 9000

Then on the Kindle, in linux/dashboard.conf:

    server = http://<this computer's address>:8765
    token = <the token this prints when it starts>

The token is kept in ~/.config/inkwell/dashboard-token, made on first run,
and every request must carry it, so another device on your network can't
run your actions. It travels unencrypted, so use this on a network you
trust, not over the internet.

Edit ACTIONS and info() below to make it yours. Only the standard library
is used.

The protocol, if you'd rather write your own server:
    GET  /kindle        -> {"title": "...", "lines": [{"label": "...", "value": "..."}],
                            "buttons": [{"id": "...", "label": "..."}]}
    POST /action/<id>   -> {"ok": true, "message": "shown on the Kindle"}
Both need the header X-Inkwell-Token: <token>.
"""
import argparse
import datetime
import hmac
import json
import os
import platform
import secrets
import shutil
import socket
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MAC = sys.platform == "darwin"

# The buttons on the Kindle. Each runs a command, given as a list of
# arguments (no shell, so nothing in it is interpreted). Up to 8 are shown;
# with none, the Kindle shows only the lines. Some ideas (uncomment and adapt):
ACTIONS = {
    # "lock": {"label": "Lock screen", "message": "Screen locked",
    #          "cmd": ["pmset", "displaysleepnow"] if MAC else ["loginctl", "lock-session"]},
    # "music": {"label": "Play / pause", "cmd": ["playerctl", "play-pause"]},
    # "backup": {"label": "Start backup", "cmd": ["/home/me/bin/backup.sh"], "message": "Backup started"},
    # "sleep": {"label": "Sleep computer", "cmd": ["systemctl", "suspend"]},
}


def info():
    """The lines shown on the Kindle, as (label, value) pairs. Up to 16."""
    lines = []
    try:
        load = os.getloadavg()[0]
        lines.append(("Load", "%.2f" % load))
    except OSError:
        pass
    total, used, _ = shutil.disk_usage(Path.home())
    lines.append(("Disk", "%d%% used of %d GB" % (used * 100 // total, total // 10**9)))
    up = uptime()
    if up:
        lines.append(("Up", up))
    bat = battery()
    if bat:
        lines.append(("Battery", bat))
    lines.append(("Updated", datetime.datetime.now().strftime("%H:%M")))
    return lines


def uptime():
    try:
        with open("/proc/uptime") as f:
            s = float(f.read().split()[0])
    except OSError:
        return None
    d, h = divmod(int(s) // 3600, 24)
    return ("%dd %dh" % (d, h)) if d else ("%dh %dm" % (h, int(s) // 60 % 60))


def battery():
    for b in Path("/sys/class/power_supply").glob("BAT*"):
        try:
            cap = (b / "capacity").read_text().strip()
            status = (b / "status").read_text().strip().lower()
            return "%s%%, %s" % (cap, status)
        except OSError:
            pass
    return None


# ---- the server ---------------------------------------------------------------

TOKEN_FILE = Path.home() / ".config" / "inkwell" / "dashboard-token"


def load_token():
    if os.environ.get("INKWELL_TOKEN"):
        return os.environ["INKWELL_TOKEN"]
    if TOKEN_FILE.exists():
        return TOKEN_FILE.read_text().strip()
    TOKEN_FILE.parent.mkdir(parents=True, exist_ok=True)
    token = secrets.token_urlsafe(18)
    TOKEN_FILE.write_text(token + "\n")
    TOKEN_FILE.chmod(0o600)
    return token


class Handler(BaseHTTPRequestHandler):
    token = ""

    def reply(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def allowed(self):
        given = self.headers.get("X-Inkwell-Token", "")
        if hmac.compare_digest(given.encode(), self.token.encode()):
            return True
        self.reply(403, {"ok": False, "message": "Wrong token in dashboard.conf"})
        return False

    def do_GET(self):
        if self.path != "/kindle":
            return self.reply(404, {"ok": False, "message": "Not found"})
        if not self.allowed():
            return
        self.reply(200, {
            "title": socket.gethostname().split(".")[0],
            "lines": [{"label": k, "value": v} for k, v in info()][:16],
            "buttons": [{"id": k, "label": a["label"]} for k, a in ACTIONS.items()][:8],
        })

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(min(n, 65536))
        if not self.path.startswith("/action/"):
            return self.reply(404, {"ok": False, "message": "Not found"})
        if not self.allowed():
            return
        ident = self.path[len("/action/"):]
        a = ACTIONS.get(ident)
        if not a:
            return self.reply(404, {"ok": False, "message": "No action called " + ident})
        try:
            r = subprocess.run(a["cmd"], capture_output=True, text=True, timeout=30)
        except FileNotFoundError:
            return self.reply(200, {"ok": False, "message": a["cmd"][0] + " isn't installed"})
        except subprocess.TimeoutExpired:
            return self.reply(200, {"ok": False, "message": "It took too long"})
        out = (r.stdout or r.stderr).strip().splitlines()
        if r.returncode == 0:
            msg = a.get("message") or (out[0] if out else "Done")
        else:
            msg = "Failed: " + (out[-1] if out else "exit %d" % r.returncode)
        self.reply(200, {"ok": r.returncode == 0, "message": msg[:120]})

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))


def main():
    p = argparse.ArgumentParser(description="The computer side of Inkwell's Dashboard.")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--bind", default="0.0.0.0", help="address to listen on (default: all)")
    args = p.parse_args()
    Handler.token = load_token()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))  # no packet is sent; this only picks the outgoing address
        ip = s.getsockname()[0]
    except OSError:
        ip = "<this computer's address>"
    print("Inkwell dashboard server on %s, port %d" % (platform.node(), args.port))
    print("In linux/dashboard.conf on the Kindle:\n    server = http://%s:%d\n    token = %s" % (ip, args.port, Handler.token))
    ThreadingHTTPServer((args.bind, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
