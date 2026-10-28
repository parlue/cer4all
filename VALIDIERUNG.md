# Validierung cer4all

## Ergebnis 28.10.2026

- C++17-Kerntests: PASS, inklusive manuell gepruefter vier Brettecken.
- `cer4all_s3mini`: BUILD SUCCESSFUL, 4 MB Flash eingestellt.
- `cer4all_s3wroom`: BUILD SUCCESSFUL, 16 MB Flash eingestellt.
- Beide Images: 633992 Bytes Flashbedarf, 85144 Bytes statischer RAM.
- Flash-Offsets aus der Buildausgabe: Bootloader 0x0, Partitionen 0x8000,
  Anwendung 0x10000. Projektversion 0.1.0-dev.
- Web-Diagnose: JavaScript-Syntax geprueft; Funkzugriff noch nicht hardwaregeprueft.

## Lokal automatisiert

- Portabler Kern: Chipkennung3 gegen20/21/22; unbekannte Chips bleiben Unknown.
- FIFO:20.000 zufaellige Push/Peek/Consume-Schritte, Ueberlauf atomar abgewiesen.
- Parserbeobachtung:8/320 Werte bei jeder Paketteilung1..256.
- LED:81-Ecken-Mapping, Blau204, native Bytestroeme unveraendert.
- RGB: alle Fragmentgroessen1..253 bei explizitem Modus, AUTO20/253 und8-Byte-Mono.
- Duplikatunterdrueckung, ungueltiger RGB-Abschluss, fragmentierte Mono-Masken.
- Filter: beliebige IDs, keine Praefixliste, neue IDs stabil einlernen,
  bekannte Figuren sofort verschieben, echte Leerwerte sofort, maximal3 Holds.
- Beide S3-Builds und Paketpruefsummen: siehe BUILD_RESULT.json nach erfolgreichem Bau.

Die erfolgreichen Erst-Builds wurden mit `pio run -j 2` erstellt.
Bei hoeherer Parallelitaet trat auf dem Entwicklungsrechner ein interner
GCC-Absturz in einer ESP-IDF-SDK-Datei auf.

## Ausstehend: echte Hardware

1. Ohne Brett booten, Diagnose verbinden; keine Neustartschleife.
2. Certabo CP2103 anstecken: GET_PARTNUM03,38400/8N1, komplette320-Werte-Frames.
3. Spectrum CP2102N: GET_PARTNUM20/21/22; Test mit DENSELBEN Figuren wie Certabo.
4. Sentio: erste 8-Werte-Zeile byte-identisch, danach weiterhin8 Werte, keine Tags.
5. Native Schachsoftware verbinden; kein Pairing und kein Startkommando noetig.
6. App-seitige Kalibrierung samt Zusatzdamen und normalen Spielen durchfuehren.
7. BLE-MTU 23 und256 testen: gleiche vollstaendige Positionen, kein fehlendes Ende.
8. USB-LEDs:8-Byte-Maske Mono,247-Byte-RGB und20-Byte-BLE-Teilungen Spectrum.
9. Schnell wechselnde LED-Kommandos und alle-Aus direkt nacheinander: Reihenfolge
   vergleichen, keine zusaetzlichen Takte, Pausen oder zusammengelegten Felder.
10. Zweites Diagnosegeraet verbinden: Status/Log lesen und waehrenddessen spielen.
11. USB ziehen waehrend LED-Ausgabe, wieder stecken: keine alten Kommandos,
    neue Chipabfrage, Anwendung neu verbinden; keine blockierte USB-Task.
12. BLE trennen bei halbem RGB-Frame, neu verbinden: kein Rest aus alter Sitzung.
13. Gegenstau absichtlich erzwingen: deutlicher Fehler/Disconnect statt stiller
    fehlender Bytes. WNR kann Ueberlast nicht per ATT quittieren.
14. Fremder Figurensatz und neue Zusatzdamen: keine hardcodierte ID-Abhaengigkeit.
15. Filter an/aus vergleichen: unbekannte stabile Stoerwerte dokumentieren.

Nicht behauptet: universelle Erkennung saemtlicher CP210x-Boards, fehlerfreie
Spectrum-Sensordaten, garantierte Kompatibilitaet jeder existierenden Software
oder bereits bestandene Funk-/USB-Hardwaretests. Es wurde nichts geflasht.
