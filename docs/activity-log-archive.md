# Aktivitätslog-Archiv

Der 3DS speichert seine Spielzeit-Historie (die Daten hinter der App „Aktivitätslog“) in
einem Ringspeicher mit höchstens 73.000 Einträgen. Ist er voll, überschreibt jeder neue
Eintrag den ältesten. Das Archiv hält alle Einträge dauerhaft fest, damit nichts verloren
geht und sie sich am PC auswerten lassen (Dashboard).

Diese Datei beschreibt, **wo die Daten liegen, wie sie gespeichert sind und wie man sie
liest**. Sie reicht, um ein eigenes Dashboard oder Werkzeug dafür zu bauen.

## Dateien

| Datei | Inhalt | wird geschrieben von |
|---|---|---|
| `playhistory.bin` | die Einträge vom 3DS, unverändert, ältester zuerst, nur angehängt | 3DS Link (bei jedem Start), Werkzeuge in `tools/activity-log/` |
| `playhistory-copy.bin` | Kopie davon (zweite Sicherung auf der SD-Karte) | 3DS Link |
| `clock.txt` | Zeitsprünge und Uhr-Korrekturen, mit Eintrags-Nummer | 3DS Link (Uhr-Menü) |
| `titles.txt` | Title-ID → Spielname | 3DS Link (bei jedem Start, nur neue) |
| `playhistory-pc.bin` | gespielt am PC ohne 3DS (Azahar), gleiches Format | Azahar Link (geplant) |

Auf der SD-Karte liegen sie in `sdmc:/3ds/3DS-Link/activity/`.

## `playhistory.bin`

Alles Little Endian.

**Kopf, 16 Bytes:**

| Offset | Größe | Inhalt |
|---|---|---|
| 0x0 | 8 | Kennung `3DSPLAY\0` |
| 0x8 | 4 | Version, zurzeit `1` |
| 0xC | 4 | Größe eines Eintrags, zurzeit `12` |

**Danach die Einträge, je 12 Bytes, genau so wie der 3DS sie speichert:**

| Offset | Größe | Inhalt |
|---|---|---|
| 0x0 | 4 | Title-ID, obere Hälfte |
| 0x4 | 4 | Title-ID, untere Hälfte |
| 0x8 | 4 | Bits 0–3: Art des Eintrags; Bits 4–31: Minuten seit 1.1.2000 00:00 |

Anzahl der Einträge = (Dateigröße − 16) / 12. Ein unvollständiger letzter Eintrag (z. B.
nach einem Abbruch beim Schreiben) wird ignoriert. Eintrag Nummer *i* liegt bei
`16 + 12·i`, man kann also direkt hinspringen.

Die Zeit ist die Uhrzeit, die am 3DS gerade eingestellt war – ohne Zeitzone, so wie sie
angezeigt wurde. Wurde die Uhr verstellt, springen die Zeiten (siehe „Zeitsprünge“).

### Art des Eintrags (Bits 0–3)

Für normale Title-IDs sind es drei Schalter (Quelle: 3dbrew, „PTM Savegame“):

| Bit | 0 | 1 |
|---|---|---|
| 0 | Öffnen | Schließen |
| 1 | Anwendung (Spiel, Systemeinstellungen …) | Applet (HOME-Menü, Notizen …) |
| 2 | Start / Ende | Fortsetzen / Pausieren (ins HOME-Menü, ohne zu beenden) |

| Wert | Bedeutung |
|---|---|
| 0 | Anwendung gestartet |
| 1 | Anwendung beendet |
| 4 | Anwendung fortgesetzt |
| 5 | Anwendung pausiert (Spielzeit steht) |
| 2, 3, 6, 7 | dasselbe für Applets |

Bei Title-ID `FFFFFFFF FFFFFFFF` (Systemereignis) ist der Wert eine Zahl:

| Wert | Bedeutung |
|---|---|
| 0 / 1 | DS-Modus gestartet / beendet |
| 7 | unbekannt, kurz vor dem Systemupdater im abgesicherten Modus |
| 8 / 9 | Standby begonnen / beendet |
| 10 | 3DS-Dienste gestoppt (Ausschalten, Start eines DS-Spiels) |
| 11 / 12 | Uhr wird umgestellt: alte Zeit / neue Zeit |

**Nur im Archiv:** Title-ID `FFFFFFFF FFFFFFFE`, Wert 15 = **Lücke**. Hier fehlen
Einträge (der 3DS hatte sie schon überschrieben, bevor sie ins Archiv kamen). Die Zeit ist
die des ersten Eintrags nach der Lücke.

## Sitzungen und Spielzeit berechnen

Für jede Title-ID der Reihe nach:

- **beginnt** bei Wert 0 (Start) oder 4 (Fortsetzen),
- **endet** beim nächsten Wert 1 (Beenden) oder 5 (Pausieren) derselben Title-ID.
- Standby (`FFFF…` 8 bis 9) innerhalb einer Sitzung abziehen.
- „Gespielt“-Zähler: nur Wert 0 zählt als neuer Start, Wert 4 nicht.
- Folgt auf einen Beginn kein passendes Ende (Absturz, leerer Akku), endet die Sitzung beim
  nächsten Systemereignis 10 oder beim nächsten Beginn eines anderen Spiels.

### Zeitsprünge

Die Reihenfolge im Archiv ist immer die echte Reihenfolge; nur die Uhrzeiten springen, wenn
die Uhr verstellt war. Zeitsprünge über 3DS Link stehen in `clock.txt` und werden beim
Auswerten herausgerechnet (siehe dort). Für alles andere (Uhr in den Systemeinstellungen
oder mit anderen Tools verstellt) sollte ein Dashboard:

