#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <string.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

USBHIDKeyboard Keyboard;
USBHIDMouse    Mouse;
WebServer      Server(80);
Preferences    Settings;

static constexpr int DISPLAY_MOSI = 3;
static constexpr int DISPLAY_SCLK = 5;
static constexpr int DISPLAY_CS   = 4;
static constexpr int DISPLAY_DC   = 2;
static constexpr int DISPLAY_RST  = 1;
static constexpr int DISPLAY_BL   = 38;

SPIClass DisplaySPI(FSPI);
Adafruit_ST7735 Display(&DisplaySPI, DISPLAY_CS, DISPLAY_DC, DISPLAY_RST);

#ifdef INITR_MINI160x80_PLUGIN
static constexpr uint8_t DISPLAY_INITR = INITR_MINI160x80_PLUGIN;
#else
static constexpr uint8_t DISPLAY_INITR = INITR_MINI160x80;
#endif

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static constexpr uint16_t COLOR_BLACK     = ST77XX_BLACK;
static constexpr uint16_t COLOR_WHITE     = ST77XX_WHITE;
static constexpr uint16_t COLOR_RED       = ST77XX_RED;
static constexpr uint16_t COLOR_GREEN     = ST77XX_GREEN;
static constexpr uint16_t COLOR_BLUE      = ST77XX_BLUE;
static constexpr uint16_t COLOR_YELLOW    = ST77XX_YELLOW;
static constexpr uint16_t COLOR_CYAN      = ST77XX_CYAN;
static constexpr uint16_t COLOR_ORANGE    = rgb565(255, 165, 0);
static constexpr uint16_t COLOR_DARKGREY  = rgb565(128, 128, 128);
static constexpr uint16_t COLOR_LIGHTGREY = rgb565(192, 192, 192);

// --- Konfiguration ---
const uint32_t CLICK_DELAY_MS     = 50;
const uint32_t AFTER_CLICK_WAIT   = 5000;          // 5 s
const uint32_t AFTER_ENTER_WAIT   = 20000;         // 20 s
const uint32_t JIGGLE_INTERVAL    = 5UL * 60UL * 1000UL; // alle 5 min kaum merkbar bewegen
const uint32_t LONG_WAIT_MIN_MS   = 25UL * 60UL * 1000UL;
const uint32_t LONG_WAIT_MAX_MS   = 35UL * 60UL * 1000UL;
const uint32_t MANUAL_GAP_MIN_MS  = 10UL * 1000UL;
const uint32_t MANUAL_GAP_MAX_MS  = 30UL * 1000UL;
const int16_t  START_RECT_CM      = 5;             // sichtbarer Start-Indikator
const int16_t  STANDBY_JIGGLE_PX  = 2;             // minimales Wachhalte-Jiggle
const int16_t  PIXELS_PER_CM      = 38;            // Naeherung bei 96 dpi
const int8_t   START_RECT_STEP    = 5;             // kleine Schritte fuer langsame Bewegung
const uint16_t START_RECT_DELAY   = 50;            // ms pro Schritt
const char*    AP_SSID            = "HID-Setup";
const char*    AP_PASSWORD        = "hidsetup123";
const uint32_t WIFI_CONNECT_MS    = 15000;
const uint32_t WIFI_RETRY_MS      = 30000;          // Reconnect-Versuch im Hintergrund
const uint32_t LONG_PRESS_MS      = 800;            // Tastendruck ab hier = Aktion statt Blaettern
const uint32_t SSE_INTERVAL_MS    = 1000;           // Push-Takt fuer Live-Status
const uint8_t  SSE_MAX_CLIENTS    = 3;
const char*    OTA_HOSTNAME       = "hid-dongle";
const int8_t   MANUAL_MOUSE_STEP  = 20;
const char*    NTP_SERVER1        = "pool.ntp.org";
const char*    NTP_SERVER2        = "time.nist.gov";
const char*    TZ_INFO            = "CET-1CEST,M3.5.0,M10.5.0/3";  // Europe/Vienna (MEZ/MESZ inkl. Sommerzeit)
const uint8_t  MAX_SCHEDULES      = 8;
const uint32_t TIME_RETRY_MS      = 60000;          // erneuter Sync-Versuch
const int32_t  SCHEDULE_GRACE_S   = 3600;           // verpasste Auslösung noch bis 1 h spaeter nachholen
const uint32_t DISPLAY_REFRESH_MS = 1000;
const uint32_t DISPLAY_PAGE_MS    = 5000;
const uint32_t BUTTON_DEBOUNCE_MS = 220;
const uint8_t  DISPLAY_PAGE_COUNT = 2;  // Basisseiten; +1 wenn Aktivitaeten existieren
const uint8_t  DISPLAY_BUTTON_A   = 0;

enum FlowState {
  FLOW_IDLE,
  FLOW_AFTER_CLICK_WAIT,
  FLOW_AFTER_ENTER_WAIT,
  FLOW_AUTO_STANDBY,
  FLOW_MANUAL_START_WAIT,
  FLOW_MANUAL_AFTER_CLICK_WAIT,
  FLOW_MANUAL_EVENT_WAIT,
  FLOW_MANUAL_GAP_WAIT
};

enum UsbRuntimeState {
  USB_WAITING,
  USB_ACTIVE,
  USB_SUSPENDED,
  USB_STOPPED
};

FlowState flowState = FLOW_IDLE;
bool autoLoopEnabled = false;
uint32_t stateUntil = 0;
uint32_t nextJiggleAt = 0;
String wifiSsid;
String wifiPassword;
String lastTrigger = "none";
time_t schedules[MAX_SCHEDULES];
time_t manualPlanStartAt = 0;
int32_t plannerUntilSeconds = -1;
uint16_t plannerMinMinutes = 15;
uint16_t plannerMaxMinutes = 44;
uint8_t plannerEventCount = 4;
bool timeSynced = false;
uint32_t nextTimeRetryAt = 0;
uint32_t nextDisplayRefreshAt = 0;
uint32_t nextDisplayPageAt = 0;
uint32_t lastDisplayButtonAt = 0;
uint8_t displayPage = 0;
bool displayDirty = true;
bool displayButtonWasPressed = false;
bool displayButtonLongFired = false;
uint32_t displayButtonDownAt = 0;
bool displayReady = false;
uint32_t nextWifiRetryAt = 0;
bool wifiWasConnected = false;
bool otaStarted = false;
WiFiClient sseClients[SSE_MAX_CLIENTS];
uint32_t nextSseAt = 0;
volatile UsbRuntimeState usbRuntimeState = USB_WAITING;
volatile bool usbWasMounted = false;

const char* flowStateName();

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
  Mouse.move(-STANDBY_JIGGLE_PX, 0);
  delay(20);
  Mouse.move(STANDBY_JIGGLE_PX, 0);
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

String displayWifiIp() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "nicht verbunden";
}

bool hidActive() {
  return (bool)USB && usbRuntimeState != USB_SUSPENDED;
}

String flowStateDisplayName() {
  switch (flowState) {
    case FLOW_IDLE:
      return "Bereit";
    case FLOW_AFTER_CLICK_WAIT:
      return "Klick warte";
    case FLOW_AFTER_ENTER_WAIT:
      return "Enter warte";
    case FLOW_AUTO_STANDBY:
      return "Auto Standby";
    case FLOW_MANUAL_START_WAIT:
      return "Plan wartet";
    case FLOW_MANUAL_AFTER_CLICK_WAIT:
      return "Plan Klick";
    case FLOW_MANUAL_EVENT_WAIT:
      return "Plan Event";
    case FLOW_MANUAL_GAP_WAIT:
      return "Plan Pause";
  }

  return "Unbekannt";
}

