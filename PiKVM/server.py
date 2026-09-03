#!/usr/bin/env python3
"""Production launcher for PiKVM HID Automation.

Runs the web/scheduler process as root only so kvmd-pstrun can safely remount
persistent storage for short writes. KVMD HID calls themselves are deliberately
executed as the unprivileged `hid-automation` Unix user and authenticated by
KVMD Unix Socket Credentials (USC).
"""
import base64
import json
import os
import subprocess
from http.server import ThreadingHTTPServer
from urllib.parse import urlencode

import app

AUTH_USER = os.environ.get("HID_AUTOMATION_USER", "admin")
AUTH_PASS = os.environ.get("HID_AUTOMATION_PASSWORD", "")
HID_USER = os.environ.get("HID_AUTOMATION_KVMD_USER", "hid-automation")


def kvmd_post(_cls, route, params=None):
    url = "http://localhost" + route
    if params:
        url += "?" + urlencode(params)
    proc = subprocess.run(
        [
            "runuser", "-u", HID_USER, "--",
            "curl", "--fail", "--silent", "--show-error",
            "--unix-socket", app.KvmdClient.socket,
            "-X", "POST", url,
        ],
        capture_output=True,
        text=True,
        timeout=10,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"curl exit {proc.returncode}")
    if proc.stdout:
        body = json.loads(proc.stdout)
        if not body.get("ok", False):
            raise RuntimeError(body)
    return True


app.KvmdClient._post = classmethod(kvmd_post)


class AuthHandler(app.Handler):
    def authenticated(self):
        if not AUTH_PASS:
            return False
        expected = "Basic " + base64.b64encode(f"{AUTH_USER}:{AUTH_PASS}".encode()).decode()
        return self.headers.get("Authorization", "") == expected

    def require_auth(self):
        if self.authenticated():
            return True
        body = b"Authentication required"
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="PiKVM HID Automation"')
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        return False

    def do_GET(self):
        if self.require_auth():
            super().do_GET()

    def do_POST(self):
        if self.require_auth():
            super().do_POST()


def main():
    if not AUTH_PASS:
        raise SystemExit("HID_AUTOMATION_PASSWORD is empty; refusing to start")
    app.load_state()
    import threading
    threading.Thread(target=app.scheduler_loop, daemon=True, name="scheduler").start()
    server = ThreadingHTTPServer((app.HOST, app.PORT), AuthHandler)
    print(f"PiKVM HID Automation läuft auf http://{app.HOST}:{app.PORT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        app.stop_event.set()
        server.server_close()


if __name__ == "__main__":
    main()
