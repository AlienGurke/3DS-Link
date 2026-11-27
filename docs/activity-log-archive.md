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
| `pld.dat` / `pld-initial.dat` | Daten der Aktivitätslog-App (siehe unten): neuester Stand / der erste je gesicherte, wird nie ersetzt | 3DS Link (bei jedem Start, nur wenn geändert) |
| `steps.bin` / `steps-initial.bin` | Schritte pro Stunde, vom System erfragt (siehe unten) | 3DS Link (bei jedem Start, nur wenn geändert) |
| `idb.dat` / `idbt.dat` (+ `-initial`) | Icon-Speicher des HOME-Menüs (Namen, Bilder) | 3DS Link (bei jedem Start, nur wenn geändert) |
| `icons/<Title-ID>.smdh` | Icon-Datei eines Titels, der im Icon-Speicher fehlt (z. B. Homebrew) | 3DS Link (einmal je Titel) |
| `console.txt` | von welcher Konsole (Modell, Kennung, Name – den darf man ändern) | 3DS Link (einmal) |
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

### Zurückgesetzte Uhr (leerer Akku)

Fällt die Uhr zurück (der 3DS setzt sie z. B. auf 01.01.2011 00:00), stehen die folgenden
Einträge mit falschem Datum da, bis die Uhr wieder gestellt wird. Das System trägt beides
selbst ein (Ereignis 11/12: alte → neue Zeit). Daraus lässt sich der Abschnitt genau
korrigieren, ohne Einstellung und für jede Konsole:

1. Ein Abschnitt beginnt bei einem Ereignis 11/12, dessen neue Zeit weit (mehr als einen
   Tag) **vor** dem letzten Eintrag davor liegt (Uhr zurückgefallen).
2. Er endet beim nächsten Ereignis 11/12, das die Uhr wieder nach vorn stellt.
3. Alle Einträge dazwischen bekommen dessen Unterschied (neu − alt) dazu – die Uhr lief in
   der Zeit ja weiter, nur von einem falschen Startwert aus.
4. Endet ein Abschnitt ohne solches Ereignis (z. B. mit einem anderen Tool gestellt), wird er
   als „Datum unbekannt“ geführt: zählt zur Spielzeit, aber zu keinem Tag.

In `pld.dat` stehen solche Stunden ebenfalls mit falschem Datum; für die Zeit, die
`playhistory.bin` abdeckt, nimmt man die Sitzungen von dort.

## `console.txt`

Text, `schlüssel=wert`: `model` (3DS, 3DS XL, New 3DS, 2DS, New 3DS XL, New 2DS XL), `id`
(eine Kennung, die je Konsole verschieden ist und sonst nichts verrät), `name` (anfangs das
Modell; frei änderbar, z. B. „Mein alter 3DS“). Mehrere Konsolen: je ein `activity`-Ordner;
ein Dashboard kann sie getrennt und zusammen zeigen.

## `clock.txt`

Text, eine Zeile pro Ereignis, nur angehängt. *n* ist eine Eintrags-Nummer in
`playhistory.bin` (0 = erster Eintrag): das Ereignis gilt **ab Eintrag n**. Minuten mit
Vorzeichen.

| Zeile | Bedeutung | Auswertung |
|---|---|---|
| `jump n ±m` | Zeitsprung über 3DS Link: ab Eintrag *n* geht die Uhr um *m* Minuten falsch (+1440 = einen Tag vor) | bei Einträgen ab *n* die *m* Minuten **abziehen** |
| `end n` | ab Eintrag *n* kein Zeitsprung mehr | ab *n* nichts mehr abziehen |
| `set n old new` | Uhr gestellt („Uhr vom PC übernehmen“): eine Korrektur, die Uhr stimmt danach. *old*, *new*: Minuten seit 2000 | **nichts** herausrechnen, nur zur Info |
| `unsure jump ±m` / `unsure set ±m` | wie oben, aber das Archiv war in dem Moment nicht lesbar: die Stelle ist unbekannt | nicht automatisch korrigieren, anzeigen |

