# Performance-Test 0.1.4-dev — 29.10.2026

## Ausgangspunkt

Tester: Spectrum an ESP32-S3-WROOM, eChessTactics, Spiel gegen Tal.
0.1.3-dev: Geister-LEDs verschwunden; spielbar. Aufforderung zum Entfernen
der Zusatzdamen verspätet, Zug-LEDs teilweise gefühlt bis zu zehn Sekunden an.
Rückstau ist eine Hypothese, noch kein gemessener Befund.

Die unveränderte lokale Rückfallkopie 0.1.3-dev liegt neben der Arbeitskopie
unter ChessL1nkwireless/.cer4all-performance/baseline-0.1.3-dev.
Sie enthält Quellcode, Firmwarepakete und die bisherigen Prüfberichte.

## Änderungen

- DLE mit 251 TX-Oktetten und 2120 us Paketzeit anfordern.
- 2M-PHY anfordern; Ablehnungen werden protokolliert und führen nicht zum Abbruch.
- Weiterhin 7,5–15 ms Intervall und Latenz 0 anfordern. Beim Aktivieren der
  Notifications einmal erneut anfordern, falls das tatsächliche Intervall noch
  über 15 ms liegt oder Peripheral Latency ungleich 0 ist. Kein endloses Erzwingen.
- Eine eventuell laufende Log-Suche beim Verbinden der Schach-App sofort abbrechen.
- Tatsächliches Intervall, MTU, PHY und DLE-Ereignisse ins vorhandene Gateway-Log.
- FIFO-Diagnose aus einer niedrig priorisierten Task alle fünf Sekunden.
  Keine Ausgabe pro Datenpaket und keine zusätzlichen Wartezeiten im Datenweg.

Unverändert: Baudrate, USB-Empfangsgröße, Positionsbytes, Reihenfolge,
Spectrum-LED-Umsetzung, wiederholte LED-Befehle und Überlaufbehandlung.
Keine Glättung, keine Duplikatunterdrückung, kein Verwerfen überholter Anzeigen.
Das Telefon entscheidet mit über die Funkparameter. Erfolgreiche Anforderungen
beweisen noch keine erfolgreiche Aushandlung oder messbare Beschleunigung.

## Messwerte lesen

`BLE link`: interval_us = tatsächliches Intervall in Mikrosekunden,
latency = erlaubte übersprungene Verbindungsereignisse, mtu = ATT-MTU.
PHY-Werte: 1 = 1M, 2 = 2M; phy_rc ungleich 0 bedeutet Abfrage fehlgeschlagen.
`BLE DLE`: ausgehandelte TX/RX-Oktette und Paketzeiten.
`BLE PHY`: Ergebnis der PHY-Änderung; status 0 = erfolgreich.
Anforderungsfehler (rc) sind von den später gemeldeten Ergebnissen zu unterscheiden.

`PERF 5s U>B`: USB-Brett → BLE-App.
in = empfangene Rohbytes, accepted = an NimBLE übergebene Bytes im Fenster.
Ohne angemeldete Notifications werden Brettdaten wie bisher nicht gepuffert;
deshalb müssen diese beiden Werte nicht gleich sein.
`PERF 5s B>U`: BLE-App → USB-Brett.
in = akzeptierte App-Bytes, written = erfolgreich an USB übergebene Bytes.
Bei Spectrum kann written wegen der 8→247-Byte-Umsetzung deutlich größer sein.

q = ausstehende Bytes, hi = maximaler Füllstand im Zeitfenster.
B>U zählt interne 6-Byte-Nachrichtenköpfe und einen laufenden USB-Schreibvorgang mit.
sample_ms = größte beobachtete Warte-/Verarbeitungszeit einer FIFO-Stichprobe
im Fenster; bei laufender Stichprobe deren bisherige Zeit. Kein exaktes Alter
jedes Bytes. Stichproben und Füllstände werden beim Löschen der Sitzung zurückgesetzt.
U>B endet bei NimBLE-Annahme, nicht beim Funkempfang durch Android.
B>U endet bei Verarbeitung des App-Schreibblocks einschließlich ausgelöstem
USB-Schreiben; unvollständige RGB-Frames können intern auf weitere Fragmente warten.
write_us = längster USB-Host-Schreibaufruf im Fenster; er misst nicht das
vollständige Abarbeiten des UART-Puffers im CP210x oder des LED-Befehls im Brett.
busy = fehlgeschlagene BLE-Übergabeversuche; Bytes bleiben dabei in der FIFO.

## Hardwarevergleich

1. WROOM über USB-UART mit 0.1.4-dev flashen, Spectrum danach an USB-OTG.
2. Wie bisher über Gateway Log die Aufzeichnung aktivieren und Log-Sitzung trennen.
3. eChessTactics verbinden, Figuren samt Zusatzdamen einlernen und gegen Tal spielen.
4. Verzögerung bei Zusatzdamen und LED-Aus beobachten; schnelle Zuganzeigen und
   wiederholte LED-Befehle prüfen. USB/BLE einmal trennen und wieder verbinden.
5. Schach-App trennen und Gateway Log herunterladen. Telefon/Android-Version,
   Testdauer und beobachtete Verzögerungen zusammen mit dem Log festhalten.
6. Bei Verschlechterung die gesicherte 0.1.3-dev erneut flashen.

Erfolg: keine Geister-LEDs, keine fehlenden Positionsbytes, stabile Verbindung
und kürzere Reaktionszeit. Anhaltend wachsende q/hi und sample_ms zeigen
Rückstau im jeweiligen Firmware-Datenweg. Kleine Firmware-Wartezeiten trotz
sichtbarer Verzögerung erfordern Untersuchung von Controller, Brett oder App.

## Erkenntnisse aus cer2nut

Gelesener Stand: gkalab/cer2nut, Commit 80ce6c8cdf89a60a7940e2233ea0bf85e667237c.
USB-Blöcke gehen direkt an einen Parser, NICHT direkt als Rohdaten auf BLE.
Die Übersetzung erzeugt 38-Byte-Chessnut-Meldungen statt ungefähr 1000 Byte
Certabo-Rohdaten. Zusätzlich glättet es über bis zu drei Stellungen.
LED-Verarbeitung entfernt alte Befehle, unterdrückt identische und taktet
standardmäßig mit 600 ms, bei RGB 200 ms. Spectrum ist berücksichtigt.
Diese Protokoll-/Verhaltensänderungen werden hier nicht übernommen.
In bleuart.cpp keine explizite DLE-/2M-/Intervallanforderung gefunden.
Die früheren Grundlagen sind historisch; deren Beschreibung einer direkten
USB→BLE-Rohdatenweitergabe in cer2nut ist damit korrigiert.

Quellen:
- https://github.com/gkalab/cer2nut/blob/80ce6c8cdf89a60a7940e2233ea0bf85e667237c/main/bleuart.cpp
- https://github.com/gkalab/cer2nut/blob/80ce6c8cdf89a60a7940e2233ea0bf85e667237c/main/adapter/lib/ChessnutConverter.cpp
- https://github.com/gkalab/cer2nut/blob/80ce6c8cdf89a60a7940e2233ea0bf85e667237c/main/adapter/lib/CertaboLedControl.cpp