String modeLabel() {
  if (manualPlanStartAt != 0) return "Plan manuell";
  return autoLoopEnabled ? "Automatik ein" : "Manuell";
}

String hidStatusLabel() {
  if (hidActive()) return "HID aktiv";
  if (usbRuntimeState == USB_SUSPENDED) return "USB pausiert";
  if (usbRuntimeState == USB_STOPPED) return "USB getrennt";
  if (usbWasMounted) return "Host verloren";
  return "warte auf Host";
}

uint16_t hidStatusColor() {
  if (hidActive()) return COLOR_GREEN;
  if (usbRuntimeState == USB_SUSPENDED) return COLOR_ORANGE;
  return COLOR_RED;
}

String durationLabelMs(uint64_t ms) {
  uint32_t seconds = (ms + 999) / 1000;
  uint32_t days = seconds / 86400;
  seconds -= days * 86400;
  uint32_t hours = seconds / 3600;
  seconds -= hours * 3600;
  uint32_t minutes = seconds / 60;
  seconds -= minutes * 60;

  if (days > 0) return String(days) + "d " + String(hours) + "h";
  if (hours > 0) return String(hours) + "h " + String(minutes) + "m";
  if (minutes > 0) return String(minutes) + "m " + String(seconds) + "s";
  return String(seconds) + "s";
}

time_t nextScheduleTime() {
  time_t next = 0;
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    if (schedules[i] == 0) continue;
    if (next == 0 || schedules[i] < next) next = schedules[i];
  }
  return next;
}

bool hasActivities() {
  if (autoLoopEnabled) return true;
  if (flowState != FLOW_IDLE || manualPlanStartAt != 0) return true;
  return nextScheduleTime() != 0;
}

uint8_t displayPageCount() {
  return hasActivities() ? DISPLAY_PAGE_COUNT + 1 : DISPLAY_PAGE_COUNT;
}

String nextTriggerLabel() {
  if (flowState == FLOW_AFTER_CLICK_WAIT || flowState == FLOW_AFTER_ENTER_WAIT ||
      flowState == FLOW_MANUAL_AFTER_CLICK_WAIT) {
    return "Flow laeuft";
  }

  if (flowState == FLOW_AUTO_STANDBY) {
    uint32_t remaining = timeReached(stateUntil) ? 0 : stateUntil - millis();
    return "Auto in " + durationLabelMs(remaining);
  }

  if (flowState == FLOW_MANUAL_GAP_WAIT) {
    uint32_t remaining = timeReached(stateUntil) ? 0 : stateUntil - millis();
    return "Klick in " + durationLabelMs(remaining);
  }

  if (flowState == FLOW_MANUAL_START_WAIT && timeSynced && manualPlanStartAt != 0) {
    time_t now = time(nullptr);
    if (manualPlanStartAt <= now) return "Plan startet";
    return "Start in " + durationLabelMs((uint64_t)(manualPlanStartAt - now) * 1000ULL);
  }

  if (autoLoopEnabled) {
    return "Auto sofort";
  }

  time_t next = nextScheduleTime();
  if (next == 0) return "keine";
  if (!timeSynced) return "Zeit fehlt";

  if (manualPlanStartAt == 0) return "Plan bereit";

  time_t now = time(nullptr);
  if (next <= now) return "faellig";
  return "Plan in " + durationLabelMs((uint64_t)(next - now) * 1000ULL);
}

void drawPageIndicator() {
  const int16_t x = Display.width() - 6;
  const int16_t h = Display.height();
  uint16_t inactive = COLOR_DARKGREY;
  uint8_t pages = displayPageCount();

  for (uint8_t i = 0; i < pages; i++) {
    int16_t top = (int32_t)h * i / pages;
    int16_t bottom = (int32_t)h * (i + 1) / pages;
    Display.fillRect(x, top, 6, (bottom - top) - 1, displayPage == i ? COLOR_YELLOW : inactive);
  }
}

String fitDisplayText(const String& value, uint8_t maxChars = 18) {
  if (value.length() <= maxChars) return value;
  return value.substring(0, maxChars - 3) + "...";
}

void drawLabelValue(int16_t y, const String& label, const String& value, uint16_t valueColor = COLOR_WHITE) {
  Display.setTextSize(1);
  Display.setTextColor(COLOR_LIGHTGREY, COLOR_BLACK);
  Display.setCursor(4, y);
  Display.print(label);
  Display.setTextColor(valueColor, COLOR_BLACK);
  Display.setCursor(48, y);
  Display.print(fitDisplayText(value, 17));
}

void drawDisplayHeader(const char* title) {
  Display.fillScreen(COLOR_BLACK);
  Display.setTextSize(2);
  Display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  Display.setCursor(4, 1);
  Display.print(title);
  Display.drawFastHLine(4, 20, Display.width() - 16, COLOR_DARKGREY);
  drawPageIndicator();
}

void drawNetworkPage() {
  drawDisplayHeader("Netz");

  bool wifiConnected = WiFi.status() == WL_CONNECTED;

  // Primaere (grosse) Adresse: WLAN sobald verbunden, sonst AP.
  const char* primaryLabel = wifiConnected ? "WLAN" : "AP";
  String primaryIp = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();

  Display.setTextColor(COLOR_CYAN, COLOR_BLACK);
  Display.setTextSize(1);
  Display.setCursor(4, 24);
  Display.print(primaryLabel);
  Display.setTextColor(wifiConnected ? COLOR_GREEN : COLOR_WHITE, COLOR_BLACK);
  Display.setTextSize(2);
  Display.setCursor(4, 36);
  Display.print(primaryIp);

  // Sekundaere (kleine) Adresse: AP wenn WLAN gross ist, sonst WLAN-Status.
  if (wifiConnected) {
    drawLabelValue(66, "AP", WiFi.softAPIP().toString(), COLOR_LIGHTGREY);
  } else {
    drawLabelValue(66, "WLAN", displayWifiIp(), COLOR_ORANGE);
  }
}

void drawStatusPage() {
  drawDisplayHeader("Status");

  drawLabelValue(24, "HID", hidStatusLabel(), hidStatusColor());
  drawLabelValue(38, "Mode", modeLabel(), autoLoopEnabled ? COLOR_GREEN : COLOR_WHITE);
  drawLabelValue(52, "Flow", flowStateDisplayName(), flowState == FLOW_IDLE ? COLOR_WHITE : COLOR_ORANGE);
  drawLabelValue(66, "Next", nextTriggerLabel(), COLOR_CYAN);
}

void drawActivityPage() {
  drawDisplayHeader("Plan");

  // "Next" gross, mit dem ganzen verfuegbaren Platz.
  Display.setTextColor(COLOR_CYAN, COLOR_BLACK);
  Display.setTextSize(1);
  Display.setCursor(4, 24);
  Display.print("Next");
  Display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  Display.setTextSize(2);
  Display.setCursor(4, 36);
  Display.print(fitDisplayText(nextTriggerLabel(), 12));

  // Bis zu zwei kommende Planzeiten auflisten.
  time_t now = timeSynced ? time(nullptr) : 0;
  int16_t y = 60;
  uint8_t shown = 0;
  for (uint8_t i = 0; i < MAX_SCHEDULES && shown < 2; i++) {
    if (schedules[i] == 0) continue;
    if (now != 0 && schedules[i] < now) continue;
    drawLabelValue(y, shown == 0 ? "Plan" : "", formatTime(schedules[i]), COLOR_LIGHTGREY);
    y += 12;
    shown++;
  }
  if (shown == 0) {
    drawLabelValue(60, "Plan", autoLoopEnabled ? "Automatik" : "keiner", COLOR_DARKGREY);
  }
}