Ein Wechsel von +1 auf +2 Tage steht als `end n` und `jump n +2880` da. Die Grenze ist
exakt: 3DS Link bringt das Archiv auf den neuesten Stand, unmittelbar bevor es die Uhr
umstellt und den 3DS neu startet.

Nicht in `clock.txt`: Uhr in den Systemeinstellungen umgestellt (das trägt der 3DS selbst
als Ereignis 11/12 ein; es ist eine Korrektur und wird nicht herausgerechnet – ein laufender
Zeitsprung von 3DS Link bleibt dabei bestehen, denn er sitzt in der Hardware-Uhr). Uhr mit
anderen Tools (z. B. RTChanger) verstellt: nicht erkennbar.

## `pld.dat` (Aktivitätslog-App)

Der Spielstand der Aktivitätslog-App (System-Spielstand `0002xxyy`, bei der europäischen
Version `00020222`), unverändert kopiert. Er reicht zurück bis zur ersten Benutzung der
Konsole – weiter als der Spielzeit-Log, der nach etwa 73.000 Einträgen überschreibt. Aus
ihm zeichnet die App ihre Grafiken und die Software-Bibliothek. Little Endian, 806.160 Bytes:

| Offset | Größe | Inhalt |
|---|---|---|
| 0x0 | 16 | Kopf (u32 0, u32 unbekannt, u32 2, u32 0) |
| 0x10 | 50.000 × 16 | **Spielzeit pro Stunde und Titel**, ältester zuerst |
| 0xC3510 | 256 × 24 | **Software-Bibliothek**: Summe pro Titel |

Stunden-Eintrag (16 Bytes): u64 Title-ID, u32 Beginn der Stunde in Sekunden seit
1.1.2000 (Ortszeit wie am 3DS), u32 in dieser Stunde gespielte Sekunden. Unbenutzte Plätze
sind `FF…`. Die Summen pro Tag/Monat/Jahr der App sind die Summen dieser Werte. Was die
App ab 50.000 belegten Stunden macht, ist nicht bekannt – deshalb `pld-initial.dat`.

Bibliotheks-Eintrag (24 Bytes): u64 Title-ID, u32 Gesamtspielzeit in Sekunden, u16 Anzahl
Starts, u16 unbekannt (bisher immer 2), u16 zuerst gespielt, u16 zuletzt gespielt (beide
Tage seit 1.1.2000), u32 unbekannt (bisher 0). Höchstens 256 Titel; ist sie voll, entfernt
die App einen (laut Handbuch den mit der kürzesten Spielzeit). Der Spielzeit-Log und
damit `playhistory.bin` haben keine solche Grenze.

Geprüft an einem echten Gerät: Pokémon Mond – 263:41, 531-mal, 28.01.2017 bis 17.02.2018,
genau wie in der App angezeigt.

## `steps.bin` / `steps-initial.bin` (Schritte)

PTM hält seine Datei `Pedometer.dat` offen, sie lässt sich nicht kopieren. Deshalb fragt
3DS Link PTM selbst (`PTM:GetStepHistory`: *n* Stunden **ab** einer Zeit in ms seit
1.1.2000, ältester zuerst – an einem echten 3DS mit den Tageswerten der Aktivitätslog-App
geprüft), ab 1.1.2011 (Erscheinungsjahr des 3DS). Tage vor der ersten Benutzung kommen als
0. Nur das erste Mal wird alles erfragt (30 Tage je Anfrage); danach bleiben die gespeicherten
Tage, und nur die letzten 3 werden neu erfragt.

Kopf 16 Bytes: `3DSSTEPS`, u32 Version (1), u32 Größe eines Datensatzes (60). Datensatz:
u64 Beginn des Tages (ms seit 1.1.2000, Ortszeit), u32 Stunden (24), 24 × u16 Schritte
(Stunde 0–23).

