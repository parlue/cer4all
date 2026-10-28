# cer4all

USB-zu-BLE-Interface fuer Certabo, Tabutronic Spectrum und Sentio auf ESP32-S3.
Eigenes Projekt; keine Aenderungen an eChessGw oder eChessTactics.

**Entwicklungsstand 0.1.0-dev. Keine Hardwarefreigabe.** Unterstuetzung einer
Anwendung plus einer optionalen zweiten BLE-Verbindung fuer Diagnose.

## Brett erkennen

Direkte USB-Abfrage `GET_PARTNUM` am Silicon-Labs-Chip, keine LED-Probe:

| Chipkennung | Brettpfad |
|---|---|
| CP2103, 0x03 | Certabo, native 8-Byte-LEDs |
| CP2102N, 0x20 / 0x21 / 0x22 | Spectrum, RGB |
| Andere / Abfrage fehlgeschlagen | Native Weiterleitung, Typ Unknown; keine LED-Umrechnung |
| Vollstaendige Positionszeile mit 8 Werten | Sentio; ueberschreibt den vorlaeufigen Chiphint |

Die Zuordnung CP2103/CP2102N ist die Vorgabe fuer die vorhandenen Bretter,
keine universelle Zusicherung fuer jedes zukuenftige Certabo-Modell. VID/PID
allein unterscheiden die Chips nicht. Figuren-IDs und deren Praefixe haben
**keinen Einfluss auf die Brettwahl**.

## Daten und LEDs

- 38400 Baud, 8N1, DTR/RTS aktiv; kein Startkommando, keine LED-Probe.
- Ein USB-IN-Transfer von 64 Bytes. Sofortige Weitergabe vorhandener Daten;
  keine Sleep-, Blink-, Zugfeld-Sammel- oder Rotationslogik im Spielpfad.
- BLE-Name `Certabo`, Nordic UART Service 6e400001-b5a3-f393-e0a9-e50e24dcca9e.
  Schreiben 0002 (mit/ohne Antwort), Notify 0003. Kein Pairing/Bonding.
- Bevorzugte MTU 256; eine kleinere MTU, auch 23, fragmentiert den Strom korrekt.
- USB nach BLE: 32 KiB FIFO. LED-Kommandos: 16 KiB mit erhaltener Nachrichtengrenze (bis 512 Bytes je Write).
  Kurze 8-Byte-Writes brauchen nur 14 Bytes inklusive Verwaltungsdaten.
  Reihenfolge bleibt erhalten. Bei Erschoepfung expliziter Fehler und Trennung;
  keine unbemerkte Weiterleitung eines abgeschnittenen Datenstroms. Unendlichen
  Rueckstau kann begrenzter RAM nicht auffangen.
- Certabo/Sentio/Unknown: LED-Bytes unveraendert durchreichen. Auch Sentio-v2
  wird nicht als Spielzug interpretiert. Keine kuenstlichen Sentio-Figuren-IDs.
- Spectrum: RGB-Rahmen (247 Bytes) zusammensetzen, pruefen und senden;
  normale 8-Byte-Masken in vier Ecken je Feld umsetzen. Standard Blau 80 %.
  Exakt unveraenderte Spectrum-LED-Ausgaben werden unterdrueckt, ohne Wartezeit.
- RGB nach Mono wird bewusst nicht konvertiert; die Anwendung muss fuer ein
  normales Certabo das passende LED-Protokoll waehlen.

Ein 247-Byte-RGB-Rahmen belegt die 38400-Baud-Leitung fuer etwa 64ms. Dauerhaft
mehr als rund 15 RGB-Ausgaben pro Sekunde erzeugen daher auch ohne kuenstliche
Pausen Rueckstau. Die Firmware erfindet keine schnellere physische Leitung.

### Grenzen der automatischen LED-Rahmenerkennung

Das Mono-Protokoll hat keine Rahmensignatur. Eine Mono-Maske kann selbst mit
FF55 beginnen. Im AUTO-Modus gelten einzelne8-Byte-BLE-Writes als Mono;
sonst kennzeichnet FF55 einen RGB-Rahmen. Das deckt die gaengigen nativen
247-Byte- und20-Byte-fragmentierten RGB-Writes ab. Frei gewaehlte Teilungen
koennen mehrdeutig sein (etwa ein8-Byte-RGB-Anfang oder eine fragmentierte
FF55-Mono-Maske). Fuer solche Programme vor der Verbindung im Diagnosefenster
`LEDS RGB` oder `LEDS MONO` festlegen. Keine zeitabhaengige Rateheuristik.