void drawBootPage(const char* message) {
  Display.fillScreen(COLOR_BLACK);
  Display.setTextSize(2);
  Display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  Display.setCursor(4, 8);
  Display.print("HID");
  Display.setTextSize(1);
  Display.setTextColor(COLOR_CYAN, COLOR_BLACK);
  Display.setCursor(4, 34);
  Display.print(message);
}

void setupDisplay() {
  pinMode(DISPLAY_BL, OUTPUT);
  digitalWrite(DISPLAY_BL, LOW);
  Serial.println("Display: Backlight eingeschaltet");

  pinMode(DISPLAY_BUTTON_A, INPUT_PULLUP);

  DisplaySPI.begin(DISPLAY_SCLK, -1, DISPLAY_MOSI, DISPLAY_CS);
  Serial.println("Display: SPI gestartet");

  Display.initR(DISPLAY_INITR);
  Serial.println("Display: ST7735 initialisiert");

  Display.setRotation(1);
  Serial.println("Display: Rotation gesetzt");

  Display.setTextWrap(false);

  Display.fillScreen(COLOR_RED);
  delay(180);
  Display.fillScreen(COLOR_GREEN);
  delay(180);
  Display.fillScreen(COLOR_BLUE);
  delay(180);
  Display.fillScreen(COLOR_BLACK);
  Serial.println("Display: Testfarben abgeschlossen");

  drawBootPage("Boot...");
  displayReady = true;
  displayDirty = true;
}

void updateDisplay(const char* message = nullptr) {
  if (!displayReady) return;

  if (message && strcmp(message, "Boot") == 0) {
    drawBootPage("Boot...");
    return;
  }

  // Falls die Aktivitaetsseite verschwindet waehrend sie aktiv ist: zurueck auf Status.
  if (displayPage >= displayPageCount()) {
    displayPage = displayPageCount() - 1;
    displayDirty = true;
  }

  String apIp = WiFi.softAPIP().toString();
  String wifiIp = displayWifiIp();
  String state = flowStateName();
  String snapshot = String(displayPage) + "|" + String(displayPageCount()) + "|" + apIp + "|" + wifiIp + "|" + state + "|" +
                    String(autoLoopEnabled) + "|" + hidStatusLabel() + "|" + nextTriggerLabel() + "|" +
                    lastTrigger + "|" + String(message ? message : "");
  static String lastSnapshot;

  if (!displayDirty && snapshot == lastSnapshot) return;
  lastSnapshot = snapshot;
  displayDirty = false;

  if (displayPage == 0) {
    drawNetworkPage();
  } else if (displayPage == 1) {
    drawStatusPage();
  } else {
    drawActivityPage();
  }
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
    case FLOW_MANUAL_START_WAIT:
      return "manual_start_wait";
    case FLOW_MANUAL_AFTER_CLICK_WAIT:
      return "manual_after_click_wait";
    case FLOW_MANUAL_EVENT_WAIT:
      return "manual_event_wait";
    case FLOW_MANUAL_GAP_WAIT:
      return "manual_gap_wait";
  }

  return "unknown";
}

void onUsbEvent(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
  (void)arg;
  (void)eventBase;
  (void)eventData;

  switch (eventId) {
    case ARDUINO_USB_STARTED_EVENT:
      usbRuntimeState = USB_ACTIVE;
      usbWasMounted = true;
      break;
    case ARDUINO_USB_STOPPED_EVENT:
      usbRuntimeState = USB_STOPPED;
      break;
    case ARDUINO_USB_SUSPEND_EVENT:
      usbRuntimeState = USB_SUSPENDED;
      break;
    case ARDUINO_USB_RESUME_EVENT:
      usbRuntimeState = USB_ACTIVE;
      usbWasMounted = true;
      break;
  }

  displayDirty = true;
}

void showNextDisplayPage() {
  displayPage = (displayPage + 1) % displayPageCount();
  nextDisplayPageAt = millis() + DISPLAY_PAGE_MS;
  displayDirty = true;
}

// Bricht einen laufenden Flow ab und schaltet die Automatik aus.
void stopFlowAndAuto() {
  autoLoopEnabled = false;
  manualPlanStartAt = 0;
  flowState = FLOW_IDLE;
  stateUntil = 0;
  saveSettings();
  saveSchedules();
  displayDirty = true;
}

// Langer Tastendruck: laeuft etwas -> stoppen, sonst Automatik einschalten.
void buttonLongAction() {
  if (autoLoopEnabled || flowState != FLOW_IDLE) {
    stopFlowAndAuto();
    Serial.println("Flow/Automatik per Taste gestoppt");
    updateDisplay("Gestoppt");
  } else {
    autoLoopEnabled = true;
    saveSettings();
    displayDirty = true;
    Serial.println("Automatik per Taste: ein");
    updateDisplay("Auto: ein");
  }
}

void handleDisplayButtons() {
  bool pressed = digitalRead(DISPLAY_BUTTON_A) == LOW;

  if (pressed && !displayButtonWasPressed) {
    if (!timeReached(lastDisplayButtonAt + BUTTON_DEBOUNCE_MS)) return;
    displayButtonWasPressed = true;
    displayButtonLongFired = false;
    displayButtonDownAt = millis();
    return;
  }

  if (pressed && displayButtonWasPressed && !displayButtonLongFired &&
      timeReached(displayButtonDownAt + LONG_PRESS_MS)) {
    // Langer Druck: Aktion sofort ausloesen (Auto-Loop umschalten).
    displayButtonLongFired = true;
    lastDisplayButtonAt = millis();
    buttonLongAction();
    return;
  }

  if (!pressed && displayButtonWasPressed) {
    displayButtonWasPressed = false;
    lastDisplayButtonAt = millis();
    if (!displayButtonLongFired) {
      // Kurzer Druck: zur naechsten Seite blaettern.
      showNextDisplayPage();
    }
  }
}

void handleDisplayPaging() {
  handleDisplayButtons();

  if (timeReached(nextDisplayPageAt)) {
    showNextDisplayPage();
  }
}

void saveSettings() {
  Settings.putString("ssid", wifiSsid);
  Settings.putString("pass", wifiPassword);
  Settings.putBool("auto", autoLoopEnabled);
  Settings.putInt("planclock", plannerUntilSeconds);
  Settings.putUInt("planmin", plannerMinMinutes);
  Settings.putUInt("planmax", plannerMaxMinutes);
  Settings.putUInt("planevents", plannerEventCount);
}

void loadSettings() {
  Settings.begin("hid", false);
  wifiSsid = Settings.getString("ssid", "");
  wifiPassword = Settings.getString("pass", "");
  autoLoopEnabled = Settings.getBool("auto", false);
  plannerUntilSeconds = Settings.getInt("planclock", -1);
  if (plannerUntilSeconds < 0 || plannerUntilSeconds >= 86400) plannerUntilSeconds = -1;
  plannerMinMinutes = constrain(Settings.getUInt("planmin", 15), 1U, 1440U);
  plannerMaxMinutes = constrain(Settings.getUInt("planmax", 44), (uint32_t)plannerMinMinutes, 1440U);
  plannerEventCount = constrain(Settings.getUInt("planevents", 4), 1U, (uint32_t)MAX_SCHEDULES);
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
  Settings.putULong("planstart", (uint32_t)manualPlanStartAt);
}

void loadSchedules() {
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) schedules[i] = 0;
  manualPlanStartAt = (time_t)Settings.getULong("planstart", 0);

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

