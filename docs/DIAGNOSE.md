# BLE-Diagnose

Dienst: ce4c0000-5887-4d42-9888-31489f8ca144

- Status/Steuerung: ce4c0001-5887-4d42-9888-31489f8ca144 (Read, Write).
- Log: ce4c0002-5887-4d42-9888-31489f8ca144 (Notify).
- NUS ist ausschliesslich fuer die Schachanwendung; Diagnose schreibt dort nicht.

Status ist JSON mit Firmwarestand, USB-Chipkennung, Bretttyp, USB-Status,
MTU-Nutzlast, Bytezaehlern,320er-Frames, Filtereingriffen und Transportfehlern.

Befehle (ASCII, keine Zeilenumbrueche), nur ohne aktive NUS-Notifications:

- STATUS: Status ins Log schreiben; laengere Statusdaten immer direkt lesen.
- FILTER 0 / FILTER 1: zeitlichen Spectrum-Filter aus/ein.
- LEDS AUTO / LEDS MONO / LEDS RGB: Eingabeformat der App fuer Spectrum.

Einstellungen gelten bis Neustart. Keine Speicherung von Figurenkennungen.
Logpuffer8 KiB, Diagnose ist best-effort; bei vollem Puffer fallen alte
Debugzeilen weg. Anwendungsdaten werden getrennt gepuffert und nie durch
Debugzeilen ersetzt. Fehler im Datenstrom fuehren zum expliziten Disconnect.

Eine Verbindung fuer Anwendung, eine fuer Diagnose; keine zweite
Schachanwendung gleichzeitig. Unverschluesselt, ohne Pairing wie NUS.
