# Automatische Ausrichtung — 0.1.5-dev

## Bedienung

1. Nach dem Einschalten/USB-Anstecken zunächst die normale Startstellung mit
   Weiß auf den physischen Reihen 1 und 2 aufbauen. Die Figuren werden weiterhin
   in der Schach-App eingelernt. cer4all merkt sich passiv nach drei vollständigen,
   übereinstimmenden Meldungen die Kennungen der 32 Figuren auf den Grundreihen.
2. Wer das ganze Brett dreht, braucht keine Umschaltung: Figur und physisches
   Feld bleiben unverändert verbunden.
3. Wer das Brett stehen lässt und Schwarz unten aufstellt, baut die gesamte
   Startstellung um 180 Grad gedreht auf, einschließlich König/Dame. Nach drei
   übereinstimmenden vollständigen Meldungen wechselt die Feldzuordnung.
4. Die vier mittleren Felder blinken zweimal kurz. Danach arbeitet die App mit
   den üblichen Koordinaten. a1 ↔ h8, h1 ↔ a8; dies gilt für Positionen und LEDs.
5. Eine später wieder vollständig normal aufgebaute Startstellung schaltet
   nach derselben Bestätigung zurück. Das bloße Abheben/Versetzen einzelner
   Figuren während einer Partie schaltet nicht um.

Zusatzdamen im Brettinneren sind für diese Erkennung nicht erforderlich und
werden dort nicht als Ausrichtungsmerkmal verwendet. Gleiche Figurentypen einer
Farbe dürfen ihre Plätze tauschen, sofern ihre Kennungen zu den beim Einschalten
gemerkten 32 Figuren gehören. Unbekannte Ersatzfiguren führen nicht zu einer
erratenen Ausrichtung. Die App lernt Zusatzdamen weiterhin für Umwandlungen ein.

Die Referenz bleibt bei einer BLE-Neuverbindung erhalten, wird bei neuer
USB-Sitzung/Neustart zurückgesetzt und wird nicht in Flash gespeichert. Nach
einem solchen Neustart muss zuerst wieder die normale weiße Startseite vorliegen.
Beim erstmaligen Einschalten bereits gedreht aufgebaute Figuren kann das Modul
ohne vorherige Kennungszuordnung nicht zuverlässig nach Farben unterscheiden.

Sentio liefert nur Besetzung und keine Figurenfarben. Dort sowie bei unbekannten
USB-Modellen bleibt der bisherige Durchleitungsbetrieb ohne diese Automatik.

## Schutz des bestehenden Verhaltens

- Normale Ausrichtung: Positionen werden wie bisher sofort bei jedem USB-Block
  unverändert weitergereicht. Beobachtung verändert weder IDs noch Rauschwerte.
- Gedrehte Ausrichtung: Ein vollständiger 320-Werte-Datensatz muss bis zum LF
  gesammelt werden. Danach werden die 64 unveränderten Fünf-Byte-Kennungen in
  umgekehrter Feldreihenfolge ausgegeben. Die Dezimaldarstellung wird dabei
  vereinheitlicht. Außerhalb dieser Meldungen bleiben Bytes unverändert;
  L/D-Bestätigungen innerhalb eines gedrehten Datensatzes werden davor ausgegeben.
- Das Sammeln verzögert den Beginn der BLE-Ausgabe nur im gedrehten Betrieb.
  Die App benötigt ohnehin eine vollständige Position zur Auswertung; wie sich
  die anschließende gebündelte BLE-Ausgabe auf ihre Reaktionszeit auswirkt,
  muss auf Spectrum/WROOM am Brett verglichen werden.
- Die Umkehrung betrifft die Feldzuordnung, nicht die Figurenkennung oder den
  Figurentyp. Mono-LED-Masken werden um 180 Grad gedreht; native RGB-Rahmen werden
  auf dem 9×9-Eckraster gedreht. RGB-Farben bleiben erhalten, außer dass ein durch
  die Neuordnung entstandenes reserviertes FF55-Paar mit 254 statt 255 entschärft
  wird, damit das Brett keinen neuen Rahmen darin erkennt.
- Wiederholte LED-Kommandos und die BLE-Performance-Einstellungen bleiben erhalten.
- Nur an einer bestätigten Umschaltgrenze werden alte LED-Kommandos/Teilrahmen
  und noch nicht übergebene alte Positionsbytes verworfen. Die neue Koordinaten-
  Sitzung beginnt sofort mit einer vollständigen passenden Stellung. Die App
  bleibt verbunden. Bereits vom BLE-Stack angenommene Bytes können nicht
  zurückgerufen werden; der neue ':'-Rahmen ermöglicht die Neusynchronisierung.
- Die zwei Blitze laufen in der USB-Ausgabetask. USB-Empfang und BLE-Weiterleitung
  warten darauf nicht. Neue LED-Kommandos warten kurz in der FIFO; die Bestätigung
  endet mit AUS und gibt danach diese neuen Kommandos frei. Es wird kein altes
  LED-Ziel zur späteren Wiederherstellung gespeichert.
- Gedrehte Rohmeldungen sind auf 2048 Byte begrenzt. Bei Überschreitung erfolgt
  ein expliziter Fehler/Disconnect statt stiller Verkürzung oder falscher Felder.
  Nach diesem Formatfehler die USB-Sitzung neu starten und die normale Referenz
  erneut aufbauen.

## Prüfung

Die portablen Tests prüfen beide Umschaltrichtungen, dreifache Bestätigung,
fehlende/duplizierte Kennungen, unterbrochene Meldungen, Ersatz gleichartiger
Figuren, Zusatzdamen, Reset und normale sofortige Byteidentität. Positionen und
RGB-Kommandos werden mit Paketgrößen 1..253 geprüft, Mono-Masken auf allen 64
Feldern und RGB auf allen 81 Eckpositionen. Die Blinkfolge wird einschließlich
Zeitstempelüberlauf, abschließendem AUS und Abbruch geprüft. Die bisherigen
Kerntests laufen weiterhin mit.

Vor einem Hardwareeinsatz: normale Partie als Regressionstest; dann Schwarz
unten komplett aufstellen, zwei Blitze beobachten, a1/h8 und h1/a8 sowie einen
Bauernzug mit LED-Anzeige prüfen. Danach zurückschalten und wieder spielen.
BLE-Neuverbindung und USB-Neustart getrennt prüfen. Gesicherter Rückfallstand:
ChessL1nkwireless/.cer4all-performance/baseline-0.1.4-dev.

Hardwaretest steht aus. Die zuvor gemeldete sporadische Geister-LED gilt durch
diese Änderung weder als erklärt noch als behoben.