### Spectrum-Rauschfilter ohne vorgegebene IDs

Es gibt keine Praefixliste und keine fest eingebauten Figurenkennungen.
Kennungen werden zur Laufzeit nach drei stabilen, nicht auf mehreren Feldern
doppelt beobachteten Messungen als bekannt
behandelt. Bekannte Kennungen und echte Leerwerte passieren danach sofort;
wechselnde unbekannte Werte halten einen vorherigen bekannten Feldinhalt
hoechstens drei Messungen, danach wird das Feld leer. Der Filter puffert nur
ein Feld (fuenf Werte), kein ganzes Brett. Seine erste Positionszeile bleibt
ungefiltert, bis 320 gegen8 Werte unterschieden sind. Alle gelernten Kennungen
werden beim USB-Neuanschluss geloescht, nichts wird dauerhaft archiviert.

**Grenze:** Dauerhaft stabiler Sensormuell kann wie eine neue Figur aussehen.
Ohne vorgegebene Tags kann die Firmware das nicht sicher unterscheiden.
Der Filter ist fuer Vergleichstests vor einer Anwendungssitzung abschaltbar.
Die Anwendungs-Kalibrierung, Figurentypzuordnung und Grundstellung bleiben
vollstaendig Aufgabe der angeschlossenen Schachsoftware.

## Hardware und Bauen

- `cer4all_s3mini`: Waveshare ESP32-S3-Zero / S3FH4R2, 4 MB Flash.
- `cer4all_s3wroom`: ESP32-S3-WROOM-1-N16R8, 16 MB Flash.
- PSRAM bleibt auf beiden absichtlich aus; fuer die FIFO-Puffer nicht noetig.
  Dadurch werden auch keine PSRAM-Pins als eigene GPIOs benutzt.
- USB-Host: nativer OTG-Port, GPIO19 D-, GPIO20 D+. Endpunkte dynamisch aus USB.
- Brettversorgung extern ueber geeignetes OTG-Y-Kabel; kein VBUS-Schaltpin.
- Mini: Brett abziehen zum Flashen; BOOT halten/RESET fuer ROM-Download-Modus.
  Einzige USB-Buchse steht zur Laufzeit als Host bereit, nicht als USB-Konsole.
- WROOM: USB-UART zum Flashen/Versorgen, OTG zum Brett.

```text
pio run -j 2 -e cer4all_s3mini
pio run -j 2 -e cer4all_s3wroom
python tools/package.py
```

Toolchain: PlatformIO espressif32 6.8.1, ESP-IDF5.3.0, Espressif CP210x2.2.0
und CDC-ACM2.1.0. Abhaengigkeiten werden ueber den IDF-Komponentenmanager bezogen.
Bei Windows: `tools/test_core.cmd` fuer portable C++-Tests mit Visual Studio.

## Diagnose, Webflasher und naechster Test

**Online testen: https://parlue.github.io/cer4all/**

`docs/index.html` enthaelt zwei getrennte Flasher-Knoepfe. Die Pakete entstehen
nur aus erfolgreichen Builds durch tools/package.py; Flash-Offsets stammen aus
der jeweiligen IDF-Buildausgabe. GitHub Pages liefert den docs-Ordner aus. Alternativ ist ein lokaler Server moeglich.
Aus dem Projektordner lokal starten:

```text
python -m http.server 8934 --bind 127.0.0.1 --directory docs
```

Dann http://localhost:8934 im Browser oeffnen. Die private Grundlagen-Datei
wird dabei nicht ausgeliefert, nur der oeffentliche docs-Ordner.

`docs/debug.html` verbindet sich ueber Web Bluetooth (Chrome/Edge), zeigt
Status und Live-Log, und speichert den Mitschnitt als Textdatei. Diagnose nutzt
einen eigenen Dienst und eine zweite Verbindung. Sie darf keinen NUS-Client
uebernehmen; Logs sind nachrangig, der Anwendungsdatenstrom hat Vorrang.

GATT-Details und Befehle: [Diagnose](docs/DIAGNOSE.md).
Geplanter Hardwaretest: [VALIDIERUNG.md](VALIDIERUNG.md).

Die private Grundlagen-Datei bleibt lokal und ist vom Veroeffentlichen
sowie vom Quellarchiv ausgeschlossen. Keine Hersteller-PDFs und kein Code aus
cer2nut oder Bauerino wurden uebernommen. Siehe [QUELLEN.md](QUELLEN.md).
