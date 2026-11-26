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
| `playhistory.bin` | die Einträge vom 3DS, ältester zuerst, nur angehängt | 3DS Link (bei jedem Start, geplant), Werkzeuge in `tools/activity-log/` |
| `playhistory-pc.bin` | gespielt am PC ohne 3DS (Azahar), gleiches Format | Azahar Link (geplant) |
| `titles.txt` | Title-ID → Spielname | 3DS Link (geplant) |

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

Wurde die Uhr verstellt (auch durch die Uhr-Funktionen von 3DS Link), stehen Einträge mit
späteren oder früheren Zeiten mitten im Archiv. Die Reihenfolge im Archiv ist immer die
echte Reihenfolge; nur die Uhrzeiten springen. Ein Dashboard sollte deshalb:

- eine Sitzung verwerfen, deren Ende vor ihrem Beginn liegt oder die unplausibel lang ist,
- Sitzungen nach ihrer Startzeit dem Tag zuordnen (so macht es auch der 3DS).

## `titles.txt`

Text, UTF-8, eine Zeile pro Spiel: `Title-ID (16 Hex-Ziffern);Name`. Spätere Zeilen gelten
vor früheren (ein Name kann sich ändern). Fehlt eine Title-ID, zeigt man die Nummer.

## Wie das Archiv wächst (3DS Link, geplant)

- **3DS Link** fragt bei jedem Start die neuesten Einträge des 3DS ab, sucht darin die
  letzten 32 Einträge des Archivs und hängt alles danach an. Findet es sie nicht (die App
  war jahrelang nicht offen), hängt es eine Lücke und dann alle Einträge an.
- Die Datei wird **nur verlängert, nie umgeschrieben**.

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
