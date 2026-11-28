# Spielzeit am PC (Azahar-Link, melonDS-Link)

Beide Emulatoren schreiben die Spielzeit auf dem PC in **`%APPDATA%\3DS-Link\activity\`**
(Linux/macOS: der allgemeine Datenordner, z. B. `~/.local/share/3DS-Link/activity/`).
Einstellung: Menü 3DS Link > Spielzeit > „Spielzeit auf diesem PC erfassen“ (Standard an).
Der Ordner lässt sich über 3DS Link > Ordner öffnen > Spielzeit öffnen und in Dashboard-Link ziehen.

| Datei | Inhalt |
|---|---|
| `pc.txt` | `id=` zufällige Kennung dieses PCs (einmal erzeugt, nicht ändern), `name=` Anzeigename (darf man ändern) |
| `titles.txt` | `Title-ID<TAB>Name` jedes hier gespielten Spiels (Namen ohne 3DS) |
| `azahar.bin` | Sitzungen in Azahar-Link |
| `melonds.bin` | Sitzungen in melonDS-Link |

## `azahar.bin` / `melonds.bin`

Alles Little Endian. Kopf 16 Bytes: `"3DSLPCPL"`, u32 Version (1), u32 Eintragsgröße (24).
Danach Einträge, ältester zuerst, nur angehängt:

| Offset | Typ | Inhalt |
|---|---|---|
| 0x00 | u64 | Title-ID. DS-Spiele (melonDS): `00048004` + Spielcode als ASCII, erster Buchstabe oben (z. B. `ADAE` → `0004800441444145`) |
| 0x08 | u32 | Start, Ortszeit des PCs in Sekunden seit 1.1.2000 (wie der 3DS zählt) |
| 0x0C | u32 | gespielte Sekunden, ohne Pausen |
| 0x10 | u32 | Spielcode (nur DS, sonst 0) |
| 0x14 | u16 | Flags: 1 = ein 3DS war verbunden, 2 = normal beendet (fehlt nach einem Absturz) |
| 0x16 | u16 | reserviert (0) |

Der Eintrag entsteht beim Start und wird alle 30 Sekunden, beim Pausieren und beim Beenden
aktualisiert – nach einem Absturz fehlen also höchstens 30 Sekunden. Ein abgeschnittener
letzter Eintrag wird beim nächsten Start überschrieben. Eine Datei, die nicht mit dem Kopf
beginnt, wird nie angefasst.

## Im Dashboard

- Jeder Emulator auf jedem PC ist eine eigene Quelle („Name · Azahar“), wählbar im Kopf.
- Gespielt mit verbundenem 3DS (Flag 1): Hat der 3DS dasselbe Spiel innerhalb von ±3 Minuten
  selbst eingetragen (Azahar-Link „Spielzeit auf dem 3DS eintragen“), zählt nur der Eintrag des 3DS.
- Derselbe Ordner doppelt (z. B. eine Kopie) zählt einmal: Quelle + Title-ID + Start.

Später (geplant): Übergabe an den 3DS (`sdmc:/3ds/3DS-Link/activity/pc/`) mit Whitelist.