- eine Sitzung verwerfen, deren Ende vor ihrem Beginn liegt oder die unplausibel lang ist,
- Sitzungen nach ihrer Startzeit dem Tag zuordnen (so macht es auch der 3DS).

## `clock.txt`

Text, eine Zeile pro Ereignis, nur angehängt. *n* ist eine Eintrags-Nummer in
`playhistory.bin` (0 = erster Eintrag): das Ereignis gilt **ab Eintrag n**. Minuten mit
Vorzeichen.

| Zeile | Bedeutung | Auswertung |
|---|---|---|
| `sprung n ±m` | Zeitsprung über 3DS Link: ab Eintrag *n* geht die Uhr um *m* Minuten falsch (+1440 = einen Tag vor) | bei Einträgen ab *n* die *m* Minuten **abziehen** |
| `ende n` | ab Eintrag *n* kein Zeitsprung mehr | ab *n* nichts mehr abziehen |
| `gestellt n alt neu` | Uhr gestellt („Uhr vom PC übernehmen“): eine Korrektur, die Uhr stimmt danach. *alt*, *neu*: Minuten seit 2000 | **nichts** herausrechnen, nur zur Info |
| `unsicher sprung ±m` / `unsicher gestellt ±m` | wie oben, aber das Archiv war in dem Moment nicht lesbar: die Stelle ist unbekannt | nicht automatisch korrigieren, anzeigen |

Ein Wechsel von +1 auf +2 Tage steht als `ende n` und `sprung n +2880` da. Die Grenze ist
exakt: 3DS Link bringt das Archiv auf den neuesten Stand, unmittelbar bevor es die Uhr
umstellt und den 3DS neu startet.

Nicht in `clock.txt`: Uhr in den Systemeinstellungen umgestellt (das trägt der 3DS selbst
als Ereignis 11/12 ein; es ist eine Korrektur und wird nicht herausgerechnet – ein laufender
Zeitsprung von 3DS Link bleibt dabei bestehen, denn er sitzt in der Hardware-Uhr). Uhr mit
anderen Tools (z. B. RTChanger) verstellt: nicht erkennbar.

## `titles.txt`

Text, UTF-8, eine Zeile pro Spiel: `Title-ID (16 Hex-Ziffern);Name`. Spätere Zeilen gelten
vor früheren (ein Name kann sich ändern). Fehlt eine Title-ID, zeigt man die Nummer.

## Wie das Archiv wächst

- **3DS Link** liest bei jedem Start den Aktivitätslog des 3DS (nur lesend), sucht darin
  die letzten 32 Einträge des Archivs und hängt alles danach an. Findet es sie nicht (die
  App war jahrelang nicht offen), hängt es eine Lücke und dann alle Einträge an.
- Die Datei wird **nur verlängert, nie umgeschrieben**. Ein beim Schreiben abgeschnittener
  letzter Eintrag wird beim nächsten Mal überschrieben. Doppelte Einträge kann es nicht
  geben: angehängt wird nur, was nach dem Archiv-Ende kommt.
- Danach wird `playhistory-copy.bin` erneuert (über eine Zwischendatei, also nie halb).
- Ist `playhistory.bin` vorhanden, aber kein Archiv (falscher Kopf), fasst 3DS Link es nicht an.

## Alte Backups dazunehmen (Merge)

Ein altes NAND-Backup enthält den Stand von damals – und damit Einträge, die der 3DS
inzwischen überschrieben hat. So kommen sie dazu:

1. Aus dem Backup holen (nur lesend, braucht `boot9.bin`; ein GodMode9-NAND-Backup
   enthält die übrigen Schlüssel schon):
   `python3 tools/activity-log/extract_playhistory.py nand <sysnand.bin> <boot9.bin> <ordner>`
   Das ergibt `<ordner>/playhistory.bin` (dazu `PlayHistory.dat` und `Pedometer.dat` wie im
   Backup).
2. Mit dem heutigen Archiv zusammenführen, das ältere zuerst:
   `python3 tools/activity-log/archive_format.py merge <alt.bin> <heute.bin> <neu.bin>`
   Das Ergebnis ist der alte Stand vollständig, danach alles aus dem neueren ab der Stelle,
   an der sich beide überschneiden (32 Einträge müssen übereinstimmen). Gibt es keine
   Überschneidung, kommt eine Lücke dazwischen.

## Größe und Geschwindigkeit

12 Bytes pro Eintrag; ein voller 3DS-Speicher (73.000 Einträge, bei viel Nutzung etwa
9 Jahre) sind 876 KB. 50 Jahre sind etwa 5 MB / 400.000 Einträge – ein Browser liest und
rechnet das in Millisekunden (`DataView` über die Datei, eine Schleife). Ein Index oder
Vorberechnung ist nicht nötig.

## Beispiel: Einlesen (JavaScript)

```js
function readArchive(buf) {            // buf: ArrayBuffer von playhistory.bin
  const v = new DataView(buf);
  const magic = new TextDecoder().decode(new Uint8Array(buf, 0, 7));
  if (magic !== "3DSPLAY" || v.getUint32(12, true) !== 12) throw "kein Archiv";
  const n = Math.floor((buf.byteLength - 16) / 12), out = [];
  for (let i = 0; i < n; i++) {
    const o = 16 + 12 * i, info = v.getUint32(o + 8, true);
    out.push({
      tid: v.getUint32(o, true).toString(16).padStart(8, "0") +
           v.getUint32(o + 4, true).toString(16).padStart(8, "0"),
      type: info & 15,
      time: new Date(Date.UTC(2000, 0, 1) + (info >>> 4) * 60000), // als UTC lesen = Anzeigezeit
    });
  }
  return out;
}
```
