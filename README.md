# HID Steuerung

ESP32-S3/Arduino-Sketch fuer eine USB-HID Maus- und Tastatursteuerung auf dem LilyGO T-Dongle-S3 mit lokaler Weboberflaeche, eigenem WLAN-Access-Point, optionaler Heimnetz-Integration und IP-Anzeige am Display.

![Weboberflaeche](docs/web-ui.png)

## Funktionen

- USB-HID Maus und Tastatur
- LilyGO T-Dongle-S3 Display zeigt AP-IP, Heimnetz-IP, HID-Status, Modus und naechste Ausloesung
- Eigener Setup-Access-Point: `HID-Setup`
- Optionaler Beitritt zum Heimnetz ueber die Weboberflaeche
- Lokale, mobilfreundliche Website ohne externe JS-/CSS-Abhaengigkeiten
- Flow per Website oder API ausloesen
- Automatische Schleife ein- und ausschaltbar
- Geplante manuelle Sequenzen mit frei waehlbarem Start und festen Event-Zeitpunkten (persistent gespeichert)
- Zeitsynchronisation per NTP beim Start (Zeitzone Mitteleuropa, inkl. Sommerzeit)
- Live-Statusanzeige mit Gerätezeit, Flow-Zustand und Countdown zur nächsten Auslösung
- Manuelle Maussteuerung per Pfeiltasten auf der Website
- Manueller Linksklick per Website oder API
- Bildschirm-Wachhalten auch im Leerlauf (Maus bewegt sich alle 5 Minuten minimal um 2 Pixel links/rechts)

## Ablauf

Beim Einstecken:

1. USB-HID wird initialisiert.
2. Das LilyGO-Dongle-Display zeigt die Setup-IP an.
3. Die Maus faehrt ein sichtbares ca. 5 x 5 cm Rechteck ab.
4. Danach startet die Websteuerung und der Sketch ist bereit.

Der Flow macht:

1. Linksklick
2. 5 Sekunden warten
3. Enter druecken
4. 20 Sekunden warten
5. `Strg + Alt + F`

Wenn die automatische Schleife aktiv ist, wartet der Sketch nach jedem Flow zufaellig 25 bis 35 Minuten und startet dann erneut.

Auch im Leerlauf (Automatik aus) bewegt sich die Maus alle 5 Minuten nur um 2 Pixel nach links und direkt wieder zurueck, damit der Bildschirm wach bleibt, ohne dass man die Bewegung praktisch bemerkt.

## Geplante manuelle Sequenz

Auf der Website lassen sich unter „Geplante Auslösung" bis zu 8 feste Event-Zeitpunkte und ein Zeitpunkt „Start ab" festlegen. „Start ab" ist standardmaessig auf die aktuelle Zeit gesetzt. Voraussetzung ist eine synchronisierte Gerätezeit – diese wird beim Start per NTP geholt, sobald eine Heimnetz-Verbindung besteht.

Alternativ berechnet die Weboberflaeche die Event-Zeitpunkte automatisch. Dazu werden „Bis Uhrzeit", die Eventanzahl sowie minimale und maximale Dauer zwischen Start/Event beziehungsweise zwei Events angegeben. Die Vorgaben sind 15 Minuten Minimum und 44 Minuten Maximum. „Events berechnen" erzeugt zufaellig verteilte Vorschlaege innerhalb dieser Grenzen. Die Vorschlaege koennen in einem Dialog angepasst und danach als neue Eventliste uebernommen werden. „Bis Uhrzeit" wird ohne Datum als reine Uhrzeit gespeichert; liegt sie beim Berechnen bereits vor „Start ab", gilt sie fuer den folgenden Tag. Uhrzeit, Eventanzahl, Minimum und Maximum werden persistent auf dem Geraet gespeichert und nach einem Neustart wiederhergestellt.

Der manuelle Plan laeuft so ab:

1. Ab dem Startzeitpunkt: Linksklick, 5 Sekunden warten, Enter.
2. Bis zum ersten Event warten und dann `Strg + Alt + F` senden.
3. Falls ein weiteres Event vorhanden ist: zufaellig 10 bis 30 Sekunden warten, erneut Linksklick, 5 Sekunden warten und Enter senden.
4. Bis zum naechsten Event warten und den Ablauf wiederholen.
5. Beim letzten Event endet der Plan nach `Strg + Alt + F`; es wird kein weiterer Linksklick vorbereitet.

Startzeit und Events werden persistent gespeichert. Ausgefuehrte Events werden automatisch entfernt. Verpasste Events werden bis zu eine Stunde spaeter nachgeholt, danach verfallen sie.

