# PiKVM HID Automation

PiKVM-Variante des ESP32-HID-Projekts. Die Automatisierung läuft direkt auf dem PiKVM, verwendet dessen lokale KVMD-HID-API über den Unix-Socket und ist in die normale PiKVM-Weboberfläche integriert.

## Funktionen

- eigener Automationsdienst mit mobilfreundlicher Weboberfläche
- zusätzlicher **Automation**-Eintrag direkt in der KVM-Navigationsleiste neben Macro/Text
- zusätzlicher **HID Automation**-Eintrag auf der PiKVM-Startseite über das Extras-System
- Zugriff über denselben HTTPS-Endpunkt und dieselbe Anmeldung wie PiKVM
- Backend lauscht nur lokal auf `127.0.0.1:8081`; Port 8081 wird nicht ins LAN veröffentlicht
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

## Integration in PiKVM

Der Installer legt unter

```text
/usr/share/kvmd/extras/hid-automation/
```

ein PiKVM-Extra mit `manifest.yaml` und einer kleinen Nginx-Konfiguration an.

Die Automationsoberfläche wird dadurch unter

```text
https://PIKVM-IP/hid-automation/
```

über den normalen PiKVM-Nginx verfügbar. Die serverweite PiKVM-Authentifizierung gilt auch für diesen Pfad.

Für die KVM-Navigationsleiste wird **keine PiKVM-Datei verändert**. PiKVM-Nginx liefert `/kvm/` normal aus und fügt lediglich beim HTTP-Response das kleine Script `integration.js` vor `</body>` ein. Das Script erzeugt den zusätzlichen Navbar-Eintrag `Automation` neben `Macro`.

Damit bleibt die Integration deutlich updatefreundlicher als ein Fork oder eine direkte Änderung von `/usr/share/kvmd/web/kvm/index.html`.

## Sicherheit / Architektur

Der Dienst benötigt für Schreibvorgänge in den PiKVM Persistent Storage `kvmd-pstrun`. Daher läuft der Launcher als root. HID-Zugriffe werden ausdrücklich **nicht als root** ausgeführt. Stattdessen legt der Installer den Unix-Benutzer `hid-automation` an und autorisiert ausschließlich diesen Benutzer über KVMD Unix Socket Credentials.

Die lokale KVMD-API wird über:

```text
/run/kvmd/kvmd.sock
```

angesprochen. Bei direktem Socket-Zugriff wird kein `/api`-Prefix verwendet.

Der HTTP-Backenddienst lauscht auf:

```text
127.0.0.1:8081
```

und ist daher nicht direkt aus dem LAN erreichbar. Die Browserzugriffe laufen ausschließlich über PiKVM-Nginx und verwenden die normale PiKVM-Sitzung. Ein separates HID-Automation-Passwort ist im Standardbetrieb nicht mehr erforderlich.

`server.py` unterstützt weiterhin optionale HTTP Basic Auth, falls der Dienst bewusst auf eine Nicht-Loopback-Adresse umkonfiguriert wird. Ohne Passwort verweigert der Server einen solchen unsicheren Start.

## Installation auf PiKVM

Repository auf den PiKVM kopieren oder klonen und dann als `root`:

```bash
cd HID/PiKVM
chmod +x install.sh
./install.sh
```

PiKVM verwendet normalerweise ein read-only Root-Dateisystem. `install.sh` schaltet es während der Installation automatisch mit `rw` beschreibbar und danach wieder mit `ro` zurück.

Der Installer:

1. installiert den Automationsdienst nach `/opt/hid-automation`,
2. legt den Benutzer `hid-automation` für die lokale KVMD-API an,
3. installiert das PiKVM-Extra und die Nginx-Integration,
4. validiert die KVMD-Konfiguration,
5. testet die Nginx-Konfiguration vor dem Neustart,
6. prüft den Unix-Socket-Zugriff,
7. startet den Automationsdienst,
8. prüft `127.0.0.1:8081/api/status`,
9. startet PiKVM-Nginx neu.

Danach die normale PiKVM-Weboberfläche öffnen und auf **KVM** gehen. In der oberen Leiste erscheint **Automation**.

Alternativ ist die Seite direkt innerhalb von PiKVM erreichbar:

```text
https://PIKVM-IP/hid-automation/
```

## Bereits installierte ältere Version aktualisieren

Wenn vorher schon die Port-8081-Version installiert war:

```bash
cd HID
git pull
cd PiKVM
./install.sh
```

Der Installer ersetzt die alte Konfiguration. Danach lauscht Port 8081 nur noch auf localhost und das bisherige separate Basic-Auth-Passwort wird nicht mehr benötigt.

## Dienst verwalten

```bash
systemctl status hid-automation
systemctl restart hid-automation
journalctl -u hid-automation -f
```

PiKVM-Nginx:

```bash
systemctl status kvmd-nginx
```

## Webinterface

Bereiche:

- **Status** – aktueller Flow, letzte und nächste Auslösung, Fehler
- **Automatik** – ein/aus und zufälliges Min-/Max-Intervall
- **Geplanter Ablauf** – Startzeit, Endzeit, Eventanzahl, Min-/Max-Abstand, berechnete Eventliste
- **Manuelle HID-Steuerung** – Maus, Klick, Enter und `Ctrl+Alt+F`
- **← KVM** – zurück zur normalen PiKVM-KVM-Ansicht

## API

Über den PiKVM-Pfad liegt die API unter:

```text
/hid-automation/api/...
```

Status:

```http
GET /hid-automation/api/status
```

Flow sofort starten:

```http
POST /hid-automation/api/trigger
{}
```

Automatik ändern:

```http
POST /hid-automation/api/settings
{
  "auto": true,
  "autoMinMinutes": 25,
  "autoMaxMinutes": 35
}
```

Maus relativ bewegen:

```http
POST /hid-automation/api/mouse
{"dx":20,"dy":0}
```

Linksklick:

```http
POST /hid-automation/api/click
{}
```

Taste:

```http
POST /hid-automation/api/key
{"key":"Enter"}
```

Shortcut:

```http
POST /hid-automation/api/shortcut
{"keys":["ControlLeft","AltLeft","KeyF"]}
```

Events berechnen:

```http
POST /hid-automation/api/planner/calculate
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
POST /hid-automation/api/schedule/replace
{"times":[1788440400,1788443100]}
```

Plan starten:

```http
POST /hid-automation/api/schedule/start
{"when":"2026-09-03T13:55"}
```

Plan stoppen:

```http
POST /hid-automation/api/schedule/stop
{}
```

Direkt auf dem PiKVM selbst bleibt die Backend-API intern unter `http://127.0.0.1:8081/api/...` erreichbar.

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
├── integration/
│   ├── manifest.yaml
│   └── nginx.ctx-server.conf
└── web/
    ├── index.html
    └── integration.js
```