bool replaceSchedules(time_t* values, uint8_t count) {
  if (count == 0 || count > MAX_SCHEDULES) return false;

  // Aufsteigend sortieren, damit Anzeige und Abarbeitung dieselbe Reihenfolge haben.
  for (uint8_t i = 1; i < count; i++) {
    time_t value = values[i];
    int8_t j = i - 1;
    while (j >= 0 && values[j] > value) {
      values[j + 1] = values[j];
      j--;
    }
    values[j + 1] = value;
  }

  for (uint8_t i = 1; i < count; i++) {
    if (values[i] == values[i - 1]) return false;
  }

  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    schedules[i] = i < count ? values[i] : 0;
  }
  saveSchedules();
  displayDirty = true;
  return true;
}

void beginAutoStandby() {
  uint32_t waitMs = random(LONG_WAIT_MIN_MS, LONG_WAIT_MAX_MS + 1);

  flowState = FLOW_AUTO_STANDBY;
  stateUntil = millis() + waitMs;
  nextJiggleAt = millis() + JIGGLE_INTERVAL;

  Serial.printf("Auto-Standby fuer %lu ms\n", (unsigned long)waitMs);
}

void finishManualPlan() {
  manualPlanStartAt = 0;
  flowState = FLOW_IDLE;
  stateUntil = 0;
  saveSchedules();
  displayDirty = true;
  Serial.println("Geplante manuelle Sequenz abgeschlossen");
}

void startManualPreparation() {
  lastTrigger = "schedule";
  leftClick();
  flowState = FLOW_MANUAL_AFTER_CLICK_WAIT;
  stateUntil = millis() + AFTER_CLICK_WAIT;
  displayDirty = true;
  Serial.println("Manuelle Sequenz: Linksklick gesendet");
}

bool armManualPlan(time_t startAt) {
  if (flowState != FLOW_IDLE || autoLoopEnabled || nextScheduleTime() == 0) return false;

  time_t now = time(nullptr);
  manualPlanStartAt = startAt <= now ? now : startAt;
  flowState = FLOW_MANUAL_START_WAIT;
  stateUntil = 0;
  nextJiggleAt = millis() + JIGGLE_INTERVAL;
  lastTrigger = "schedule";
  saveSchedules();
  displayDirty = true;
  Serial.printf("Manuelle Sequenz geplant ab: %s\n", formatTime(manualPlanStartAt).c_str());
  return true;
}

bool startFlow(const String& source) {
  if (flowState == FLOW_AFTER_CLICK_WAIT || flowState == FLOW_AFTER_ENTER_WAIT ||
      flowState == FLOW_MANUAL_START_WAIT || flowState == FLOW_MANUAL_AFTER_CLICK_WAIT ||
      flowState == FLOW_MANUAL_EVENT_WAIT || flowState == FLOW_MANUAL_GAP_WAIT) {
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
    if (manualPlanStartAt != 0) {
      flowState = FLOW_MANUAL_START_WAIT;
      displayDirty = true;
      return;
    }

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
    return;
  }

  if (flowState == FLOW_MANUAL_START_WAIT) {
    if (nextScheduleTime() == 0) {
      finishManualPlan();
      return;
    }

    if (!timeSynced || time(nullptr) < manualPlanStartAt) {
      if (timeReached(nextJiggleAt)) {
        jiggleOnce();
        nextJiggleAt = millis() + JIGGLE_INTERVAL;
      }
      return;
    }

    startManualPreparation();
    return;
  }

  if (flowState == FLOW_MANUAL_AFTER_CLICK_WAIT) {
    if (timeReached(stateUntil)) {
      Keyboard.write(KEY_RETURN);
      flowState = FLOW_MANUAL_EVENT_WAIT;
      nextJiggleAt = millis() + JIGGLE_INTERVAL;
      displayDirty = true;
      Serial.println("Manuelle Sequenz: ENTER gesendet, warte auf Event");
    }
    return;
  }

  if (flowState == FLOW_MANUAL_EVENT_WAIT) {
    time_t next = nextScheduleTime();
    if (next == 0) {
      finishManualPlan();
      return;
    }

    time_t now = time(nullptr);
    if (timeSynced && now >= next) {
      if (now - next > SCHEDULE_GRACE_S) {
        Serial.printf("Geplantes Event verfallen: %s\n", formatTime(next).c_str());
        removeSchedule(next);
        return;
      }

      pressCtrlAltF();
      Serial.printf("Geplantes Event ausgefuehrt: %s\n", formatTime(next).c_str());
      removeSchedule(next);

      if (nextScheduleTime() == 0) {
        finishManualPlan();
      } else {
        uint32_t gapMs = random(MANUAL_GAP_MIN_MS, MANUAL_GAP_MAX_MS + 1);
        flowState = FLOW_MANUAL_GAP_WAIT;
        stateUntil = millis() + gapMs;
        displayDirty = true;
        Serial.printf("Naechste Vorbereitung in %lu ms\n", (unsigned long)gapMs);
      }
      return;
    }

    if (timeReached(nextJiggleAt)) {
      jiggleOnce();
      nextJiggleAt = millis() + JIGGLE_INTERVAL;
    }
    return;
  }

  if (flowState == FLOW_MANUAL_GAP_WAIT) {
    if (timeReached(stateUntil)) startManualPreparation();
  }
}

void checkSchedules() {
  if (!timeSynced || manualPlanStartAt != 0) return;

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

    // Ohne gestartete manuelle Sequenz bleiben faellige Events innerhalb
    // der Nachholfrist bestehen. Sie werden beim Start des Plans verarbeitet.
  }
}

void connectWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  updateDisplay("AP gestartet");

  Serial.printf("AP gestartet: %s / http://%s/\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  if (wifiSsid.length() == 0) {
    Serial.println("Keine Heimnetz-Zugangsdaten gespeichert.");
    updateDisplay("AP-Modus");
    return;
  }

  Serial.printf("Verbinde mit WLAN: %s\n", wifiSsid.c_str());
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  updateDisplay("WLAN verbindet");

  uint32_t timeoutAt = millis() + WIFI_CONNECT_MS;
  while (WiFi.status() != WL_CONNECTED && !timeReached(timeoutAt)) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiWasConnected = true;
    Serial.printf("Heimnetz verbunden: http://%s/\n", WiFi.localIP().toString().c_str());
    updateDisplay("WLAN verbunden");
  } else {
    Serial.println("Heimnetz-Verbindung fehlgeschlagen. AP bleibt aktiv.");
    updateDisplay("AP-Modus");
  }
}

void setupOta() {
  if (otaStarted) return;

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(AP_PASSWORD);

  ArduinoOTA.onStart([]() {
    autoLoopEnabled = false;  // Automatik waehrend des Updates anhalten
    flowState = FLOW_IDLE;
    Serial.println("OTA: Update startet");
    if (displayReady) drawBootPage("OTA Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA: fertig");
    if (displayReady) drawBootPage("OTA fertig");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static uint8_t lastPct = 255;
    uint8_t pct = total ? (uint8_t)(progress * 100 / total) : 0;
    if (pct == lastPct) return;
    lastPct = pct;
    Serial.printf("OTA: %u%%\n", pct);
    char msg[16];
    snprintf(msg, sizeof(msg), "OTA %u%%", pct);
    if (displayReady) drawBootPage(msg);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Fehler [%u]\n", error);
    if (displayReady) drawBootPage("OTA Fehler");
    displayDirty = true;
  });

  ArduinoOTA.begin();
  otaStarted = true;
  Serial.printf("OTA bereit: %s.local\n", OTA_HOSTNAME);
}

