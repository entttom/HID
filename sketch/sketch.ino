#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

USBHIDKeyboard Keyboard;
USBHIDMouse    Mouse;
WebServer      Server(80);
Preferences    Settings;

// --- Konfiguration ---
const uint32_t CLICK_DELAY_MS     = 50;
const uint32_t AFTER_CLICK_WAIT   = 5000;          // 5 s
const uint32_t AFTER_ENTER_WAIT   = 20000;         // 20 s
const uint32_t JIGGLE_INTERVAL    = 10000;         // alle 10 s im Standby sichtbar bewegen
const uint32_t LONG_WAIT_MIN_MS   = 25UL * 60UL * 1000UL;
const uint32_t LONG_WAIT_MAX_MS   = 35UL * 60UL * 1000UL;
const int16_t  START_RECT_CM      = 5;             // sichtbarer Start-Indikator
const int16_t  STANDBY_WANDER_CM  = 1;             // sichtbarer Lauf-Indikator
const int16_t  PIXELS_PER_CM      = 38;            // Naeherung bei 96 dpi
const int8_t   START_RECT_STEP    = 5;             // kleine Schritte fuer langsame Bewegung
const uint16_t START_RECT_DELAY   = 50;            // ms pro Schritt
const char*    AP_SSID            = "HID-Setup";
const char*    AP_PASSWORD        = "hidsetup123";
const uint32_t WIFI_CONNECT_MS    = 15000;
const int8_t   MANUAL_MOUSE_STEP  = 20;
const char*    NTP_SERVER1        = "pool.ntp.org";
const char*    NTP_SERVER2        = "time.nist.gov";
const char*    TZ_INFO            = "CET-1CEST,M3.5.0,M10.5.0/3";  // Europe/Vienna (MEZ/MESZ inkl. Sommerzeit)
const uint8_t  MAX_SCHEDULES      = 8;
const uint32_t TIME_RETRY_MS      = 60000;          // erneuter Sync-Versuch
const int32_t  SCHEDULE_GRACE_S   = 3600;           // verpasste Auslösung noch bis 1 h spaeter nachholen

enum FlowState {
  FLOW_IDLE,
  FLOW_AFTER_CLICK_WAIT,
  FLOW_AFTER_ENTER_WAIT,
  FLOW_AUTO_STANDBY
};

FlowState flowState = FLOW_IDLE;
bool autoLoopEnabled = false;
uint32_t stateUntil = 0;
uint32_t nextJiggleAt = 0;
String wifiSsid;
String wifiPassword;
String lastTrigger = "none";
time_t schedules[MAX_SCHEDULES];
bool timeSynced = false;
uint32_t nextTimeRetryAt = 0;

void moveMouseSlow(int16_t dx, int16_t dy) {
  bool horizontal = dx != 0;
  int16_t distance = horizontal ? dx : dy;
  int16_t remaining = distance < 0 ? -distance : distance;
  int8_t direction = distance < 0 ? -1 : 1;

  while (remaining > 0) {
    int8_t stepSize = remaining > START_RECT_STEP ? START_RECT_STEP : remaining;
    int8_t step = direction * stepSize;

    if (horizontal) {
      Mouse.move(step, 0);
    } else {
      Mouse.move(0, step);
    }

    remaining -= stepSize;
    delay(START_RECT_DELAY);
  }
}

void jiggleOnce() {
  int16_t distance = STANDBY_WANDER_CM * PIXELS_PER_CM;

  moveMouseSlow(-distance, 0);
  moveMouseSlow(distance, 0);
}

void drawStartupRectangle() {
  int16_t side = START_RECT_CM * PIXELS_PER_CM;

  moveMouseSlow(side, 0);
  moveMouseSlow(0, side);
  moveMouseSlow(-side, 0);
  moveMouseSlow(0, -side);
}

void pressCtrlAltF() {
  Keyboard.press(KEY_LEFT_CTRL);
  Keyboard.press(KEY_LEFT_ALT);
  Keyboard.write('f');
  delay(50);
  Keyboard.releaseAll();
}

void leftClick() {
  Mouse.press(MOUSE_LEFT);
  delay(CLICK_DELAY_MS);
  Mouse.release(MOUSE_LEFT);
}

bool timeReached(uint32_t target) {
  return (int32_t)(millis() - target) >= 0;
}

String htmlEscape(const String& value) {
  String escaped = value;
  escaped.replace("&", "&amp;");
  escaped.replace("\"", "&quot;");
  escaped.replace("<", "&lt;");
  escaped.replace(">", "&gt;");
  return escaped;
}

String jsonEscape(const String& value) {
  String escaped = value;
  escaped.replace("\\", "\\\\");
  escaped.replace("\"", "\\\"");
  escaped.replace("\n", "\\n");
  escaped.replace("\r", "\\r");
  escaped.replace("\t", "\\t");
  return escaped;
}

const char* flowStateName() {
  switch (flowState) {
    case FLOW_IDLE:
      return "idle";
    case FLOW_AFTER_CLICK_WAIT:
      return "after_click_wait";
    case FLOW_AFTER_ENTER_WAIT:
      return "after_enter_wait";
    case FLOW_AUTO_STANDBY:
      return "auto_standby";
  }

  return "unknown";
}

