#!/usr/bin/env python3
import json
import os
import random
import subprocess
import threading
import time
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

BASE_DIR = Path(__file__).resolve().parent
WEB_DIR = BASE_DIR / "web"
STATE_DIR = Path("/var/lib/kvmd/pst/data/hid-automation")
STATE_FILE = STATE_DIR / "state.json"
PORT = int(os.environ.get("HID_AUTOMATION_PORT", "8081"))
HOST = os.environ.get("HID_AUTOMATION_HOST", "0.0.0.0")

DEFAULT_STATE = {
    "auto": False,
    "autoMinMinutes": 25,
    "autoMaxMinutes": 35,
    "nextAutoAt": None,
    "plannerUntil": "17:30",
    "plannerMinMinutes": 15,
    "plannerMaxMinutes": 44,
    "plannerEvents": 4,
    "manualPlanActive": False,
    "manualStartAt": None,
    "schedules": [],
    "completed": [],
    "flowRunning": False,
    "flowState": "idle",
    "lastTrigger": None,
    "lastTriggerSource": None,
    "lastError": None,
}

lock = threading.RLock()
state = dict(DEFAULT_STATE)
stop_event = threading.Event()


def now_ts():
    return int(time.time())


def iso_local(ts=None):
    dt = datetime.fromtimestamp(ts or time.time()).astimezone()
    return dt.isoformat(timespec="seconds")


def parse_local_datetime(value):
    if not value:
        return now_ts()
    dt = datetime.fromisoformat(value)
    if dt.tzinfo is None:
        dt = dt.astimezone()
    return int(dt.timestamp())


