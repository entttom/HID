#!/usr/bin/env python3
"""Production launcher for PiKVM HID Automation.

The HTTP service is intended to listen on loopback and to be exposed through
PiKVM's authenticated nginx frontend. Optional HTTP Basic Auth is still
available for deliberate direct/LAN exposure.

The scheduler process runs as root only so kvmd-pstrun can safely remount
persistent storage for short writes. KVMD HID calls themselves are deliberately
executed as the unprivileged `hid-automation` Unix user and authenticated by
KVMD Unix Socket Credentials (USC).
"""
import base64
import ipaddress
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


def is_loopback_host(host):
    if host in {"localhost", "ip6-localhost"}:
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


class AuthHandler(app.Handler):
    def authenticated(self):
        if not AUTH_PASS:
            return True
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
    # Empty Basic-Auth credentials are safe only when the backend is reachable
    # from the local PiKVM host. Nginx then provides the normal PiKVM login.
    if not AUTH_PASS and not is_loopback_host(app.HOST):
        raise SystemExit(
            "HID_AUTOMATION_PASSWORD is empty while the service is not bound "
            "to loopback; refusing to expose an unauthenticated HID endpoint"
        )

    app.load_state()
    import threading
    threading.Thread(target=app.scheduler_loop, daemon=True, name="scheduler").start()
    server = ThreadingHTTPServer((app.HOST, app.PORT), AuthHandler)
    mode = "PiKVM nginx auth" if not AUTH_PASS else "HTTP Basic Auth"
    print(f"PiKVM HID Automation läuft auf http://{app.HOST}:{app.PORT} ({mode})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        app.stop_event.set()
        server.server_close()


if __name__ == "__main__":
    main()