void saveSettings() {
  Settings.putString("ssid", wifiSsid);
  Settings.putString("pass", wifiPassword);
  Settings.putBool("auto", autoLoopEnabled);
}

void loadSettings() {
  Settings.begin("hid", false);
  wifiSsid = Settings.getString("ssid", "");
  wifiPassword = Settings.getString("pass", "");
  autoLoopEnabled = Settings.getBool("auto", false);
}

// --- Zeit (NTP) ---
bool syncTimeNow(uint32_t timeoutMs) {
  struct tm t;
  if (getLocalTime(&t, timeoutMs)) {
    timeSynced = true;
    Serial.printf("Zeit synchronisiert: %04d-%02d-%02d %02d:%02d\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    return true;
  }
  return false;
}

void maintainTime() {
  if (timeSynced) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!timeReached(nextTimeRetryAt)) return;

  nextTimeRetryAt = millis() + TIME_RETRY_MS;
  syncTimeNow(2000);
}

String formatTime(time_t value) {
  if (value <= 0) return "";
  struct tm lt;
  localtime_r(&value, &lt);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
  return String(buf);
}

time_t parseLocalDateTime(const String& value) {
  struct tm t = {0};
  int y, mo, d, h, mi, se = 0;
  // erwartet "YYYY-MM-DDTHH:MM" (Sekunden optional)
  if (sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 5) {
    t.tm_year = y - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min  = mi;
    t.tm_sec  = se;
    t.tm_isdst = -1;
    return mktime(&t);
  }
  return 0;
}

// --- Geplante Auslösungen ---
void saveSchedules() {
  String serialized;
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    if (schedules[i] != 0) {
      if (serialized.length()) serialized += ",";
      serialized += String((uint32_t)schedules[i]);
    }
  }
  Settings.putString("sched", serialized);
}

void loadSchedules() {
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) schedules[i] = 0;

  String serialized = Settings.getString("sched", "");
  uint8_t idx = 0;
  int start = 0;
  while (start <= (int)serialized.length() && idx < MAX_SCHEDULES) {
    int comma = serialized.indexOf(',', start);
    String part = comma < 0 ? serialized.substring(start) : serialized.substring(start, comma);
    part.trim();
    if (part.length()) {
      schedules[idx++] = (time_t)strtoul(part.c_str(), nullptr, 10);
    }
    if (comma < 0) break;
    start = comma + 1;
  }
}

bool addSchedule(time_t when) {
  if (when <= 0) return false;
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    if (schedules[i] == 0) {
      schedules[i] = when;
      saveSchedules();
      return true;
    }
  }
  return false;  // keine freien Plaetze
}

bool removeSchedule(time_t when) {
  bool removed = false;
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    if (schedules[i] == when) {
      schedules[i] = 0;
      removed = true;
    }
  }
  if (removed) saveSchedules();
  return removed;
}

void beginAutoStandby() {
  uint32_t waitMs = random(LONG_WAIT_MIN_MS, LONG_WAIT_MAX_MS + 1);

  flowState = FLOW_AUTO_STANDBY;
  stateUntil = millis() + waitMs;
  nextJiggleAt = millis() + JIGGLE_INTERVAL;

  Serial.printf("Auto-Standby fuer %lu ms\n", (unsigned long)waitMs);
}

bool startFlow(const String& source) {
  if (flowState == FLOW_AFTER_CLICK_WAIT || flowState == FLOW_AFTER_ENTER_WAIT) {
    return false;
  }

  lastTrigger = source;
  leftClick();
  flowState = FLOW_AFTER_CLICK_WAIT;
  stateUntil = millis() + AFTER_CLICK_WAIT;
  Serial.printf("Flow gestartet durch: %s\n", source.c_str());
  return true;
}

void handleFlow() {
  if (flowState == FLOW_IDLE) {
    if (autoLoopEnabled) {
      startFlow("auto");
      return;
    }

    // Auch ohne Automatik den Bildschirm wachhalten
    if (timeReached(nextJiggleAt)) {
      jiggleOnce();
      nextJiggleAt = millis() + JIGGLE_INTERVAL;
    }
    return;
  }

  if (flowState == FLOW_AFTER_CLICK_WAIT) {
    if (timeReached(stateUntil)) {
      Keyboard.write(KEY_RETURN);
      flowState = FLOW_AFTER_ENTER_WAIT;
      stateUntil = millis() + AFTER_ENTER_WAIT;
      Serial.println("ENTER gesendet");
    }
    return;
  }

  if (flowState == FLOW_AFTER_ENTER_WAIT) {
    if (timeReached(stateUntil)) {
      pressCtrlAltF();
      Serial.println("STRG + ALT + F gesendet");

      if (autoLoopEnabled) {
        beginAutoStandby();
      } else {
        flowState = FLOW_IDLE;
      }
    }
    return;
  }

  if (flowState == FLOW_AUTO_STANDBY) {
    if (!autoLoopEnabled) {
      flowState = FLOW_IDLE;
      return;
    }

    if (timeReached(nextJiggleAt)) {
      jiggleOnce();
      nextJiggleAt += JIGGLE_INTERVAL;
    }

    if (timeReached(stateUntil)) {
      startFlow("auto");
    }
  }
}

