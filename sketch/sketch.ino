#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

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
  json += "}";
  return json;
}

String indexHtml() {
  String checked = autoLoopEnabled ? "checked" : "";
  String wifiState = WiFi.status() == WL_CONNECTED
    ? String("Verbunden: ") + WiFi.localIP().toString()
    : "Nicht verbunden";
  String autoText = autoLoopEnabled ? "Ein" : "Aus";

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

    @media (max-width: 760px) {
      main { padding: 10px; }
      header { padding: 20px; }
      .lead { font-size: 15px; }
      .status-strip { grid-template-columns: 1fr; }
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
        <div>
          <h1>HID Steuerung</h1>
          <p class="lead">Flow, Auto-Schleife und WLAN direkt am Geraet steuern.</p>
        </div>

        <div class="status-strip" aria-live="polite">
          <div class="status-tile">
            <span class="status-label">Flow</span>
            <span id="flowState" class="status-value">...</span>
          </div>
          <div class="status-tile">
            <span class="status-label">Auto</span>
            <span id="autoState" class="status-value">AUTO_STATE</span>
          </div>
          <div class="status-tile">
            <span class="status-label">WLAN</span>
            <span id="wifiState" class="status-value">WIFI_STATE</span>
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

    async function refreshStatus() {
      try {
        const response = await fetch('/api/status');
        const data = await response.json();
        document.getElementById('flowState').textContent = data.flowState;
        document.getElementById('autoState').textContent = data.autoLoopEnabled ? 'Ein' : 'Aus';
        document.getElementById('wifiState').textContent = data.wifiConnected ? data.wifiIp : 'Nicht verbunden';
        document.getElementById('autoInput').checked = !!data.autoLoopEnabled;
      } catch (error) {
        setMessage('Status konnte nicht geladen werden.', true);
      }
    }

    refreshStatus();
    setInterval(refreshStatus, 3000);
  </script>
</body>
</html>
)HTML";

  html.replace("WIFI_STATE", htmlEscape(wifiState));
  html.replace("AUTO_STATE", autoText);
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
  connectWifi();
  setupRoutes();

  delay(1000);
  drawStartupRectangle();

  // Initialisierungszeit vor dem Automatikzyklus: 10 Sekunden
  for (int i=10; i>0; i--) {
    Serial.printf("Starte in %d...\n", i);
    delay(1000);
    Server.handleClient();
  }
  Serial.println("Bereit.");
}

void loop() {
  Server.handleClient();
  handleFlow();
  delay(5);
}
