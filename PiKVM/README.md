# PiKVM HID Automation

PiKVM-Variante des ESP32-HID-Projekts. Die Automatisierung läuft direkt auf dem PiKVM und verwendet dessen lokale KVMD-HID-API über den Unix-Socket.

## Funktionen

- eigener lokaler Webserver mit mobilfreundlicher Oberfläche
- HTTP Basic Auth mit bei der Installation zufällig erzeugtem Passwort
- manueller Flow:
  1. Linksklick
  2. 5 Sekunden warten
  3. Enter
  4. 20 Sekunden warten
  5. `Ctrl + Alt + F`
- automatische Wiederholung mit zufälligem Intervall, standardmäßig 25–35 Minuten
- geplante Sequenz mit Startzeit und bis zu 8 Events
- zufällige Berechnung der Events zwischen Mindest- und Maximalabstand
- nach jedem Event 10–30 Sekunden Zufallswartezeit, danach Klick + 5 Sekunden + Enter zur Vorbereitung des nächsten Events
- beim letzten Event endet der Plan ohne erneute Vorbereitung
- verpasste Events werden bis zu 60 Minuten nachgeholt und danach verworfen
- manuelle Mausbewegung, Linksklick, Enter und `Ctrl+Alt+F` im Webinterface
- Live-Status und nächste Auslösung
- persistente Einstellungen und Plan-Daten im PiKVM Persistent Storage
- automatischer Start über systemd
- kein PiKVM-Web/API-Passwort im Programm

## Sicherheit / Architektur

Der Dienst benötigt für Schreibvorgänge in den PiKVM Persistent Storage `kvmd-pstrun`. Daher läuft der Launcher als root. HID-Zugriffe werden ausdrücklich **nicht als root** ausgeführt. Stattdessen legt der Installer den Unix-Benutzer `hid-automation` an und autorisiert ausschließlich diesen Benutzer über KVMD Unix Socket Credentials.

Die lokale KVMD-API wird über:

```text
/run/kvmd/kvmd.sock
```

angesprochen. Bei direktem Socket-Zugriff wird laut PiKVM-API kein `/api`-Prefix verwendet.

Das Webinterface ist durch ein separates, zufälliges Passwort geschützt. Standardmäßig lauscht es auf Port `8081` im LAN. Da es normales HTTP ist, sollte Port 8081 nicht direkt ins Internet weitergeleitet werden. Für externen Zugriff empfiehlt sich z. B. Tailscale/WireGuard oder ein HTTPS-Reverse-Proxy.

## Installation auf PiKVM

Repository auf den PiKVM kopieren oder klonen und dann als `root`:

```bash
cd HID/PiKVM
chmod +x install.sh
./install.sh
```

PiKVM verwendet normalerweise ein read-only Root-Dateisystem. `install.sh` schaltet es während der Installation automatisch mit `rw` beschreibbar und danach wieder mit `ro` zurück.

Nach erfolgreicher Installation zeigt das Script URL, Benutzername und das zufällig erzeugte Passwort an, z. B.:

```text
Webinterface: http://192.168.1.123:8081/
Benutzer: admin
Passwort:  <zufällig erzeugt>
```

Das Passwort wird zusätzlich in `/etc/hid-automation.env` gespeichert. Die Datei ist nur für root lesbar.

## Dienst verwalten

```bash
systemctl status hid-automation
systemctl restart hid-automation
journalctl -u hid-automation -f
```

## Webinterface

Im Browser:

```text
http://PIKVM-IP:8081/
```

Bereiche:

- **Status** – aktueller Flow, letzte und nächste Auslösung, Fehler
- **Automatik** – ein/aus und zufälliges Min-/Max-Intervall
- **Geplanter Ablauf** – Startzeit, Endzeit, Eventanzahl, Min-/Max-Abstand, berechnete Eventliste
- **Manuelle HID-Steuerung** – Maus, Klick, Enter und `Ctrl+Alt+F`

## API

Status:

```http
GET /api/status
```

Flow sofort starten:

```http
POST /api/trigger
{}
```

Automatik ändern:

```http
POST /api/settings
{
  "auto": true,
  "autoMinMinutes": 25,
  "autoMaxMinutes": 35
}
```

Maus relativ bewegen:

```http
POST /api/mouse
{"dx":20,"dy":0}
```

Linksklick:

```http
POST /api/click
{}
```

Taste:

```http
POST /api/key
{"key":"Enter"}
```

Shortcut:

```http
POST /api/shortcut
{"keys":["ControlLeft","AltLeft","KeyF"]}
```

Events berechnen:

```http
POST /api/planner/calculate
{
  "startAt":"2026-09-03T13:55",
  "until":"17:30",
  "events":4,
  "min":15,
  "max":44
}
```

Eventliste ersetzen:

```http
POST /api/schedule/replace
{"times":[1788440400,1788443100]}
```

Plan starten:

```http
POST /api/schedule/start
{"when":"2026-09-03T13:55"}
```

Plan stoppen:

```http
POST /api/schedule/stop
{}
```

Alle Web/API-Aufrufe verwenden HTTP Basic Auth.

## Persistent Storage

Der Zustand liegt unter:

```text
/var/lib/kvmd/pst/data/hid-automation/state.json
```

Geschrieben wird über `kvmd-pstrun`, sodass die PiKVM-PST-Partition nicht dauerhaft read-write gemountet bleibt.

## PiKVM-HID-Endpunkte

Intern verwendet das Projekt folgende KVMD-Routen über den Unix-Socket:

```text
/hid/events/send_mouse_button
/hid/events/send_mouse_relative
/hid/events/send_key
/hid/events/send_shortcut
```

Damit werden keine Zugangsdaten für die normale PiKVM-Weboberfläche benötigt.

## Mouse Jiggler / Bildschirm wach halten

Für das reine Wachhalten des Zielsystems kann der bereits in PiKVM vorhandene Mouse Jiggler verwendet werden. Dadurch muss diese Funktion nicht doppelt in der Automatisierung laufen. Der eigentliche Flow und der Zeitplaner dieses Projekts funktionieren unabhängig davon.

## Dateien

```text
PiKVM/
├── app.py
├── server.py
├── install.sh
├── hid-automation.service
├── README.md
└── web/
    └── index.html
```