void checkSchedules() {
  if (!timeSynced) return;

  time_t now = time(nullptr);
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    if (schedules[i] == 0 || now < schedules[i]) continue;

    // Lange verpasste Zeitpunkte verfallen, ohne den Flow auszuloesen.
    if (now - schedules[i] > SCHEDULE_GRACE_S) {
      Serial.printf("Geplante Ausloesung verfallen: %s\n", formatTime(schedules[i]).c_str());
      schedules[i] = 0;
      saveSchedules();
      continue;
    }

    if (startFlow("schedule")) {
      Serial.printf("Geplante Ausloesung gestartet: %s\n", formatTime(schedules[i]).c_str());
      schedules[i] = 0;
      saveSchedules();
    }
    break;  // pro Durchlauf nur eine Auslösung
  }
}

void connectWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.printf("AP gestartet: %s / http://%s/\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  if (wifiSsid.length() == 0) {
    Serial.println("Keine Heimnetz-Zugangsdaten gespeichert.");
    return;
  }

  Serial.printf("Verbinde mit WLAN: %s\n", wifiSsid.c_str());
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  uint32_t timeoutAt = millis() + WIFI_CONNECT_MS;
  while (WiFi.status() != WL_CONNECTED && !timeReached(timeoutAt)) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Heimnetz verbunden: http://%s/\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("Heimnetz-Verbindung fehlgeschlagen. AP bleibt aktiv.");
  }
}

String schedulesJson() {
  String arr = "[";
  bool first = true;
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    if (schedules[i] == 0) continue;
    if (!first) arr += ",";
    first = false;
    arr += "{\"when\":" + String((uint32_t)schedules[i]);
    arr += ",\"label\":\"" + formatTime(schedules[i]) + "\"}";
  }
  arr += "]";
  return arr;
}

String statusJson() {
  String json = "{";
  json += "\"ok\":true";
  json += ",\"flowState\":\"" + String(flowStateName()) + "\"";
  json += ",\"autoLoopEnabled\":" + String(autoLoopEnabled ? "true" : "false");
  json += ",\"lastTrigger\":\"" + jsonEscape(lastTrigger) + "\"";
  json += ",\"apSsid\":\"" + String(AP_SSID) + "\"";
  json += ",\"apIp\":\"" + WiFi.softAPIP().toString() + "\"";
  json += ",\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  json += ",\"wifiIp\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"wifiSsid\":\"" + jsonEscape(wifiSsid) + "\"";
  json += ",\"time\":" + String((uint32_t)time(nullptr));
  json += ",\"timeSynced\":" + String(timeSynced ? "true" : "false");
  json += ",\"maxSchedules\":" + String(MAX_SCHEDULES);
  json += ",\"schedules\":" + schedulesJson();
  json += "}";
  return json;
}