def persist_state():
    with lock:
        payload = json.dumps(state, ensure_ascii=False, indent=2)
    try:
        subprocess.run(
            ["kvmd-pstrun", "--", "mkdir", "-p", str(STATE_DIR)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        proc = subprocess.run(
            ["kvmd-pstrun", "--", "tee", str(STATE_FILE)],
            input=payload,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        return True
    except Exception as exc:
        with lock:
            state["lastError"] = f"Persistenz fehlgeschlagen: {exc}"
        return False


def load_state():
    if not STATE_FILE.exists():
        return
    try:
        saved = json.loads(STATE_FILE.read_text(encoding="utf-8"))
        if isinstance(saved, dict):
            with lock:
                state.update(saved)
                state["flowRunning"] = False
                state["flowState"] = "idle"
    except Exception as exc:
        state["lastError"] = f"State konnte nicht geladen werden: {exc}"


class KvmdClient:
    socket = "/run/kvmd/kvmd.sock"

    @classmethod
    def _post(cls, route, params=None):
        from urllib.parse import urlencode
        url = "http://localhost" + route
        if params:
            url += "?" + urlencode(params)
        proc = subprocess.run(
            ["curl", "--fail", "--silent", "--show-error", "--unix-socket", cls.socket, "-X", "POST", url],
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

    @classmethod
    def click(cls):
        cls._post("/hid/events/send_mouse_button", {"button": "left"})

    @classmethod
    def key(cls, key):
        cls._post("/hid/events/send_key", {"key": key})

    @classmethod
    def shortcut(cls, *keys):
        cls._post("/hid/events/send_shortcut", {"keys": ",".join(keys)})

    @classmethod
    def mouse(cls, dx, dy):
        cls._post("/hid/events/send_mouse_relative", {"delta_x": int(dx), "delta_y": int(dy)})


def set_flow_state(value):
    with lock:
        state["flowState"] = value


def perform_flow(source="manual"):
    with lock:
        if state["flowRunning"]:
            return False
        state["flowRunning"] = True
        state["flowState"] = "starting"
        state["lastTrigger"] = iso_local()
        state["lastTriggerSource"] = source
        state["lastError"] = None

    try:
        set_flow_state("click")
        KvmdClient.click()
        time.sleep(5)
        set_flow_state("enter")
        KvmdClient.key("Enter")
        time.sleep(20)
        set_flow_state("ctrl-alt-f")
        KvmdClient.shortcut("ControlLeft", "AltLeft", "KeyF")
        set_flow_state("done")
        return True
    except Exception as exc:
        with lock:
            state["lastError"] = str(exc)
            state["flowState"] = "error"
        return False
    finally:
        with lock:
            state["flowRunning"] = False
        persist_state()


def perform_pre_event():
    KvmdClient.click()
    time.sleep(5)
    KvmdClient.key("Enter")


def perform_event(event_ts):
    with lock:
        if state["flowRunning"]:
            return False
        state["flowRunning"] = True
        state["flowState"] = "scheduled-event"
        state["lastTrigger"] = iso_local()
        state["lastTriggerSource"] = "schedule"
        state["lastError"] = None
    try:
        KvmdClient.shortcut("ControlLeft", "AltLeft", "KeyF")
        with lock:
            remaining = [x for x in state["schedules"] if int(x) != int(event_ts)]
            state["schedules"] = remaining
            state["completed"] = (state.get("completed", []) + [int(event_ts)])[-20:]
            last = len(remaining) == 0
        if not last:
            time.sleep(random.randint(10, 30))
            set_flow_state("prepare-next")
            perform_pre_event()
        else:
            with lock:
                state["manualPlanActive"] = False
                state["manualStartAt"] = None
        return True
    except Exception as exc:
        with lock:
            state["lastError"] = str(exc)
            state["flowState"] = "error"
        return False
    finally:
        with lock:
            state["flowRunning"] = False
            if state["flowState"] != "error":
                state["flowState"] = "idle"
        persist_state()


def start_manual_plan(start_ts):
    with lock:
        state["manualPlanActive"] = True
        state["manualStartAt"] = int(start_ts)
    persist_state()


def schedule_next_auto():
    with lock:
        lo = max(1, int(state["autoMinMinutes"]))
        hi = max(lo, int(state["autoMaxMinutes"]))
        state["nextAutoAt"] = now_ts() + random.randint(lo * 60, hi * 60)
    persist_state()


def scheduler_loop():
    prepared_for_start = None
    while not stop_event.wait(1):
        now = now_ts()
        with lock:
            auto = bool(state["auto"])
            next_auto = state.get("nextAutoAt")
            plan_active = bool(state["manualPlanActive"])
            start_at = state.get("manualStartAt")
            schedules = sorted(int(x) for x in state.get("schedules", []))
            busy = bool(state["flowRunning"])

        if auto and not next_auto:
            schedule_next_auto()
            continue
        if not auto and next_auto:
            with lock:
                state["nextAutoAt"] = None
            persist_state()
        if auto and next_auto and now >= int(next_auto) and not busy:
            if perform_flow("auto"):
                schedule_next_auto()
            else:
                with lock:
                    state["nextAutoAt"] = now + 60
            continue

        if plan_active and start_at and now >= int(start_at) and prepared_for_start != int(start_at) and not busy:
            try:
                with lock:
                    state["flowRunning"] = True
                    state["flowState"] = "plan-start"
                perform_pre_event()
                prepared_for_start = int(start_at)
                with lock:
                    state["flowRunning"] = False
                    state["flowState"] = "idle"
                persist_state()
            except Exception as exc:
                with lock:
                    state["flowRunning"] = False
                    state["flowState"] = "error"
                    state["lastError"] = str(exc)
            continue

        if plan_active and schedules and not busy:
            due = schedules[0]
            # Events bis zu einer Stunde nachholen, danach verwerfen.
            if now > due + 3600:
                with lock:
                    state["schedules"] = [x for x in state["schedules"] if int(x) != due]
                    if not state["schedules"]:
                        state["manualPlanActive"] = False
                persist_state()
            elif now >= due:
                perform_event(due)


def public_state():
    with lock:
        data = dict(state)
    data["time"] = now_ts()
    data["timeLabel"] = iso_local()
    data["maxSchedules"] = 8
    candidates = []
    if data.get("auto") and data.get("nextAutoAt"):
        candidates.append(int(data["nextAutoAt"]))
    if data.get("manualPlanActive"):
        candidates.extend(int(x) for x in data.get("schedules", []))
    data["nextTriggerAt"] = min(candidates) if candidates else None
    return data


def calculate_events(start_ts, until_hhmm, count, min_minutes, max_minutes):
    start = datetime.fromtimestamp(start_ts).astimezone()
    hh, mm = [int(x) for x in until_hhmm.split(":", 1)]
    end = start.replace(hour=hh, minute=mm, second=0, microsecond=0)
    if end <= start:
        end += timedelta(days=1)
    count = max(1, min(8, int(count)))
    min_s = max(1, int(min_minutes)) * 60
    max_s = max(min_s, int(max_minutes) * 60)
    available = int((end - start).total_seconds())
    if available < min_s * count:
        raise ValueError("Zeitraum ist für Eventanzahl und Mindestabstand zu kurz")

    for _ in range(5000):
        gaps = [random.randint(min_s, max_s) for _ in range(count)]
        total = sum(gaps)
        if total <= available:
            current = int(start.timestamp())
            events = []
            for gap in gaps:
                current += gap
                events.append(current)
            return events
    raise ValueError("Keine gültige zufällige Verteilung gefunden")


class Handler(BaseHTTPRequestHandler):
    server_version = "PiKVM-HID-Automation/1.0"

    def log_message(self, fmt, *args):
        print(f"[{iso_local()}] {self.client_address[0]} {fmt % args}")

    def send_json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        if not length:
            return {}
        return json.loads(self.rfile.read(length).decode())

    def serve_file(self, path):
        target = WEB_DIR / path
        if not target.exists() or not target.is_file():
            self.send_error(404)
            return
        mime = "text/html; charset=utf-8" if target.suffix == ".html" else "text/plain; charset=utf-8"
        if target.suffix == ".css": mime = "text/css; charset=utf-8"
        if target.suffix == ".js": mime = "application/javascript; charset=utf-8"
        body = target.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/api/status":
            self.send_json(200, {"ok": True, "result": public_state()})
            return
        if parsed.path in ("/", "/index.html"):
            self.serve_file("index.html")
            return
        if parsed.path.startswith("/static/"):
            self.serve_file(parsed.path.removeprefix("/static/"))
            return
        self.send_error(404)

    def do_POST(self):
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)
        try:
            body = self.read_json()
        except Exception:
            body = {}
        try:
            if parsed.path == "/api/trigger":
                started = False
                with lock:
                    if not state["flowRunning"]:
                        started = True
                if started:
                    threading.Thread(target=perform_flow, args=("manual",), daemon=True).start()
                self.send_json(202 if started else 409, {"ok": started, "result": public_state()})
                return
            if parsed.path == "/api/click":
                KvmdClient.click()
                self.send_json(200, {"ok": True})
                return
            if parsed.path == "/api/key":
                KvmdClient.key(str(body.get("key", "Enter")))
                self.send_json(200, {"ok": True})
                return
            if parsed.path == "/api/shortcut":
                keys = body.get("keys", [])
                if not isinstance(keys, list) or not keys:
                    raise ValueError("keys muss eine nicht-leere Liste sein")
                KvmdClient.shortcut(*[str(x) for x in keys])
                self.send_json(200, {"ok": True})
                return
            if parsed.path == "/api/mouse":
                KvmdClient.mouse(int(body.get("dx", 0)), int(body.get("dy", 0)))
                self.send_json(200, {"ok": True})
                return
            if parsed.path == "/api/settings":
                with lock:
                    if "auto" in body: state["auto"] = bool(body["auto"])
                    if "autoMinMinutes" in body: state["autoMinMinutes"] = int(body["autoMinMinutes"])
                    if "autoMaxMinutes" in body: state["autoMaxMinutes"] = int(body["autoMaxMinutes"])
                    if not state["auto"]:
                        state["nextAutoAt"] = None
                    elif not state.get("nextAutoAt"):
                        lo = max(1, int(state["autoMinMinutes"]))
                        hi = max(lo, int(state["autoMaxMinutes"]))
                        state["nextAutoAt"] = now_ts() + random.randint(lo * 60, hi * 60)
                persist_state()
                self.send_json(200, {"ok": True, "result": public_state()})
                return
            if parsed.path == "/api/planner/calculate":
                start_ts = parse_local_datetime(body.get("startAt"))
                events = calculate_events(start_ts, body.get("until", "17:30"), body.get("events", 4), body.get("min", 15), body.get("max", 44))
                with lock:
                    state["plannerUntil"] = body.get("until", "17:30")
                    state["plannerEvents"] = int(body.get("events", 4))
                    state["plannerMinMinutes"] = int(body.get("min", 15))
                    state["plannerMaxMinutes"] = int(body.get("max", 44))
                persist_state()
                self.send_json(200, {"ok": True, "result": {"events": events}})
                return
            if parsed.path == "/api/schedule/replace":
                times = sorted(set(int(x) for x in body.get("times", [])))[:8]
                with lock:
                    state["schedules"] = times
                persist_state()
                self.send_json(200, {"ok": True, "result": public_state()})
                return
            if parsed.path == "/api/schedule/start":
                start_manual_plan(parse_local_datetime(body.get("when")))
                self.send_json(200, {"ok": True, "result": public_state()})
                return
            if parsed.path == "/api/schedule/stop":
                with lock:
                    state["manualPlanActive"] = False
                    state["manualStartAt"] = None
                persist_state()
                self.send_json(200, {"ok": True, "result": public_state()})
                return
            if parsed.path == "/api/schedule/delete":
                target = int(body.get("when"))
                with lock:
                    state["schedules"] = [x for x in state["schedules"] if int(x) != target]
                persist_state()
                self.send_json(200, {"ok": True, "result": public_state()})
                return
            self.send_error(404)
        except Exception as exc:
            with lock:
                state["lastError"] = str(exc)
            self.send_json(400, {"ok": False, "error": str(exc)})


def main():
    load_state()
    threading.Thread(target=scheduler_loop, daemon=True, name="scheduler").start()
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"PiKVM HID Automation läuft auf http://{HOST}:{PORT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        server.server_close()


if __name__ == "__main__":
    main()