// Haelt die WLAN-Verbindung am Leben und meldet Statuswechsel.
void maintainWifi() {
  if (wifiSsid.length() == 0) return;

  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected != wifiWasConnected) {
    wifiWasConnected = connected;
    displayDirty = true;
    if (connected) {
      Serial.printf("WLAN wieder verbunden: http://%s/\n", WiFi.localIP().toString().c_str());
      if (!timeSynced) syncTimeNow(2000);
      setupOta();
    } else {
      Serial.println("WLAN-Verbindung verloren, versuche Reconnect.");
    }
  }

  if (connected) return;
  if (!timeReached(nextWifiRetryAt)) return;

  nextWifiRetryAt = millis() + WIFI_RETRY_MS;
  Serial.println("WLAN-Reconnect-Versuch ...");
  WiFi.disconnect();
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
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
  json += ",\"manualPlanActive\":" + String(manualPlanStartAt != 0 ? "true" : "false");
  json += ",\"manualStartAt\":" + String((uint32_t)manualPlanStartAt);
  json += ",\"plannerUntilSeconds\":" + String(plannerUntilSeconds);
  json += ",\"plannerMinMinutes\":" + String(plannerMinMinutes);
  json += ",\"plannerMaxMinutes\":" + String(plannerMaxMinutes);
  json += ",\"plannerEventCount\":" + String(plannerEventCount);
  json += ",\"lastTrigger\":\"" + jsonEscape(lastTrigger) + "\"";
  json += ",\"apSsid\":\"" + String(AP_SSID) + "\"";
  json += ",\"apIp\":\"" + WiFi.softAPIP().toString() + "\"";
  json += ",\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  json += ",\"wifiIp\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"wifiSsid\":\"" + jsonEscape(wifiSsid) + "\"";
  json += ",\"hidActive\":" + String(hidActive() ? "true" : "false");
  json += ",\"usbMounted\":" + String((bool)USB ? "true" : "false");
  json += ",\"usbState\":\"" + jsonEscape(hidStatusLabel()) + "\"";
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

    .planner-grid {
      display: grid;
      grid-template-columns: repeat(5, minmax(0, 1fr));
      gap: 10px;
      margin-top: 14px;
    }

    .planner-field { min-width: 0; }
    .planner-field.wide-field { grid-column: span 2; }
    .planner-field label { margin-top: 0; }

    .planner-field input,
    .suggestion-row input {
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

    .planner-actions {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin-top: 14px;
    }

    dialog {
      width: min(620px, calc(100% - 24px));
      max-height: calc(100vh - 32px);
      overflow: auto;
      border: 0;
      border-radius: 12px;
      padding: 22px;
      color: var(--text);
      background: var(--surface);
      box-shadow: var(--shadow);
    }

    dialog::backdrop { background: rgba(19, 32, 37, 0.58); }
    dialog h2 { margin-bottom: 8px; }

    .suggestion-list {
      display: grid;
      gap: 9px;
      margin: 18px 0;
    }

    .suggestion-row {
      display: grid;
      grid-template-columns: 74px minmax(0, 1fr);
      gap: 10px;
      align-items: center;
    }

    .suggestion-row span {
      color: var(--muted);
      font-size: 13px;
      font-weight: 800;
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
      .planner-grid { grid-template-columns: 1fr 1fr; }
      .planner-field.wide-field { grid-column: span 2; }
      section, section:nth-child(odd) {
        padding: 18px;
        border-right: 0;
      }
      .actions { grid-template-columns: 1fr; }
      .planner-actions button { flex: 1 1 100%; }
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
            <button id="stopButton" class="secondary" type="button" onclick="stopFlow()">Flow stoppen</button>
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
          <label class="switch-row">
            <input id="autoInput" type="checkbox" onchange="setAuto(this.checked)" AUTO_CHECKED>
            <span class="switch-copy">
              <strong>Automatische Schleife</strong>
              <span class="muted">Wird sofort gespeichert. Aus: Flow nur per Website oder API.</span>
            </span>
          </label>
        </section>

        <section class="wide">
          <h2>Geplante Auslösung</h2>
          <p class="muted">Die manuelle Sequenz beginnt ab dem Startzeitpunkt mit Linksklick und Enter. Zu jedem Event folgt Strg+Alt+F. Vor weiteren Events wird nach 10–30 Sekunden erneut mit Linksklick und Enter vorbereitet.</p>
          <div class="planner-grid">
            <div class="planner-field wide-field">
              <label for="manualStartInput">Start ab</label>
              <input id="manualStartInput" type="datetime-local" step="1" aria-label="Startzeitpunkt der manuellen Sequenz">
            </div>
            <div class="planner-field wide-field">
              <label for="manualUntilInput">Bis Uhrzeit</label>
              <input id="manualUntilInput" type="time" step="1" onchange="savePlannerSettings()" aria-label="Spaeteste Event-Uhrzeit">
            </div>
            <div class="planner-field">
              <label for="eventCountInput">Events</label>
              <input id="eventCountInput" type="number" min="1" max="8" value="4" onchange="savePlannerSettings()">
            </div>
            <div class="planner-field">
              <label for="minDurationInput">Min. Dauer (Min.)</label>
              <input id="minDurationInput" type="number" min="1" value="15" onchange="savePlannerSettings()" aria-label="Minimale Dauer in Minuten">
            </div>
            <div class="planner-field">
              <label for="maxDurationInput">Max. Dauer (Min.)</label>
              <input id="maxDurationInput" type="number" min="1" value="44" onchange="savePlannerSettings()" aria-label="Maximale Dauer in Minuten">
            </div>
          </div>
          <div class="planner-actions">
            <button id="calculateEventsButton" type="button" onclick="calculateSuggestions()">Events berechnen</button>
            <button id="planStartButton" type="button" onclick="startManualPlan()">Sequenz starten</button>
          </div>
          <p id="planState" class="muted">Noch nicht gestartet.</p>
          <label for="scheduleInput">Event hinzufügen</label>
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
            <p><code>POST /api/planner/settings</code> Planerwerte dauerhaft speichern.</p>
            <p><code>POST /api/schedule?when=2026-06-01T14:30</code> Event planen.</p>
            <p><code>POST /api/schedule/replace</code> Berechnete Eventliste übernehmen.</p>
            <p><code>POST /api/schedule/start?when=2026-06-01T13:55:00</code> Manuelle Sequenz starten.</p>
            <p><code>POST /api/schedule/delete?when=EPOCH</code> Auslösung löschen.</p>
          </div>
        </section>
      </div>
    </div>
  </main>

  <dialog id="suggestionModal" aria-labelledby="suggestionTitle">
    <h2 id="suggestionTitle">Event-Vorschläge</h2>
    <p id="suggestionHint" class="muted">Die berechneten Zeiten können vor dem Übernehmen angepasst werden.</p>
    <div id="suggestionList" class="suggestion-list"></div>
    <div class="planner-actions">
      <button class="secondary" type="button" onclick="closeSuggestionModal()">Abbrechen</button>
      <button id="applySuggestionsButton" type="button" onclick="applySuggestions()">Vorschläge übernehmen</button>
    </div>
  </dialog>

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

    async function stopFlow() {
      setMessage('Flow wird gestoppt ...', false);
      try {
        const response = await fetch('/api/stop', { method: 'POST' });
        const data = await response.json();
        applyStatus(data);
        setMessage(data.ok ? 'Flow gestoppt.' : (data.error || 'Konnte nicht gestoppt werden.'), !data.ok);
      } catch (error) {
        setMessage('Keine Verbindung zum Geraet.', true);
      }
    }

    let autoPending = false;
    async function setAuto(on) {
      autoPending = true;
      setMessage(on ? 'Automatik wird aktiviert ...' : 'Automatik wird deaktiviert ...', false);
      try {
        const response = await fetch('/api/settings?auto=' + (on ? '1' : '0'), { method: 'POST' });
        const data = await response.json();
        applyStatus(data);
        setMessage(on ? 'Automatik aktiviert.' : 'Automatik deaktiviert.', false);
      } catch (error) {
        setMessage('Keine Verbindung zum Geraet.', true);
      } finally {
        autoPending = false;
      }
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
      auto_standby: { label: 'Standby (Automatik)', dot: 'standby' },
      manual_start_wait: { label: 'Manueller Start geplant', dot: 'standby' },
      manual_after_click_wait: { label: 'Manuell: Klick gesendet', dot: 'running' },
      manual_event_wait: { label: 'Manuell: warte auf Event', dot: 'standby' },
      manual_gap_wait: { label: 'Manuell: nächste Vorbereitung', dot: 'running' }
    };

    const WEEKDAYS = ['Sonntag', 'Montag', 'Dienstag', 'Mittwoch', 'Donnerstag', 'Freitag', 'Samstag'];
    const MONTHS = ['Jan.', 'Feb.', 'März', 'Apr.', 'Mai', 'Juni', 'Juli', 'Aug.', 'Sep.', 'Okt.', 'Nov.', 'Dez.'];

    let serverEpoch = 0;
    let fetchedAt = 0;
    let timeSynced = false;
    let schedules = [];
    let manualPlanActive = false;
    let manualStartAt = 0;
    let maxSchedules = 8;
    let suggestionConfig = null;
    let plannerSettingsLoaded = false;

    function pad(n) { return String(n).padStart(2, '0'); }

    function localInputValue(epochMs, withSeconds = true) {
      const local = new Date(epochMs - new Date(epochMs).getTimezoneOffset() * 60000);
      return local.toISOString().slice(0, withSeconds ? 19 : 16);
    }

    function timeInputSeconds(value) {
      const parts = String(value || '').split(':').map(Number);
      if (parts.length < 2 || parts.some(part => !Number.isFinite(part))) return -1;
      return parts[0] * 3600 + parts[1] * 60 + (parts[2] || 0);
    }

    function timeInputValue(seconds) {
      const hours = Math.floor(seconds / 3600) % 24;
      const minutes = Math.floor(seconds / 60) % 60;
      const secs = seconds % 60;
      return pad(hours) + ':' + pad(minutes) + ':' + pad(secs);
    }

    function untilEpochMs(startMs, untilSeconds) {
      const result = new Date(startMs);
      result.setHours(Math.floor(untilSeconds / 3600), Math.floor(untilSeconds / 60) % 60, untilSeconds % 60, 0);
      if (result.getTime() <= startMs) result.setDate(result.getDate() + 1);
      return result.getTime();
    }

    function randomInt(min, max) {
      return Math.floor(Math.random() * (max - min + 1)) + min;
    }

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

    function renderPlanState() {
      const state = document.getElementById('planState');
      const button = document.getElementById('planStartButton');
      const calculateButton = document.getElementById('calculateEventsButton');
      if (manualPlanActive) {
        state.textContent = 'Sequenz aktiv – Start: ' + fmtStamp(manualStartAt);
        button.disabled = true;
        calculateButton.disabled = true;
        button.textContent = 'Sequenz läuft';
      } else {
        state.textContent = schedules.length ? 'Events bereit. Startzeit festlegen und Sequenz starten.' : 'Zuerst mindestens ein Event hinzufügen.';
        button.disabled = schedules.length === 0 || !timeSynced;
        calculateButton.disabled = !timeSynced;
        button.textContent = 'Sequenz starten';
      }
    }

    async function savePlannerSettings() {
      const untilSeconds = timeInputSeconds(document.getElementById('manualUntilInput').value);
      const minMinutes = Number(document.getElementById('minDurationInput').value);
      const maxMinutes = Number(document.getElementById('maxDurationInput').value);
      const eventCount = Number(document.getElementById('eventCountInput').value);

      if (untilSeconds < 0 || untilSeconds >= 86400 || !Number.isInteger(minMinutes) || !Number.isInteger(maxMinutes) ||
          !Number.isInteger(eventCount) || minMinutes < 1 || maxMinutes < minMinutes ||
          maxMinutes > 1440 || eventCount < 1 || eventCount > maxSchedules) {
        setMessage('Planerwerte konnten noch nicht gespeichert werden.', true);
        return false;
      }

      try {
        const body = new URLSearchParams({
          until: String(untilSeconds),
          min: String(minMinutes),
          max: String(maxMinutes),
          events: String(eventCount)
        });
        const response = await fetch('/api/planner/settings', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: body.toString()
        });
        const data = await response.json();
        if (!data.ok) throw new Error(data.error || 'Speichern fehlgeschlagen');
        setMessage('Planerwerte gespeichert.', false);
        return true;
      } catch (error) {
        setMessage('Planerwerte konnten nicht gespeichert werden.', true);
        return false;
      }
    }

    function calculateSuggestions() {
      const startMs = new Date(document.getElementById('manualStartInput').value).getTime();
      const untilSeconds = timeInputSeconds(document.getElementById('manualUntilInput').value);
      const untilMs = Number.isFinite(startMs) && untilSeconds >= 0
        ? untilEpochMs(startMs, untilSeconds)
        : NaN;
      const count = Number(document.getElementById('eventCountInput').value);
      const minMinutes = Number(document.getElementById('minDurationInput').value);
      const maxMinutes = Number(document.getElementById('maxDurationInput').value);

      if (!Number.isFinite(startMs) || !Number.isFinite(untilMs)) {
        setMessage('Start und „Bis Uhrzeit“ sind ungültig.', true);
        return;
      }
      if (!Number.isInteger(count) || count < 1 || count > maxSchedules) {
        setMessage('Die Eventanzahl muss zwischen 1 und ' + maxSchedules + ' liegen.', true);
        return;
      }
      if (!Number.isFinite(minMinutes) || !Number.isFinite(maxMinutes) ||
          minMinutes < 1 || maxMinutes < minMinutes) {
        setMessage('Minimale und maximale Dauer sind ungültig.', true);
        return;
      }

      const minSeconds = Math.round(minMinutes * 60);
      const maxSeconds = Math.round(maxMinutes * 60);
      const windowSeconds = Math.floor((untilMs - startMs) / 1000);
      const minimumSpan = count * minSeconds;
      const maximumSpan = Math.min(windowSeconds, count * maxSeconds);

      if (windowSeconds < minimumSpan) {
        setMessage('Der Zeitraum ist zu kurz: Für ' + count + ' Events werden mindestens ' +
                   fmtDuration(minimumSpan * 1000) + ' benötigt.', true);
        return;
      }

      savePlannerSettings();

      // Zuerst eine zufällige Gesamtdauer wählen, anschließend diese so aufteilen,
      // dass jeder Abstand weiterhin innerhalb der Min-/Max-Grenzen liegt.
      let remaining = randomInt(minimumSpan, maximumSpan);
      let cursorMs = startMs;
      const suggestions = [];
      for (let i = 0; i < count; i++) {
        const slotsAfter = count - i - 1;
        const low = Math.max(minSeconds, remaining - slotsAfter * maxSeconds);
        const high = Math.min(maxSeconds, remaining - slotsAfter * minSeconds);
        const gap = i === count - 1 ? remaining : randomInt(low, high);
        cursorMs += gap * 1000;
        suggestions.push(cursorMs);
        remaining -= gap;
      }

      suggestionConfig = { startMs, untilMs, minSeconds, maxSeconds };
      const list = document.getElementById('suggestionList');
      list.innerHTML = suggestions.map((value, index) =>
        '<label class="suggestion-row">' +
          '<span>Event ' + (index + 1) + '</span>' +
          '<input class="suggestion-input" type="datetime-local" step="1" value="' + localInputValue(value) + '">' +
        '</label>'
      ).join('');

      document.getElementById('suggestionHint').textContent =
        'Zufällig verteilt mit ' + minMinutes + '–' + maxMinutes + ' Minuten Abstand. ' +
        'Letzter Vorschlag: ' + fmtStamp(Math.floor(suggestions[suggestions.length - 1] / 1000)) + '.';
      document.getElementById('suggestionHint').style.color = '';
      document.getElementById('suggestionModal').showModal();
      setMessage('', false);
    }

    function closeSuggestionModal() {
      document.getElementById('suggestionModal').close();
    }

    async function applySuggestions() {
      if (!suggestionConfig) return;
      const inputs = Array.from(document.querySelectorAll('.suggestion-input'));
      const values = inputs.map(input => new Date(input.value).getTime()).sort((a, b) => a - b);
      if (values.some(value => !Number.isFinite(value))) {
        const hint = document.getElementById('suggestionHint');
        hint.textContent = 'Mindestens ein Vorschlag enthält keine gültige Zeit.';
        hint.style.color = 'var(--danger)';
        return;
      }

      let previous = suggestionConfig.startMs;
      for (const value of values) {
        const gapSeconds = Math.round((value - previous) / 1000);
        if (value > suggestionConfig.untilMs || gapSeconds < suggestionConfig.minSeconds ||
            gapSeconds > suggestionConfig.maxSeconds) {
          const hint = document.getElementById('suggestionHint');
          hint.textContent = 'Angepasste Zeiten müssen innerhalb von „Bis Uhrzeit“ und den Min-/Max-Abständen bleiben.';
          hint.style.color = 'var(--danger)';
          return;
        }
        previous = value;
      }

      const button = document.getElementById('applySuggestionsButton');
      button.disabled = true;
      try {
        const body = new URLSearchParams({
          times: values.map(value => Math.floor(value / 1000)).join(',')
        });
        const response = await fetch('/api/schedule/replace', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: body.toString()
        });
        const data = await response.json();
        if (!data.ok) {
          const hint = document.getElementById('suggestionHint');
          hint.textContent = data.error || 'Vorschläge konnten nicht übernommen werden.';
          hint.style.color = 'var(--danger)';
        } else {
          applyStatus(data);
          closeSuggestionModal();
          setMessage('Event-Vorschläge übernommen.', false);
        }
      } catch (error) {
        setMessage('Keine Verbindung zum Geraet.', true);
      } finally {
        button.disabled = false;
      }
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
      manualPlanActive = !!data.manualPlanActive;
      manualStartAt = data.manualStartAt || 0;
      maxSchedules = data.maxSchedules || 8;
      document.getElementById('eventCountInput').max = maxSchedules;
      if (!plannerSettingsLoaded) {
        if (data.plannerUntilSeconds >= 0) {
          document.getElementById('manualUntilInput').value = timeInputValue(data.plannerUntilSeconds);
        }
        document.getElementById('minDurationInput').value = data.plannerMinMinutes || 15;
        document.getElementById('maxDurationInput').value = data.plannerMaxMinutes || 44;
        document.getElementById('eventCountInput').value = data.plannerEventCount || 4;
        plannerSettingsLoaded = true;
      }

      const info = FLOW_INFO[data.flowState] || { label: data.flowState, dot: 'idle' };
      document.getElementById('flowState').textContent = info.label;
      document.getElementById('flowDot').className = 'dot ' + info.dot;
      document.getElementById('autoState').textContent = data.autoLoopEnabled ? 'Ein' : 'Aus';
      document.getElementById('wifiState').textContent = data.wifiConnected ? data.wifiIp : 'AP-Modus';
      if (!autoPending) {
        document.getElementById('autoInput').checked = !!data.autoLoopEnabled;
      }

      const sync = document.getElementById('clockSync');
      sync.textContent = timeSynced ? 'Zeit synchronisiert' : 'Zeit nicht synchronisiert';
      sync.className = 'clock-sync ' + (timeSynced ? 'ok' : 'bad');

      renderSchedules();
      renderNextTrigger();
      renderPlanState();
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
          applyStatus(data);
        }
      } catch (error) {
        setMessage('Keine Verbindung zum Geraet.', true);
      }
    }

    async function startManualPlan() {
      const input = document.getElementById('manualStartInput');
      if (!input.value) { setMessage('Bitte einen Startzeitpunkt wählen.', true); return; }

      setMessage('Manuelle Sequenz wird geplant …', false);
      try {
        const response = await fetch('/api/schedule/start?when=' + encodeURIComponent(input.value), { method: 'POST' });
        const data = await response.json();
        if (!data.ok) {
          setMessage(data.error || 'Sequenz konnte nicht gestartet werden.', true);
        } else {
          applyStatus(data);
          setMessage('Manuelle Sequenz gestartet.', false);
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
      const eventInput = document.getElementById('scheduleInput');
      const startInput = document.getElementById('manualStartInput');
      const untilInput = document.getElementById('manualUntilInput');
      eventInput.min = localInputValue(Date.now(), false);
      eventInput.value = localInputValue(Date.now() + 5 * 60000, false);
      startInput.min = localInputValue(Date.now());
      startInput.value = localInputValue(Date.now());
      const defaultUntil = new Date(Date.now() + 3 * 60 * 60 * 1000);
      untilInput.value = timeInputValue(defaultUntil.getHours() * 3600 + defaultUntil.getMinutes() * 60 + defaultUntil.getSeconds());
    }

    let pollTimer = null;
    function startPolling() {
      if (pollTimer) return;
      pollTimer = setInterval(refreshStatus, 3000);
    }
    function stopPolling() {
      if (!pollTimer) return;
      clearInterval(pollTimer);
      pollTimer = null;
    }

    function connectEvents() {
      if (!window.EventSource) { startPolling(); return; }
      try {
        const es = new EventSource('/api/events');
        es.onopen = () => stopPolling();
        es.onmessage = (ev) => {
          try { applyStatus(JSON.parse(ev.data)); } catch (e) {}
        };
        es.onerror = () => { startPolling(); };
      } catch (e) {
        startPolling();
      }
    }

    initScheduleInput();
    refreshStatus();
    connectEvents();
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

// Server-Sent-Events: haelt die Verbindung offen und behaelt eine Kopie des Clients.
void handleEvents() {
  WiFiClient client = Server.client();
  client.print(F("HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/event-stream\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Connection: keep-alive\r\n"
                 "Access-Control-Allow-Origin: *\r\n\r\n"));
  client.print(F("retry: 3000\n\n"));
  client.print("data: " + statusJson() + "\n\n");

  for (uint8_t i = 0; i < SSE_MAX_CLIENTS; i++) {
    if (!sseClients[i] || !sseClients[i].connected()) {
      sseClients[i] = client;
      return;
    }
  }
  // Kein freier Platz: aeltesten Client ersetzen.
  sseClients[0].stop();
  sseClients[0] = client;
}

void broadcastStatus() {
  bool any = false;
  for (uint8_t i = 0; i < SSE_MAX_CLIENTS; i++) {
    if (sseClients[i] && sseClients[i].connected()) { any = true; break; }
  }
  if (!any) return;

  String payload = "data: " + statusJson() + "\n\n";
  for (uint8_t i = 0; i < SSE_MAX_CLIENTS; i++) {
    if (!sseClients[i]) continue;
    if (sseClients[i].connected()) {
      sseClients[i].print(payload);
    } else {
      sseClients[i].stop();
      sseClients[i] = WiFiClient();
    }
  }
}

void handleTrigger() {
  if (!startFlow("api")) {
    Server.send(409, "application/json", "{\"ok\":false,\"error\":\"flow already running\"}");
    return;
  }

  Server.send(200, "application/json", "{\"ok\":true}");
}

void handleStop() {
  stopFlowAndAuto();
  lastTrigger = "stop";
  Serial.println("Flow gestoppt (API)");
  Server.send(200, "application/json", statusJson());
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

  if (autoLoopEnabled && manualPlanStartAt != 0) {
    manualPlanStartAt = 0;
    flowState = FLOW_IDLE;
    stateUntil = 0;
    saveSchedules();
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

void handlePlannerSettings() {
  int32_t untilSeconds = Server.arg("until").toInt();
  uint32_t minMinutes = (uint32_t)Server.arg("min").toInt();
  uint32_t maxMinutes = (uint32_t)Server.arg("max").toInt();
  uint32_t eventCount = (uint32_t)Server.arg("events").toInt();

  if (!Server.hasArg("until") || untilSeconds < 0 || untilSeconds >= 86400 ||
      minMinutes < 1 || maxMinutes < minMinutes || maxMinutes > 1440 ||
      eventCount < 1 || eventCount > MAX_SCHEDULES) {
    Server.send(400, "application/json", "{\"ok\":false,\"error\":\"ungueltige planerwerte\"}");
    return;
  }

  plannerUntilSeconds = untilSeconds;
  plannerMinMinutes = minMinutes;
  plannerMaxMinutes = maxMinutes;
  plannerEventCount = eventCount;
  saveSettings();
  Server.send(200, "application/json", statusJson());
}

void handleManualPlanStart() {
  if (!timeSynced) {
    Server.send(409, "application/json", "{\"ok\":false,\"error\":\"zeit noch nicht synchronisiert\"}");
    return;
  }
  if (autoLoopEnabled) {
    Server.send(409, "application/json", "{\"ok\":false,\"error\":\"automatik zuerst ausschalten\"}");
    return;
  }
  if (nextScheduleTime() == 0) {
    Server.send(409, "application/json", "{\"ok\":false,\"error\":\"keine events geplant\"}");
    return;
  }

  String startStr = Server.arg("when");
  time_t startAt = startStr.indexOf('-') >= 0
    ? parseLocalDateTime(startStr)
    : (time_t)strtoul(startStr.c_str(), nullptr, 10);
  if (startAt <= 0) startAt = time(nullptr);

  if (!armManualPlan(startAt)) {
    Server.send(409, "application/json", "{\"ok\":false,\"error\":\"flow bereits aktiv\"}");
    return;
  }

  Server.send(200, "application/json", statusJson());
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

void handleScheduleReplace() {
  if (!timeSynced) {
    Server.send(409, "application/json", "{\"ok\":false,\"error\":\"zeit noch nicht synchronisiert\"}");
    return;
  }
  if (manualPlanStartAt != 0) {
    Server.send(409, "application/json", "{\"ok\":false,\"error\":\"manuelle sequenz bereits aktiv\"}");
    return;
  }

  String serialized = Server.arg("times");
  time_t values[MAX_SCHEDULES];
  uint8_t count = 0;
  int start = 0;
  time_t now = time(nullptr);

  while (start <= (int)serialized.length()) {
    if (count >= MAX_SCHEDULES) {
      Server.send(400, "application/json", "{\"ok\":false,\"error\":\"zu viele events\"}");
      return;
    }
    int comma = serialized.indexOf(',', start);
    String part = comma < 0 ? serialized.substring(start) : serialized.substring(start, comma);
    part.trim();
    if (part.length()) {
      time_t value = (time_t)strtoul(part.c_str(), nullptr, 10);
      if (value <= now) {
        Server.send(400, "application/json", "{\"ok\":false,\"error\":\"event liegt nicht in der zukunft\"}");
        return;
      }
      values[count++] = value;
    }
    if (comma < 0) break;
    start = comma + 1;
  }

  if (count == 0 || !replaceSchedules(values, count)) {
    Server.send(400, "application/json", "{\"ok\":false,\"error\":\"ungueltige oder doppelte events\"}");
    return;
  }

  Serial.printf("Event-Vorschlaege uebernommen: %u\n", count);
  Server.send(200, "application/json", statusJson());
}

void handleWifiSave() {
  wifiSsid = Server.arg("ssid");
  wifiPassword = Server.arg("pass");
  saveSettings();

  if (wifiSsid.length() > 0) {
    WiFi.disconnect();
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    updateDisplay("WLAN verbindet");
  }

  redirectHome();
}

void setupRoutes() {
  Server.on("/", HTTP_GET, handleRoot);
  Server.on("/api/status", HTTP_GET, handleStatus);
  Server.on("/api/events", HTTP_GET, handleEvents);
  Server.on("/api/trigger", HTTP_POST, handleTrigger);
  Server.on("/api/trigger", HTTP_GET, handleTrigger);
  Server.on("/api/stop", HTTP_POST, handleStop);
  Server.on("/api/stop", HTTP_GET, handleStop);
  Server.on("/api/mouse", HTTP_POST, handleMouseMove);
  Server.on("/api/mouse", HTTP_GET, handleMouseMove);
  Server.on("/api/click", HTTP_POST, handleMouseClick);
  Server.on("/api/click", HTTP_GET, handleMouseClick);
  Server.on("/api/settings", HTTP_POST, handleSettings);
  Server.on("/api/planner/settings", HTTP_POST, handlePlannerSettings);
  Server.on("/settings", HTTP_POST, handleSettings);
  Server.on("/api/schedule", HTTP_POST, handleScheduleAdd);
  Server.on("/api/schedule", HTTP_GET, handleScheduleAdd);
  Server.on("/api/schedule/start", HTTP_POST, handleManualPlanStart);
  Server.on("/api/schedule/start", HTTP_GET, handleManualPlanStart);
  Server.on("/api/schedule/delete", HTTP_POST, handleScheduleDelete);
  Server.on("/api/schedule/delete", HTTP_GET, handleScheduleDelete);
  Server.on("/api/schedule/replace", HTTP_POST, handleScheduleReplace);
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
  delay(200);
  Serial.println("Boot HID Steuerung");

  setupDisplay();
  updateDisplay("Boot");

  loadSettings();
  loadSchedules();
  if (manualPlanStartAt != 0 && autoLoopEnabled) {
    autoLoopEnabled = false;
    saveSettings();
  }
  connectWifi();
  updateDisplay("Netz bereit");
  setupRoutes();

  Serial.println("Starte USB HID...");
  USB.onEvent(onUsbEvent);
  Keyboard.begin();
  Mouse.begin();
  USB.begin();

  // Zeit beim Start holen (Mitteleuropa). SNTP laeuft im Hintergrund weiter.
  configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2);
  if (WiFi.status() == WL_CONNECTED) {
    syncTimeNow(5000);
    setupOta();
  }

  updateDisplay("Bereit");

  delay(1000);
  drawStartupRectangle();

  // Initialisierungszeit vor dem Automatikzyklus: 10 Sekunden
  for (int i=10; i>0; i--) {
    Serial.printf("Starte in %d...\n", i);
    delay(1000);
    Server.handleClient();
  }
  nextJiggleAt = millis() + JIGGLE_INTERVAL;
  nextDisplayPageAt = millis() + DISPLAY_PAGE_MS;
  updateDisplay("Bereit");
  Serial.println("Bereit.");
}

void loop() {
  Server.handleClient();
  if (otaStarted) ArduinoOTA.handle();
  maintainWifi();
  maintainTime();
  checkSchedules();
  handleFlow();
  handleDisplayPaging();
  if (timeReached(nextSseAt)) {
    nextSseAt = millis() + SSE_INTERVAL_MS;
    broadcastStatus();
  }
  if (timeReached(nextDisplayRefreshAt)) {
    nextDisplayRefreshAt = millis() + DISPLAY_REFRESH_MS;
    updateDisplay();
  }
  delay(5);
}