## `idb.dat` + `idbt.dat` (Namen und Bilder)

Der Icon-Speicher des HOME-Menüs (NAND Shared Extdata `F000000B`), unverändert kopiert. Er
enthält jeden Titel, der gestartet wurde – auch längst gelöschte –, und ist die Quelle für
Namen und Bilder (die Aktivitätslog-App nimmt sie auch von dort). 512 Plätze; Platz *i* in
`idbt.dat` gehört zu Datensatz *i* in `idb.dat`. Ist er voll, werden Plätze neu belegt –
darum gibt es auch hier eine `-initial`-Kopie.

`idbt.dat` (8.200 Bytes): u32 unbekannt (vermutlich nächster zu belegender Platz), u32 0,
dann 512 × 16 Bytes: u64 Title-ID, u32 Zeit (Sekunden seit 2000), u32 unbekannt. Leere
Plätze: Title-ID 0. Ein Titel kann mehrere Plätze haben – der mit der neuesten Zeit gilt.

`idb.dat` (512 × 0x36A0 Bytes), je Datensatz:

| Offset | Größe | Inhalt |
|---|---|---|
| 0x0 | 0x20 | Einstellungen (Altersfreigaben, Region …) wie im SMDH |
| 0x20 | 16 × 0x200 | Namen je Sprache wie im SMDH: 0x80 kurz, 0x100 lang, 0x80 Hersteller (UTF-16) |
| 0x2020 | 0x480 | Bild 24×24 |
| 0x24A0 | 0x1200 | Bild 48×48 |

Bilder: RGB565, in 8×8-Kacheln, die Pixel einer Kachel in Z-Reihenfolge (wie im SMDH).
Sprachen: 0 Japanisch, 1 Englisch, 2 Französisch, 3 Deutsch, 4 Italienisch, 5 Spanisch, …
`tools/activity-log/icon_cache.py` macht daraus `titles.csv` und ein PNG pro Titel.

## `icons/` (Titel, die das HOME-Menü nicht kennt)

Programme, die nie über das HOME-Menü gestartet wurden (z. B. Homebrew aus dem Homebrew
Launcher oder FBI heraus), fehlen im Icon-Speicher. Für jeden solchen Titel, der in der
Bibliothek von `pld.dat` oder in `playhistory.bin` vorkommt, liest 3DS Link die Icon-Datei
des installierten Titels (ExeFS `icon`, SMDH, 0x36C0 Bytes) und legt sie unverändert als
`icons/<Title-ID>.smdh` ab (Title-ID 16 Hex-Zeichen). Aufbau wie überall dokumentiert:
`SMDH`, Namen ab 0x08 (16 Sprachen × 0x200: kurz 0x80, lang 0x100, Herausgeber 0x80,
UTF-16), 48×48-Icon (RGB565, gekachelt) ab 0x24C0.

Nicht mehr installierte Titel haben keine; sie werden bei jedem Start kurz neu versucht.
`icons/scanned.txt` merkt sich, bis zu welchem Eintrag von `playhistory.bin` schon geschaut
wurde. Hat ein Titel beides, gilt der Icon-Speicher.

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
   Weil vorne Einträge dazukommen, verschieben sich die Eintrags-Nummern; `clock.txt` mit
   angeben, dann wird eine passende neue geschrieben:
   `... merge <alt.bin> <heute.bin> <neu.bin> <clock.txt> <clock-neu.txt>`
3. Beide neuen Dateien anstelle der alten auf die SD-Karte, während 3DS Link geschlossen ist.
   Das Ende des Archivs ist unverändert, also macht 3DS Link danach einfach weiter.
   (Das ist der einzige Fall, in dem das Archiv neu geschrieben wird – als neue Datei; die
   alte bleibt, bis man sie ersetzt.)

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