## Weboberflaeche

Nach dem Flashen verbindet man sich mit dem Access-Point:

- SSID: `HID-Setup`
- Passwort: `hidsetup123`
- URL: `http://192.168.4.1/`

Auf der Website kann man:

- den Flow ausloesen
- die automatische Schleife aktivieren/deaktivieren
- WLAN-Zugangsdaten fuer das Heimnetz speichern
- die Maus mit Pfeiltasten verschieben
- einen Linksklick senden

Der Access-Point bleibt aktiv, auch wenn das Geraet zusaetzlich im Heimnetz verbunden ist.

Auf dem LilyGO-Dongle-Display gibt es zwei Seiten:

1. Netzwerk: AP-IP (`192.168.4.1`), Heimnetz-IP und SSID.
2. Status: HID/USB-Status, Modus, Flow-Zustand, naechste Ausloesung und letzte Ausloesungsquelle.

Die Seiten wechseln automatisch alle 5 Sekunden. Mit der BOOT-Taste kann man sofort zur naechsten Seite wechseln. Rechts am Display zeigt ein schmaler gelber Balken die aktuelle Seite: oben gelb fuer Netzwerk, unten gelb fuer Status.

Die Anzeige `HID aktiv` bedeutet, dass TinyUSB vom Host konfiguriert/gemountet wurde. Ob das Betriebssystem das Geraet in seiner Oberflaeche explizit als Maus/Tastatur benennt, kann der Mikrocontroller nicht direkt zuruecklesen.

## API

Status lesen:

```http
GET /api/status
```

Flow ausloesen:

```http
POST /api/trigger
```

Maus bewegen:

```http
POST /api/mouse?dx=20&dy=0
```

Linksklick senden:

```http
POST /api/click
```

Automatische Schleife einschalten:

```http
POST /api/settings?auto=1
```

Automatische Schleife ausschalten:

```http
POST /api/settings?auto=0
```

Planerwerte persistent speichern (URL-kodierter POST-Body):

```http
POST /api/planner/settings
until=63000&min=15&max=44&events=4
```

`until` sind die Sekunden seit Mitternacht; `63000` entspricht `17:30:00`.

Event hinzufügen (lokale Gerätezeit, Format `YYYY-MM-DDTHH:MM`):

```http
POST /api/schedule?when=2026-06-01T14:30
```

Berechnete Eventliste atomar ersetzen (URL-kodierter POST-Body, Epoch-Werte):

```http
POST /api/schedule/replace
times=1780315200,1780317000,1780319100
```

Manuelle Sequenz starten; ohne `when` wird sofort gestartet:

```http
POST /api/schedule/start?when=2026-06-01T13:55:00
```

Geplante Auslösung löschen (Epoch-Zeit wie in `/api/status` geliefert):

```http
POST /api/schedule/delete?when=1780000000
```

Das Statusobjekt aus `GET /api/status` enthaelt zusaetzlich `time` (Epoch), `timeSynced`, `manualPlanActive`, `manualStartAt`, die persistenten `planner...`-Werte, `maxSchedules` und die Liste `schedules` mit `when` und `label`.

## Wichtige Hinweise

- Das Board muss nativen USB-HID-Betrieb unterstuetzen, z. B. ESP32-S3 mit aktivem TinyUSB/USB-OTG-Modus.
- Der richtige native USB-Port muss verwendet werden. Ein reiner UART/Serial-Bridge-Port sendet keine HID-Eingaben.
- Nach dem Flashen das Board einmal abstecken und wieder anstecken, damit der Rechner die HID-Descriptors neu erkennt.
- Zentimeter-Angaben fuer Mausbewegungen sind Naeherungen. Betriebssystem, Bildschirmaufloesung und Mausbeschleunigung koennen die sichtbare Distanz veraendern.

## Build

Mit PlatformIO:

```bash
pio run -e lilygo-t-dongle-s3
pio run -e lilygo-t-dongle-s3 -t upload
```

Die Projektkonfiguration nutzt ein lokales Boardprofil `lilygo-t-dongle-s3-hid`. Das integrierte 80 x 160 ST7735-SPI-Display des LilyGO T-Dongle-S3 wird direkt im Sketch ueber `Adafruit GFX Library` und `Adafruit ST7735 and ST7789 Library` konfiguriert. In der Arduino IDE muessen diese beiden Bibliotheken ueber den Library Manager installiert sein; eine manuelle Aenderung globaler Display-Konfigurationsdateien ist nicht noetig.