String indexHtml() {
  String checked = autoLoopEnabled ? "checked" : "";

  String html = R"HTML(
<!doctype html>
<html lang="de">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>HID Steuerung</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #eef3f5;
      --surface: #ffffff;
      --surface-soft: #f7fafb;
      --text: #132025;
      --muted: #5d6b73;
      --line: #d8e2e6;
      --line-strong: #b7c7ce;
      --primary: #0f766e;
      --primary-dark: #0a5f59;
      --danger: #b42318;
      --ink: #24323a;
      --shadow: 0 18px 45px rgba(33, 49, 56, 0.14);
    }

    * { box-sizing: border-box; }

    body {
      margin: 0;
      min-height: 100vh;
      background:
        linear-gradient(135deg, rgba(15, 118, 110, 0.10), transparent 34%),
        linear-gradient(315deg, rgba(36, 50, 58, 0.10), transparent 38%),
        var(--bg);
      color: var(--text);
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      font-size: 16px;
      line-height: 1.5;
    }

    main {
      width: min(100%, 940px);
      margin: 0 auto;
      padding: 18px;
    }

    .app-shell {
      overflow: hidden;
      background: rgba(255, 255, 255, 0.92);
      border: 1px solid rgba(216, 226, 230, 0.9);
      border-radius: 8px;
      box-shadow: var(--shadow);
    }

    header {
      display: grid;
      gap: 18px;
      padding: 24px;
      background: linear-gradient(135deg, #ffffff 0%, #eff7f6 100%);
      border-bottom: 1px solid var(--line);
    }

    h1, h2, p { margin-top: 0; }
    p, .lead, .muted, .status-value { overflow-wrap: anywhere; }

    h1 {
      margin-bottom: 6px;
      font-size: clamp(28px, 7vw, 42px);
      line-height: 1.05;
      letter-spacing: 0;
    }

    h2 {
      margin-bottom: 14px;
      font-size: 18px;
      line-height: 1.25;
    }

    .lead {
      max-width: 640px;
      margin-bottom: 0;
      color: var(--muted);
      font-size: 16px;
    }

    .status-strip {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 10px;
    }

    .status-tile {
      min-width: 0;
      padding: 12px;
      background: var(--surface);
      border: 1px solid var(--line);
      border-radius: 8px;
    }

    .status-label {
      display: block;
      color: var(--muted);
      font-size: 12px;
      font-weight: 700;
      letter-spacing: 0.04em;
      text-transform: uppercase;
    }

    .status-value {
      display: block;
      overflow-wrap: anywhere;
      margin-top: 4px;
      font-size: 17px;
      font-weight: 800;
    }

    .content {
      display: grid;
      grid-template-columns: minmax(0, 1.1fr) minmax(280px, 0.9fr);
      gap: 0;
    }

    section {
      padding: 20px 24px 24px;
      border-bottom: 1px solid var(--line);
    }

    section:nth-child(odd) {
      border-right: 1px solid var(--line);
    }

    .wide {
      grid-column: 1 / -1;
      border-right: 0;
    }

    .actions {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
      margin-top: 16px;
    }

    button {
      appearance: none;
      min-height: 48px;
      border: 0;
      border-radius: 8px;
      padding: 12px 14px;
      color: #fff;
      background: var(--primary);
      cursor: pointer;
      font: inherit;
      font-size: 16px;
      font-weight: 800;
      line-height: 1.15;
      transition: transform 120ms ease, background 120ms ease, opacity 120ms ease;
    }

    button:active { transform: translateY(1px); }
    button:focus-visible { outline: 3px solid rgba(15, 118, 110, 0.28); outline-offset: 2px; }
    button.secondary { background: var(--ink); }
    button:disabled { cursor: wait; opacity: 0.72; }

    label {
      display: block;
      margin: 13px 0 7px;
      color: var(--ink);
      font-weight: 800;
    }

    input[type="text"],
    input[type="password"] {
      width: 100%;
      min-height: 46px;
      border: 1px solid var(--line-strong);
      border-radius: 8px;
      background: #fff;
      color: var(--text);
      padding: 10px 12px;
      font: inherit;
      font-size: 16px;
    }

    input:focus {
      outline: 3px solid rgba(15, 118, 110, 0.18);
      border-color: var(--primary);
    }

    .switch-row {
      display: grid;
      grid-template-columns: auto 1fr;
      gap: 12px;
      align-items: center;
      padding: 14px;
      background: var(--surface-soft);
      border: 1px solid var(--line);
      border-radius: 8px;
    }

    .switch-row input {
      width: 24px;
      height: 24px;
      accent-color: var(--primary);
    }

    .switch-copy strong {
      display: block;
      margin-bottom: 2px;
    }

    .muted {
      margin-bottom: 0;
      color: var(--muted);
      font-size: 14px;
    }

    .form-actions {
      display: flex;
      justify-content: flex-end;
      margin-top: 16px;
    }

    .api-list {
      display: grid;
      gap: 8px;
      margin: 0;
    }

    code {
      display: inline-block;
      max-width: 100%;
      overflow-wrap: anywhere;
      background: #edf2f4;
      border: 1px solid #dce5e8;
      border-radius: 6px;
      padding: 3px 6px;
      color: #203038;
      font-size: 14px;
    }

    .toast {
      min-height: 24px;
      margin-top: 12px;
      color: var(--muted);
      font-size: 14px;
      font-weight: 700;
    }

    .toast.error { color: var(--danger); }

    .mouse-pad {
      display: grid;
      grid-template-columns: repeat(3, minmax(64px, 1fr));
      grid-template-rows: repeat(3, 58px);
      gap: 10px;
      max-width: 340px;
      margin: 16px auto 0;
    }

    .mouse-pad button {
      min-height: 58px;
      padding: 8px;
      background: var(--ink);
      font-size: 24px;
      line-height: 1;
    }

    .mouse-pad .click-button {
      background: var(--primary);
      font-size: 15px;
      line-height: 1.15;
    }

    .mouse-pad .empty {
      min-height: 58px;
      border: 1px dashed var(--line);
      border-radius: 8px;
      background: var(--surface-soft);
    }

    .hero {
      display: grid;
      grid-template-columns: minmax(0, 1fr) auto;
      gap: 18px;
      align-items: start;
    }

    .clock-card {
      display: grid;
      gap: 2px;
      min-width: 200px;
      padding: 14px 18px;
      background: linear-gradient(135deg, var(--ink), #0d171c);
      color: #eaf3f2;
      border-radius: 12px;
      text-align: right;
    }

    .clock-label {
      color: #9fb4bb;
      font-size: 11px;
      font-weight: 700;
      letter-spacing: 0.08em;
      text-transform: uppercase;
    }

    .clock-time {
      font-size: 30px;
      font-weight: 800;
      line-height: 1.1;
      font-variant-numeric: tabular-nums;
    }

    .clock-date { color: #c4d3d8; font-size: 14px; }

    .clock-sync { margin-top: 4px; font-size: 12px; font-weight: 700; color: #9fb4bb; }
    .clock-sync.ok { color: #63d6a4; }
    .clock-sync.bad { color: #f0a36b; }

    .status-grid {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 10px;
    }

    .status-card {
      min-width: 0;
      padding: 12px 14px;
      background: var(--surface);
      border: 1px solid var(--line);
      border-radius: 10px;
    }

    .dot {
      display: inline-block;
      width: 10px;
      height: 10px;
      margin-right: 8px;
      border-radius: 50%;
      background: var(--muted);
      vertical-align: middle;
    }

    .dot.idle { background: #7c8a91; }
    .dot.running { background: #0f9d6b; box-shadow: 0 0 0 4px rgba(15, 157, 107, 0.18); }
    .dot.standby { background: #d9a23b; box-shadow: 0 0 0 4px rgba(217, 162, 59, 0.18); }

    .schedule-add {
      display: grid;
      grid-template-columns: 1fr auto;
      gap: 10px;
      margin-top: 14px;
    }

    .schedule-add input {
      width: 100%;
      min-height: 46px;
      border: 1px solid var(--line-strong);
      border-radius: 8px;
      background: #fff;
      color: var(--text);
      padding: 10px 12px;
      font: inherit;
      font-size: 16px;
    }

    .schedule-add input:focus {
      outline: 3px solid rgba(15, 118, 110, 0.18);
      border-color: var(--primary);
    }

    .schedule-list {
      display: grid;
      gap: 8px;
      margin: 14px 0 0;
      padding: 0;
      list-style: none;
    }

    .schedule-item {
      display: grid;
      grid-template-columns: 1fr auto auto;
      gap: 10px;
      align-items: center;
      padding: 10px 12px;
      background: var(--surface-soft);
      border: 1px solid var(--line);
      border-radius: 10px;
    }

    .schedule-when { font-weight: 800; }
    .schedule-count {
      color: var(--muted);
      font-size: 13px;
      font-weight: 700;
      font-variant-numeric: tabular-nums;
    }

    .schedule-item button {
      min-height: auto;
      padding: 7px 12px;
      font-size: 13px;
      background: var(--danger);
    }

    @media (max-width: 760px) {
      main { padding: 10px; }
      header { padding: 20px; }
      .lead { font-size: 15px; }
      .hero { grid-template-columns: 1fr; }
      .clock-card { text-align: left; }
      .status-grid { grid-template-columns: 1fr 1fr; }
      .content { grid-template-columns: 1fr; }
      section, section:nth-child(odd) {
        padding: 18px;
        border-right: 0;
      }
      .actions { grid-template-columns: 1fr; }
      .form-actions, .form-actions button { width: 100%; }
    }

    @media (prefers-reduced-motion: reduce) {
      button { transition: none; }
    }
  </style>
</head>
<body>
  <main>
    <div class="app-shell">
      <header>
        <div class="hero">
          <div class="hero-text">
            <h1>HID Steuerung</h1>
            <p class="lead">Flow, Automatik, geplante Auslösung und WLAN direkt am Geraet steuern.</p>
          </div>
          <div class="clock-card">
            <span class="clock-label">Gerätezeit</span>
            <span id="clockTime" class="clock-time">--:--:--</span>
            <span id="clockDate" class="clock-date">– – –</span>
            <span id="clockSync" class="clock-sync">Synchronisiere …</span>
          </div>
        </div>

        <div class="status-grid" aria-live="polite">
          <div class="status-card">
            <span class="status-label">Flow</span>
            <span class="status-value"><span id="flowDot" class="dot idle"></span><span id="flowState">...</span></span>
          </div>
          <div class="status-card">
            <span class="status-label">Automatik</span>
            <span id="autoState" class="status-value">…</span>
          </div>
          <div class="status-card">
            <span class="status-label">WLAN</span>
            <span id="wifiState" class="status-value">…</span>
          </div>
          <div class="status-card">
            <span class="status-label">Nächste Auslösung</span>
            <span id="nextTrigger" class="status-value">–</span>
          </div>
        </div>
      </header>

      <div class="content">
        <section>
          <h2>Direktsteuerung</h2>
          <p class="muted">Ablauf: Linksklick, 5 s warten, Enter, 20 s warten, Strg+Alt+F.</p>
          <div class="actions">
            <button id="triggerButton" type="button" onclick="triggerFlow()">Flow ausloesen</button>
            <button class="secondary" type="button" onclick="refreshStatus()">Status aktualisieren</button>
          </div>
          <div id="message" class="toast" role="status"></div>
        </section>

        <section>
          <h2>Maus</h2>
          <p class="muted">Verschiebt den Cursor in kleinen Schritten oder simuliert einen linken Mausklick.</p>
          <div class="mouse-pad" aria-label="Maussteuerung">
            <span class="empty" aria-hidden="true"></span>
            <button type="button" onclick="moveMouse(0, -1)" aria-label="Maus nach oben">&uarr;</button>
            <span class="empty" aria-hidden="true"></span>
            <button type="button" onclick="moveMouse(-1, 0)" aria-label="Maus nach links">&larr;</button>
            <button class="click-button" type="button" onclick="clickMouse()">Linksklick</button>
            <button type="button" onclick="moveMouse(1, 0)" aria-label="Maus nach rechts">&rarr;</button>
            <span class="empty" aria-hidden="true"></span>
            <button type="button" onclick="moveMouse(0, 1)" aria-label="Maus nach unten">&darr;</button>
            <span class="empty" aria-hidden="true"></span>
          </div>
        </section>

        <section>
          <h2>Betrieb</h2>
          <form method="post" action="/settings">
            <label class="switch-row">
              <input id="autoInput" type="checkbox" name="auto" value="1" AUTO_CHECKED>
              <span class="switch-copy">
                <strong>Automatische Schleife</strong>
                <span class="muted">Aus: Flow nur per Website oder API.</span>
              </span>
            </label>
            <div class="form-actions">
              <button type="submit">Einstellung speichern</button>
            </div>
          </form>
        </section>

        <section class="wide">
          <h2>Geplante Auslösung</h2>
          <p class="muted">Lege Zeitpunkte fest, an denen der Flow einmalig automatisch startet. Benötigt eine synchronisierte Gerätezeit.</p>
          <div class="schedule-add">
            <input id="scheduleInput" type="datetime-local" aria-label="Zeitpunkt für Auslösung">
            <button type="button" onclick="addSchedule()">Hinzufügen</button>
          </div>
          <ul id="scheduleList" class="schedule-list"></ul>
          <p id="scheduleEmpty" class="muted">Keine geplanten Auslösungen.</p>
        </section>

        <section>
          <h2>Heimnetz</h2>
          <form method="post" action="/wifi">
            <label for="ssid">SSID</label>
            <input id="ssid" name="ssid" type="text" value="SSID_VALUE" autocomplete="wifi ssid">

            <label for="pass">Passwort</label>
            <input id="pass" name="pass" type="password" value="" autocomplete="current-password">

            <p class="muted">Setup-AP bleibt aktiv: <code>AP_SSID_VALUE</code>, IP <code>AP_IP_VALUE</code>.</p>
            <div class="form-actions">
              <button type="submit">WLAN speichern</button>
            </div>
          </form>
        </section>

        <section>
          <h2>API</h2>
          <div class="api-list">
            <p><code>POST /api/trigger</code> Flow starten.</p>
            <p><code>POST /api/mouse?dx=20&amp;dy=0</code> Maus bewegen.</p>
            <p><code>POST /api/click</code> Linksklick senden.</p>
            <p><code>GET /api/status</code> Status lesen.</p>
            <p><code>POST /api/settings?auto=1</code> Auto ein.</p>
            <p><code>POST /api/settings?auto=0</code> Auto aus.</p>
            <p><code>POST /api/schedule?when=2026-06-01T14:30</code> Auslösung planen.</p>
            <p><code>POST /api/schedule/delete?when=EPOCH</code> Auslösung löschen.</p>
          </div>
        </section>
      </div>
    </div>
  </main>

  <script>
    const triggerButton = document.getElementById('triggerButton');
    const message = document.getElementById('message');

    function setMessage(text, isError) {
      message.textContent = text || '';
      message.className = isError ? 'toast error' : 'toast';
    }

    async function triggerFlow() {
      triggerButton.disabled = true;
      setMessage('Flow wird gestartet ...', false);

      try {
        const response = await fetch('/api/trigger', { method: 'POST' });
        const data = await response.json();
        if (!data.ok) {
          setMessage(data.error || 'Flow konnte nicht gestartet werden.', true);
        } else {
          setMessage('Flow gestartet.', false);
        }
      } catch (error) {
        setMessage('Keine Verbindung zum Geraet.', true);
      }

      await refreshStatus();
      triggerButton.disabled = false;
    }

    async function moveMouse(x, y) {
      const dx = x * MANUAL_MOUSE_STEP_VALUE;
      const dy = y * MANUAL_MOUSE_STEP_VALUE;
      setMessage('Maus wird bewegt ...', false);

      try {
        const response = await fetch('/api/mouse?dx=' + dx + '&dy=' + dy, { method: 'POST' });
        const data = await response.json();
        setMessage(data.ok ? 'Maus bewegt.' : (data.error || 'Maus konnte nicht bewegt werden.'), !data.ok);
      } catch (error) {
        setMessage('Keine Verbindung zum Geraet.', true);
      }
    }

    async function clickMouse() {
      setMessage('Linksklick wird gesendet ...', false);

      try {
        const response = await fetch('/api/click', { method: 'POST' });
        const data = await response.json();
        setMessage(data.ok ? 'Linksklick gesendet.' : (data.error || 'Linksklick fehlgeschlagen.'), !data.ok);
      } catch (error) {
        setMessage('Keine Verbindung zum Geraet.', true);
      }
    }

    const FLOW_INFO = {
      idle: { label: 'Bereit', dot: 'idle' },
      after_click_wait: { label: 'Klick gesendet – warte …', dot: 'running' },
      after_enter_wait: { label: 'Enter gesendet – warte …', dot: 'running' },
      auto_standby: { label: 'Standby (Automatik)', dot: 'standby' }
    };

    const WEEKDAYS = ['Sonntag', 'Montag', 'Dienstag', 'Mittwoch', 'Donnerstag', 'Freitag', 'Samstag'];
    const MONTHS = ['Jan.', 'Feb.', 'März', 'Apr.', 'Mai', 'Juni', 'Juli', 'Aug.', 'Sep.', 'Okt.', 'Nov.', 'Dez.'];

    let serverEpoch = 0;
    let fetchedAt = 0;
    let timeSynced = false;
    let schedules = [];

    function pad(n) { return String(n).padStart(2, '0'); }

    function nowEpochMs() {
      if (!serverEpoch) return Date.now();
      return serverEpoch * 1000 + (Date.now() - fetchedAt);
    }

    function fmtStamp(epochSec) {
      const d = new Date(epochSec * 1000);
      return WEEKDAYS[d.getDay()].slice(0, 2) + ', ' + d.getDate() + '. ' + MONTHS[d.getMonth()] +
             ' ' + d.getFullYear() + ', ' + pad(d.getHours()) + ':' + pad(d.getMinutes()) + ' Uhr';
    }

    function fmtDuration(ms) {
      if (ms < 0) ms = 0;
      let s = Math.floor(ms / 1000);
      const d = Math.floor(s / 86400); s -= d * 86400;
      const h = Math.floor(s / 3600); s -= h * 3600;
      const m = Math.floor(s / 60); s -= m * 60;
      if (d > 0) return d + ' T ' + h + ' h';
      if (h > 0) return h + ' h ' + pad(m) + ' min';
      if (m > 0) return m + ' min ' + pad(s) + ' s';
      return s + ' s';
    }

    function tick() {
      const clockTime = document.getElementById('clockTime');
      const clockDate = document.getElementById('clockDate');

      if (!timeSynced) {
        clockTime.textContent = '--:--:--';
        clockDate.textContent = '– – –';
      } else {
        const d = new Date(nowEpochMs());
        clockTime.textContent = pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
        clockDate.textContent = WEEKDAYS[d.getDay()] + ', ' + d.getDate() + '. ' + MONTHS[d.getMonth()] + ' ' + d.getFullYear();
      }

      renderSchedules();
      renderNextTrigger();
    }

    function renderNextTrigger() {
      const el = document.getElementById('nextTrigger');
      const now = nowEpochMs();
      const future = schedules.filter(s => s.when * 1000 > now).sort((a, b) => a.when - b.when);
      el.textContent = future.length ? 'in ' + fmtDuration(future[0].when * 1000 - now) : '–';
    }

    function renderSchedules() {
      const list = document.getElementById('scheduleList');
      const empty = document.getElementById('scheduleEmpty');
      const sorted = schedules.slice().sort((a, b) => a.when - b.when);

      if (sorted.length === 0) {
        list.innerHTML = '';
        empty.style.display = '';
        return;
      }

      empty.style.display = 'none';
      const now = nowEpochMs();
      list.innerHTML = sorted.map(s =>
        '<li class="schedule-item">' +
          '<span class="schedule-when">' + fmtStamp(s.when) + '</span>' +
          '<span class="schedule-count">in ' + fmtDuration(s.when * 1000 - now) + '</span>' +
          '<button type="button" onclick="deleteSchedule(' + s.when + ')">Löschen</button>' +
        '</li>'
      ).join('');
    }

    function applyStatus(data) {
      serverEpoch = data.time || 0;
      fetchedAt = Date.now();
      timeSynced = !!data.timeSynced;
      schedules = data.schedules || [];

      const info = FLOW_INFO[data.flowState] || { label: data.flowState, dot: 'idle' };
      document.getElementById('flowState').textContent = info.label;
      document.getElementById('flowDot').className = 'dot ' + info.dot;
      document.getElementById('autoState').textContent = data.autoLoopEnabled ? 'Ein' : 'Aus';
      document.getElementById('wifiState').textContent = data.wifiConnected ? data.wifiIp : 'AP-Modus';
      document.getElementById('autoInput').checked = !!data.autoLoopEnabled;

      const sync = document.getElementById('clockSync');
      sync.textContent = timeSynced ? 'Zeit synchronisiert' : 'Zeit nicht synchronisiert';
      sync.className = 'clock-sync ' + (timeSynced ? 'ok' : 'bad');

      renderSchedules();
      renderNextTrigger();
    }

    async function refreshStatus() {
      try {
        const response = await fetch('/api/status');
        applyStatus(await response.json());
      } catch (error) {
        setMessage('Status konnte nicht geladen werden.', true);
      }
    }

    async function addSchedule() {
      const input = document.getElementById('scheduleInput');
      if (!input.value) { setMessage('Bitte Datum und Uhrzeit wählen.', true); return; }

      setMessage('Zeitpunkt wird gespeichert …', false);
      try {
        const response = await fetch('/api/schedule?when=' + encodeURIComponent(input.value), { method: 'POST' });
        const data = await response.json();
        if (!data.ok) {
          setMessage(data.error || 'Konnte nicht gespeichert werden.', true);
        } else {
          setMessage('Auslösung geplant.', false);
          input.value = '';
          applyStatus(data);
        }
      } catch (error) {
        setMessage('Keine Verbindung zum Geraet.', true);
      }
    }

    async function deleteSchedule(when) {
      try {
        const response = await fetch('/api/schedule/delete?when=' + when, { method: 'POST' });
        applyStatus(await response.json());
        setMessage('Auslösung gelöscht.', false);
      } catch (error) {
        setMessage('Keine Verbindung zum Geraet.', true);
      }
    }

    function initScheduleInput() {
      const input = document.getElementById('scheduleInput');
      const soon = new Date(Date.now() + 5 * 60000 - new Date().getTimezoneOffset() * 60000);
      const now = new Date(Date.now() - new Date().getTimezoneOffset() * 60000);
      input.min = now.toISOString().slice(0, 16);
      input.value = soon.toISOString().slice(0, 16);
    }

    initScheduleInput();
    refreshStatus();
    setInterval(refreshStatus, 3000);
    setInterval(tick, 1000);
  </script>
</body>
</html>
)HTML";

  html.replace("AP_SSID_VALUE", AP_SSID);
  html.replace("AP_IP_VALUE", WiFi.softAPIP().toString());
  html.replace("AUTO_CHECKED", checked);
  html.replace("MANUAL_MOUSE_STEP_VALUE", String(MANUAL_MOUSE_STEP));
  html.replace("SSID_VALUE", htmlEscape(wifiSsid));
  return html;
}

void redirectHome() {
  Server.sendHeader("Location", "/", true);
  Server.send(303, "text/plain", "");
}

void handleRoot() {
  Server.send(200, "text/html", indexHtml());
}

void handleStatus() {
  Server.send(200, "application/json", statusJson());
}

void handleTrigger() {
  if (!startFlow("api")) {
    Server.send(409, "application/json", "{\"ok\":false,\"error\":\"flow already running\"}");
    return;
  }

  Server.send(200, "application/json", "{\"ok\":true}");
}

void handleMouseMove() {
  int16_t dx = Server.arg("dx").toInt();
  int16_t dy = Server.arg("dy").toInt();

  dx = constrain(dx, -100, 100);
  dy = constrain(dy, -100, 100);
  Serial.printf("Maus bewegen: dx=%d dy=%d\n", dx, dy);
  Mouse.move((int8_t)dx, (int8_t)dy);

  Server.send(200, "application/json", "{\"ok\":true}");
}

void handleMouseClick() {
  Serial.println("Linksklick senden");
  leftClick();
  Server.send(200, "application/json", "{\"ok\":true}");
}

void handleSettings() {
  if (Server.hasArg("auto")) {
    String autoValue = Server.arg("auto");
    autoLoopEnabled = autoValue == "1" || autoValue == "true" || autoValue == "on";
  } else {
    autoLoopEnabled = false;
  }

  saveSettings();

  if (autoLoopEnabled && flowState == FLOW_IDLE) {
    startFlow("auto");
  }

  if (Server.uri().startsWith("/api/")) {
    Server.send(200, "application/json", statusJson());
  } else {
    redirectHome();
  }
}

void handleScheduleAdd() {
  String whenStr = Server.arg("when");
  time_t when = whenStr.indexOf('-') >= 0
    ? parseLocalDateTime(whenStr)
    : (time_t)strtoul(whenStr.c_str(), nullptr, 10);

  if (when <= 0) {
    Server.send(400, "application/json", "{\"ok\":false,\"error\":\"ungueltige zeit\"}");
    return;
  }
  if (!timeSynced) {
    Server.send(409, "application/json", "{\"ok\":false,\"error\":\"zeit noch nicht synchronisiert\"}");
    return;
  }
  if (when <= time(nullptr)) {
    Server.send(400, "application/json", "{\"ok\":false,\"error\":\"zeitpunkt liegt in der vergangenheit\"}");
    return;
  }
  if (!addSchedule(when)) {
    Server.send(409, "application/json", "{\"ok\":false,\"error\":\"keine freien plaetze\"}");
    return;
  }

  Serial.printf("Auslösung geplant: %s\n", formatTime(when).c_str());
  Server.send(200, "application/json", statusJson());
}

void handleScheduleDelete() {
  time_t when = (time_t)strtoul(Server.arg("when").c_str(), nullptr, 10);
  removeSchedule(when);
  Server.send(200, "application/json", statusJson());
}

void handleWifiSave() {
  wifiSsid = Server.arg("ssid");
  wifiPassword = Server.arg("pass");
  saveSettings();

  if (wifiSsid.length() > 0) {
    WiFi.disconnect();
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  }

  redirectHome();
}

void setupRoutes() {
  Server.on("/", HTTP_GET, handleRoot);
  Server.on("/api/status", HTTP_GET, handleStatus);
  Server.on("/api/trigger", HTTP_POST, handleTrigger);
  Server.on("/api/trigger", HTTP_GET, handleTrigger);
  Server.on("/api/mouse", HTTP_POST, handleMouseMove);
  Server.on("/api/mouse", HTTP_GET, handleMouseMove);
  Server.on("/api/click", HTTP_POST, handleMouseClick);
  Server.on("/api/click", HTTP_GET, handleMouseClick);
  Server.on("/api/settings", HTTP_POST, handleSettings);
  Server.on("/settings", HTTP_POST, handleSettings);
  Server.on("/api/schedule", HTTP_POST, handleScheduleAdd);
  Server.on("/api/schedule", HTTP_GET, handleScheduleAdd);
  Server.on("/api/schedule/delete", HTTP_POST, handleScheduleDelete);
  Server.on("/api/schedule/delete", HTTP_GET, handleScheduleDelete);
  Server.on("/api/wifi", HTTP_POST, handleWifiSave);
  Server.on("/wifi", HTTP_POST, handleWifiSave);
  Server.onNotFound([]() {
    Server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
  });
  Server.begin();
  Serial.println("Webserver gestartet.");
}

void setup() {
  Serial.begin(115200);

  Keyboard.begin();
  Mouse.begin();
  USB.begin();

  loadSettings();
  loadSchedules();
  connectWifi();

  // Zeit beim Start holen (Mitteleuropa). SNTP laeuft im Hintergrund weiter.
  configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2);
  if (WiFi.status() == WL_CONNECTED) {
    syncTimeNow(5000);
  }

  setupRoutes();

  delay(1000);
  drawStartupRectangle();

  // Initialisierungszeit vor dem Automatikzyklus: 10 Sekunden
  for (int i=10; i>0; i--) {
    Serial.printf("Starte in %d...\n", i);
    delay(1000);
    Server.handleClient();
  }
  nextJiggleAt = millis() + JIGGLE_INTERVAL;
  Serial.println("Bereit.");
}

void loop() {
  Server.handleClient();
  maintainTime();
  checkSchedules();
  handleFlow();
  delay(5);
}
