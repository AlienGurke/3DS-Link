/*
    melonDS Link - 3DS side

    Turns the 3DS into a controller (buttons + touch) for melonDS on the PC,
    shows the DS bottom screen streamed from the PC, and gives melonDS
    access to files on the SD card (saves, ROMs).

    C-Stick up: switch between original size (1:1) and zoom (full screen).
    Hold SELECT + START for about one second to quit.
*/

#include <3ds.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "LinkProto.h"
#include "lz4.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

static bool readRawRtc(u64* ms);

/* ------------------------------------------------------------------ */
/* shared state between main thread and server thread                  */

static LightLock g_lock;
static volatile bool g_running = true;
static volatile bool g_tcpConnected = false;
static char g_statusText[128] = "";
static char g_peerName[32] = "";

/* ------------------------------------------------------------------ */
/* text: the console font is code page 437 (has ä ö ü ß é ...), our    */
/* strings and file names are UTF-8                                     */

static void utf8ToCp437(char* out, size_t outSize, const char* in)
{
    static const struct { u32 cp; u8 c; } map[] = {
        {0xE4,0x84},{0xF6,0x94},{0xFC,0x81},{0xC4,0x8E},{0xD6,0x99},{0xDC,0x9A},{0xDF,0xE1},
        {0xE9,0x82},{0xE8,0x8A},{0xEA,0x88},{0xEB,0x89},{0xE0,0x85},{0xE1,0xA0},{0xE2,0x83},
        {0xE7,0x87},{0xC7,0x80},{0xC9,0x90},{0xED,0xA1},{0xEC,0x8D},{0xEE,0x8C},{0xEF,0x8B},
        {0xF3,0xA2},{0xF2,0x95},{0xF4,0x93},{0xFA,0xA3},{0xF9,0x97},{0xFB,0x96},{0xF1,0xA4},
        {0xD1,0xA5},{0xE5,0x86},{0xC5,0x8F},{0xE6,0x91},{0xC6,0x92},{0xFF,0x98},{0xBF,0xA8},
        {0xA1,0xAD},{0xB0,0xF8},{0x2013,'-'},{0x2014,'-'},{0x2026,'.'},{0x2019,'\''},{0x201E,'"'},
        {0x201C,'"'},{0x201D,'"'},{0xE9,0x82},
    };
    size_t o = 0;
    const u8* p = (const u8*)in;
    while (*p && o + 1 < outSize)
    {
        u32 cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else { p++; while ((*p & 0xC0) == 0x80) p++; cp = '?'; }
        u8 c = '?';
        if (cp < 0x80) c = (u8)cp;
        else for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (map[i].cp == cp) { c = map[i].c; break; }
        out[o++] = (char)c;
    }
    out[o] = 0;
}

/* languages (English is the default) */
enum { LANG_EN, LANG_DE, LANG_COUNT };
static int g_lang = LANG_EN;
static const char* const g_langCodes[LANG_COUNT] = { "en", "de" };
static volatile bool g_haveLangEvent = false;   /* tell melonDS about a new language */

/* the emulator on the PC (LINK_CLIENT_*, from its HELLO; remembered in the
   settings). Azahar runs 3DS games: their bottom screen is shown 1:1, the
   C-stick belongs to the game and there is no game list. */
static volatile u32 g_client = LINK_CLIENT_MELONDS;
static volatile bool g_clientChanged = false;   /* server thread -> main loop */
#define NATIVE() (g_client == LINK_CLIENT_AZAHAR)
static const char* emulatorName(void) { return NATIVE() ? "Azahar" : "melonDS"; }

enum StrId
{
    S_TITLE, S_IP, S_CONNECTED, S_WAITING, S_CONTROLS, S_ACTIVE, S_PICTURE, S_DISPLAY,
    S_ENLARGED, S_ORIGINAL, S_PICK_GAME, S_NO_GAMES, S_LIST_HINT, S_LIST_HINT_BACK, S_RUNNING,
    S_HINT_MENU, S_HINT_CSTICK, S_HINT_CONNECT, S_MENU_TITLE, S_M_GAMES, S_M_DISPLAY, S_M_TOPOFF,
    S_M_SHOT, S_M_LANG, S_M_BACK, S_M_QUIT, S_MENU_HINT, S_STARTING, S_SHOT_REQ, S_NOT_CONNECTED,
    S_WIFI_FAIL, S_WIFI_FAIL2, S_ASK_HINT, S_PC_COPIED, S_XFER_WIFI, S_XFER_DATA, S_XFER_WAIT,
    S_LANGNAME,
    S_M_BOTTOM, S_ON, S_OFF, S_M_STOP, S_D_RESUME_T, S_D_CONTINUE, S_D_LIST, S_D_LOST, S_D_START,
    S_D_BACK, S_D_STOP_T, S_D_STOP_A, S_D_QUIT_T, S_D_QUIT_1, S_D_SAVING, S_D_QUIT_A, S_STOPPING,
    S_D_SWITCH_T,
    S_M_BACKUPS, S_B_LOADING, S_B_NONE, S_B_HINT, S_D_RESTORE_T, S_D_RESTORE_1, S_D_RESTORE_2,
    S_D_RESTORE_A, S_RESTORING, S_CONFIRM_HINT,
    S_M_SHOTMODE, S_SHOT_BOTH, S_SHOT_TOP, S_SHOT_BOTTOM, S_SHOT_SEPARATE,
    S_M_STATUSBAR, S_BAT, S_WIFI,
    S_TAB_DS, S_TAB_3DS, S_NO_LIB, S_NEED_MELONDS, S_NEED_AZAHAR, S_ON3DS,
    S_M_GLITCH, S_GLITCH_ALLOW, S_GLITCH_NEVER,
    S_COUNT
};

static const char* const g_strs[LANG_COUNT][S_COUNT] = {
    {   /* English */
        "melonDS Link", "IP address:", "connected", "waiting...", "Controls:", "active", "Picture:",
        "Display:", "Enlarged", "Original (1:1)", "Pick a game", "No .nds files found.",
        "A start  X fav  Y reload  L/R tab", "A start  X fav  Y reload  L/R tab  B back", "Running:",
        "SELECT+START: menu   L+R+SELECT+START hold: quit", "C-stick: down = games, up = display",
        "In melonDS: 3DS Link > Connect to 3DS", "melonDS Link menu", "Game list", "Display",
        "Turn top screen off", "Screenshot (saved on the PC)", "Language", "Back to the game", "Quit the app",
        "A select   B close", "Starting game - waiting for melonDS...", "Screenshot requested",
        "melonDS is not connected", "WiFi could not be started.", "Is WiFi on? START to quit.",
        "A / B to choose", "PC = copied to the PC", "WiFi", "data", "Waiting:", "English",
        "Bottom screen picture", "on", "off (touch still works)", "End the game on the PC",
        "Still running in melonDS:", "Continue playing", "Game list",
        "Progress since the last in-game save will be lost.", "Start", "Back",
        "End the game in melonDS?", "End the game", "Quit melonDS Link?",
        "melonDS pauses the game until you connect again.", "A save is being written right now!",
        "Quit", "Ending the game...", "Start another game?",
        "Save backups", "Getting the list from the PC...", "No backups of this game on the PC.",
        "A load   B back", "Load this backup onto the 3DS?", "The current save is kept as a backup.",
        "The game in melonDS is ended for this.", "Load backup", "Loading the backup...",
        "L+R+A confirm   B back",
        "Screenshot shows", "both screens", "top screen", "bottom screen", "both, separate files",
        "Battery/WiFi at the top", "Battery", "WiFi",
        "DS - melonDS", "3DS - Azahar", "No games in Azahar's game list.",
        "To start a DS game: connect melonDS", "To start a 3DS game: connect Azahar",
        "3DS = also installed on this 3DS",
        "Picture glitches", "allow (smoother)", "never (pauses briefly)",
    },
    {   /* Deutsch */
        "melonDS Link", "IP-Adresse:", "verbunden", "wartet...", "Steuerung:", "aktiv", "Bild:",
        "Anzeige:", "Vergrößert", "Original (1:1)", "Spiel wählen", "Keine .nds-Dateien gefunden.",
        "A Start  X Favorit  Y neu  L/R Tab", "A Start  X Favorit  Y neu  L/R Tab  B zurück", "Läuft:",
        "SELECT+START: Menü   L+R+SELECT+START halten: Ende", "C-Stick: runter = Spiele, hoch = Anzeige",
        "In melonDS: 3DS Link > Mit 3DS verbinden", "melonDS-Link-Menü", "Spieleliste", "Anzeige",
        "Oberen Bildschirm ausschalten", "Screenshot (wird am PC gespeichert)", "Sprache",
        "Zurück zum Spiel", "App beenden", "A auswählen   B schließen",
        "Starte Spiel - warte auf melonDS...", "Screenshot angefordert", "melonDS ist nicht verbunden",
        "WLAN konnte nicht gestartet werden.", "Ist WLAN an? START zum Beenden.", "Mit A / B wählen",
        "PC = schon auf dem PC", "WLAN", "Daten", "Wartet:", "Deutsch",
        "Bild unten", "an", "aus (Touch geht weiter)", "Spiel am PC beenden",
        "In melonDS läuft noch:", "Weiterspielen", "Spieleliste",
        "Fortschritt seit dem letzten Speichern im Spiel geht verloren.", "Starten", "Zurück",
        "Spiel in melonDS beenden?", "Spiel beenden", "melonDS Link beenden?",
        "melonDS pausiert das Spiel, bis du wieder verbindest.", "Gerade wird ein Spielstand geschrieben!",
        "Beenden", "Spiel wird beendet...", "Ein anderes Spiel starten?",
        "Spielstand-Backups", "Hole die Liste vom PC...", "Keine Backups von diesem Spiel am PC.",
        "A laden   B zurück", "Dieses Backup auf den 3DS laden?", "Der aktuelle Spielstand wird als Backup aufgehoben.",
        "Das Spiel in melonDS wird dafür beendet.", "Backup laden", "Backup wird geladen...",
        "L+R+A bestätigen   B zurück",
        "Screenshot zeigt", "beide Bildschirme", "oberen Bildschirm", "unteren Bildschirm", "beide, eigene Dateien",
        "Akku/WLAN oben anzeigen", "Akku", "WLAN",
        "DS - melonDS", "3DS - Azahar", "Keine Spiele in Azahars Spieleliste.",
        "Für DS-Spiele: melonDS verbinden", "Für 3DS-Spiele: Azahar verbinden",
        "3DS = auch auf diesem 3DS installiert",
        "Bildfehler", "erlauben (flüssiger)", "nie (hält kurz an)",
    },
};
static char g_tr[S_COUNT][80];   /* current language, already in code page 437 */

/* the texts can be changed in sdmc:/3ds/melonDS-Link/Language/<code>.txt
   (key=text, UTF-8); the files are written with all keys when missing */
static const char* const g_strKeys[S_COUNT] = {
    "title", "ip", "connected", "waiting", "controls", "active", "picture", "display",
    "enlarged", "original", "pick_game", "no_games", "list_hint", "list_hint_back", "running",
    "hint_menu", "hint_cstick", "hint_connect", "menu_title", "menu_games", "menu_display", "menu_topoff",
    "menu_shot", "menu_lang", "menu_back", "menu_quit", "menu_hint", "starting", "shot_requested", "not_connected",
    "wifi_fail", "wifi_fail2", "ask_hint", "pc_copied", "xfer_wifi", "xfer_data", "xfer_wait",
    "language_name",
    "menu_bottom", "on", "off", "menu_stop", "resume_title", "continue", "game_list", "progress_lost", "start",
    "back", "stop_title", "stop_confirm", "quit_title", "quit_note", "saving_now", "quit_confirm", "stopping",
    "switch_title",
    "menu_backups", "backups_loading", "backups_none", "backups_hint", "restore_title", "restore_note",
    "restore_note_running", "restore_confirm", "restoring", "confirm_hint",
    "menu_shotmode", "shot_both", "shot_top", "shot_bottom", "shot_separate",
    "menu_statusbar", "battery", "wifi",
    "tab_ds", "tab_3ds", "no_library", "need_melonds", "need_azahar", "on_3ds",
    "menu_glitch", "glitch_allow", "glitch_never",
};
/* the files carry a fingerprint of the built-in texts: when an app update
   changes a text, they are written anew (so the change shows up) */
static u32 langFileVersion(void)
{
    u32 h = 2166136261u;
    for (int l = 0; l < LANG_COUNT; l++)
        for (int i = 0; i < S_COUNT; i++)
            for (const char* c = g_strs[l][i]; *c; c++) h = (h ^ (u8)*c) * 16777619u;
    return h;
}
static char g_custom[LANG_COUNT][S_COUNT][80];   /* from the files ("" = built-in) */
#define LANG_DIR "sdmc:/3ds/melonDS-Link/Language"

static void loadLanguageFiles(void)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/melonDS-Link", 0777);
    mkdir(LANG_DIR, 0777);
    for (int l = 0; l < LANG_COUNT; l++)
    {
        char path[96];
        snprintf(path, sizeof(path), LANG_DIR "/%s.txt", g_langCodes[l]);
        int found = 0;
        u32 version = 0;
        FILE* f = fopen(path, "r");
        if (f)
        {
            char line[256];
            while (fgets(line, sizeof(line), f))
            {
                line[strcspn(line, "\r\n")] = 0;
                char* eq = strchr(line, '=');
                if (line[0] == '#' || !eq) continue;
                *eq = 0;
                if (strcmp(line, "version") == 0) { version = (u32)strtoul(eq + 1, NULL, 16); continue; }
                for (int i = 0; i < S_COUNT; i++)
                    if (strcmp(line, g_strKeys[i]) == 0)
                    {
                        snprintf(g_custom[l][i], sizeof(g_custom[l][i]), "%s", eq + 1);
                        found++;
                        break;
                    }
            }
            fclose(f);
        }
        if (version != langFileVersion())
        {
            /* texts of an older app version: start from the current ones */
            memset(g_custom[l], 0, sizeof(g_custom[l]));
            found = 0;
        }
        if (found >= S_COUNT) continue;
        /* new or incomplete (a newer app version): write all keys, keep the user's texts */
        f = fopen(path, "w");
        if (!f) continue;
        fprintf(f, "# melonDS Link (3DS) - key=text, UTF-8. Restart the app after editing.\n");
        fprintf(f, "version=%lx\n", (unsigned long)langFileVersion());
        for (int i = 0; i < S_COUNT; i++)
            fprintf(f, "%s=%s\n", g_strKeys[i], g_custom[l][i][0] ? g_custom[l][i] : g_strs[l][i]);
        fclose(f);
    }
}

/* the texts say "melonDS"; with another emulator its name is put in */
static void putEmulatorName(char* s, size_t size)
{
    const char* name = emulatorName();
    if (strcmp(name, "melonDS") == 0) return;
    char out[160];
    size_t o = 0;
    for (const char* p = s; *p && o + 1 < sizeof(out);)
    {
        if (strncmp(p, "melonDS", 7) == 0)
        {
            for (const char* n = name; *n && o + 1 < sizeof(out); n++) out[o++] = *n;
            p += 7;
        }
        else out[o++] = *p++;
    }
    out[o] = 0;
    snprintf(s, size, "%s", out);
}

static void applyLanguage(void)
{
    if (g_lang < 0 || g_lang >= LANG_COUNT) g_lang = LANG_EN;
    for (int i = 0; i < S_COUNT; i++)
    {
        utf8ToCp437(g_tr[i], sizeof(g_tr[i]), g_custom[g_lang][i][0] ? g_custom[g_lang][i] : g_strs[g_lang][i]);
        /* texts that name a particular emulator keep their name */
        if (i != S_TAB_DS && i != S_TAB_3DS && i != S_NEED_MELONDS && i != S_NEED_AZAHAR)
            putEmulatorName(g_tr[i], sizeof(g_tr[i]));
    }
}
#define T(id) (g_tr[id])

/* text from melonDS arrives as UTF-8 */
static void setStatusText(const char* s)
{
    char c[128];
    utf8ToCp437(c, sizeof(c), s);
    LightLock_Lock(&g_lock);
    snprintf(g_statusText, sizeof(g_statusText), "%s", c);
    LightLock_Unlock(&g_lock);
}

/* user interface state (main thread draws, input thread routes keys) */
static volatile bool g_linkMenuOpen = false;   /* SELECT+START menu */
static volatile bool g_menuToggle = false;     /* SELECT+START tapped (input -> main) */
static volatile bool g_topOff = false;         /* top screen backlight off */

/* a question from melonDS (LINK_CMD_ASK), answered with A or B */
static volatile bool g_askActive = false;
static u32 g_askId = 0;
#define ASK_LINES 12
static char g_askLines[ASK_LINES][52];
static int g_askLineCount = 0;
static char g_askA[48], g_askB[48];
/* events for melonDS besides the launch (guarded by g_lock) */
static bool g_haveAnswer = false;
static u32 g_answerId = 0, g_answerChoice = 0;
static bool g_haveShot = false;

/* ------------------------------------------------------------------ */
/* socket helpers                                                      */

/* IMPORTANT: all threads share ONE session with the 3DS socket service, and it
   handles requests one after another. A blocking call (poll/recv/send with a
   timeout) therefore stalls every other thread's socket calls - the video
   receiver and the input sender included. So we never block inside the socket
   service: we try, and if it would block, we sleep in our own thread. */
/* set while a big transfer (ROM stream, speed test) runs: the video and
   input threads then leave the shared socket service alone */
static volatile bool g_bulkActive = false;

/* measurements for the transfer statistics (see streamFile) */
static volatile u64 g_statSendSleeps;   /* 0.5 ms sleeps because the WiFi send buffer was full */
static volatile u64 g_statReadTicks, g_statReadBytes;   /* SD card */
static volatile u64 g_statPackTicks, g_statPackBytes;   /* LZ4 */

/* send settings, adjustable from melonDS (LINK_CMD_SETTUNING / speed test) */
/* upload statistics (files written by melonDS, i.e. saves) */
static u64 g_upBytes = 0, g_upRecvTicks = 0, g_upSdTicks = 0;
static volatile u32 g_sendPiece = 0x40000;     /* bytes per send() call */
static volatile u32 g_sendSleepUs = 500;       /* pause when the send buffer is full */
static volatile u32 g_sendBuf = 0x40000;       /* SO_SNDBUF of the data connection */
static volatile bool g_sendBlocking = true;    /* blocking sends during big transfers (best on average) */

/* During a big transfer nobody else uses the socket service (g_bulkActive),
   so the sender may wait inside it like ftpd does: the system wakes it as soon
   as there is room, instead of us sleeping and asking again. */
static void setBlocking(int fd, bool on)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
}

static bool sendAll(int fd, const void* buf, size_t len)
{
    const uint8_t* p = (const uint8_t*)buf;
    while (len > 0)
    {
        size_t piece = len > g_sendPiece ? g_sendPiece : len;
        ssize_t n = send(fd, p, piece, 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (!g_running) return false;
                g_statSendSleeps++;
                svcSleepThread((s64)g_sendSleepUs * 1000);   /* outside the socket service */
                continue;
            }
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool recvAll(int fd, void* buf, size_t len)
{
    uint8_t* p = (uint8_t*)buf;
    while (len > 0)
    {
        if (!g_running) return false;
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { svcSleepThread(1000000); continue; }
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool sendResponse(int fd, const LinkHeader* req, uint16_t status, const void* payload, uint32_t len)
{
    LinkHeader h;
    h.magic = LINK_MAGIC;
    h.cmd = (uint16_t)(req->cmd | LINK_RESP);
    h.status = status;
    h.seq = req->seq;
    h.len = len;
    if (!sendAll(fd, &h, sizeof(h))) return false;
    if (len && !sendAll(fd, payload, len)) return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* path handling: everything is relative to the SD card root           */

static bool makeSdPath(char* out, size_t outSize, const char* in, size_t inLen)
{
    if (inLen == 0 || inLen >= LINK_MAX_PATH) return false;
    char tmp[LINK_MAX_PATH + 1];
    memcpy(tmp, in, inLen);
    tmp[inLen] = '\0';
    if (strlen(tmp) != inLen) return false;       /* embedded NUL */
    if (tmp[0] != '/') return false;
    if (strstr(tmp, "..")) return false;
    int n = snprintf(out, outSize, "sdmc:%s", tmp);
    return n > 0 && (size_t)n < outSize;
}

static void mkdirParents(const char* path)
{
    char tmp[LINK_MAX_PATH + 8];
    snprintf(tmp, sizeof(tmp), "%s", path);
    /* skip "sdmc:/" */
    for (char* p = tmp + 6; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
}

/* ------------------------------------------------------------------ */
/* game list and launch requests                                       */

#define MAX_GAMES 512
static char g_romDir[128] = "/roms/nds";
static char* g_games[MAX_GAMES];
static int g_gameCount = 0;
static volatile bool g_menuOpen = true;      /* game list shown, input not sent to the game */
static volatile bool g_gameStarted = false;
static char g_currentGame[128] = "";
/* dialogs of the 3DS app itself (A / B), shown like the questions from melonDS */
enum { LA_NONE, LA_RESUME, LA_SWITCH, LA_STOP, LA_QUIT, LA_RESTORE };
static volatile int g_localAsk = LA_NONE;
static int g_switchIdx = 0;                      /* LA_SWITCH: the game to start */
static int g_switchTab = 0;                      /* ... in this tab */
static volatile bool g_launchPending = false;    /* picked a game, melonDS has not answered yet */
static volatile bool g_sessionNew = false;       /* melonDS runs a game we did not know about */
static volatile bool g_sessionSame = false;      /* melonDS confirmed the game we play */
static volatile bool g_sessionEnded = false;     /* melonDS runs no game any more */
static volatile bool g_videoOff = false;         /* bottom screen picture off (setting) */
static volatile bool g_haveStopEv = false, g_haveResumeEv = false, g_haveVideoEv = false;
static volatile bool g_saveWriting = false;      /* a save is between WRITE and REPLACE */
static volatile u64 g_saveWriteTime = 0;
static volatile bool g_confirmPress = false;     /* L + R + A pressed (input -> main) */
static int g_shotMode = LINK_SHOT_BOTH;          /* what the screenshot shows (setting) */
static bool g_statusBar = true;                  /* battery/WiFi top right (setting) */
/* state of the console, sent to melonDS with every input packet */
static volatile u8 g_battery = 255, g_wifi = 255;
static volatile bool g_charging = false, g_lidClosed = false;
static volatile bool g_keepAwake = false;        /* melonDS: closing the 3DS keeps the game running */
static volatile u32 g_sockGen = 0;               /* bumped after waking up: sockets are made anew */
static volatile bool g_netPause = false;         /* the socket service is being restarted: hands off */
static volatile bool g_wokeUp = false;           /* woke up, not yet connected to melonDS again */
/* save backups on the PC, listed on the 3DS (guarded by g_lock) */
#define BAK_MAX 14
static volatile bool g_bakOpen = false, g_bakWaiting = false;
static char g_bakRom[LINK_MAX_PATH];             /* ROM path the list is for */
static char g_bakLines[BAK_MAX][48];
static int g_bakCount = 0, g_bakCursor = 0, g_restoreIdx = 0;
static u32 g_bakToken = 0;
static bool g_haveBakReqEv = false, g_haveRestoreEv = false;

/* pending launch for melonDS (guarded by g_lock) */
/* the picture: DS 256x192 (melonDS) or 3DS 320x240 (Azahar), RGB565, row-major */
static u16 g_dsImage[240 * 320];          /* what is shown (row length g_vidW) */
static u16 g_dsBack[240 * 320];           /* tiles are decoded here first */
static volatile int g_vidW = 256, g_vidH = 192;

/* H.264 picture (New 3DS hardware decoder, Azahar): already laid out like the bottom
   framebuffer, two buffers so the screen never gets a half written one */
/* decoded H.264 pictures wait here until they are due: shown at the same steady pace the
   PC made them, not whenever the WiFi happened to bring them (that looked like micro lags) */
#define ROT_SLOTS 4
static u16 g_rotImage[ROT_SLOTS][LINK_H264_WIDTH * LINK_H264_HEIGHT];
static volatile int g_rotIdx = 0;          /* the slot on the screen */
static volatile u32 g_rotWritten = 0;      /* pictures decoded; picture n is in slot n % ROT_SLOTS */
static volatile u64 g_rotDue[ROT_SLOTS];   /* when to show it (osGetTime) */
static volatile u64 g_rotMade[ROT_SLOTS];  /* when it was decoded */
static volatile u32 g_rotSrc[ROT_SLOTS];   /* the PC's time of it (0 = unknown) */
static volatile u32 g_h264Glitched = 0;   /* frames decoded although one before got lost */
static volatile u32 g_loopSlow = 0;
static volatile int g_tosResult = -1;   /* input packets' WiFi priority: 0 = set, else the error; -1 = not tried */
static volatile u32 g_tileRebuilt = 0;
/* input round trip (3DS -> PC -> 3DS): when each input packet went out, and the results */
static volatile u64 g_inSentTick[256];
static volatile u32 g_rttCount = 0, g_rttSumUs = 0, g_rttMaxUs = 0;   /* tile packets rebuilt from parity */
static volatile u32 g_tileShown = 0, g_tileForced = 0;   /* tiles: pictures shown, by the 250 ms safety net */
static u64 g_vblankTime = 0;   /* when the last screen refresh began (main loop) */
/* while it moves: how many screen refreshes each picture stood (1, 2, 3, 4+) */
static volatile u32 g_dispGaps[4];
static volatile u32 g_rotLate = 0, g_rotStutter = 0, g_rotWaitSum = 0, g_rotWaitN = 0;
static volatile bool g_rotMode = false;   /* the picture comes as H.264 */
static bool g_mvdReady = false;
static u8* g_mvdIn = NULL;                /* linear memory: one NAL unit */
static u8* g_mvdOut = NULL;               /* linear memory: the decoded picture */
static MVDSTD_Config g_mvdConfig;
static volatile bool g_h264Reset = false;   /* server thread -> video thread */
static volatile u32 g_vidOverruns = 0, g_vidRcvBuf = 0;
/* freezes: from a lost frame until the next key frame was shown */
static volatile u32 g_stallCount = 0, g_stallTotalMs = 0, g_stallMaxMs = 0;
static u64 g_stallSince = 0;   /* picture packets dropped: decoder behind */
static volatile u32 g_h264Shown = 0, g_h264Dropped = 0, g_h264Errors = 0, g_h264Repaired = 0;

/* the hardware decoder (New 3DS only); set up once, when the PC asks */
static bool mvdSetup(void)
{
    if (g_mvdReady) return true;
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    if (!isNew) return false;
    if (!g_mvdIn) g_mvdIn = (u8*)linearMemAlign(0x40000, 0x40);
    if (!g_mvdOut) g_mvdOut = (u8*)linearMemAlign(LINK_H264_WIDTH * LINK_H264_HEIGHT * 2, 0x40);
    if (!g_mvdIn || !g_mvdOut) return false;
    if (R_FAILED(mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565,
                            MVD_DEFAULT_WORKBUF_SIZE, NULL)))
        return false;
    mvdstdGenerateDefaultConfig(&g_mvdConfig, LINK_H264_WIDTH, LINK_H264_HEIGHT, LINK_H264_WIDTH,
                                LINK_H264_HEIGHT, NULL, (u32*)g_mvdOut, (u32*)g_mvdOut);
    /* the colour conversion stays libctru's default: BT.601 TV range, 1.164 (Y - 16) +
       1.596 (V - 128) ... - the PC encodes for exactly that (measured, see VIDEOPROBE) */
    g_mvdReady = true;
    return true;
}

/* picture size for the emulator on the PC; the old picture is dropped */
static void setVideoGeometry(u32 client)
{
    int w = client == LINK_CLIENT_AZAHAR ? 320 : 256;
    int h = client == LINK_CLIENT_AZAHAR ? 240 : 192;
    if (w == g_vidW && h == g_vidH) return;
    g_vidW = w;
    g_vidH = h;
    memset(g_dsBack, 0, sizeof(g_dsBack));
    memset(g_dsImage, 0, sizeof(g_dsImage));
}
static volatile u64 g_lastVideoTime = 0;

static bool g_haveEvent = false;
static LinkEvent g_event;
static char g_eventRom[LINK_MAX_PATH];
static char g_eventSave[LINK_MAX_PATH];

/* favourites: one file name per line in FAVS_PATH */
#define FAVS_PATH "sdmc:/3ds/melonDS-Link-favoriten.txt"
#define MAX_FAVS 128
static char* g_favs[MAX_FAVS];
static int g_favCount = 0;
static char g_lastGame[256] = "";   /* file name of the last launched game */

/* games that are completely on the PC already (sent by melonDS, guarded by g_lock) */
#define MAX_ONPC 512
static char* g_onPcBuf = NULL;
static char* g_onPc[MAX_ONPC];
static int g_onPcCount = 0;
static volatile u32 g_onPcVersion = 0;   /* changes whenever the set changes */

/* 3DS games: Azahar's game list (LINK_CMD_LIBRARY). The server thread only hands the
   text over (g_libPending); the main loop parses, marks and sorts it. */
#define MAX_LIB 512
typedef struct { const char* path; const char* name; u64 tid; bool on3ds; } LibGame;
static char* g_libPending = NULL;    /* new list from Azahar (g_lock) */
static char* g_libBuf = NULL;        /* the text g_lib points into (main thread) */
static LibGame g_lib[MAX_LIB];
static int g_libCount = 0;
static int g_tab = 0;                /* game list tab: 0 DS (melonDS), 1 3DS (Azahar) */
static bool g_haveLibReqEv = false;  /* Y on the 3DS tab: Azahar sends its list anew */

static bool isOnPcLocked(const char* name)
{
    for (int i = 0; i < g_onPcCount; i++)
        if (strcasecmp(g_onPc[i], name) == 0) return true;
    return false;
}

static bool isFav(const char* name)
{
    for (int i = 0; i < g_favCount; i++)
        if (strcasecmp(g_favs[i], name) == 0) return true;
    return false;
}

static void loadFavs(void)
{
    FILE* f = fopen(FAVS_PATH, "r");
    if (!f) return;
    char line[300];
    while (g_favCount < MAX_FAVS && fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] && !isFav(line)) g_favs[g_favCount++] = strdup(line);
    }
    fclose(f);
}

static void saveFavs(void)
{
    FILE* f = fopen(FAVS_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_favCount; i++) fprintf(f, "%s\n", g_favs[i]);
    fclose(f);
}

static void toggleFav(const char* name)
{
    for (int i = 0; i < g_favCount; i++)
    {
        if (strcasecmp(g_favs[i], name) == 0)
        {
            free(g_favs[i]);
            g_favs[i] = g_favs[--g_favCount];
            saveFavs();
            return;
        }
    }
    if (g_favCount < MAX_FAVS) g_favs[g_favCount++] = strdup(name);
    saveFavs();
}

/* favourites first, then alphabetical */
static int cmpNoCase(const void* a, const void* b)
{
    const char* x = *(char* const*)a;
    const char* y = *(char* const*)b;
    bool fx = isFav(x), fy = isFav(y);
    if (fx != fy) return fx ? -1 : 1;
    return strcasecmp(x, y);
}

static void sortGames(void)
{
    qsort(g_games, g_gameCount, sizeof(char*), cmpNoCase);
}

static int findGame(const char* name)
{
    for (int i = 0; i < g_gameCount; i++)
        if (strcasecmp(g_games[i], name) == 0) return i;
    return -1;
}

static bool hasNdsExt(const char* name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".nds") == 0;
}

static void scanGames(void)
{
    for (int i = 0; i < g_gameCount; i++) free(g_games[i]);
    g_gameCount = 0;
    char dir[160];
    snprintf(dir, sizeof(dir), "sdmc:%s", g_romDir);
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && g_gameCount < MAX_GAMES)
    {
        if (e->d_type == DT_DIR || !hasNdsExt(e->d_name)) continue;
        g_games[g_gameCount++] = strdup(e->d_name);
    }
    closedir(d);
    sortGames();
}

static bool sdExists(const char* sdPath)
{
    char full[LINK_MAX_PATH + 8];
    snprintf(full, sizeof(full), "sdmc:%s", sdPath);
    struct stat st;
    return stat(full, &st) == 0;
}

/* the user picked a game: prepare the event melonDS will fetch */
static void requestLaunch(int idx)
{
    if (idx < 0 || idx >= g_gameCount) return;
    const char* name = g_games[idx];

    char rom[LINK_MAX_PATH], base[LINK_MAX_PATH], save[LINK_MAX_PATH], alt[LINK_MAX_PATH];
    snprintf(rom, sizeof(rom), "%.100s/%.150s", g_romDir, name);
    snprintf(base, sizeof(base), "%s", name);
    base[strlen(base) - 4] = '\0';                          /* drop ".nds" */
    /* TWiLight Menu++ keeps saves in "saves/" next to the ROMs (default)
       or right next to the ROM */
    snprintf(save, sizeof(save), "%.100s/saves/%.140s.sav", g_romDir, base);
    snprintf(alt, sizeof(alt), "%.100s/%.140s.sav", g_romDir, base);
    if (!sdExists(save) && sdExists(alt)) snprintf(save, sizeof(save), "%s", alt);

    char full[LINK_MAX_PATH + 8];
    snprintf(full, sizeof(full), "sdmc:%s", rom);
    struct stat st;
    u64 size = (stat(full, &st) == 0) ? (u64)st.st_size : 0;

    LightLock_Lock(&g_lock);
    memset(&g_event, 0, sizeof(g_event));
    g_event.type = LINK_EVENT_LAUNCH;
    g_event.romSize = size;
    snprintf(g_eventRom, sizeof(g_eventRom), "%s", rom);
    snprintf(g_eventSave, sizeof(g_eventSave), "%s", save);
    g_event.romPathLen = (uint16_t)strlen(g_eventRom);
    g_event.savePathLen = (uint16_t)strlen(g_eventSave);
    g_haveEvent = true;
    snprintf(g_currentGame, sizeof(g_currentGame), "%.120s", base);
    snprintf(g_lastGame, sizeof(g_lastGame), "%s", name);
    snprintf(g_statusText, sizeof(g_statusText), "%s", T(S_STARTING));
    LightLock_Unlock(&g_lock);

    g_gameStarted = true;
    g_launchPending = true;
    g_menuOpen = false;
}

/* the favourites key of a 3DS game */
static void libFavKey(const LibGame* g, char* out, size_t size)
{
    if (g->tid) snprintf(out, size, "3DS:%016llX", (unsigned long long)g->tid);
    else snprintf(out, size, "3DS:%.200s", g->name);
}

static bool libIsFav(const LibGame* g)
{
    char k[220];
    libFavKey(g, k, sizeof(k));
    return isFav(k);
}

static int cmpLib(const void* a, const void* b)
{
    const LibGame* x = (const LibGame*)a;
    const LibGame* y = (const LibGame*)b;
    bool fx = libIsFav(x), fy = libIsFav(y);
    if (fx != fy) return fx ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static int findLib(const char* path)
{
    for (int i = 0; i < g_libCount; i++)
        if (strcmp(g_lib[i].path, path) == 0) return i;
    return -1;
}

/* a new list from Azahar: lines "title id (hex) TAB path TAB name" */
static void takeLibrary(char* text)
{
    free(g_libBuf);
    g_libBuf = text;
    g_libCount = 0;
    for (char* p = text; p && *p && g_libCount < MAX_LIB; )
    {
        char* nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        char* t1 = strchr(p, '\t');
        char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2)
        {
            *t1 = *t2 = '\0';
            LibGame* g = &g_lib[g_libCount++];
            g->tid = strtoull(p, NULL, 16);
            g->path = t1 + 1;
            g->name = t2[1] ? t2 + 1 : t1 + 1;
            g->on3ds = false;
        }
        p = nl ? nl + 1 : NULL;
    }
    /* which of them are installed here (their save can come from this 3DS) */
    if (g_libCount && R_SUCCEEDED(amInit()))
    {
        const FS_MediaType media[2] = { MEDIATYPE_SD, MEDIATYPE_GAME_CARD };
        static u64 ids[1024];
        for (int m = 0; m < 2; m++)
        {
            u32 count = 0, read = 0;
            if (R_FAILED(AM_GetTitleCount(media[m], &count))) continue;
            if (count > 1024) count = 1024;
            if (count && R_FAILED(AM_GetTitleList(&read, media[m], count, ids))) read = 0;
            for (u32 j = 0; j < read; j++)
                for (int i = 0; i < g_libCount; i++)
                    if (g_lib[i].tid == ids[j]) g_lib[i].on3ds = true;
        }
        amExit();
    }
    qsort(g_lib, g_libCount, sizeof(LibGame), cmpLib);
}

/* the user picked a 3DS game: Azahar starts it (path from its own game list) */
static void requestLaunchLib(int idx)
{
    if (idx < 0 || idx >= g_libCount) return;
    const LibGame* g = &g_lib[idx];
    LightLock_Lock(&g_lock);
    memset(&g_event, 0, sizeof(g_event));
    g_event.type = LINK_EVENT_LAUNCH;
    g_event.romSize = g->tid;
    snprintf(g_eventRom, sizeof(g_eventRom), "%s", g->path);
    g_eventSave[0] = 0;
    g_event.romPathLen = (uint16_t)strlen(g_eventRom);
    g_event.savePathLen = 0;
    g_haveEvent = true;
    snprintf(g_currentGame, sizeof(g_currentGame), "%.120s", g->name);
    snprintf(g_statusText, sizeof(g_statusText), "%s", T(S_STARTING));
    LightLock_Unlock(&g_lock);
    g_gameStarted = true;
    g_launchPending = true;
    g_menuOpen = false;
}

/* ------------------------------------------------------------------ */
/* command handlers                                                    */

static uint8_t* g_payload;   /* LINK_MAX_PAYLOAD + header space */
static uint8_t* g_outbuf;    /* LINK_MAX_PAYLOAD */

/* ------------------------------------------------------------------ */
/* streaming big files: a reader thread fills buffers from the SD card  */
/* while the server thread sends the previous one                       */

#define STREAM_BUFS 3
#define STREAM_CHUNK LINK_STREAM_BLOCK_SIZE
/* both buffers keep STREAM_HDR bytes free in front of the data, so the
   block header and the data go out in a single send */
#define STREAM_HDR 16
static u8* g_streamBuf[STREAM_BUFS];               /* raw data from the SD card */
static u8* g_streamEnc[STREAM_BUFS];               /* LZ4 output */
static LinkStreamBlock g_streamHdr[STREAM_BUFS];   /* how block i is sent (compressed mode) */
static volatile u32 g_streamLen[STREAM_BUFS];
static volatile int g_streamState[STREAM_BUFS];    /* 0 empty, 1 full, 2 end/error */
static FILE* g_streamFile;
static volatile u64 g_streamLeft;
static volatile bool g_streamStop;
static volatile bool g_streamCompress;   /* LZ4 on (switched off if it can't keep up) */
static volatile bool g_streamPack;       /* compressed protocol (fill blocks etc.) */
static u64 g_streamPos;                  /* file offset of the next block */
static u64 g_skipFrom = ~0ull;           /* blocks from here on: only spot-checked */
static int g_skipFill = -1;              /* the padding byte (-1 = not seen yet) */
static volatile u64 g_statSkipped;       /* bytes not read thanks to that */
static u8 g_sample[2][0x1000] __attribute__((aligned(0x40)));
static volatile u32 g_senderStarved;     /* sender had to wait for the reader */
static LZ4_stream_t g_lz4State;          /* ~16 KB: must not live on a thread stack */

/* transfer info for the top screen (guarded by g_lock) */
static char g_xferInfo[64] = "";
static char g_xferInfo2[64] = "";

/* one value repeated over the whole block (cartridge padding)? */
static bool isUniform(const u8* p, u32 n)
{
    const u32* w = (const u32*)p;
    u32 first = w[0];
    if ((first & 0xFF) * 0x01010101u != first) return false;
    for (u32 i = 1; i < n / 4; i++) if (w[i] != first) return false;
    for (u32 i = n & ~3u; i < n; i++) if (p[i] != p[0]) return false;
    return true;
}

static void streamReader(void* arg)
{
    (void)arg;
    int i = 0;
    while (!g_streamStop)
    {
        if (g_streamState[i] != 0) { svcSleepThread(200000); continue; }   /* wait for the sender */
        if (g_streamLeft == 0) { g_streamState[i] = 2; break; }
        u32 want = g_streamLeft > STREAM_CHUNK ? STREAM_CHUNK : (u32)g_streamLeft;

        /* behind the game data: look at the first and last 4 KB only */
        if (g_streamPack && g_streamPos >= g_skipFrom && want >= 0x2000)
        {
            u64 r0 = svcGetSystemTick();
            bool pad = fseeko(g_streamFile, (off_t)g_streamPos, SEEK_SET) == 0 &&
                       fread(g_sample[0], 1, 0x1000, g_streamFile) == 0x1000 &&
                       fseeko(g_streamFile, (off_t)(g_streamPos + want - 0x1000), SEEK_SET) == 0 &&
                       fread(g_sample[1], 1, 0x1000, g_streamFile) == 0x1000 &&
                       isUniform(g_sample[0], 0x1000) && isUniform(g_sample[1], 0x1000) &&
                       g_sample[0][0] == g_sample[1][0] &&
                       (g_skipFill < 0 || g_skipFill == g_sample[0][0]);
            g_statReadTicks += svcGetSystemTick() - r0;
            if (pad)
            {
                g_skipFill = g_sample[0][0];
                LinkStreamBlock* b = &g_streamHdr[i];
                memset(b, 0, sizeof(*b));
                b->rawLen = want;
                b->mode = LINK_BLOCK_FILL;
                b->fill = (u8)g_skipFill;
                g_streamLen[i] = want;
                g_statSkipped += want;
                g_streamPos += want;
                g_streamLeft -= want;
                if (fseeko(g_streamFile, (off_t)g_streamPos, SEEK_SET) != 0) { g_streamState[i] = 2; g_streamLen[i] = 0; break; }
                g_streamState[i] = 1;
                i = (i + 1) % STREAM_BUFS;
                continue;
            }
            /* something else is there: read everything from now on */
            g_skipFrom = ~0ull;
            if (fseeko(g_streamFile, (off_t)g_streamPos, SEEK_SET) != 0) { g_streamState[i] = 2; g_streamLen[i] = 0; break; }
        }

        u64 r0 = svcGetSystemTick();
        size_t got = fread(g_streamBuf[i], 1, want, g_streamFile);
        g_statReadTicks += svcGetSystemTick() - r0;
        g_statReadBytes += got;
        if (got != want) { g_streamState[i] = 2; g_streamLen[i] = 0; break; }
        g_streamLen[i] = want;
        g_streamPos += want;

        if (g_streamPack)
        {
            LinkStreamBlock* b = &g_streamHdr[i];
            memset(b, 0, sizeof(*b));
            b->rawLen = want;
            if (isUniform(g_streamBuf[i], want))
            {
                b->mode = LINK_BLOCK_FILL;
                b->fill = g_streamBuf[i][0];
            }
            else if (g_streamCompress)
            {
                u64 p0 = svcGetSystemTick();
                int enc = LZ4_compress_fast_extState(&g_lz4State, (const char*)g_streamBuf[i], (char*)g_streamEnc[i],
                                                     (int)want, LZ4_compressBound(STREAM_CHUNK), 1);
                g_statPackTicks += svcGetSystemTick() - p0;
                g_statPackBytes += want;
                if (enc > 0 && (u32)enc < want - want / 32)
                {
                    b->mode = LINK_BLOCK_LZ4;
                    b->encLen = (u32)enc;
                }
                else
                {
                    b->mode = LINK_BLOCK_RAW;   /* already compressed data: send as is */
                    b->encLen = want;
                }
            }
            else
            {
                b->mode = LINK_BLOCK_RAW;       /* LZ4 switched off: the CPU was the bottleneck */
                b->encLen = want;
            }
        }

        g_streamLeft -= want;
        g_streamState[i] = 1;
        i = (i + 1) % STREAM_BUFS;
    }
}

/* end of the real data of a DS ROM according to its header (0 = unknown) */
static u64 ndsDataEnd(FILE* f, u64 size)
{
    u8 hdr[0x220];
    if (fseeko(f, 0, SEEK_SET) != 0 || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return 0;
    u32 ntr, dsi = 0;
    memcpy(&ntr, hdr + 0x80, 4);           /* used ROM size (DS part) */
    if (hdr[0x12] & 0x02) memcpy(&dsi, hdr + 0x210, 4);   /* DSi-enhanced: total size */
    u64 end = (u64)ntr + 0x88;             /* + the signature that may follow */
    if ((u64)dsi > end) end = dsi;
    if (ntr < 0x4000 || end > size) return 0;   /* not a sensible header */
    return end + 0x10000;                  /* a little margin */
}

static bool streamFile(int fd, const LinkHeader* h, const char* path, u64 offset, u64 length, bool compress, bool skipPadding)
{
    FILE* f = fopen(path, "rb");
    if (!f) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
    struct stat st;
    u64 size = (fstat(fileno(f), &st) == 0) ? (u64)st.st_size : 0;
    if (offset > size) offset = size;
    if (length > size - offset) length = size - offset;
    g_skipFrom = ~0ull;
    g_skipFill = -1;
    if (compress && skipPadding)
    {
        u64 end = ndsDataEnd(f, size);
        if (end) g_skipFrom = end;
    }
    g_streamPos = offset;
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) { fclose(f); return sendResponse(fd, h, LINK_ERR_IO, NULL, 0); }
    setvbuf(f, NULL, _IONBF, 0);   /* we read big blocks ourselves */

    if (!g_streamBuf[0])
        for (int i = 0; i < STREAM_BUFS; i++)
        {
            u8* m = (u8*)memalign(0x1000, STREAM_CHUNK + 0x1000);
            g_streamBuf[i] = m ? m + 0x1000 : NULL;   /* room for the header in front */
        }
    if (compress && !g_streamEnc[0])
        for (int i = 0; i < STREAM_BUFS; i++)
        {
            u8* m = (u8*)memalign(0x1000, LZ4_compressBound(STREAM_CHUNK) + 0x1000);
            g_streamEnc[i] = m ? m + 0x1000 : NULL;
        }
    if (!g_streamBuf[STREAM_BUFS - 1] || (compress && !g_streamEnc[STREAM_BUFS - 1]))
    {
        fclose(f);
        return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
    }
    for (int i = 0; i < STREAM_BUFS; i++) { g_streamState[i] = 0; g_streamLen[i] = 0; }
    g_streamFile = f;
    g_streamLeft = length;
    g_streamStop = false;
    g_streamPack = compress;
    g_streamCompress = compress;
    g_senderStarved = 0;
    g_statSendSleeps = 0;
    g_statReadTicks = g_statReadBytes = 0;
    g_statPackTicks = g_statPackBytes = 0;
    g_statSkipped = 0;

    u64 len64 = length;
    if (!sendResponse(fd, h, LINK_OK, &len64, sizeof(len64))) { fclose(f); return false; }
    g_bulkActive = true;
    bool blocking = g_sendBlocking;
    if (blocking) setBlocking(fd, true);

    /* on a New 3DS the reading/compressing runs on its own CPU core */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    Thread rd = NULL;
    if (isNew) rd = threadCreate(streamReader, NULL, 0x10000, prio, 2, false);
    if (!rd) rd = threadCreate(streamReader, NULL, 0x10000, prio, -2, false);

    bool ok = true;
    u64 sent = 0, wire = 0;
    int i = 0;
    u64 t0 = osGetTime();
    u64 waitTicks = 0, blocks = 0, waitTotal = 0, tick0 = svcGetSystemTick();
    while (sent < length && ok && g_running)
    {
        if (g_streamState[i] == 0)
        {
            u64 w0 = svcGetSystemTick();
            svcSleepThread(200000);
            waitTicks += svcGetSystemTick() - w0;
            waitTotal += svcGetSystemTick() - w0;
            continue;
        }
        if (g_streamState[i] == 2) { ok = false; break; }           /* SD read error */
        if (compress)
        {
            LinkStreamBlock* b = &g_streamHdr[i];
            u8* data = b->mode == LINK_BLOCK_LZ4 ? g_streamEnc[i] : g_streamBuf[i];
            u32 dataLen = b->mode == LINK_BLOCK_FILL ? 0 : b->encLen;
            memcpy(data - sizeof(*b), b, sizeof(*b));      /* header right before the data */
            ok = sendAll(fd, data - sizeof(*b), sizeof(*b) + dataLen);
        }
        else ok = sendAll(fd, g_streamBuf[i], g_streamLen[i]);
        sent += g_streamLen[i];
        wire += compress ? sizeof(LinkStreamBlock) + (g_streamHdr[i].mode == LINK_BLOCK_FILL ? 0 : g_streamHdr[i].encLen)
                         : g_streamLen[i];
        g_streamState[i] = 0;
        i = (i + 1) % STREAM_BUFS;
        blocks++;

        /* every 8 blocks: switch LZ4 off only if packing itself is too slow,
           i.e. LZ4 runs at less than 3x the WiFi rate. (Waiting for the reader
           alone is no reason: in padding areas the SD card is the limit, and
           LZ4 does not run there at all.) */
        if (compress && g_streamCompress && blocks % 8 == 0)
        {
            u64 now = osGetTime();
            u64 ms = now - t0;
            double wifiBps = ms ? wire * 1000.0 / ms : 0;
            double lz4Bps = g_statPackTicks ? g_statPackBytes / (g_statPackTicks / (double)SYSCLOCK_ARM11) : 1e12;
            if (wifiBps > 0 && g_statPackBytes > 4 * STREAM_CHUNK && lz4Bps < 3 * wifiBps) g_streamCompress = false;
            waitTicks = 0;
        }
        u64 secs1000 = osGetTime() - t0;
        if (secs1000 > 0 && blocks % 4 == 0)
        {
            LightLock_Lock(&g_lock);
            const bool de = g_lang == LANG_DE;
            snprintf(g_xferInfo, sizeof(g_xferInfo), "%s %lu KB/s, %s %lu KB/s, LZ4 %s", T(S_XFER_WIFI),
                     (unsigned long)(wire / secs1000), T(S_XFER_DATA), (unsigned long)(sent / secs1000),
                     !compress ? "-" : g_streamCompress ? (de ? "an" : "on") : (de ? "aus (CPU zu langsam)" : "off (CPU too slow)"));
            /* where the time goes: waiting for the reader / for the WiFi buffer,
               and how fast the SD card and LZ4 are on their own (MB/s) */
            u64 el = svcGetSystemTick() - tick0;
            unsigned wPct = el ? (unsigned)(waitTotal * 100 / el) : 0;
            unsigned fPct = secs1000 ? (unsigned)(g_statSendSleeps * g_sendSleepUs / 10 / secs1000) : 0;
            double sd = g_statReadTicks ? g_statReadBytes / (g_statReadTicks / (double)SYSCLOCK_ARM11) / 1048576.0 : 0;
            double lz = g_statPackTicks ? g_statPackBytes / (g_statPackTicks / (double)SYSCLOCK_ARM11) / 1048576.0 : 0;
            snprintf(g_xferInfo2, sizeof(g_xferInfo2), "%s %s %u%% %s %u%%  SD %.1f LZ4 %.1f", T(S_XFER_WAIT),
                     de ? "Leser" : "reader", wPct > 100 ? 100 : wPct, T(S_XFER_WIFI), fPct > 100 ? 100 : fPct, sd, lz);
            LightLock_Unlock(&g_lock);
        }
    }
    g_streamStop = true;
    if (blocking) setBlocking(fd, false);
    g_bulkActive = false;
    threadJoin(rd, U64_MAX);
    threadFree(rd);
    fclose(f);
    /* on an error mid-stream the framing is broken: closing the connection
       (returning false) makes the PC retry from where it got to */
    return ok && sent == length;
}

/* system archives (LINK_CMD_ARC*): the path inside the archive, as UTF-16 */
static u16 g_arcPath[LINK_MAX_PATH + 2];

static bool arcPath(const uint8_t* in, size_t len)
{
    if (len >= LINK_MAX_PATH) return false;
    u8 tmp[LINK_MAX_PATH + 2];
    size_t o = 0;
    if (len == 0 || in[0] != '/') tmp[o++] = '/';
    memcpy(tmp + o, in, len);
    tmp[o + len] = 0;
    memset(g_arcPath, 0, sizeof(g_arcPath));
    ssize_t n = utf8_to_utf16(g_arcPath, tmp, LINK_MAX_PATH);
    return n > 0;
}

static Result g_arcResult;   /* why the last archive call failed (sent with the error) */
static u8* g_arcRaw;          /* LINK_ARCZ_MAX_CHUNK bytes for reading archive files */

static u8* arcRawBuffer(void)
{
    if (!g_arcRaw) g_arcRaw = (u8*)memalign(0x40, LINK_ARCZ_MAX_CHUNK);
    return g_arcRaw;
}

/* size and FNV-1a 64 of the file `name` in the folder g_arcPath */
static bool hashArcFile(FS_Archive arch, const u8* name, u64* outSize, u64* outHash)
{
    u8* raw = arcRawBuffer();
    if (!raw) return false;
    static u16 path[LINK_MAX_PATH + 300];
    size_t n = 0;
    while (g_arcPath[n] && n < LINK_MAX_PATH) { path[n] = g_arcPath[n]; n++; }
    if (n == 0 || path[n - 1] != '/') path[n++] = '/';
    ssize_t m = utf8_to_utf16(path + n, name, 290);
    if (m <= 0) return false;
    path[n + (size_t)m] = 0;
    Handle file;
    if (R_FAILED(FSUSER_OpenFile(&file, arch, fsMakePath(PATH_UTF16, path), FS_OPEN_READ, 0))) return false;
    u64 size = 0, hash = LINK_FNV64_INIT;
    bool ok = R_SUCCEEDED(FSFILE_GetSize(file, &size));
    for (u64 pos = 0; ok && pos < size;)
    {
        u32 want = size - pos > LINK_ARCZ_MAX_CHUNK ? LINK_ARCZ_MAX_CHUNK : (u32)(size - pos);
        u32 got = 0;
        if (R_FAILED(FSFILE_Read(file, &got, pos, raw, want)) || got != want) { ok = false; break; }
        for (u32 i = 0; i < got; i++) hash = (hash ^ raw[i]) * LINK_FNV64_PRIME;
        pos += got;
    }
    FSFILE_Close(file);
    *outSize = size;
    *outHash = hash;
    return ok;
}

/* The archive stays open between requests: opening a save archive takes the 3DS
   a while, and copying a save asks for every file twice (hash, data). */
static bool g_arcOpen = false;
static FS_Archive g_arcHandle;
static LinkArchiveReq g_arcKey;

/* LINK_CMD_SYSROMFS: the RomFS being read stays open until it is done */
static Handle g_romfsFile;
static bool g_romfsOpen = false;
static u64 g_romfsTid = 0;

static void closeLinkArchive(void)
{
    if (g_arcOpen) FSUSER_CloseArchive(g_arcHandle);
    g_arcOpen = false;
    if (g_romfsOpen) FSFILE_Close(g_romfsFile);
    g_romfsOpen = false;
}

static bool openLinkArchive(const LinkArchiveReq* req, FS_Archive* arch)
{
    if (g_arcOpen && g_arcKey.archive == req->archive && g_arcKey.mediaType == req->mediaType &&
        g_arcKey.lowId == req->lowId && g_arcKey.highId == req->highId)
    {
        *arch = g_arcHandle;
        return true;
    }
    closeLinkArchive();
    u32 lowpath[3] = { req->mediaType, req->lowId, req->highId };
    if (req->mediaType == LINK_MEDIA_NOPATH)   /* e.g. CTR-NAND: the archive has no path */
        g_arcResult = FSUSER_OpenArchive(arch, (FS_ArchiveID)req->archive, (FS_Path){ PATH_EMPTY, 1, "" });
    else
        g_arcResult = FSUSER_OpenArchive(arch, (FS_ArchiveID)req->archive,
                                         (FS_Path){ PATH_BINARY, sizeof(lowpath), lowpath });
    if (R_FAILED(g_arcResult)) return false;
    g_arcOpen = true;
    g_arcHandle = *arch;
    g_arcKey = *req;
    return true;
}

static bool handleCommand(int fd, const LinkHeader* h, uint8_t* payload)
{
    char path[LINK_MAX_PATH + 8];

    switch (h->cmd)
    {
    case LINK_CMD_HELLO:
    {
        g_h264Reset = true;   /* a new emulator session: its frame numbers start anew */
        LinkHelloResp r;
        memset(&r, 0, sizeof(r));
        r.version = LINK_VERSION;
        r.udpPort = LINK_UDP_PORT;
        bool isNew = false;
        APT_CheckNew3DS(&isNew);
        r.isNew3DS = isNew ? 1 : 0;
        snprintf(r.name, sizeof(r.name), "%s", isNew ? "New 3DS" : "3DS");
        snprintf(r.lang, sizeof(r.lang), "%s", g_langCodes[g_lang]);
        r.flags = g_videoOff ? LINK_HELLO_VIDEO_OFF : 0;
        uint32_t ver = (h->len >= 4) ? *(uint32_t*)payload : 0;
        if (ver == LINK_VERSION)
        {
            uint32_t client = (h->len >= 8) ? ((uint32_t*)payload)[1] : LINK_CLIENT_MELONDS;
            if (client != LINK_CLIENT_AZAHAR) client = LINK_CLIENT_MELONDS;
            setVideoGeometry(client);
            if (client != g_client)
            {
                /* another emulator: the game of the old one does not run there */
                LightLock_Lock(&g_lock);
                g_gameStarted = false;
                g_launchPending = false;
                g_currentGame[0] = 0;
                LightLock_Unlock(&g_lock);
                g_client = client;
                g_lastVideoTime = 0;
                g_clientChanged = true;
            }
        }
        return sendResponse(fd, h, ver == LINK_VERSION ? LINK_OK : LINK_ERR_VERSION, &r, sizeof(r));
    }

    case LINK_CMD_PING:
        return sendResponse(fd, h, LINK_OK, NULL, 0);

    case LINK_CMD_STATUSTEXT:
    {
        char txt[128];
        size_t n = h->len < sizeof(txt) - 1 ? h->len : sizeof(txt) - 1;
        memcpy(txt, payload, n);
        txt[n] = '\0';
        setStatusText(txt);
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    case LINK_CMD_STAT:
    {
        if (!makeSdPath(path, sizeof(path), (const char*)payload, h->len))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        struct stat st;
        if (stat(path, &st) != 0)
            return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        LinkStatResp r;
        memset(&r, 0, sizeof(r));
        r.size = (uint64_t)st.st_size;
        r.isDir = S_ISDIR(st.st_mode) ? 1 : 0;
        return sendResponse(fd, h, LINK_OK, &r, sizeof(r));
    }

    case LINK_CMD_READ:
    {
        if (h->len <= sizeof(LinkReadReq))
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        LinkReadReq req;
        memcpy(&req, payload, sizeof(req));
        if (req.length > LINK_MAX_PAYLOAD)
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        if (!makeSdPath(path, sizeof(path), (const char*)payload + sizeof(req), h->len - sizeof(req)))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        FILE* f = fopen(path, "rb");
        if (!f) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        size_t got = 0;
        if (fseeko(f, (off_t)req.offset, SEEK_SET) == 0)
            got = fread(g_outbuf, 1, req.length, f);
        fclose(f);
        return sendResponse(fd, h, LINK_OK, g_outbuf, (uint32_t)got);
    }

    case LINK_CMD_WRITE:
    {
        if (h->len < sizeof(LinkWriteReq))
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        LinkWriteReq req;
        memcpy(&req, payload, sizeof(req));
        if (sizeof(req) + req.pathLen > h->len)
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        if (!makeSdPath(path, sizeof(path), (const char*)payload + sizeof(req), req.pathLen))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        const uint8_t* data = payload + sizeof(req) + req.pathLen;
        size_t dataLen = h->len - sizeof(req) - req.pathLen;
        {
            /* melonDS writes saves into "<save>.tmp" and then swaps it in */
            size_t pl = strlen(path);
            if (pl > 4 && strcmp(path + pl - 4, ".tmp") == 0) { g_saveWriting = true; g_saveWriteTime = osGetTime(); }
        }
        /* the data sits right behind the path, at an odd address: the 3DS file
           system writes such buffers very slowly (512 KB took 10-18 s, the same
           from an aligned buffer ~0.2 s). Copy it to an aligned buffer first. */
        static u8* aligned = NULL;
        if (!aligned) aligned = (u8*)memalign(0x1000, LINK_MAX_PAYLOAD + LINK_MAX_PATH + 64);
        if (aligned && dataLen <= LINK_MAX_PAYLOAD + LINK_MAX_PATH + 64)
        {
            memcpy(aligned, data, dataLen);
            data = aligned;
        }

        u64 w0 = svcGetSystemTick();
        FILE* f = fopen(path, "r+b");
        if (!f && (req.flags & LINK_WRITE_CREATE))
        {
            mkdirParents(path);
            f = fopen(path, "w+b");
        }
        if (!f) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        /* no stdio buffer: otherwise the data goes to the SD card in ~1 KB
           pieces, each committed on its own (512 KB took ~10 s) */
        setvbuf(f, NULL, _IONBF, 0);
        bool ok = fseeko(f, (off_t)req.offset, SEEK_SET) == 0 &&
                  fwrite(data, 1, dataLen, f) == dataLen;
        ok = (fflush(f) == 0) && ok;
        fclose(f);
        g_upSdTicks += svcGetSystemTick() - w0;
        return sendResponse(fd, h, ok ? LINK_OK : LINK_ERR_IO, NULL, 0);
    }

    case LINK_CMD_SETSIZE:
    {
        if (h->len <= sizeof(LinkSetSizeReq))
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        LinkSetSizeReq req;
        memcpy(&req, payload, sizeof(req));
        if (!makeSdPath(path, sizeof(path), (const char*)payload + sizeof(req), h->len - sizeof(req)))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        bool ok = truncate(path, (off_t)req.size) == 0;
        return sendResponse(fd, h, ok ? LINK_OK : LINK_ERR_IO, NULL, 0);
    }

    case LINK_CMD_LISTDIR:
    {
        if (!makeSdPath(path, sizeof(path), (const char*)payload, h->len))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        DIR* d = opendir(path);
        if (!d) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        uint32_t used = 0;
        struct dirent* e;
        while ((e = readdir(d)) != NULL)
        {
            size_t nameLen = strlen(e->d_name);
            if (nameLen == 0 || nameLen > 255) continue;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (used + sizeof(LinkDirEntry) + nameLen > LINK_MAX_PAYLOAD) break;

            LinkDirEntry de;
            memset(&de, 0, sizeof(de));
            de.isDir = (e->d_type == DT_DIR) ? 1 : 0;
            de.nameLen = (uint8_t)nameLen;
            if (!de.isDir)
            {
                char full[LINK_MAX_PATH * 2 + 16];
                snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
                struct stat st;
                if (stat(full, &st) == 0) de.size = (uint64_t)st.st_size;
            }
            memcpy(g_outbuf + used, &de, sizeof(de));
            memcpy(g_outbuf + used + sizeof(de), e->d_name, nameLen);
            used += sizeof(de) + nameLen;
        }
        closedir(d);
        return sendResponse(fd, h, LINK_OK, g_outbuf, used);
    }

    case LINK_CMD_POLLEVENT:
    {
        LightLock_Lock(&g_lock);
        bool have = g_haveEvent;
        uint32_t used = 0;
        if (have)
        {
            memcpy(g_outbuf, &g_event, sizeof(g_event));
            used = sizeof(g_event);
            memcpy(g_outbuf + used, g_eventRom, g_event.romPathLen);
            used += g_event.romPathLen;
            memcpy(g_outbuf + used, g_eventSave, g_event.savePathLen);
            used += g_event.savePathLen;
            g_haveEvent = false;
        }
        else if (g_haveAnswer || g_haveShot || g_haveLangEvent || g_haveStopEv || g_haveResumeEv || g_haveVideoEv ||
                 g_haveBakReqEv || g_haveRestoreEv || g_haveLibReqEv)
        {
            LinkEvent ev;
            memset(&ev, 0, sizeof(ev));
            const char* text = NULL;
            if (g_haveAnswer)
            {
                ev.type = LINK_EVENT_ANSWER;
                ev.romSize = g_answerId;
                ev.reserved = g_answerChoice;
                g_haveAnswer = false;
            }
            else if (g_haveStopEv) { ev.type = LINK_EVENT_STOPGAME; g_haveStopEv = false; }
            else if (g_haveShot) { ev.type = LINK_EVENT_SCREENSHOT; ev.reserved = (u32)g_shotMode; g_haveShot = false; }
            else if (g_haveVideoEv)
            {
                ev.type = LINK_EVENT_VIDEO;
                ev.reserved = g_videoOff ? 0 : 1;
                g_haveVideoEv = false;
            }
            else if (g_haveResumeEv) { ev.type = LINK_EVENT_RESUME; g_haveResumeEv = false; }
            else if (g_haveLibReqEv) { ev.type = LINK_EVENT_LIBRARY; g_haveLibReqEv = false; }
            else if (g_haveBakReqEv || g_haveRestoreEv)
            {
                if (g_haveRestoreEv)
                {
                    ev.type = LINK_EVENT_RESTORE;
                    ev.reserved = (u32)g_restoreIdx;
                    ev.romSize = g_bakToken;
                    g_haveRestoreEv = false;
                }
                else { ev.type = LINK_EVENT_BACKUPS; g_haveBakReqEv = false; }
                text = g_bakRom;
                ev.romPathLen = (uint16_t)strlen(text);
            }
            else
            {
                ev.type = LINK_EVENT_LANGUAGE;
                text = g_langCodes[g_lang];
                ev.romPathLen = (uint16_t)strlen(text);
                g_haveLangEvent = false;
            }
            memcpy(g_outbuf, &ev, sizeof(ev));
            used = sizeof(ev);
            if (text)
            {
                memcpy(g_outbuf + used, text, ev.romPathLen);
                used += ev.romPathLen;
            }
            have = true;
        }
        LightLock_Unlock(&g_lock);
        if (!have) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        return sendResponse(fd, h, LINK_OK, g_outbuf, used);
    }

    case LINK_CMD_ASK:
    {
        if (h->len < 4) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        u32 id;
        memcpy(&id, payload, 4);
        if (h->len == 4)
        {
            /* take it away (melonDS decided otherwise / gave up) */
            if (g_askActive && g_askId == id) g_askActive = false;
            return sendResponse(fd, h, LINK_OK, NULL, 0);
        }
        /* split into lines; the last two are the labels for A and B */
        static char text[1024];
        size_t n = h->len - 4 < sizeof(text) - 1 ? h->len - 4 : sizeof(text) - 1;
        memcpy(text, payload + 4, n);
        text[n] = 0;
        char* lines[ASK_LINES + 2];
        int count = 0;
        for (char* q = text; count < ASK_LINES + 2; )
        {
            lines[count++] = q;
            char* nl = strchr(q, '\n');
            if (!nl) break;
            *nl = 0;
            q = nl + 1;
        }
        if (count < 3) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        LightLock_Lock(&g_lock);
        g_askLineCount = 0;
        for (int i = 0; i < count - 2 && g_askLineCount < ASK_LINES; i++)
        {
            /* wrap at 48 columns */
            char conv[256];
            utf8ToCp437(conv, sizeof(conv), lines[i]);
            size_t len = strlen(conv), pos = 0;
            do
            {
                size_t take = len - pos > 48 ? 48 : len - pos;
                if (take == 48)
                    for (size_t b = 48; b > 20; b--) if (conv[pos + b] == ' ') { take = b; break; }
                snprintf(g_askLines[g_askLineCount++], sizeof(g_askLines[0]), "%.*s", (int)take, conv + pos);
                pos += take;
                while (conv[pos] == ' ') pos++;
            } while (pos < len && g_askLineCount < ASK_LINES);
        }
        utf8ToCp437(g_askA, sizeof(g_askA), lines[count - 2]);
        utf8ToCp437(g_askB, sizeof(g_askB), lines[count - 1]);
        g_askId = id;
        g_askActive = true;
        LightLock_Unlock(&g_lock);
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    case LINK_CMD_OPTIONS:
    {
        if (h->len < 4) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        u32 opts;
        memcpy(&opts, payload, 4);
        g_keepAwake = (opts & LINK_OPT_KEEP_AWAKE) != 0;   /* the main thread applies it */
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    case LINK_CMD_BACKUPLIST:
    {
        if (h->len < 4) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        static char text[4096];
        u32 token;
        memcpy(&token, payload, 4);
        size_t n = h->len - 4 < sizeof(text) - 1 ? h->len - 4 : sizeof(text) - 1;
        memcpy(text, payload + 4, n);
        text[n] = 0;
        char* nl = strchr(text, '\n');
        if (nl) *nl = 0;
        LightLock_Lock(&g_lock);
        if (g_bakWaiting && strcmp(text, g_bakRom) == 0)
        {
            g_bakToken = token;
            g_bakCount = 0;
            for (char* q = nl ? nl + 1 : NULL; q && *q && g_bakCount < BAK_MAX; )
            {
                char* e = strchr(q, '\n');
                if (e) *e = 0;
                utf8ToCp437(g_bakLines[g_bakCount], sizeof(g_bakLines[0]), q);
                g_bakCount++;
                q = e ? e + 1 : NULL;
            }
            g_bakCursor = 0;
            g_bakWaiting = false;
        }
        LightLock_Unlock(&g_lock);
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    case LINK_CMD_SESSION:
    {
        if (h->len < 1) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        u8 flags = payload[0];
        char name[128];
        size_t n = h->len - 1 < sizeof(name) - 1 ? h->len - 1 : sizeof(name) - 1;
        memcpy(name, payload + 1, n);
        name[n] = 0;
        LightLock_Lock(&g_lock);
        if (flags & LINK_SESSION_LAUNCH_DONE) g_launchPending = false;
        if (name[0])
        {
            /* the game runs: "Starting game - waiting..." is over */
            if (strcmp(g_statusText, T(S_STARTING)) == 0) g_statusText[0] = 0;
            bool same = g_gameStarted && strcmp(g_currentGame, name) == 0;
            g_launchPending = false;
            snprintf(g_currentGame, sizeof(g_currentGame), "%s", name);
            if (same) g_sessionSame = true;
            else { g_gameStarted = true; g_sessionNew = true; }
        }
        else if (g_gameStarted && !g_launchPending)
        {
            /* the game in melonDS was ended */
            g_gameStarted = false;
            g_currentGame[0] = 0;
            g_sessionEnded = true;
        }
        LightLock_Unlock(&g_lock);
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    case LINK_CMD_READSTREAM:
    case LINK_CMD_READSTREAMZ:
    case LINK_CMD_READSTREAMZS:
    {
        if (h->len <= sizeof(LinkStreamReq)) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        LinkStreamReq req;
        memcpy(&req, payload, sizeof(req));
        if (!makeSdPath(path, sizeof(path), (const char*)payload + sizeof(req), h->len - sizeof(req)))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        return streamFile(fd, h, path, req.offset, req.length, h->cmd != LINK_CMD_READSTREAM,
                          h->cmd == LINK_CMD_READSTREAMZS);
    }

    case LINK_CMD_SPEEDTEST:
    {
        /* pure network test: data straight from RAM, with the given settings */
        LinkTuning t;
        memset(&t, 0, sizeof(t));
        memcpy(&t, payload, h->len < sizeof(t) ? h->len : sizeof(t));
        u32 mb = t.megabytes;
        if (mb == 0 || mb > 256) mb = 16;
        static u8* junk = NULL;
        if (!junk)
        {
            junk = (u8*)memalign(0x1000, 0x40000);
            if (!junk) return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
            u32 x = 0x12345678;
            for (int k = 0; k < 0x40000; k++) { x = x * 1664525u + 1013904223u; junk[k] = (u8)(x >> 24); }
        }
        u32 oldPiece = g_sendPiece, oldSleep = g_sendSleepUs, oldBuf = g_sendBuf;
        if (t.piece) g_sendPiece = t.piece > 0x40000 ? 0x40000 : t.piece;
        if (t.sleepUs) g_sendSleepUs = t.sleepUs;
        if (t.sndbuf) { int b = (int)t.sndbuf; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &b, sizeof(b)); }
        bool blocking = (t.flags & LINK_TUNE_SET) ? (t.flags & LINK_TUNE_BLOCKING) != 0 : g_sendBlocking;

        u64 total = (u64)mb * 1024 * 1024;
        bool okSend = sendResponse(fd, h, LINK_OK, &total, sizeof(total));
        g_statSendSleeps = 0;
        g_bulkActive = true;
        if (blocking) setBlocking(fd, true);
        u64 t0 = osGetTime();
        for (u64 sent = 0; sent < total && okSend; sent += 0x40000)
            okSend = sendAll(fd, junk, 0x40000);
        u64 ms = osGetTime() - t0;
        if (blocking) setBlocking(fd, false);
        g_bulkActive = false;

        g_sendPiece = oldPiece;
        g_sendSleepUs = oldSleep;
        if (t.sndbuf) { int b = (int)oldBuf; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &b, sizeof(b)); }
        if (!okSend) return false;

        LightLock_Lock(&g_lock);
        snprintf(g_xferInfo, sizeof(g_xferInfo), "Speedtest: %lu KB/s (%luK/%luK%s)",
                 ms ? (unsigned long)(total / ms) : 0, (unsigned long)((t.sndbuf ? t.sndbuf : oldBuf) / 1024),
                 (unsigned long)((t.piece ? t.piece : oldPiece) / 1024), blocking ? ", blockierend" : "");
        snprintf(g_xferInfo2, sizeof(g_xferInfo2), "Wartet: WLAN-Puffer voll %lu%%",
                 ms ? (unsigned long)(g_statSendSleeps * g_sendSleepUs / 10 / ms) : 0);
        LightLock_Unlock(&g_lock);
        return true;
    }

    case LINK_CMD_GETTIME:
    {
        /* the clock the games see, so the emulator can run on the same time */
        /* plus the user time offset (config block 0x30001: shown time minus the RTC).
           Games keep the raw RTC (shown time minus this offset) in their saves to spot a
           changed clock, so the emulator needs the same offset or the save "jumps". */
        /* then, for finding out what the clock values mean (LINK_TIME_UNKNOWN where not
           readable): the raw RTC from the MCU registers, the RTC as the system (PTM)
           reads it, and config block 0x30002 */
        u64 t[5] = { osGetTime(), LINK_TIME_UNKNOWN, LINK_TIME_UNKNOWN, LINK_TIME_UNKNOWN, LINK_TIME_UNKNOWN };
        if (R_SUCCEEDED(cfguInit()))
        {
            u64 v = 0;
            if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(8, 0x00030001, &v))) t[1] = v;
            if (R_SUCCEEDED(CFG_GetConfigInfoBlk8(8, 0x00030002, &v))) t[4] = v;
            cfguExit();
        }
        if (!readRawRtc(&t[2])) t[2] = LINK_TIME_UNKNOWN;
        if (R_SUCCEEDED(ptmSysmInit()))
        {
            s64 y2k = 0;
            if (R_SUCCEEDED(PTMSYSM_GetRtcTime(&y2k))) t[3] = (u64)(y2k + 3155673600000LL);   /* 1900 -> 2000 */
            ptmSysmExit();
        }
        size_t n = sizeof(t);
        return sendResponse(fd, h, LINK_OK, t, n);
    }

    case LINK_CMD_ARCREAD:
    {
        /* a part of a file in a system archive (Mii database, saves, extdata) */
        LinkArchiveReq req;
        FS_Archive arch;
        if (h->len <= sizeof(req) || !arcPath(payload + sizeof(req), h->len - sizeof(req)))
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        memcpy(&req, payload, sizeof(req));
        if (!openLinkArchive(&req, &arch))   /* archive not accessible */
            return sendResponse(fd, h, LINK_ERR_DENIED, &g_arcResult, sizeof(g_arcResult));
        Handle file;
        uint16_t st = LINK_OK;
        u32 got = 0;
        if (R_FAILED(g_arcResult = FSUSER_OpenFile(&file, arch, fsMakePath(PATH_UTF16, g_arcPath), FS_OPEN_READ, 0)))
            st = LINK_ERR_NOTFOUND;
        else
        {
            /* answer: u32 size of the whole file, then the part asked for */
            u64 size = 0;
            u32 want = req.length < LINK_MAX_PAYLOAD - 4 ? req.length : LINK_MAX_PAYLOAD - 4;
            if (R_FAILED(FSFILE_GetSize(file, &size)) || size > 0xFFFFFFFFull)
                st = LINK_ERR_IO;
            else
            {
                if ((u64)req.offset >= size) want = 0;
                else if (req.offset + (u64)want > size) want = (u32)(size - req.offset);
                u32 total = (u32)size;
                memcpy(g_outbuf, &total, 4);
                if (want && (R_FAILED(FSFILE_Read(file, &got, req.offset, g_outbuf + 4, want)) || got != want))
                    st = LINK_ERR_IO;
            }
            FSFILE_Close(file);
        }
        (void)arch;   /* stays open for the next request */
        if (st == LINK_ERR_NOTFOUND) return sendResponse(fd, h, st, &g_arcResult, sizeof(g_arcResult));
        return sendResponse(fd, h, st, g_outbuf, st == LINK_OK ? got + 4 : 0);
    }

    case LINK_CMD_ARCREADZ:
    case LINK_CMD_ARCHASH:
    {
        /* a part of a file, compressed (saves are often mostly empty), or its hash */
        u8* raw = arcRawBuffer();
        LinkArchiveReq req;
        FS_Archive arch;
        if (!raw) return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
        if (h->len <= sizeof(req) || !arcPath(payload + sizeof(req), h->len - sizeof(req)))
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        memcpy(&req, payload, sizeof(req));
        if (!openLinkArchive(&req, &arch))
            return sendResponse(fd, h, LINK_ERR_DENIED, &g_arcResult, sizeof(g_arcResult));
        Handle file;
        if (R_FAILED(g_arcResult = FSUSER_OpenFile(&file, arch, fsMakePath(PATH_UTF16, g_arcPath), FS_OPEN_READ, 0)))
        {
            (void)arch;   /* stays open for the next request */
            return sendResponse(fd, h, LINK_ERR_NOTFOUND, &g_arcResult, sizeof(g_arcResult));
        }
        uint16_t st = LINK_OK;
        u32 outLen = 0;
        u64 size = 0;
        if (R_FAILED(FSFILE_GetSize(file, &size)) || size > 0xFFFFFFFFull) st = LINK_ERR_IO;
        else if (h->cmd == LINK_CMD_ARCHASH)
        {
            u64 hash = LINK_FNV64_INIT;
            for (u64 pos = 0; pos < size && st == LINK_OK;)
            {
                u32 want = size - pos > LINK_ARCZ_MAX_CHUNK ? LINK_ARCZ_MAX_CHUNK : (u32)(size - pos);
                u32 got = 0;
                if (R_FAILED(FSFILE_Read(file, &got, pos, raw, want)) || got != want) { st = LINK_ERR_IO; break; }
                for (u32 i = 0; i < got; i++) hash = (hash ^ raw[i]) * LINK_FNV64_PRIME;
                pos += got;
            }
            memcpy(g_outbuf, &size, 8);
            memcpy(g_outbuf + 8, &hash, 8);
            outLen = 16;
        }
        else
        {
            u32 want = req.length < LINK_ARCZ_MAX_CHUNK ? req.length : LINK_ARCZ_MAX_CHUNK;
            if ((u64)req.offset >= size) want = 0;
            else if (req.offset + (u64)want > size) want = (u32)(size - req.offset);
            u32 got = 0;
            if (want && (R_FAILED(FSFILE_Read(file, &got, req.offset, raw, want)) || got != want)) st = LINK_ERR_IO;
            else
            {
                LinkArcChunk c;
                memset(&c, 0, sizeof(c));
                c.totalSize = (u32)size;
                c.rawLen = want;
                c.mode = LINK_BLOCK_RAW;
                int enc = want ? LZ4_compress_fast_extState(&g_lz4State, (const char*)raw,
                                                            (char*)g_outbuf + sizeof(c), (int)want,
                                                            (int)(LINK_MAX_PAYLOAD - sizeof(c)), 1) : 0;
                if (enc > 0 && (u32)enc < want) { c.mode = LINK_BLOCK_LZ4; outLen = sizeof(c) + (u32)enc; }
                else { memcpy(g_outbuf + sizeof(c), raw, want); outLen = sizeof(c) + want; }
                memcpy(g_outbuf, &c, sizeof(c));
            }
        }
        FSFILE_Close(file);
        (void)arch;   /* stays open for the next request */
        return sendResponse(fd, h, st, g_outbuf, st == LINK_OK ? outLen : 0);
    }

    case LINK_CMD_ARCWRITE:
    {
        /* a part of a file in a save / extdata (the emulator writes a save back) */
        LinkArcWriteReq req;
        FS_Archive arch;
        if (h->len < sizeof(req)) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        memcpy(&req, payload, sizeof(req));
        if (req.archive.archive != LINK_ARCHIVE_USER_SAVEDATA && req.archive.archive != LINK_ARCHIVE_EXTDATA)
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);   /* saves only */
        if (h->len != sizeof(req) + req.pathLen + req.archive.length ||
            (u64)req.archive.offset + req.archive.length > req.fileSize ||
            !arcPath(payload + sizeof(req), req.pathLen))
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        if (!openLinkArchive(&req.archive, &arch))
            return sendResponse(fd, h, LINK_ERR_DENIED, &g_arcResult, sizeof(g_arcResult));
        FS_Path fp = fsMakePath(PATH_UTF16, g_arcPath);
        if (req.flags & LINK_AW_CREATE)
        {
            FSUSER_DeleteFile(arch, fp);   /* not there is fine */
            if (R_FAILED(g_arcResult = FSUSER_CreateFile(arch, fp, 0, req.fileSize)))
                return sendResponse(fd, h, LINK_ERR_IO, &g_arcResult, sizeof(g_arcResult));
        }
        Handle file;
        if (R_FAILED(g_arcResult = FSUSER_OpenFile(&file, arch, fp, FS_OPEN_WRITE, 0)))
            return sendResponse(fd, h, LINK_ERR_NOTFOUND, &g_arcResult, sizeof(g_arcResult));
        u32 written = 0;
        uint16_t st = LINK_OK;
        if (req.archive.length &&
            (R_FAILED(g_arcResult = FSFILE_Write(file, &written, req.archive.offset,
                                                 payload + sizeof(req) + req.pathLen,
                                                 req.archive.length, 0)) ||
             written != req.archive.length))
            st = LINK_ERR_IO;
        FSFILE_Close(file);
        return sendResponse(fd, h, st, st == LINK_OK ? NULL : &g_arcResult, st == LINK_OK ? 0 : sizeof(g_arcResult));
    }

    case LINK_CMD_ARCDELETE:
    case LINK_CMD_ARCMKDIR:
    case LINK_CMD_ARCCOMMIT:
    {
        LinkArchiveReq req;
        FS_Archive arch;
        if (h->len < sizeof(req)) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        memcpy(&req, payload, sizeof(req));
        if (req.archive != LINK_ARCHIVE_USER_SAVEDATA && req.archive != LINK_ARCHIVE_EXTDATA)
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);   /* saves only */
        if (h->cmd != LINK_CMD_ARCCOMMIT && !arcPath(payload + sizeof(req), h->len - sizeof(req)))
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        if (!openLinkArchive(&req, &arch))
            return sendResponse(fd, h, LINK_ERR_DENIED, &g_arcResult, sizeof(g_arcResult));
        FS_Path fp = fsMakePath(PATH_UTF16, g_arcPath);
        if (h->cmd == LINK_CMD_ARCDELETE)
            g_arcResult = req.offset ? FSUSER_DeleteDirectoryRecursively(arch, fp) : FSUSER_DeleteFile(arch, fp);
        else if (h->cmd == LINK_CMD_ARCMKDIR)
        {
            Handle d;
            if (R_SUCCEEDED(FSUSER_OpenDirectory(&d, arch, fp))) { FSDIR_Close(d); g_arcResult = 0; }
            else g_arcResult = FSUSER_CreateDirectory(arch, fp, 0);
        }
        else
        {
            /* offset 1: throw the changes away (closing without commit) */
            bool commit = req.archive == LINK_ARCHIVE_USER_SAVEDATA && req.offset == 0;
            g_arcResult = commit
                ? FSUSER_ControlArchive(arch, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0) : 0;
            closeLinkArchive();   /* done with this save */
            if (commit && R_SUCCEEDED(g_arcResult) && req.mediaType == LINK_MEDIA_SD)
            {
                /* Games on the SD card keep a "secure value" of their save in the system;
                   a save written from outside does not match it, and the game calls the
                   save damaged. Deleting it (as save managers like Checkpoint do after a
                   restore) makes the game take the save as it is. */
                u8 out = 0;
                u64 secureValue = ((u64)SECUREVALUE_SLOT_SD << 32) | ((u64)((req.lowId >> 8) & 0xFFFFF) << 8);
                FSUSER_ControlSecureSave(SECURESAVE_ACTION_DELETE, &secureValue, 8, &out, 1);
            }
        }
        if (R_FAILED(g_arcResult))
            return sendResponse(fd, h, h->cmd == LINK_CMD_ARCDELETE ? LINK_ERR_NOTFOUND : LINK_ERR_IO,
                                &g_arcResult, sizeof(g_arcResult));
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    case LINK_CMD_SECUREDEL:
    {
        LinkArchiveReq req;
        if (h->len < sizeof(req)) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        memcpy(&req, payload, sizeof(req));
        if (req.archive != LINK_ARCHIVE_USER_SAVEDATA || req.mediaType != LINK_MEDIA_SD)
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        u8 out = 0;
        u64 secureValue = ((u64)SECUREVALUE_SLOT_SD << 32) | ((u64)((req.lowId >> 8) & 0xFFFFF) << 8);
        g_arcResult = FSUSER_ControlSecureSave(SECURESAVE_ACTION_DELETE, &secureValue, 8, &out, 1);
        if (R_FAILED(g_arcResult)) return sendResponse(fd, h, LINK_ERR_IO, &g_arcResult, sizeof(g_arcResult));
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    case LINK_CMD_SYSROMFS:
    {
        /* the RomFS of a system data archive, read through the 3DS's own file system */
        u8* raw = arcRawBuffer();
        LinkRomfsReq req;
        if (!raw) return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
        if (h->len < sizeof(req)) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        memcpy(&req, payload, sizeof(req));
        /* only system data archives (shared / system data): never games or other titles */
        u32 category = (u32)(req.titleId >> 32);
        if (category != 0x0004009Bu && category != 0x000400DBu)
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        if (g_romfsOpen && g_romfsTid != req.titleId) { FSFILE_Close(g_romfsFile); g_romfsOpen = false; }
        if (!g_romfsOpen)
        {
            u32 archPath[4] = { (u32)req.titleId, (u32)(req.titleId >> 32), req.mediaType, 0 };
            u32 filePath[5] = { 0, 0, 0, 0, 0 };   /* type 0: RomFS */
            g_arcResult = FSUSER_OpenFileDirectly(&g_romfsFile, (FS_ArchiveID)0x2345678A,
                                                  (FS_Path){ PATH_BINARY, sizeof(archPath), archPath },
                                                  (FS_Path){ PATH_BINARY, sizeof(filePath), filePath },
                                                  FS_OPEN_READ, 0);
            if (R_FAILED(g_arcResult))
                return sendResponse(fd, h, LINK_ERR_NOTFOUND, &g_arcResult, sizeof(g_arcResult));
            g_romfsOpen = true;
            g_romfsTid = req.titleId;
        }
        u64 size = 0;
        if (R_FAILED(FSFILE_GetSize(g_romfsFile, &size)) || size > 0xFFFFFFFFull)
            return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
        u32 want = req.length < LINK_ARCZ_MAX_CHUNK ? req.length : LINK_ARCZ_MAX_CHUNK;
        if ((u64)req.offset >= size) want = 0;
        else if (req.offset + (u64)want > size) want = (u32)(size - req.offset);
        u32 got = 0;
        if (want && (R_FAILED(FSFILE_Read(g_romfsFile, &got, req.offset, raw, want)) || got != want))
            return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
        LinkArcChunk c;
        memset(&c, 0, sizeof(c));
        c.totalSize = (u32)size;
        c.rawLen = want;
        c.mode = LINK_BLOCK_RAW;
        u32 outLen;
        int enc = want ? LZ4_compress_fast_extState(&g_lz4State, (const char*)raw, (char*)g_outbuf + sizeof(c),
                                                    (int)want, (int)(LINK_MAX_PAYLOAD - sizeof(c)), 1) : 0;
        if (enc > 0 && (u32)enc < want) { c.mode = LINK_BLOCK_LZ4; outLen = sizeof(c) + (u32)enc; }
        else { memcpy(g_outbuf + sizeof(c), raw, want); outLen = sizeof(c) + want; }
        memcpy(g_outbuf, &c, sizeof(c));
        if (want == 0 || req.offset + want >= size) { FSFILE_Close(g_romfsFile); g_romfsOpen = false; }
        return sendResponse(fd, h, LINK_OK, g_outbuf, outLen);
    }

    case LINK_CMD_ARCLIST:
    case LINK_CMD_ARCLISTHASH:
    {
        /* the entries of a folder in a system archive */
        LinkArchiveReq req;
        FS_Archive arch;
        if (h->len < sizeof(req) || !arcPath(payload + sizeof(req), h->len - sizeof(req)))
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        memcpy(&req, payload, sizeof(req));
        if (!openLinkArchive(&req, &arch))
            return sendResponse(fd, h, LINK_ERR_DENIED, &g_arcResult, sizeof(g_arcResult));
        Handle dir;
        if (R_FAILED(g_arcResult = FSUSER_OpenDirectory(&dir, arch, fsMakePath(PATH_UTF16, g_arcPath))))
        {
            (void)arch;   /* stays open for the next request */
            return sendResponse(fd, h, LINK_ERR_NOTFOUND, &g_arcResult, sizeof(g_arcResult));
        }
        static FS_DirectoryEntry entries[8];   /* big (0x228 each): not on the thread stack */
        size_t used = 0;
        uint16_t st = LINK_OK;
        for (;;)
        {
            u32 n = 0;
            if (R_FAILED(FSDIR_Read(dir, &n, 8, entries))) { st = LINK_ERR_IO; break; }
            if (n == 0) break;
            for (u32 i = 0; i < n; i++)
            {
                u8 name[256];
                memset(name, 0, sizeof(name));
                utf16_to_utf8(name, entries[i].name, sizeof(name) - 1);
                size_t nameLen = strlen((const char*)name);
                LinkDirEntry de;
                de.size = entries[i].fileSize;
                de.isDir = (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) ? 1 : 0;
                de.nameLen = (u8)nameLen;
                u64 hash = 0;
                if (h->cmd == LINK_CMD_ARCLISTHASH && !de.isDir && !hashArcFile(arch, name, &de.size, &hash))
                { st = LINK_ERR_IO; break; }
                size_t extra = h->cmd == LINK_CMD_ARCLISTHASH ? 8 : 0;
                if (used + sizeof(de) + nameLen + extra > LINK_MAX_PAYLOAD) { st = LINK_ERR_IO; break; }
                memcpy(g_outbuf + used, &de, sizeof(de));
                memcpy(g_outbuf + used + sizeof(de), name, nameLen);
                if (extra) memcpy(g_outbuf + used + sizeof(de) + nameLen, &hash, 8);
                used += sizeof(de) + nameLen + extra;
            }
            if (st != LINK_OK) break;
        }
        FSDIR_Close(dir);
        (void)arch;   /* stays open for the next request */
        return sendResponse(fd, h, st, g_outbuf, st == LINK_OK ? (uint32_t)used : 0);
    }

    case LINK_CMD_TITLES:
    {
        /* installed titles (SD card) and the game card, e.g. to find a game's save */
        size_t used = 0;
        if (R_SUCCEEDED(amInit()))
        {
            const FS_MediaType media[2] = { MEDIATYPE_SD, MEDIATYPE_GAME_CARD };
            for (int m = 0; m < 2; m++)
            {
                u32 count = 0, read = 0;
                u32 maxIds = (u32)((LINK_MAX_PAYLOAD / 2 - 4) / 8);
                if (R_FAILED(AM_GetTitleCount(media[m], &count))) count = 0;
                if (count > maxIds) count = maxIds;
                if (count && R_FAILED(AM_GetTitleList(&read, media[m], count, (u64*)(g_outbuf + used + 4)))) read = 0;
                memcpy(g_outbuf + used, &read, 4);
                used += 4 + (size_t)read * 8;
            }
            amExit();
        }
        return sendResponse(fd, h, used ? LINK_OK : LINK_ERR_IO, g_outbuf, (uint32_t)used);
    }

    case LINK_CMD_ARCINFO:
    {
        /* how the archive was formatted (the emulator needs it for a new save) */
        LinkArchiveReq req;
        if (h->len < sizeof(req)) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        memcpy(&req, payload, sizeof(req));
        u32 lowpath[3] = { req.mediaType, req.lowId, req.highId };
        LinkFormatInfo fi;
        memset(&fi, 0, sizeof(fi));
        bool dup = false;
        if (R_FAILED(FSUSER_GetFormatInfo(&fi.totalSize, &fi.directories, &fi.files, &dup,
                                          (FS_ArchiveID)req.archive,
                                          (FS_Path){ PATH_BINARY, sizeof(lowpath), lowpath })))
            return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        fi.duplicateData = dup ? 1 : 0;
        return sendResponse(fd, h, LINK_OK, &fi, sizeof(fi));
    }

    case LINK_CMD_GETSTATS:
    {
        char txt[384];
        LightLock_Lock(&g_lock);
        snprintf(txt, sizeof(txt), "%s | %s | Puffer %luK, Stueck %luK, Pause %luus%s | Fuellbereich nicht gelesen: %lu MB"
                 " | Upload: %lu KB, Empfang %lu ms, SD %lu ms",
                 g_xferInfo, g_xferInfo2, (unsigned long)(g_sendBuf / 1024),
                 (unsigned long)(g_sendPiece / 1024), (unsigned long)g_sendSleepUs,
                 g_sendBlocking ? ", blockierend" : "", (unsigned long)(g_statSkipped >> 20),
                 (unsigned long)(g_upBytes / 1024), (unsigned long)(g_upRecvTicks * 1000 / SYSCLOCK_ARM11),
                 (unsigned long)(g_upSdTicks * 1000 / SYSCLOCK_ARM11));
        g_upBytes = g_upRecvTicks = g_upSdTicks = 0;
        LightLock_Unlock(&g_lock);
        return sendResponse(fd, h, LINK_OK, txt, (uint32_t)strlen(txt));
    }

    case LINK_CMD_SETTUNING:
    {
        LinkTuning t;
        memset(&t, 0, sizeof(t));
        memcpy(&t, payload, h->len < sizeof(t) ? h->len : sizeof(t));
        if (t.piece) g_sendPiece = t.piece > 0x40000 ? 0x40000 : t.piece;
        if (t.sleepUs) g_sendSleepUs = t.sleepUs;
        if (t.flags & LINK_TUNE_SET) g_sendBlocking = (t.flags & LINK_TUNE_BLOCKING) != 0;
        if (t.sndbuf)
        {
            g_sendBuf = t.sndbuf;
            int b = (int)t.sndbuf;
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &b, sizeof(b));
        }
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    case LINK_CMD_VIDEOSTATS:
    {
        /* H.264 on the 3DS since the last time asked: shown, dropped while waiting for a
           key frame, rebuilt from parity, decoder errors */
        static u32 last[21];
        u32 now[21] = { g_h264Shown, g_h264Dropped, g_h264Repaired, g_h264Errors, g_vidOverruns,
                        g_stallCount, g_stallTotalMs, g_h264Glitched, g_rotLate, g_rotStutter, g_rotWaitN,
                        g_loopSlow, g_dispGaps[0], g_dispGaps[1], g_dispGaps[2], g_dispGaps[3],
                        g_tileShown, g_tileForced, g_tileRebuilt, g_rttCount, g_rttSumUs };
        u32 d[24];
        for (int i = 0; i < 5; i++) { d[i] = now[i] - last[i]; last[i] = now[i]; }
        d[5] = g_vidRcvBuf;
        d[6] = now[5] - last[5]; last[5] = now[5];
        d[7] = now[6] - last[6]; last[6] = now[6];
        d[8] = g_stallMaxMs;
        g_stallMaxMs = 0;
        /* glitched (decoded after a loss), shown late, uneven, average wait in the buffer (ms) */
        for (int i = 7; i < 10; i++) { d[i + 2] = now[i] - last[i]; last[i] = now[i]; }
        u32 waitN = now[10] - last[10];
        last[10] = now[10];
        static u32 lastWait = 0;
        u32 waitSum = g_rotWaitSum;
        d[12] = waitN ? (waitSum - lastWait) / waitN : 0;
        lastWait = waitSum;
        d[13] = now[11] - last[11];   /* screen refreshes the main loop missed */
        last[11] = now[11];
        for (int i = 12; i < 19; i++) { d[i + 2] = now[i] - last[i]; last[i] = now[i]; }
        /* input round trip: count, average and longest in microseconds */
        d[21] = now[19] - last[19];
        d[22] = d[21] ? (now[20] - last[20]) / d[21] : 0;
        last[19] = now[19];
        last[20] = now[20];
        d[23] = g_rttMaxUs;
        g_rttMaxUs = 0;
        return sendResponse(fd, h, LINK_OK, d, sizeof(d));
    }

    case LINK_CMD_VIDEOPROBE:
    {
        static u8 probe[4 + sizeof(g_rotImage[0])];
        if (!g_rotMode) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        int idx = g_rotIdx;
        u32 ts = g_rotSrc[idx];
        memcpy(probe, &ts, 4);
        memcpy(probe + 4, g_rotImage[idx], sizeof(g_rotImage[0]));
        return sendResponse(fd, h, LINK_OK, probe, sizeof(probe));
    }

    case LINK_CMD_VIDEOCAPS:
    {
        linkGfInit();
        /* then (newer apps) whether the input packets got the WiFi priority (0 = yes) */
        u32 caps[2] = { (mvdSetup() ? (LINK_VIDCAP_H264 | LINK_VIDCAP_H264_RS) : 0) |
                            LINK_VIDCAP_TILE_RS | LINK_VIDCAP_TILE_TS | LINK_VIDCAP_ECHO | LINK_VIDCAP_TILE_LZ4,
                        (u32)g_tosResult };
        return sendResponse(fd, h, LINK_OK, caps, sizeof(caps));
    }

    case LINK_CMD_LIBRARY:
    {
        /* Azahar's game list for the 3DS tab */
        char* buf = (char*)malloc(h->len + 1);
        if (!buf) return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
        memcpy(buf, payload, h->len);
        buf[h->len] = '\0';
        LightLock_Lock(&g_lock);
        free(g_libPending);
        g_libPending = buf;
        LightLock_Unlock(&g_lock);
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    case LINK_CMD_GAMELIST:
    {
        /* the folder the list on the 3DS shows, and its .nds files with sizes */
        char dir[128];
        LightLock_Lock(&g_lock);
        snprintf(dir, sizeof(dir), "%s", g_romDir);
        LightLock_Unlock(&g_lock);
        uint16_t dirLen = (uint16_t)strlen(dir);
        memcpy(g_outbuf, &dirLen, 2);
        memcpy(g_outbuf + 2, dir, dirLen);
        uint32_t used = 2 + dirLen;

        snprintf(path, sizeof(path), "sdmc:%s", dir);
        DIR* d = opendir(path);
        if (d)
        {
            struct dirent* e;
            while ((e = readdir(d)) != NULL)
            {
                size_t nameLen = strlen(e->d_name);
                if (e->d_type == DT_DIR || nameLen == 0 || nameLen > 255 || !hasNdsExt(e->d_name)) continue;
                if (used + sizeof(LinkDirEntry) + nameLen > LINK_MAX_PAYLOAD) break;
                LinkDirEntry de;
                memset(&de, 0, sizeof(de));
                de.nameLen = (uint8_t)nameLen;
                char full[LINK_MAX_PATH * 2 + 16];
                snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
                struct stat st;
                if (stat(full, &st) == 0) de.size = (uint64_t)st.st_size;
                memcpy(g_outbuf + used, &de, sizeof(de));
                memcpy(g_outbuf + used + sizeof(de), e->d_name, nameLen);
                used += sizeof(de) + nameLen;
            }
            closedir(d);
        }
        return sendResponse(fd, h, LINK_OK, g_outbuf, used);
    }

    case LINK_CMD_SETONPC:
    {
        char* buf = (char*)malloc(h->len + 1);
        if (!buf) return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
        memcpy(buf, payload, h->len);
        buf[h->len] = '\0';
        LightLock_Lock(&g_lock);
        free(g_onPcBuf);
        g_onPcBuf = buf;
        g_onPcCount = 0;
        for (char* p = buf; *p && g_onPcCount < MAX_ONPC; )
        {
            char* nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (*p) g_onPc[g_onPcCount++] = p;
            if (!nl) break;
            p = nl + 1;
        }
        g_onPcVersion++;
        LightLock_Unlock(&g_lock);
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    case LINK_CMD_SDTEST:
    {
        /* write the same 128 KB in different ways and time each (diagnostics) */
        char dir[LINK_MAX_PATH + 8];
        if (h->len == 0 || !makeSdPath(dir, sizeof(dir), (const char*)payload, h->len))
        {
            LightLock_Lock(&g_lock);
            snprintf(dir, sizeof(dir), "sdmc:%.100s/saves", g_romDir);
            LightLock_Unlock(&g_lock);
        }
        char tpath[LINK_MAX_PATH + 40];
        snprintf(tpath, sizeof(tpath), "%s/melonDS-Link-sdtest.bin", dir);
        const u32 N = 128 * 1024;
        u8* heap = (u8*)malloc(N);
        if (!heap) return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
        for (u32 k = 0; k < N; k++) heap[k] = (u8)(k * 7 + 3);
        char txt[512];
        int tl = snprintf(txt, sizeof(txt), "128 KB in %.60s:", dir);
#define SDT(label, code) do { u64 t0_ = svcGetSystemTick(); code; \
            tl += snprintf(txt + tl, sizeof(txt) - tl, " %s %lu ms,", label, \
                (unsigned long)((svcGetSystemTick() - t0_) * 1000 / SYSCLOCK_ARM11)); } while (0)
        FILE* f;
        remove(tpath);
        SDT("neue Datei", { f = fopen(tpath, "wb"); if (f) { fwrite(heap, 1, N, f); fclose(f); } });
        SDT("ueberschreiben", { f = fopen(tpath, "r+b"); if (f) { setvbuf(f, NULL, _IONBF, 0); fwrite(heap, 1, N, f); fclose(f); } });
        SDT("ueberschreiben 32K-Stuecke", { f = fopen(tpath, "r+b"); if (f) { setvbuf(f, NULL, _IONBF, 0);
            for (u32 o = 0; o < N; o += 0x8000) fwrite(heap + o, 1, 0x8000, f); fclose(f); } });
#ifdef __3DS__
        {
            static u16 w[LINK_MAX_PATH + 48];
            memset(w, 0, sizeof(w));
            ssize_t wl = utf8_to_utf16(w, (const uint8_t*)tpath + 5, LINK_MAX_PATH + 32);   /* without "sdmc:" */
            if (wl > 0) w[wl] = 0;
            FS_Archive ar;
            Handle fh;
            u32 wr;
            bool arOk = R_SUCCEEDED(FSUSER_OpenArchive(&ar, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
            if (arOk)
            {
                SDT("FS direkt", { if (R_SUCCEEDED(FSUSER_OpenFile(&fh, ar, fsMakePath(PATH_UTF16, w), FS_OPEN_WRITE, 0)))
                    { FSFILE_Write(fh, &wr, 0, heap, N, 0); FSFILE_Close(fh); } });
                u8* lin = (u8*)linearAlloc(N);
                if (lin)
                {
                    memcpy(lin, heap, N);
                    SDT("FS linear", { if (R_SUCCEEDED(FSUSER_OpenFile(&fh, ar, fsMakePath(PATH_UTF16, w), FS_OPEN_WRITE, 0)))
                        { FSFILE_Write(fh, &wr, 0, lin, N, 0); FSFILE_Close(fh); } });
                    linearFree(lin);
                }
                SDT("FS unausgerichtet", { if (R_SUCCEEDED(FSUSER_OpenFile(&fh, ar, fsMakePath(PATH_UTF16, w), FS_OPEN_WRITE, 0)))
                    { FSFILE_Write(fh, &wr, 0, heap + 97, N - 128, 0); FSFILE_Close(fh); } });
                g_bulkActive = true;
                svcSleepThread(50000000);   /* let the other threads notice */
                SDT("FS ohne Netz", { if (R_SUCCEEDED(FSUSER_OpenFile(&fh, ar, fsMakePath(PATH_UTF16, w), FS_OPEN_WRITE, 0)))
                    { FSFILE_Write(fh, &wr, 0, heap, N, 0); FSFILE_Close(fh); } });
                g_bulkActive = false;
                FSUSER_CloseArchive(ar);
            }
        }
#endif
        SDT("lesen", { f = fopen(tpath, "rb"); if (f) { setvbuf(f, NULL, _IONBF, 0); fread(heap, 1, N, f); fclose(f); } });
#undef SDT
        remove(tpath);
        free(heap);
        if (tl > 0 && txt[tl - 1] == ',') txt[--tl] = 0;
        return sendResponse(fd, h, LINK_OK, txt, (uint32_t)strlen(txt));
    }

    case LINK_CMD_REPLACE:
    {
        /* replace `to` with `from`, keeping the old file until the new one is in place */
        if (h->len < 2) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        uint16_t fromLen;
        memcpy(&fromLen, payload, 2);
        if (2u + fromLen >= h->len) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        char from[LINK_MAX_PATH + 8], to[LINK_MAX_PATH + 8], bak[LINK_MAX_PATH + 16];
        if (!makeSdPath(from, sizeof(from), (const char*)payload + 2, fromLen) ||
            !makeSdPath(to, sizeof(to), (const char*)payload + 2 + fromLen, h->len - 2 - fromLen))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        struct stat st;
        if (stat(from, &st) != 0) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        snprintf(bak, sizeof(bak), "%s.old", to);
        bool hadOld = stat(to, &st) == 0;
        if (hadOld)
        {
            remove(bak);
            if (rename(to, bak) != 0) return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
        }
        else mkdirParents(to);
        g_saveWriting = false;
        if (rename(from, to) != 0)
        {
            if (hadOld) rename(bak, to);   /* put the old one back */
            return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
        }
        /* the old file becomes the next `from`: it already has its space on
           the SD card, so the next upload only overwrites (finding free space
           for a new file is very slow on big, full SD cards: ~17 s for 512 KB) */
        if (hadOld && rename(bak, from) != 0) remove(bak);
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    default:
        return sendResponse(fd, h, LINK_ERR_BADCMD, NULL, 0);
    }
}

/* ------------------------------------------------------------------ */
/* TCP server thread                                                   */

static int g_listenFd = -1;

/* sockets (also made anew after the 3DS woke up: sleep can break them) */
static int openListenSocket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return fd;
    int one = 1;   /* allow restarting right away (port may still be in TIME_WAIT) */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(LINK_TCP_PORT);
    bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(fd, 1);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

/* The input packets as the WiFi's top priority class (DSCP CS6 -> WMM "voice"): with
   many picture packets in the air the 3DS then gets to send sooner. Whether the 3DS's WiFi
   honours it is not known; the PC logs the round trip (LINK_VIDCAP_ECHO) to see. */
static void markInputPriority(int fd)
{
    if (fd < 0) return;
    int tos = 0xC0;
    g_tosResult = setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == 0 ? 0 : (errno ? errno : 1);
}

static int openUdpSocket(u16 port, int rcvbuf)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) return fd;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (rcvbuf) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    return fd;
}

static void serverThread(void* arg)
{
    (void)arg;
    u32 gen = g_sockGen;
    while (g_running)
    {
        if (g_netPause) { svcSleepThread(20000000); continue; }
        if (gen != g_sockGen)
        {
            gen = g_sockGen;
            if (g_listenFd >= 0) close(g_listenFd);
            g_listenFd = openListenSocket();
        }
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(g_listenFd, (struct sockaddr*)&peer, &plen);
        if (fd < 0)
        {
            svcSleepThread(50 * 1000000LL);
            continue;
        }

        /* non-blocking: never block inside the shared socket service (see sendAll) */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        {
            int buf = (int)g_sendBuf;   /* 32 KB like ftpd unless melonDS tuned it */
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
            int rbuf = 128 * 1024;   /* saves arrive in 256 KB pieces */
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));
        }

        LightLock_Lock(&g_lock);
        snprintf(g_peerName, sizeof(g_peerName), "%s", inet_ntoa(peer.sin_addr));
        LightLock_Unlock(&g_lock);
        g_tcpConnected = true;

        while (g_running && gen == g_sockGen && !g_netPause)
        {
            LinkHeader h;
            if (!recvAll(fd, &h, sizeof(h))) break;
            if (h.magic != LINK_MAGIC || h.len > LINK_MAX_PAYLOAD + LINK_MAX_PATH + 64) break;
            u64 r0 = svcGetSystemTick();
            if (h.len && !recvAll(fd, g_payload, h.len)) break;
            if (h.cmd == LINK_CMD_WRITE)
            {
                g_upBytes += h.len;
                g_upRecvTicks += svcGetSystemTick() - r0;
            }
            if (!handleCommand(fd, &h, g_payload)) break;
        }

        closeLinkArchive();   /* the emulator is gone: let go of the save */
        close(fd);
        g_tcpConnected = false;
        setStatusText("");
    }
}

/* ------------------------------------------------------------------ */
/* DS bottom screen image (filled by the video thread)                 */

static volatile u32 g_videoFrames = 0;    /* bumps on every received packet */
static volatile bool g_zoom = true;   /* enlarged is the default */
static u32 g_menuKeys = 0;            /* key presses for the game list (g_lock) */
static int g_videoFd = -1;

/* receipt of video packets, reported back to the PC in every input packet */
static LightLock g_ackLock;
static u32 g_ackSeq = 0;
static u64 g_ackMask = 0;
static bool g_ackValid = false;

static void noteVideoSeq(u32 seq)
{
    LightLock_Lock(&g_ackLock);
    /* a big jump backwards means melonDS was restarted: start over */
    if (!g_ackValid || (s32)(seq - g_ackSeq) < -4096)
    {
        g_ackSeq = seq;
        g_ackMask = 1;
        g_ackValid = true;
    }
    else if ((s32)(seq - g_ackSeq) > 0)
    {
        u32 d = seq - g_ackSeq;
        g_ackMask = (d >= 64) ? 0 : (g_ackMask << d);
        g_ackMask |= 1;
        g_ackSeq = seq;
    }
    else
    {
        u32 d = g_ackSeq - seq;
        if (d < 64) g_ackMask |= (1ULL << d);
    }
    LightLock_Unlock(&g_ackLock);
}

static void decodeTile(int type, const u8* p, int tx, int ty, int W)
{
    u16* dst = &g_dsBack[ty * W + tx];
    switch (type)
    {
    case LINK_TILE_RAW:
        for (int y = 0; y < 8; y++) memcpy(dst + y * W, p + y * 16, 16);
        break;
    case LINK_TILE_SOLID_T:
    {
        u16 c; memcpy(&c, p, 2);
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) dst[y * W + x] = c;
        break;
    }
    case LINK_TILE_PAL2:
    {
        u16 pal[2]; memcpy(pal, p, 4);
        const u8* bits = p + 4;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) dst[y * W + x] = pal[(bits[y] >> x) & 1];
        break;
    }
    case LINK_TILE_PAL4:
    {
        u16 pal[4]; memcpy(pal, p, 8);
        for (int y = 0; y < 8; y++)
        {
            u16 row; memcpy(&row, p + 8 + y * 2, 2);
            for (int x = 0; x < 8; x++) dst[y * W + x] = pal[(row >> (x * 2)) & 3];
        }
        break;
    }
    case LINK_TILE_QUARTER:
    {
        u16 px[4]; memcpy(px, p, 8);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) dst[y * W + x] = px[(y >> 2) * 2 + (x >> 2)];
        break;
    }
    case LINK_TILE_HALF:
    {
        u16 px[16]; memcpy(px, p, 32);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) dst[y * W + x] = px[(y >> 1) * 4 + (x >> 1)];
        break;
    }
    case LINK_TILE_PAL16:
    {
        u16 pal[16]; memcpy(pal, p, 32);
        for (int y = 0; y < 8; y++)
        {
            u32 row; memcpy(&row, p + 32 + y * 4, 4);
            for (int x = 0; x < 8; x++) dst[y * W + x] = pal[(row >> (x * 4)) & 15];
        }
        break;
    }
    }
}

static int tilePayloadSize(int type)
{
    switch (type)
    {
    case LINK_TILE_RAW: return 128;
    case LINK_TILE_SOLID_T: return 2;
    case LINK_TILE_PAL2: return 12;
    case LINK_TILE_PAL4: return 24;
    case LINK_TILE_PAL16: return 64;
    case LINK_TILE_HALF: return 32;
    case LINK_TILE_QUARTER: return 8;
    case LINK_TILE_META: return 4;
    default: return -1;
    }
}

/* show the decoded image; called when a frame is complete */
#ifdef LINK_HOST_TEST
void link_test_on_present(const u16* img);   /* only for the PC simulation */
#endif

static u32 g_tileTs = 0;   /* the PC's time of the tile picture being received (0 = none) */
static u64 rotDue(u32 ts, u64 now);
static void presentFrame(void)
{
    /* into the same ring as the H.264 pictures: the main loop shows it when due */
    u32 no = g_rotWritten + 1;
    int slot = (int)(no % ROT_SLOTS);
    u64 now = osGetTime();
    memcpy(g_rotImage[slot], g_dsBack, sizeof(g_dsBack));
    g_rotMade[slot] = now;
    /* the same picture again (refined, refreshed): at once, and it must not count as a
       delay of the WiFi in rotDue */
    static u32 lastTs = 0;
    g_rotDue[slot] = g_tileTs && g_tileTs == lastTs ? now : rotDue(g_tileTs, now);
    lastTs = g_tileTs;
    g_rotSrc[slot] = g_tileTs;
    __sync_synchronize();
    g_rotWritten = no;
    g_tileShown++;
#ifdef LINK_HOST_TEST
    link_test_on_present(g_dsBack);
#endif
}

/* which packet seqs have arrived (ring, slot holds the seq that filled it) */
#define SEQ_RING 4096
static u32 g_seqRing[SEQ_RING];
static bool g_seqRingInit = false;

static void markReceived(u32 seq)
{
    if (!g_seqRingInit)
    {
        /* slot i can only ever hold seqs with (seq & mask) == i, so i + 1 means "empty" */
        for (int i = 0; i < SEQ_RING; i++) g_seqRing[i] = (u32)i + 1;
        g_seqRingInit = true;
    }
    g_seqRing[seq & (SEQ_RING - 1)] = seq;
}

static bool haveAll(u32 from, u32 to)
{
    if ((s32)(to - from) < 0 || to - from >= SEQ_RING) return false;
    for (u32 s = from; s != to + 1; s++)
        if (g_seqRing[s & (SEQ_RING - 1)] != s) return false;
    return true;
}

/* ---- H.264: put the packets of a frame together, decode it in hardware ---- */
#define H264_MAX_PARTS 160
static u8 g_h264Buf[H264_MAX_PARTS * LINK_H264_PART];
static u8 g_h264Have[H264_MAX_PARTS];
static u32 g_h264Frame = 0;
static bool g_h264Open = false, g_h264Key = false;
static volatile bool g_h264Broken = true;   /* waits for a key frame (asked for in the input packets) */
static bool g_h264Clean = false;   /* a key frame was decoded since the decoder started */
/* option: decode on after a lost frame (smoother; where it moved, the picture is slightly
   wrong until the key frame the 3DS asks for at once arrives) */
static volatile bool g_allowGlitch = true;
static u32 g_h264Ts = 0;           /* the PC's time of the frame being received (0 = none) */
static int g_h264Got = 0, g_h264Total = -1;
static u32 g_h264Size = 0;

/* When to show a picture the PC made at `ts` (its clock, ms) and that is decoded now. The
   delay from the PC to here varies (WiFi, decoding): each picture waits until the delay that
   97 of 100 recent pictures had - then they come out as evenly as they were made. */
static u64 rotDue(u32 ts, u64 now)
{
    static bool have = false;
    static u32 base;
    static s32 win[64], tmp[64];
    static int n = 0, pos = 0;
    if (!ts) return now;
    u32 d = (u32)now - ts;
    s32 rel = (s32)(d - base);
    if (!have || rel < -5000 || rel > 5000) { have = true; base = d; rel = 0; n = 0; pos = 0; }
    win[pos] = rel;
    pos = (pos + 1) % 64;
    if (n < 64) n++;
    for (int i = 0; i < n; i++)   /* insertion sort, 64 values */
    {
        s32 v = win[i];
        int j = i;
        while (j > 0 && tmp[j - 1] > v) { tmp[j] = tmp[j - 1]; j--; }
        tmp[j] = v;
    }
    /* 97 of 100 recent pictures were here by then (tiles vary more in size than H.264) */
    s32 lo = tmp[0], hi = tmp[(n * 97) / 100 < n ? (n * 97) / 100 : n - 1];
    if (hi - lo > 50) hi = lo + 50;   /* never more than 50 ms extra */
    /* the wait itself only moves slowly: else its own wobble makes the pictures uneven */
    static s32 cur;
    if (n == 1) cur = hi;
    else if (hi > cur) cur += hi - cur > 2 ? 2 : hi - cur;
    else if (hi < cur) cur -= 1;
    return cur > rel ? now + (u64)(cur - rel) : now;
}

static bool h264Decode(u32 size)
{
    const u8* p = g_h264Buf;
    const u8* end = g_h264Buf + size;
    /* the NAL units, each after a start code 00 00 01 (or 00 00 00 01) */
    const u8* nal = NULL;
    for (const u8* q = p; ; q++)
    {
        bool startCode = q + 3 <= end && q[0] == 0 && q[1] == 0 && q[2] == 1;
        if (startCode || q >= end)
        {
            if (nal)
            {
                const u8* nalEnd = q;
                if (startCode && nalEnd > nal && nalEnd[-1] == 0) nalEnd--;   /* 4-byte code */
                size_t len = (size_t)(nalEnd - nal);
                if (len > 0 && len <= 0x40000)
                {
                    memcpy(g_mvdIn, nal, len);
                    GSPGPU_FlushDataCache(g_mvdIn, len);
                    MVDSTD_ProcessNALUnitOut out;
                    Result ret = mvdstdProcessVideoFrame(g_mvdIn, len, 0, &out);
                    if (!MVD_CHECKNALUPROC_SUCCESS(ret)) { g_h264Errors++; return false; }
                    if (ret != MVD_STATUS_PARAMSET && ret != MVD_STATUS_INCOMPLETEPROCESSING)
                    {
                        g_mvdConfig.physaddr_outdata0 = osConvertVirtToPhys(g_mvdOut);
                        if (mvdstdRenderVideoFrame(&g_mvdConfig, true) != MVD_STATUS_OK) { g_h264Errors++; return false; }
                        GSPGPU_InvalidateDataCache(g_mvdOut, LINK_H264_WIDTH * LINK_H264_HEIGHT * 2);
                        u32 no = g_rotWritten + 1;
                        int slot = (int)(no % ROT_SLOTS);
                        u64 now = osGetTime();
                        memcpy(g_rotImage[slot], g_mvdOut, sizeof(g_rotImage[0]));
                        g_rotMade[slot] = now;
                        static u32 lastTs = 0;   /* the same picture again: at once */
                        g_rotDue[slot] = g_h264Ts && g_h264Ts == lastTs ? now : rotDue(g_h264Ts, now);
                        lastTs = g_h264Ts;
                        g_rotSrc[slot] = g_h264Ts;
                        __sync_synchronize();
                        g_rotWritten = no;
                        g_rotMode = true;
                        g_h264Shown++;
                    }
                }
            }
            if (q >= end) break;
            nal = q + 3;
            q += 2;
        }
    }
    return true;
}

#define H264_MAX_GROUPS LINK_H264_MAX_PARITY
static u8 g_h264Par[H264_MAX_GROUPS][LINK_H264_PART];
static bool g_h264ParHave[H264_MAX_GROUPS];
static int g_h264Groups = 0;          /* parity packets of this frame (0 = none seen) */
static u32 g_h264LastLen = 0, g_h264SeqBase = 0;
static bool g_h264SeqKnown = false;

/* Reed-Solomon: rebuild up to m missing data packets from the parity packets */
/* Reed-Solomon (see LinkProto.h): rebuilds missing data packets 0..k-1 (`part` bytes each,
   in buf, have[i] = 0 if missing) from the parity rows that arrived. Returns how many it
   rebuilt (their indices in `rebuilt`), 0 if there is not enough parity yet. */
#define RS_MAX_ROWS 15
static int rsRebuild(u8* buf, int part, u8* have, int k, const u8* par, const bool* parHave, int m,
                     int* rebuilt)
{
    if (m <= 0 || m > RS_MAX_ROWS || k <= 0) return 0;
    int miss[RS_MAX_ROWS], rows[RS_MAX_ROWS], e = 0, r = 0;
    for (int i = 0; i < k; i++)
        if (!have[i]) { if (e == RS_MAX_ROWS) return 0; miss[e++] = i; }
    for (int j = 0; j < m && r < e; j++)
        if (parHave[j]) rows[r++] = j;
    if (e == 0 || r < e) return 0;   /* not enough parity yet */

    /* A[r][c] = coefficient of missing packet c in parity row r; invert it */
    u8 A[RS_MAX_ROWS][2 * RS_MAX_ROWS];
    for (int i = 0; i < e; i++)
        for (int c = 0; c < 2 * e; c++)
            A[i][c] = c < e ? linkCauchy(rows[i], miss[c], m) : (c - e == i);
    for (int c = 0; c < e; c++)
    {
        int p = c;
        while (p < e && !A[p][c]) p++;
        if (p == e) return 0;   /* cannot happen with a Cauchy matrix */
        if (p != c)
            for (int x = 0; x < 2 * e; x++) { u8 t = A[p][x]; A[p][x] = A[c][x]; A[c][x] = t; }
        u8 inv = linkGfInv(A[c][c]);
        for (int x = 0; x < 2 * e; x++) A[c][x] = linkGfMul(A[c][x], inv);
        for (int i = 0; i < e; i++)
        {
            if (i == c || !A[i][c]) continue;
            u8 f = A[i][c];
            for (int x = 0; x < 2 * e; x++) A[i][x] ^= linkGfMul(f, A[c][x]);
        }
    }
    /* syndromes: parity minus what the packets that arrived contribute */
    static u8 syn[RS_MAX_ROWS][LINK_FRAME_MAX_BYTES];
    u8 tab[256];
    for (int i = 0; i < e; i++)
    {
        memcpy(syn[i], par + (size_t)rows[i] * part, (size_t)part);
        for (int d = 0; d < k; d++)
        {
            if (!have[d]) continue;
            u8 c = linkCauchy(rows[i], d, m);
            for (int x = 0; x < 256; x++) tab[x] = linkGfMul(c, (u8)x);
            const u8* src = buf + (size_t)d * part;
            for (int b = 0; b < part; b++) syn[i][b] ^= tab[src[b]];
        }
    }
    /* missing packet c = sum over r of inverse[c][r] * syndrome r */
    for (int c = 0; c < e; c++)
    {
        u8* dst = buf + (size_t)miss[c] * part;
        memset(dst, 0, (size_t)part);
        for (int i = 0; i < e; i++)
        {
            u8 f = A[c][e + i];
            if (!f) continue;
            for (int x = 0; x < 256; x++) tab[x] = linkGfMul(f, (u8)x);
            for (int b = 0; b < part; b++) dst[b] ^= tab[syn[i][b]];
        }
        have[miss[c]] = 1;
        rebuilt[c] = miss[c];
    }
    return e;
}

static void h264Repair(void)
{
    int k = g_h264Total, m = g_h264Groups;
    if (m <= 0 || k <= 0 || k > H264_MAX_PARTS) return;
    int rebuilt[RS_MAX_ROWS];
    int e = rsRebuild(g_h264Buf, LINK_H264_PART, g_h264Have, k, &g_h264Par[0][0], g_h264ParHave, m, rebuilt);
    for (int c = 0; c < e; c++)
    {
        g_h264Got++;
        g_h264Repaired++;
        if (rebuilt[c] == k - 1) g_h264Size = (u32)rebuilt[c] * LINK_H264_PART + g_h264LastLen;
        if (g_h264SeqKnown) noteVideoSeq(g_h264SeqBase + (u32)rebuilt[c]);   /* as if it arrived */
    }
}

static void h264Packet(const LinkFramePacket* fp, const u8* data, size_t avail)
{
    if (g_h264Reset)
    {
        g_h264Reset = false;
        g_h264Frame = 0;
        g_h264Open = false;
        g_h264Broken = true;   /* the decoder needs a key frame first */
        g_h264Clean = false;
    }
    if (g_h264Open || g_h264Frame != 0)
    {
        s32 d = (s32)(fp->frameId - g_h264Frame);
        /* far back: Azahar was restarted and counts from 1 again */
        if (d < -1000) { g_h264Frame = 0; g_h264Open = false; g_h264Broken = true; g_h264Clean = false; d = 1; }
        if (d < 0) return;   /* a late packet of an older frame */
        if (d > 0)
        {
            if (g_h264Open)
            {
                /* never complete: the picture stands until the next key frame */
                if (!g_h264Broken) g_stallSince = osGetTime();
                g_h264Broken = true;
                g_h264Dropped++;
            }
            g_h264Open = false;
        }
    }
    if (!g_h264Open && fp->frameId != g_h264Frame)
    {
        g_h264Frame = fp->frameId;
        g_h264Open = true;
        memset(g_h264Have, 0, sizeof(g_h264Have));
        memset(g_h264ParHave, 0, sizeof(g_h264ParHave));
        g_h264Groups = 0;
        g_h264Got = 0;
        g_h264Total = -1;
        g_h264Size = 0;
        g_h264SeqKnown = false;
        g_h264Key = (fp->flags & LINK_FRAME_KEY) != 0;
    }
    if (!g_h264Open) return;   /* this frame is done already */
    u32 idx = (fp->flags & 0x7FFF) >> LINK_FRAME_INDEX_SHIFT;
    u32 len = fp->entryCount;
    if (fp->baseSeq & LINK_H264_PARITY)
    {
        /* a parity packet */
        int parts = (int)((fp->baseSeq >> 8) & 0xFFFF);
        int groups = (int)((fp->baseSeq >> 4) & 0xF);
        int g = (int)(fp->baseSeq & 0xF);   /* parity row j */
        if (parts <= 0 || parts > H264_MAX_PARTS || groups < 1 || groups > H264_MAX_GROUPS || g >= groups ||
            avail < LINK_H264_PART || len > LINK_H264_PART) return;
        memcpy(g_h264Par[g], data, LINK_H264_PART);
        g_h264ParHave[g] = true;
        g_h264Groups = groups;
        g_h264Total = parts;
        g_h264LastLen = len;
        if (!g_h264SeqKnown) { g_h264SeqBase = fp->seq - (u32)(parts + g); g_h264SeqKnown = true; }
    }
    else
    {
        if (idx >= H264_MAX_PARTS || len > avail || len > LINK_H264_PART) return;
        if (!g_h264Have[idx])
        {
            u8* dst = g_h264Buf + idx * LINK_H264_PART;
            memcpy(dst, data, len);
            if (len < LINK_H264_PART) memset(dst + len, 0, LINK_H264_PART - len);   /* for the parity */
            g_h264Have[idx] = 1;
            g_h264Got++;
        }
        if (!g_h264SeqKnown) { g_h264SeqBase = fp->seq - idx; g_h264SeqKnown = true; }
        g_h264Ts = fp->baseSeq;   /* data packets: the PC's time of the frame (LinkProto.h) */
        if (fp->flags & LINK_FRAME_LAST)
        {
            g_h264Total = (int)idx + 1;
            g_h264Size = idx * LINK_H264_PART + len;
            g_h264LastLen = len;
        }
    }
    if (g_h264Total > 0 && g_h264Got < g_h264Total) h264Repair();
    if (g_h264Total > 0 && g_h264Got >= g_h264Total && g_h264Size > 0)
    {
        g_h264Open = false;
        /* a frame got lost: wait for a key frame - or, if glitches are allowed, decode on
           (the picture is slightly wrong where it moved until the key frame arrives) */
        if (g_h264Broken && !g_h264Key && !(g_allowGlitch && g_h264Clean)) { g_h264Dropped++; return; }
        if (g_h264Broken && !g_h264Key) g_h264Glitched++;
        bool ok = h264Decode(g_h264Size);
        if (!ok)
        {
            if (!g_h264Broken) g_stallSince = osGetTime();
            g_h264Broken = true;
        }
        else if (g_h264Key)
        {
            if (g_h264Broken && g_stallSince)
            {
                u32 ms = (u32)(osGetTime() - g_stallSince);
                g_stallCount++;
                g_stallTotalMs += ms;
                if (ms > g_stallMaxMs) g_stallMaxMs = ms;
            }
            g_stallSince = 0;
            g_h264Broken = false;
            g_h264Clean = true;
        }
    }
}

/* ---- the picture packets: one thread only takes them from the socket (quickly, at
   a high priority, so the small socket buffer never runs over while a frame is
   decoded), the other decodes them ---- */
#define VRING 384
#define VSLOT 1424
static u8 g_vring[VRING][VSLOT];
static u16 g_vringLen[VRING];
static volatile u32 g_vringHead = 0, g_vringTail = 0;   /* written by receiver / decoder */

static void videoRecvThread(void* arg)
{
    (void)arg;
    u32 gen = g_sockGen;
    {
        int v = 0;
        socklen_t l = sizeof(v);
        if (g_videoFd >= 0 && getsockopt(g_videoFd, SOL_SOCKET, SO_RCVBUF, &v, &l) == 0) g_vidRcvBuf = (u32)v;
    }
    while (g_running)
    {
        if (gen != g_sockGen)
        {
            gen = g_sockGen;
            if (g_videoFd >= 0) close(g_videoFd);
            g_videoFd = openUdpSocket(LINK_VIDEO_PORT, 128 * 1024);
        }
        if (g_netPause) { svcSleepThread(20000000); continue; }
        /* big transfer or SD card write: leave the socket service alone, even
           if packets keep coming (they wait in the buffer or get resent) */
        if (g_bulkActive) { svcSleepThread(20000000); continue; }
        int got = 0;
        while (got < 64)
        {
            u32 head = g_vringHead;
            u32 next = (head + 1) % VRING;
            static u8 spill[VSLOT];
            bool full = next == __atomic_load_n(&g_vringTail, __ATOMIC_ACQUIRE);
            u8* dst = full ? spill : g_vring[head];
            ssize_t n = recvfrom(g_videoFd, dst, VSLOT, 0, NULL, NULL);
            if (n <= 0) break;
            got++;
            if (n == 8)
            {
                /* the PC's echo of an input packet: how long the round trip took */
                u32 msg[2];
                memcpy(msg, dst, 8);
                if (msg[0] == LINK_ECHO_MAGIC)
                {
                    u64 sent = g_inSentTick[msg[1] & 255];
                    u64 us = sent ? (svcGetSystemTick() - sent) * 1000000ULL / SYSCLOCK_ARM11 : 0;
                    if (sent && us < 2000000)
                    {
                        g_rttCount++;
                        g_rttSumUs += (u32)us;
                        if (us > g_rttMaxUs) g_rttMaxUs = (u32)us;
                    }
                    continue;
                }
            }
            if ((size_t)n >= sizeof(LinkFramePacket))
            {
                /* receipts at once, not only after decoding */
                LinkFramePacket fp;
                memcpy(&fp, dst, sizeof(fp));
                if (fp.magic == LINK_H264_MAGIC || fp.magic == LINK_FRAME_MAGIC || fp.magic == LINK_FRAMEZ_MAGIC)
                    noteVideoSeq(fp.seq);
                g_lastVideoTime = osGetTime();
            }
            if (full) { g_vidOverruns++; continue; }   /* the decoder is far behind */
            g_vringLen[head] = (u16)n;
            __atomic_store_n(&g_vringHead, next, __ATOMIC_RELEASE);
        }
        /* nothing (more) there: sleep in our own thread (never block inside the
           shared socket service, see sendAll) */
        if (got == 0) svcSleepThread(500000);   /* 0.5 ms */
    }
}

/* ---- tiles: a frame's packets, kept for the Reed-Solomon repair ---- */
static u8 g_tpBuf[LINK_TILE_MAX_PACKETS * LINK_FRAME_MAX_BYTES];
static u8 g_tpHave[LINK_TILE_MAX_PACKETS];
static u8 g_tpPar[LINK_TILE_MAX_PARITY * LINK_FRAME_MAX_BYTES];
static bool g_tpParHave[LINK_TILE_MAX_PARITY];
static u32 g_tpFrame = 0;
static int g_tpK = -1, g_tpM = 0;
/* the frame being put together */
static u32 g_tfCur = 0;
static bool g_tfOpen = false, g_tfHaveLast = false;
static u32 g_tfLastSeq = 0, g_tfBaseSeq = 0;
static u64 g_tfLastPresent = 0;

static void tpReset(u32 frameId)
{
    g_tpFrame = frameId;
    g_tpK = -1;
    g_tpM = 0;
    memset(g_tpHave, 0, sizeof(g_tpHave));
    memset(g_tpParHave, 0, sizeof(g_tpParHave));
}

/* one packet of tiles (arrived, or rebuilt): draw it, and show the frame once consistent */
static void tileData(const u8* buf, size_t n)
{
    LinkFramePacket fp;
    memcpy(&fp, buf, sizeof(fp));
    if (fp.frameId != g_tfCur)
    {
        /* a frame that never became consistent is simply not shown;
           its lost parts are repeated in a later frame */
        g_tfCur = fp.frameId;
        g_tfHaveLast = false;
    }
    if (g_tfLastPresent == 0) g_tfLastPresent = osGetTime();
    g_tfOpen = true;

    const u8* p = buf + sizeof(fp);
    const u8* end = buf + n;
    /* read the size once: it may change (other emulator) while we decode */
    const int vw = g_vidW, vh = g_vidH;
    const int tilesX = vw / 8, tilesY = vh / 8;
    for (u32 e = 0; e < fp.entryCount; e++)
    {
        if (p + 2 > end) break;
        u16 head;
        memcpy(&head, p, 2);
        p += 2;
        int idx = head & LINK_TILE_INDEX_MASK;
        int type = head >> LINK_TILE_TYPE_SHIFT;
        int size = tilePayloadSize(type);
        if (size < 0 || p + size > end) break;
        if (type == LINK_TILE_META)
        {
            if (idx == 0) memcpy(&g_tileTs, p, 4);   /* when the PC made the picture */
            p += size;
            continue;
        }
        if (idx >= tilesX * tilesY) break;
        decodeTile(type, p, (idx % tilesX) * 8, (idx / tilesX) * 8, vw);
        p += size;
    }
    markReceived(fp.seq);

    if (fp.flags & LINK_FRAME_LAST)
    {
        g_tfHaveLast = true;
        g_tfLastSeq = fp.seq;
        g_tfBaseSeq = fp.baseSeq;
    }
    if (g_tfHaveLast && haveAll(g_tfBaseSeq, g_tfLastSeq))
    {
        presentFrame();
        g_tfOpen = false;
        g_tfHaveLast = false;
        g_tfLastPresent = osGetTime();
    }
}

/* a packet of tiles, packed (LINK_FRAMEZ_MAGIC) or not */
static void tileDataAny(const u8* buf, size_t n)
{
    LinkFramePacket fp;
    memcpy(&fp, buf, sizeof(fp));
    if (fp.magic != LINK_FRAMEZ_MAGIC)
    {
        tileData(buf, n);
        return;
    }
    static u8 un[sizeof(LinkFramePacket) + 4096];
    if (n < sizeof(fp) + 2) return;
    u16 rawLen;
    memcpy(&rawLen, buf + sizeof(fp), 2);
    if (rawLen > 4096) return;
    int got = LZ4_decompress_safe((const char*)buf + sizeof(fp) + 2, (char*)un + sizeof(fp),
                                  (int)(n - sizeof(fp) - 2), 4096);
    if (got != (int)rawLen) return;
    fp.magic = LINK_FRAME_MAGIC;
    memcpy(un, &fp, sizeof(fp));
    tileData(un, sizeof(fp) + (size_t)got);
}

/* packets of the current frame got lost: rebuild them from the parity if it suffices */
static void tileRepair(u32 seqBase)
{
    if (g_tpK <= 0 || g_tpM <= 0) return;
    int rebuilt[RS_MAX_ROWS];
    int e = rsRebuild(g_tpBuf, LINK_FRAME_MAX_BYTES, g_tpHave, g_tpK, g_tpPar, g_tpParHave, g_tpM, rebuilt);
    for (int c = 0; c < e; c++)
    {
        const u8* pk = g_tpBuf + (size_t)rebuilt[c] * LINK_FRAME_MAX_BYTES;
        LinkFramePacket fp;
        memcpy(&fp, pk, sizeof(fp));
        /* a sane packet of this frame (else the parity was not for these packets) */
        if ((fp.magic != LINK_FRAME_MAGIC && fp.magic != LINK_FRAMEZ_MAGIC) || fp.frameId != g_tpFrame ||
            fp.seq != seqBase + (u32)rebuilt[c])
            continue;
        g_tileRebuilt++;
        noteVideoSeq(fp.seq);   /* confirmed as if it had arrived */
        tileDataAny(pk, LINK_FRAME_MAX_BYTES);
    }
}

static void videoThread(void* arg)
{
    (void)arg;
    static u8 buf[VSLOT];

    while (g_running)
    {
        ssize_t n = 0;
        u32 tail = g_vringTail;
        if (tail != __atomic_load_n(&g_vringHead, __ATOMIC_ACQUIRE))
        {
            n = g_vringLen[tail];
            memcpy(buf, g_vring[tail], (size_t)n);
            __atomic_store_n(&g_vringTail, (tail + 1) % VRING, __ATOMIC_RELEASE);
        }
        if (n <= 0)
        {
            /* safety net: never freeze, even if consistency cannot be reached */
            if (g_tfOpen && osGetTime() - g_tfLastPresent > 250)
            {
                g_tileForced++;
                presentFrame();
                g_tfOpen = false;
                g_tfLastPresent = osGetTime();
            }
            svcSleepThread(g_bulkActive ? 20000000 : 1000000);   /* 20 ms / 1 ms */
            continue;
        }
        if ((size_t)n < sizeof(LinkFramePacket)) continue;
        LinkFramePacket fp;
        memcpy(&fp, buf, sizeof(fp));
        if (fp.magic == LINK_H264_MAGIC)
        {
            if (g_mvdReady) h264Packet(&fp, buf + sizeof(fp), (size_t)n - sizeof(fp));
            continue;
        }
        if (fp.magic == LINK_TILEPAR_MAGIC)
        {
            /* parity for the tiles of frame fp.frameId */
            int k = (int)((fp.baseSeq >> 8) & 0xFFFF), m = (int)((fp.baseSeq >> 4) & 0xF), j = (int)(fp.baseSeq & 0xF);
            if (k <= 0 || k > LINK_TILE_MAX_PACKETS || m <= 0 || m > LINK_TILE_MAX_PARITY || j >= m ||
                (size_t)n < sizeof(fp) + LINK_FRAME_MAX_BYTES)
                continue;
            if (fp.frameId != g_tpFrame)
            {
                if ((s32)(fp.frameId - g_tpFrame) < 0 && g_tpFrame != 0) continue;   /* an older frame */
                tpReset(fp.frameId);
            }
            g_tpK = k;
            g_tpM = m;
            memcpy(g_tpPar + (size_t)j * LINK_FRAME_MAX_BYTES, buf + sizeof(fp), LINK_FRAME_MAX_BYTES);
            g_tpParHave[j] = true;
            tileRepair(fp.seq);
            continue;
        }
        if (fp.magic != LINK_FRAME_MAGIC && fp.magic != LINK_FRAMEZ_MAGIC) continue;
        g_rotMode = false;   /* tiles again */

        /* keep it for a repair of the frame */
        u32 idx = (fp.flags & 0x7FFF) >> LINK_FRAME_INDEX_SHIFT;
        if (fp.frameId != g_tpFrame && (s32)(fp.frameId - g_tpFrame) > 0) tpReset(fp.frameId);
        else if (fp.frameId != g_tpFrame && g_tpFrame == 0) tpReset(fp.frameId);
        if (fp.frameId == g_tpFrame && idx < LINK_TILE_MAX_PACKETS && (size_t)n <= LINK_FRAME_MAX_BYTES)
        {
            u8* dst = g_tpBuf + (size_t)idx * LINK_FRAME_MAX_BYTES;
            memcpy(dst, buf, (size_t)n);
            memset(dst + n, 0, LINK_FRAME_MAX_BYTES - (size_t)n);
            g_tpHave[idx] = 1;
        }
        tileDataAny(buf, (size_t)n);
        if (fp.frameId == g_tpFrame) tileRepair(fp.seq - idx);
    }
}

/* ------------------------------------------------------------------ */
/* input thread: samples HID every 1 ms and sends it straight away     */

typedef struct
{
    u32 keys;
    touchPosition touch;
    bool dsTouch;
    u16 dsX, dsY;
    bool quit;
    bool haveUdpPeer;
} InputState;

static InputState g_input;    /* snapshot for the UI, guarded by g_lock */
static int g_udpFd = -1;

static bool mapTouch(u16 px, u16 py, bool zoom, u16* dx, u16* dy)
{
    if (zoom)
    {
        /* 320x240 -> 256x192 (factor 0.8) */
        int x = px * 4 / 5, y = py * 4 / 5;
        if (x > 255) x = 255;
        if (y > 191) y = 191;
        *dx = (u16)x;
        *dy = (u16)y;
        return true;
    }
    int x = (int)px - LINK_DS_OFFSET_X, y = (int)py - LINK_DS_OFFSET_Y;
    if (x < 0 || x >= 256 || y < 0 || y >= 192) return false;
    *dx = (u16)x;
    *dy = (u16)y;
    return true;
}

static void inputThread(void* arg)
{
    (void)arg;
    struct sockaddr_in peer;
    bool havePeer = false;
    u64 lastHello = 0;
    u32 packetId = 0;
    int quitHold = 0;
    bool comboSeen = false, quitSeen = false;
    bool cstickUpWas = false;
    u32 prevHeld = 0;
    int heloCheck = 0;
    u64 upSince[32];          /* per key: since when it is released */
    for (int i = 0; i < 32; i++) upSince[i] = 0;
    u32 gen = g_sockGen;
    LinkInputPacket lastSent;
    memset(&lastSent, 0, sizeof(lastSent));
    u64 lastSendTime = 0;
    int repeatSends = 0;

    while (g_running)
    {
        u64 start = osGetTime();
        if (g_netPause) { svcSleepThread(10000000); continue; }
        if (gen != g_sockGen)
        {
            /* woke up: new socket, melonDS says hello again */
            gen = g_sockGen;
            if (g_udpFd >= 0) close(g_udpFd);
            g_udpFd = openUdpSocket(LINK_UDP_PORT, 0);
            markInputPriority(g_udpFd);
            havePeer = false;
        }

        hidScanInput();
        u32 held = hidKeysHeld();

        /* SELECT + START: melonDS Link menu (on release).
           L + R + SELECT + START held ~1 s: quit - hard to do by accident */
        bool combo = (held & KEY_SELECT) && (held & KEY_START);
        bool quitCombo = combo && (held & KEY_L) && (held & KEY_R);
        if (combo) comboSeen = true;
        if (quitCombo) { quitHold++; quitSeen = true; }
        else quitHold = 0;
        if (!(held & (KEY_SELECT | KEY_START)))
        {
            if (comboSeen && !quitSeen) g_menuToggle = true;
            comboSeen = false;
            quitSeen = false;
        }

        /* C-stick up toggles zoom */
        (void)cstickUpWas;

        /* presses for the menus: a key only counts as pressed again after it was
           really released (>= 60 ms). Reading the buttons can now and then give
           one stale sample, which would otherwise count as a second press. */
        u32 down = 0;
        {
            u64 now = osGetTime();
            u32 rawDown = held & ~prevHeld, rawUp = prevHeld & ~held;
            for (int b = 0; b < 32; b++)
            {
                u32 bit = 1u << b;
                if (rawUp & bit) upSince[b] = now;
                if ((rawDown & bit) && now - upSince[b] >= 60) down |= bit;
            }
        }
        prevHeld = held;
        /* L + R + A: confirmation for things that cannot be undone */
        if ((down & KEY_A) && (held & KEY_L) && (held & KEY_R)) g_confirmPress = true;

        /* C-stick down opens/closes the game list */
        bool dialog = g_askActive || g_localAsk != LA_NONE;
        bool capture = g_menuOpen || g_linkMenuOpen || g_bakOpen || dialog;
        /* with a 3DS game (Azahar) the C-stick belongs to the game */
        bool native = NATIVE();
        if ((down & KEY_CSTICK_UP) && !dialog && !native) g_zoom = !g_zoom;
        if ((down & KEY_CSTICK_DOWN) && !dialog && !native)
        {
            if (!g_menuOpen) { g_menuOpen = true; g_linkMenuOpen = false; g_bakOpen = false; }
            else if (g_gameStarted) g_menuOpen = false;
        }
        else if (capture)
        {
            LightLock_Lock(&g_lock);
            g_menuKeys |= down;
            LightLock_Unlock(&g_lock);
        }
        /* while a list, the menu or a question is shown, the game gets no input */
        u32 gameKeys = capture ? 0 : held;
        if (combo) gameKeys &= ~(KEY_SELECT | KEY_START | (quitCombo ? (KEY_L | KEY_R) : 0));

        /* HELO from the PC (keep-alive, once a second): checking every 50 ms
           is plenty and keeps the socket service free for the transfers */
        if (!g_bulkActive && ++heloCheck >= 48)   /* every ~48 ms (loop: 1 ms) */
        for (heloCheck = 0;;)
        {
            uint32_t msg = 0;
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(g_udpFd, &msg, sizeof(msg), 0, (struct sockaddr*)&from, &flen);
            if (n <= 0) break;
            if (n == sizeof(msg) && msg == LINK_UDP_HELLO)
            {
                peer = from;
                havePeer = true;
                lastHello = osGetTime();
            }
        }
        if (g_bulkActive) lastHello = osGetTime();   /* we were not looking */
        if (havePeer && osGetTime() - lastHello > 5000) havePeer = false;

        touchPosition touch;
        circlePosition cpad, cstick;
        hidTouchRead(&touch);
        hidCircleRead(&cpad);
        irrstCstickRead(&cstick);

        u16 dsX = 0, dsY = 0;
        bool dsTouch = !native && (gameKeys & KEY_TOUCH) && mapTouch(touch.px, touch.py, g_zoom, &dsX, &dsY);

        if (havePeer && !g_bulkActive)
        {
            LinkInputPacket p;
            memset(&p, 0, sizeof(p));
            p.magic = LINK_MAGIC;
            p.keys = gameKeys;
            p.cpadX = cpad.dx;
            p.cpadY = cpad.dy;
            p.touchX = touch.px;
            p.touchY = touch.py;
            p.cstickX = cstick.dx;
            p.cstickY = cstick.dy;
            p.dsTouchX = dsX;
            p.dsTouchY = dsY;
            p.flags = (dsTouch ? LINK_INPUT_DSTOUCH : 0) | (g_zoom ? LINK_INPUT_ZOOM : 0);
            p.status = (u32)g_battery | ((u32)g_wifi << 8) | (g_charging ? LINK_STATUS_CHARGING : 0) |
                       (g_lidClosed ? LINK_STATUS_LIDCLOSED : 0);
            LightLock_Lock(&g_ackLock);
            if (g_ackValid)
            {
                p.flags |= LINK_INPUT_VIDEOACK;
                p.videoAckSeq = g_ackSeq;
                p.videoAckMask = g_ackMask;
            }
            LightLock_Unlock(&g_ackLock);
            /* a frame could not be rebuilt: ask for a key frame right away (a changed flag
               goes out at once) instead of letting the PC work it out from the receipts */
            if (g_rotMode && g_h264Broken && g_tcpConnected) p.flags |= LINK_INPUT_NEEDKEY;

            /* Only send what is new: a change of the input goes out at once (and
               twice more, in case one gets lost on the WiFi); receipts for the
               picture at most every 12 ms; otherwise a sign of life every 50 ms.
               Saves most of the 1000 packets a second (WiFi time and battery). */
            u64 now = osGetTime();
            #define NEAR(a, b) ((a) - (b) <= 2 && (b) - (a) <= 2)
            bool inputChanged = p.keys != lastSent.keys || p.status != lastSent.status ||
                (p.flags & ~LINK_INPUT_VIDEOACK) != (lastSent.flags & ~LINK_INPUT_VIDEOACK) ||
                p.touchX != lastSent.touchX || p.touchY != lastSent.touchY ||
                p.dsTouchX != lastSent.dsTouchX || p.dsTouchY != lastSent.dsTouchY;
            /* the sticks move all the time while held: at most every 4 ms (each packet takes
               WiFi time), buttons and touch at once */
            bool stickChanged = !NEAR(p.cpadX, lastSent.cpadX) || !NEAR(p.cpadY, lastSent.cpadY) ||
                !NEAR(p.cstickX, lastSent.cstickX) || !NEAR(p.cstickY, lastSent.cstickY);
            if (stickChanged && now - lastSendTime >= 4) inputChanged = true;
            #undef NEAR
            bool ackChanged = p.videoAckSeq != lastSent.videoAckSeq || p.videoAckMask != lastSent.videoAckMask ||
                (p.flags & LINK_INPUT_VIDEOACK) != (lastSent.flags & LINK_INPUT_VIDEOACK);
            /* a change goes out the same millisecond, then twice more 4 ms apart (in case
               the WiFi loses one) */
            bool repeatDue = repeatSends > 0 && now - lastSendTime >= 4;
            if (inputChanged) repeatSends = 2;
            if (inputChanged || repeatDue || (ackChanged && now - lastSendTime >= 12) || now - lastSendTime >= 50)
            {
                if (repeatDue && !inputChanged) repeatSends--;
                p.id = packetId++;
                g_inSentTick[p.id & 255] = svcGetSystemTick();
                sendto(g_udpFd, &p, sizeof(p), 0, (struct sockaddr*)&peer, sizeof(peer));
                lastSent = p;
                lastSendTime = now;
            }
        }

        LightLock_Lock(&g_lock);
        g_input.keys = held;
        g_input.touch = touch;
        g_input.dsTouch = dsTouch;
        g_input.dsX = dsX;
        g_input.dsY = dsY;
        g_input.quit = quitHold > 1000;   /* 1000 x 1 ms */
        g_input.haveUdpPeer = havePeer;
        LightLock_Unlock(&g_lock);

        /* every millisecond: a press goes out at most 1 ms after the 3DS saw it */
        u64 el = osGetTime() - start;
        if (el < 1) svcSleepThread(1000000LL);
    }
}

/* ------------------------------------------------------------------ */
/* bottom screen drawing (framebuffer is 240x320, rotated, RGB565)     */

#define LINK_RGB565(r, g, b) (u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))

static inline void putPx(u16* fb, int x, int y, u16 c)
{
    fb[x * 240 + (239 - y)] = c;
}

static void fillRect(u16* fb, int x0, int y0, int w, int h, u16 c)
{
    for (int x = x0; x < x0 + w; x++)
    {
        if (x < 0 || x >= 320) continue;
        for (int y = y0; y < y0 + h; y++)
        {
            if (y < 0 || y >= 240) continue;
            putPx(fb, x, y, c);
        }
    }
}

static void drawBottom(bool haveVideo, bool zoom, bool touching, int tx, int ty)
{
    u16* fb = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    const u16 bg = LINK_RGB565(0x10, 0x10, 0x14);
    const u16 frame = LINK_RGB565(0x50, 0x50, 0x60);
    const u16 empty = 0;   /* no picture: black, like a 3DS whose game draws nothing yet */

    if (g_vidW == 320)
    {
        /* 3DS bottom screen (Azahar): exactly the size of ours */
        if (!haveVideo)
            for (int i = 0; i < 320 * 240; i++) fb[i] = empty;
        else if (g_rotMode)
            memcpy(fb, g_rotImage[g_rotIdx], sizeof(g_rotImage[0]));   /* H.264: same layout */
        else
            for (int x = 0; x < 320; x++)
            {
                const u16* src = g_dsImage + x;
                u16* col = fb + x * 240 + 239;
                for (int y = 0; y < 240; y++) col[-y] = src[y * 320];
            }
    }
    else if (zoom)
    {
        /* 256x192 -> 320x240, nearest neighbour, with precomputed source rows/columns */
        static u16 srcRow[240];   /* framebuffer y (rotated) -> DS image row offset */
        static u8 srcCol[320];
        static bool init = false;
        if (!init)
        {
            for (int y = 0; y < 240; y++) srcRow[239 - y] = (u16)((y * 4 / 5) * 256);
            for (int x = 0; x < 320; x++) srcCol[x] = (u8)(x * 4 / 5);
            init = true;
        }
        if (!haveVideo)
        {
            for (int i = 0; i < 320 * 240; i++) fb[i] = empty;
        }
        else
        {
            for (int x = 0; x < 320; x++)
            {
                const u16* src = g_dsImage + srcCol[x];
                u16* col = fb + x * 240;
                for (int j = 0; j < 240; j++) col[j] = src[srcRow[j]];
            }
        }
    }
    else
    {
        fillRect(fb, 0, 0, 320, 240, bg);
        fillRect(fb, LINK_DS_OFFSET_X - 2, LINK_DS_OFFSET_Y - 2, 260, 196, frame);
        for (int x = 0; x < 256; x++)
        {
            u16* col = fb + (x + LINK_DS_OFFSET_X) * 240 + (239 - LINK_DS_OFFSET_Y);
            const u16* src = g_dsImage + x;
            for (int y = 0; y < 192; y++)
                col[-y] = haveVideo ? src[y * 256] : empty;
        }
    }

    /* without a picture, show where the finger is */
    if (!haveVideo && touching)
        fillRect(fb, tx - 2, ty - 2, 5, 5, LINK_RGB565(0xE0, 0x60, 0x60));
}

/* ------------------------------------------------------------------ */
/* top screen text: only redraw lines that changed (the console draws  */
/* in software; redrawing everything 60x a second starved the network)  */

#define CON_ROWS 31
static char g_conCache[CON_ROWS][160];

static void setLine(int row, const char* fmt, ...)
{
    if (row < 1 || row >= CON_ROWS) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (strcmp(buf, g_conCache[row]) == 0) return;
    snprintf(g_conCache[row], sizeof(g_conCache[row]), "%s", buf);
    printf("\x1b[%d;1H%s\x1b[0m\x1b[K", row, buf);
}

/* ------------------------------------------------------------------ */
/* settings on the SD card                                             */

#define CONFIG_PATH "sdmc:/3ds/melonDS-Link.cfg"

/* Screen backlights. The gsp::Lcd service is opened only for the moment of
   switching: keeping a session open all the time left the HOME menu without
   one - the 3DS froze when HOME was pressed. */
static void lcdBacklight(u32 screen, bool on)
{
    if (R_FAILED(gspLcdInit())) return;
    if (on) GSPLCD_PowerOnBacklight(screen);
    else GSPLCD_PowerOffBacklight(screen);
    gspLcdExit();
}

static bool g_bottomDark = false;
static void setBottomBacklight(bool on)
{
    if (on == !g_bottomDark) return;
    lcdBacklight(GSPLCD_SCREEN_BOTTOM, on);
    g_bottomDark = !on;
}

static void setTopBacklight(bool on)
{
    if (on == !g_topOff) return;
    lcdBacklight(GSPLCD_SCREEN_TOP, on);
    g_topOff = !on;
}

/* going to the HOME menu (or closing): both screens on, the main loop
   switches them off again afterwards if the settings say so */
static void aptHookFunc(APT_HookType hook, void* param)
{
    (void)param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONEXIT)
    {
        setTopBacklight(true);
        setBottomBacklight(true);
    }
    else if (hook == APTHOOK_ONSLEEP)
    {
        /* closed: tell melonDS right away (it pauses), before the WiFi goes */
        g_lidClosed = true;
        svcSleepThread(60000000LL);
    }
    else if (hook == APTHOOK_ONWAKEUP)
    {
        /* the WiFi needs a few seconds to come back; the main loop then makes
           the sockets anew (and restarts the socket service if needed) */
        g_lidClosed = false;
        g_wokeUp = true;
        g_sockGen++;
    }
}

static void loadConfig(void)
{
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[320];
    while (fgets(line, sizeof(line), f))
    {
        int v;
        char dir[128];
        if (sscanf(line, "zoom=%d", &v) == 1) g_zoom = v != 0;
        else if (sscanf(line, "videooff=%d", &v) == 1) g_videoOff = v != 0;
        else if (sscanf(line, "shotmode=%d", &v) == 1) g_shotMode = v >= 0 && v < LINK_SHOT_MODES ? v : 0;
        else if (sscanf(line, "statusbar=%d", &v) == 1) g_statusBar = v != 0;
        else if (sscanf(line, "glitch=%d", &v) == 1) g_allowGlitch = v != 0;
        else if (sscanf(line, "lang=%d", &v) == 1) g_lang = v;
        else if (sscanf(line, "client=%d", &v) == 1) g_client = v == (int)LINK_CLIENT_AZAHAR ? LINK_CLIENT_AZAHAR : LINK_CLIENT_MELONDS;
        else if (strncmp(line, "last=", 5) == 0)
        {
            snprintf(g_lastGame, sizeof(g_lastGame), "%s", line + 5);
            g_lastGame[strcspn(g_lastGame, "\r\n")] = '\0';
        }
        else if (sscanf(line, "romdir=%127[^\r\n]", dir) == 1 && dir[0] == '/')
        {
            size_t n = strlen(dir);
            while (n > 1 && dir[n - 1] == '/') dir[--n] = '\0';
            snprintf(g_romDir, sizeof(g_romDir), "%s", dir);
        }
    }
    fclose(f);
}

static void saveConfig(void)
{
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "zoom=%d\n", g_zoom ? 1 : 0);
    fprintf(f, "videooff=%d\n", g_videoOff ? 1 : 0);
    fprintf(f, "shotmode=%d\n", g_shotMode);
    fprintf(f, "statusbar=%d\n", g_statusBar ? 1 : 0);
    fprintf(f, "glitch=%d\n", g_allowGlitch ? 1 : 0);
    fprintf(f, "lang=%d\n", g_lang);
    fprintf(f, "client=%d\n", (int)g_client);
    fprintf(f, "romdir=%s\n", g_romDir);
    if (g_lastGame[0]) fprintf(f, "last=%s\n", g_lastGame);
    fclose(f);
}

/* ------------------------------------------------------------------ */

/* days from 1900-01-01 to a date (proleptic Gregorian) */
static s64 daysSince1900(int y, int m, int d)
{
    y -= m <= 2;
    const s64 era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (s64)doe - 719468 + 25567;   /* 1970 -> 1900 */
}

/* the raw hardware clock (MCU registers 0x30..0x36, BCD: s, min, h, weekday, day,
   month, year since 2000) in milliseconds since 1900, like osGetTime */
static bool readRawRtc(u64* ms)
{
    u8 r[7];
    bool ok = false;
    if (R_SUCCEEDED(mcuHwcInit()))
    {
        ok = R_SUCCEEDED(MCUHWC_ReadRegister(0x30, r, sizeof(r)));
        mcuHwcExit();
    }
    if (!ok) return false;
#define BCD(x) (((x) >> 4) * 10 + ((x) & 15))
    const int sec = BCD(r[0] & 0x7F), min = BCD(r[1] & 0x7F), hour = BCD(r[2] & 0x3F);
    const int day = BCD(r[4] & 0x3F), mon = BCD(r[5] & 0x1F), year = 2000 + BCD(r[6]);
#undef BCD
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59)
        return false;
    *ms = (u64)(((daysSince1900(year, mon, day) * 24 + hour) * 60 + min) * 60 + sec) * 1000;
    return true;
}

/* battery, charging, WiFi and the lid. The services are only opened for the
   moment of asking (see lcdBacklight). */
static void readConsoleState(bool battery)
{
    if (battery)
    {
        u8 pct = 255;
        if (R_SUCCEEDED(mcuHwcInit()))
        {
            if (R_FAILED(MCUHWC_GetBatteryLevel(&pct)) || pct > 100) pct = 255;
            mcuHwcExit();
        }
        if (R_SUCCEEDED(ptmuInit()))
        {
            u8 charging = 0, level = 0;
            if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&charging))) g_charging = charging != 0;
            if (pct == 255 && R_SUCCEEDED(PTMU_GetBatteryLevel(&level)) && level <= 5) pct = level * 20;
            ptmuExit();
        }
        g_battery = pct;
    }
    u8 w = osGetWifiStrength();
    g_wifi = w <= 3 ? w : 255;
}

/* connected to the WiFi access point (ac service, opened only for asking) */
static bool wifiConnected(void)
{
    u32 st = 0;
    if (R_SUCCEEDED(acInit()))
    {
        Result r = ACU_GetWifiStatus(&st);
        acExit();
        if (R_SUCCEEDED(r)) return st != 0;
    }
    return osGetWifiStrength() > 0;
}

static bool readLidClosed(void)
{
    u8 open = 1;
    if (R_SUCCEEDED(ptmuInit()))
    {
        PTMU_GetShellState(&open);
        ptmuExit();
    }
    return open == 0;
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    gfxInitDefault();
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    PrintConsole top;
    consoleInit(GFX_TOP, &top);
    osSetSpeedupEnable(true);
    irrstInit();   /* C-stick / ZL / ZR on New 3DS */
    LightLock_Init(&g_lock);
    LightLock_Init(&g_ackLock);
    static aptHookCookie hookCookie;
    aptHook(&hookCookie, aptHookFunc, NULL);
    loadConfig();
    setVideoGeometry(g_client);
    loadLanguageFiles();
    applyLanguage();
    loadFavs();
    scanGames();

    u32* socBuf = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    g_payload = (uint8_t*)malloc(LINK_MAX_PAYLOAD + LINK_MAX_PATH + 64);
    g_outbuf = (uint8_t*)malloc(LINK_MAX_PAYLOAD);
    if (!socBuf || !g_payload || !g_outbuf || R_FAILED(socInit(socBuf, SOC_BUFFERSIZE)))
    {
        printf("\x1b[1;1H\x1b[36m%s\x1b[0m\n\n", T(S_TITLE));
        printf("\x1b[31m%s\x1b[0m\n", T(S_WIFI_FAIL));
        printf("%s\n", T(S_WIFI_FAIL2));
        while (aptMainLoop())
        {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        irrstExit();
        gfxExit();
        return 0;
    }

    struct in_addr ip;
    ip.s_addr = gethostid();

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    g_listenFd = openListenSocket();
    g_udpFd = openUdpSocket(LINK_UDP_PORT, 0);
    markInputPriority(g_udpFd);
    g_videoFd = openUdpSocket(LINK_VIDEO_PORT, 128 * 1024);   /* room for bursts of tile packets */

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* lower number = higher priority. The input thread is tiny and sleeps 4 ms
       per round: it goes first. The video and server threads come AFTER the main
       thread: with a lot of picture data they can keep the CPU busy all the time,
       and above the main thread they starved it - the app froze and not even the
       HOME button worked any more. The main thread sleeps most of each frame
       (waiting for the screen), so they still get plenty of time. */
    Thread inp = threadCreate(inputThread, NULL, 0x4000, prio - 2, -2, false);
    Thread vid = threadCreate(videoThread, NULL, 0x4000, prio + 1, -2, false);
    /* taking packets from the socket is tiny work: above the main thread, so the
       socket buffer is emptied even while the main thread or the decoder is busy */
    Thread vrx = threadCreate(videoRecvThread, NULL, 0x4000, prio - 1, -2, false);
    Thread srv = threadCreate(serverThread, NULL, 0x4000, prio + 1, -2, false);

    u32 lastVideoFrames = (u32)-1;
    bool lastZoom = !g_zoom, lastHaveVideo = true;
    bool savedZoom = g_zoom;
    int menuCursor = 0, menuTop = 0, linkCursor = 0;
    u64 lastStateRead = 0, lastBatteryRead = 0, lastWakeTry = 0, lastWakeCheck = 0, wifiSince = 0;
    int wakeTries = 0;
    bool sleepApplied = false, keepAwakeApplied = false, wasLidDark = false;
    bool isNew3DS = false;
    APT_CheckNew3DS(&isNew3DS);
    if (g_lastGame[0]) { int i = findGame(g_lastGame); if (i >= 0) menuCursor = i; }
    g_tab = NATIVE() ? 1 : 0;
    int tabCursor[2] = { menuCursor, 0 }, tabTop[2] = { 0, 0 };
    if (g_tab == 1) { menuCursor = 0; }
    const int LIST_ROWS = 14;
    int redrawBoth = 2;

    while (aptMainLoop() && g_running)
    {
        InputState in;
        LightLock_Lock(&g_lock);
        in = g_input;
        LightLock_Unlock(&g_lock);
        if (in.quit) break;

        if (g_clientChanged)
        {
            /* another emulator connected: its name in the texts, its picture size */
            g_clientChanged = false;
            applyLanguage();
            saveConfig();
            redrawBoth = 2;
            /* the list shows the games of the emulator that is connected */
            int want = NATIVE() ? 1 : 0;
            if (want != g_tab)
            {
                tabCursor[g_tab] = menuCursor; tabTop[g_tab] = menuTop;
                g_tab = want;
                menuCursor = tabCursor[g_tab]; menuTop = tabTop[g_tab];
            }
            if (!g_gameStarted && !g_linkMenuOpen) g_menuOpen = true;
        }
        bool native = NATIVE();

        /* a new game list from Azahar */
        {
            LightLock_Lock(&g_lock);
            char* lib = g_libPending;
            g_libPending = NULL;
            LightLock_Unlock(&g_lock);
            if (lib)
            {
                char keep[LINK_MAX_PATH] = "";
                if (menuCursor >= 0 && g_tab == 1 && menuCursor < g_libCount) snprintf(keep, sizeof(keep), "%s", g_lib[menuCursor].path);
                takeLibrary(lib);
                int i = keep[0] ? findLib(keep) : -1;
                if (g_tab == 1) menuCursor = i >= 0 ? i : (menuCursor < g_libCount ? menuCursor : 0);
                else tabCursor[1] = 0;
            }
        }
        int listCount = g_tab == 1 ? g_libCount : g_gameCount;

        /* H.264 sends nothing while the picture stands still: keep showing it */
        bool haveVideo = g_lastVideoTime != 0 &&
                         (osGetTime() - g_lastVideoTime < 3000 || (g_rotMode && g_tcpConnected));
        bool zoom = g_zoom;

        /* H.264: the newest decoded picture that is due (a backlog is shown at once) */
        bool newPicture = false;
        {
            static u64 lastIter = 0;
            u64 t = osGetTime();
            if (lastIter && t - lastIter > 25) g_loopSlow++;
            lastIter = t;
        }
        /* both H.264 and tiles: decoded pictures wait in the ring until they are due */
        {
            static u32 shownNo = 0, prevSrc = 0;
            static u64 prevShown = 0;
            u32 w = g_rotWritten;
            __sync_synchronize();
            /* the time of the refresh, not of this line: how long the loop took to get here
               varies, and would shift pictures near the edge to the next refresh */
            u64 now = g_vblankTime ? g_vblankTime : osGetTime();
            if (w - shownNo > 1000) shownNo = w - 1;   /* restarted */
            u32 first = shownNo + 1;
            if (w >= ROT_SLOTS - 1 && first < w - (ROT_SLOTS - 2)) first = w - (ROT_SLOTS - 2);
            int pick = -1;
            u32 pickNo = 0;
            for (u32 no = first; no != w + 1 && (s32)(w - no) >= 0; no++)
            {
                int slot = (int)(no % ROT_SLOTS);
                u64 due = g_rotDue[slot];
                u32 src = g_rotSrc[slot];
                s32 gap = (s32)(src - prevSrc);
                bool ok;
                if (src && src == prevSrc) ok = true;   /* the same picture, only refined: at once */
                else if (src && prevSrc && gap > 0 && gap <= 100 && prevShown)
                {
                    /* Locked to the refreshes: made 2 refreshes after the picture on the screen,
                       it is shown exactly 2 refreshes after it - a due time a few ms early or
                       late (the wait adapting, clocks drifting) does not make it 1 or 3.
                       Only when clearly off by more does it move by a refresh. */
                    u32 k = ((u32)gap * 1000 + 8355) / 16711;   /* refreshes (59.83 a second) */
                    if (k < 1) k = 1;
                    u64 slotTime = prevShown + ((u64)k * 16711 + 500) / 1000;
                    ok = (now + 4 >= slotTime && due <= now + 25) ||
                         (now + 4 + 17 >= slotTime && due + 25 < now);   /* far behind: catch up */
                }
                else ok = due <= now + 8;   /* shown at the next refresh: within half a refresh */
                if (ok || w - shownNo >= 3) { pick = slot; pickNo = no; }
            }
            if (pick >= 0)
            {
                u32 pickSrc = g_rotSrc[pick];
                bool refine = pickSrc && pickSrc == prevSrc;   /* not a new picture: no pacing, no statistics */
                if (!refine && now > g_rotDue[pick] + 17) g_rotLate++;
                if (!refine)
                {
                    g_rotWaitSum += (u32)(g_rotDue[pick] > g_rotMade[pick] ? g_rotDue[pick] - g_rotMade[pick] : 0);
                    g_rotWaitN++;
                }
                /* a picture shown clearly later after the one before than the PC made it */
                u32 src = g_rotSrc[pick];
                s32 srcGap = (s32)(src - prevSrc);
                if (src && prevSrc && srcGap > 0 && srcGap <= 40 && now - prevShown > (u64)srcGap + 12) g_rotStutter++;
                if (src && prevSrc && srcGap > 0 && srcGap <= 60)
                {
                    u32 v = (u32)((now - prevShown + 8) / 17);   /* screen refreshes */
                    g_dispGaps[v < 1 ? 0 : v > 4 ? 3 : v - 1]++;
                }
                if (!refine)
                {
                    prevSrc = src;
                    prevShown = now;
                }
                g_rotIdx = pick;
                shownNo = pickNo;
                newPicture = true;
                if (!g_rotMode) memcpy(g_dsImage, g_rotImage[pick], sizeof(g_dsImage));   /* tiles */
            }
        }

        /* redraw when something changed; both buffers need it (double buffering) */
        if (newPicture || g_videoFrames != lastVideoFrames || zoom != lastZoom || haveVideo != lastHaveVideo ||
            (!haveVideo && (in.keys & KEY_TOUCH)))
            redrawBoth = 2;
        if (zoom != savedZoom) { saveConfig(); savedZoom = zoom; }
        lastVideoFrames = g_videoFrames;
        lastZoom = zoom;
        lastHaveVideo = haveVideo;
        if (g_videoOff) redrawBoth = 2;   /* screen is dark; draw again once it is back on */
        else if (redrawBoth > 0)
        {
            drawBottom(haveVideo, zoom, (in.keys & KEY_TOUCH) != 0, in.touch.px, in.touch.py);
            redrawBoth--;
        }

        /* ---------------- user interface ---------------- */
        u32 mk;
        LightLock_Lock(&g_lock);
        mk = g_menuKeys;
        g_menuKeys = 0;
        bool confirm = g_confirmPress;
        g_confirmPress = false;
        LightLock_Unlock(&g_lock);

        /* news from melonDS about the running game */
        if (g_sessionNew)
        {
            /* e.g. the app was restarted while melonDS kept the game */
            g_sessionNew = false;
            if (g_menuOpen && !g_askActive) { g_localAsk = LA_RESUME; }
        }
        if (g_sessionSame)
        {
            /* reconnected while playing: melonDS may continue (it pauses when the 3DS goes away) */
            g_sessionSame = false;
            if (!g_menuOpen && !g_linkMenuOpen && g_localAsk == LA_NONE) g_haveResumeEv = true;
        }
        if (g_sessionEnded)
        {
            g_sessionEnded = false;
            if (g_localAsk == LA_RESUME || g_localAsk == LA_STOP || g_localAsk == LA_SWITCH) g_localAsk = LA_NONE;
            if (!g_linkMenuOpen) g_menuOpen = true;
        }
        /* a save that never got swapped in (connection lost) does not block quitting forever */
        if (g_saveWriting && osGetTime() - g_saveWriteTime > 15000) g_saveWriting = false;

        if (in.quit && g_localAsk != LA_QUIT)
        {
            /* L+R+SELECT+START held: quit at once, unless a save is on its way */
            if (!g_saveWriting) break;
            g_localAsk = LA_QUIT;
            g_linkMenuOpen = false;
        }

        bool askActive = g_askActive;
        int localAsk = askActive ? LA_NONE : g_localAsk;
        if (g_menuToggle)
        {
            /* SELECT + START tapped: open/close the melonDS Link menu */
            g_menuToggle = false;
            if (!askActive && localAsk == LA_NONE)
            {
                if (g_linkMenuOpen)
                {
                    g_linkMenuOpen = false;
                    if (!g_gameStarted) g_menuOpen = true;
                }
                else
                {
                    g_linkMenuOpen = true;
                    g_menuOpen = false;
                    linkCursor = 0;
                }
            }
            mk = 0;
        }
        /* whenever something is shown, the top screen has to be on */
        /* after waking up: as soon as the WiFi is connected again, restart the
           socket service and make the sockets anew (the sleep breaks them).
           If melonDS still does not get through, again every 4 seconds. */
        if (g_wokeUp)
        {
            u64 now = osGetTime();
            if (g_tcpConnected) { g_wokeUp = false; wakeTries = 0; wifiSince = 0; }
            else if (now - lastWakeCheck >= 250)
            {
                lastWakeCheck = now;
                if (!wifiConnected()) wifiSince = 0;
                else
                {
                    if (!wifiSince) wifiSince = now;
                    if (now - wifiSince >= 300 && (wakeTries == 0 || now - lastWakeTry >= 4000) && wakeTries < 40)
                    {
                        lastWakeTry = now;
                        wakeTries++;
                        g_netPause = true;
                        svcSleepThread(60000000LL);   /* the threads step back */
                        /* close the old sockets here (not in the threads): after the
                           restart a new socket may get the same number again */
                        close(g_listenFd); g_listenFd = -1;
                        close(g_udpFd); g_udpFd = -1;
                        close(g_videoFd); g_videoFd = -1;
                        socExit();
                        socInit(socBuf, SOC_BUFFERSIZE);
                        g_netPause = false;
                        g_sockGen++;
                        u32 newIp = (u32)gethostid();
                        if (newIp != 0 && newIp != 0xFFFFFFFFu) ip.s_addr = newIp;
                    }
                }
            }
        }

        /* battery, WiFi, lid: twice a second (the battery every 30 s) */
        {
            u64 now = osGetTime();
            if (now - lastStateRead >= 500)
            {
                bool bat = lastBatteryRead == 0 || now - lastBatteryRead >= 30000;
                if (bat) lastBatteryRead = now;
                lastStateRead = now;
                readConsoleState(bat);
                if (!sleepApplied || keepAwakeApplied != g_keepAwake)
                {
                    /* melonDS setting: closed = sleep (it pauses the game) or stay awake */
                    aptSetSleepAllowed(!g_keepAwake);
                    keepAwakeApplied = g_keepAwake;
                    sleepApplied = true;
                }
                if (g_keepAwake) g_lidClosed = readLidClosed();   /* else the sleep hooks tell */
            }
        }
        bool lidDark = g_keepAwake && g_lidClosed;   /* closed, game runs on: screens off */
        if (lidDark) setTopBacklight(false);
        else if (wasLidDark) setTopBacklight(true);
        wasLidDark = lidDark;
        if (!lidDark && g_topOff && (g_menuOpen || g_linkMenuOpen || askActive || localAsk != LA_NONE)) setTopBacklight(true);
        setBottomBacklight(!g_videoOff && !lidDark);

        /* the menu: only what makes sense right now */
        enum { MI_BACK, MI_GAMES, MI_SHOT, MI_SHOTMODE, MI_BACKUPS, MI_DISPLAY, MI_BOTTOM, MI_TOPOFF, MI_STATUSBAR, MI_GLITCH, MI_LANG, MI_STOP, MI_QUIT };
        int items[13], itemCount = 0;
        bool playing = g_gameStarted && !g_launchPending;
        if (g_gameStarted) items[itemCount++] = MI_BACK;
        items[itemCount++] = MI_GAMES;
        if (playing && g_tcpConnected && !native) items[itemCount++] = MI_SHOT;
        if (!native) items[itemCount++] = MI_SHOTMODE;
        /* backups: of the running game, else of the game selected in the list */
        int bakGame = -1;
        if (g_gameStarted)
        {
            for (int i = 0; i < g_gameCount && bakGame < 0; i++)
            {
                size_t l = strlen(g_games[i]);
                if (l > 4 && strlen(g_currentGame) == l - 4 && strncmp(g_games[i], g_currentGame, l - 4) == 0) bakGame = i;
            }
        }
        else if (g_tab == 0 && menuCursor >= 0 && menuCursor < g_gameCount) bakGame = menuCursor;
        if (bakGame >= 0 && g_tcpConnected && !g_launchPending && !native) items[itemCount++] = MI_BACKUPS;
        if (!native) items[itemCount++] = MI_DISPLAY;
        items[itemCount++] = MI_BOTTOM;
        items[itemCount++] = MI_TOPOFF;
        items[itemCount++] = MI_STATUSBAR;
        if (g_mvdReady && g_client == LINK_CLIENT_AZAHAR) items[itemCount++] = MI_GLITCH;
        items[itemCount++] = MI_LANG;
        if (playing && g_tcpConnected) items[itemCount++] = MI_STOP;
        items[itemCount++] = MI_QUIT;
        if (linkCursor >= itemCount) linkCursor = itemCount - 1;
        if (linkCursor < 0) linkCursor = 0;

        /* back to the game if there is one, else to the game list */
        #define CLOSE_MENU() do { g_linkMenuOpen = false; if (!g_gameStarted) g_menuOpen = true; } while (0)

        if (askActive && mk)
        {
            if (mk & (KEY_A | KEY_B))
            {
                LightLock_Lock(&g_lock);
                g_answerId = g_askId;
                g_answerChoice = (mk & KEY_A) ? 0 : 1;
                g_haveAnswer = true;
                g_askActive = false;
                LightLock_Unlock(&g_lock);
            }
        }
        else if (localAsk != LA_NONE && (mk || confirm))
        {
            /* things that cannot be undone need L + R + A, the rest plain A */
            bool strong = localAsk != LA_RESUME;
            bool yes = strong ? confirm : (mk & KEY_A) != 0, no = (mk & KEY_B) != 0;
            if (yes || no)
            {
                g_localAsk = LA_NONE;
                switch (localAsk)
                {
                case LA_RESUME:
                    if (yes) { g_menuOpen = false; g_haveResumeEv = true; }
                    else g_menuOpen = true;
                    break;
                case LA_SWITCH:
                    if (yes && g_switchTab == 1) requestLaunchLib(g_switchIdx);
                    else if (yes) { requestLaunch(g_switchIdx); saveConfig(); }
                    break;
                case LA_STOP:
                    if (yes)
                    {
                        LightLock_Lock(&g_lock);
                        g_haveStopEv = true;
                        snprintf(g_statusText, sizeof(g_statusText), "%s", T(S_STOPPING));
                        LightLock_Unlock(&g_lock);
                        g_linkMenuOpen = false;
                    }
                    break;
                case LA_QUIT:
                    if (yes) g_running = false;
                    break;
                case LA_RESTORE:
                    if (yes)
                    {
                        LightLock_Lock(&g_lock);
                        g_haveRestoreEv = true;
                        snprintf(g_statusText, sizeof(g_statusText), "%s", T(S_RESTORING));
                        LightLock_Unlock(&g_lock);
                        g_bakOpen = false;
                        if (!g_gameStarted) g_menuOpen = true;
                    }
                    break;
                }
            }
        }
        else if (g_linkMenuOpen && mk)
        {
            if (mk & (KEY_DUP | KEY_CPAD_UP)) linkCursor = (linkCursor + itemCount - 1) % itemCount;
            if (mk & (KEY_DDOWN | KEY_CPAD_DOWN)) linkCursor = (linkCursor + 1) % itemCount;
            if (mk & KEY_B) CLOSE_MENU();
            else if (mk & KEY_A)
            {
                switch (items[linkCursor])
                {
                case MI_BACK:
                    g_linkMenuOpen = false;
                    g_menuOpen = false;
                    g_haveResumeEv = true;
                    break;
                case MI_GAMES: g_linkMenuOpen = false; g_menuOpen = true; break;
                case MI_DISPLAY: g_zoom = !g_zoom; break;
                case MI_SHOTMODE:
                    g_shotMode = (g_shotMode + 1) % LINK_SHOT_MODES;
                    saveConfig();
                    break;
                case MI_STATUSBAR:
                    g_statusBar = !g_statusBar;
                    saveConfig();
                    break;
                case MI_GLITCH:
                    g_allowGlitch = !g_allowGlitch;
                    saveConfig();
                    break;
                case MI_BOTTOM:
                    g_videoOff = !g_videoOff;
                    g_haveVideoEv = true;
                    saveConfig();
                    break;
                case MI_TOPOFF:
                    CLOSE_MENU();
                    if (g_gameStarted) setTopBacklight(false);
                    break;
                case MI_SHOT:
                    LightLock_Lock(&g_lock);
                    g_haveShot = true;
                    snprintf(g_statusText, sizeof(g_statusText), "%s", T(S_SHOT_REQ));
                    LightLock_Unlock(&g_lock);
                    CLOSE_MENU();
                    break;
                case MI_LANG:
                    g_lang = (g_lang + 1) % LANG_COUNT;
                    applyLanguage();
                    g_haveLangEvent = true;
                    saveConfig();
                    break;
                case MI_STOP: g_localAsk = LA_STOP; break;
                case MI_BACKUPS:
                    LightLock_Lock(&g_lock);
                    snprintf(g_bakRom, sizeof(g_bakRom), "%.100s/%.150s", g_romDir, g_games[bakGame]);
                    g_bakCount = 0;
                    g_bakCursor = 0;
                    g_bakWaiting = true;
                    g_haveBakReqEv = true;
                    LightLock_Unlock(&g_lock);
                    g_linkMenuOpen = false;
                    g_bakOpen = true;
                    break;
                case MI_QUIT:
                    /* with a game running (or a save on its way) ask first */
                    if (g_gameStarted || g_saveWriting) g_localAsk = LA_QUIT;
                    else g_running = false;
                    break;
                }
            }
        }
        else if (g_bakOpen && mk)
        {
            LightLock_Lock(&g_lock);
            if (g_bakCount > 0)
            {
                if (mk & (KEY_DUP | KEY_CPAD_UP)) g_bakCursor = (g_bakCursor + g_bakCount - 1) % g_bakCount;
                if (mk & (KEY_DDOWN | KEY_CPAD_DOWN)) g_bakCursor = (g_bakCursor + 1) % g_bakCount;
            }
            bool pick = (mk & KEY_A) && g_bakCount > 0 && !g_bakWaiting;
            if (pick) g_restoreIdx = g_bakCursor;
            LightLock_Unlock(&g_lock);
            if (pick) g_localAsk = LA_RESTORE;
            else if (mk & KEY_B) { g_bakOpen = false; g_bakWaiting = false; g_linkMenuOpen = true; }
        }
        else if (g_menuOpen && mk)
        {
            if (mk & (KEY_L | KEY_R))
            {
                /* the other tab */
                tabCursor[g_tab] = menuCursor; tabTop[g_tab] = menuTop;
                g_tab ^= 1;
                menuCursor = tabCursor[g_tab]; menuTop = tabTop[g_tab];
                listCount = g_tab == 1 ? g_libCount : g_gameCount;
                mk &= ~(KEY_A | KEY_X | KEY_Y);
            }
            if (mk & (KEY_DUP | KEY_CPAD_UP)) menuCursor--;
            if (mk & (KEY_DDOWN | KEY_CPAD_DOWN)) menuCursor++;
            if (mk & KEY_DLEFT) menuCursor -= LIST_ROWS;
            if (mk & KEY_DRIGHT) menuCursor += LIST_ROWS;
            if (menuCursor >= listCount) menuCursor = listCount - 1;
            if (menuCursor < 0) menuCursor = 0;
            if (g_tab == 0 && (mk & (KEY_X | KEY_Y)) && g_gameCount > 0)
            {
                char keep[256];
                snprintf(keep, sizeof(keep), "%s", g_games[menuCursor]);
                if (mk & KEY_X) { toggleFav(keep); sortGames(); }
                if (mk & KEY_Y) scanGames();
                int i = findGame(keep);
                if (i >= 0) menuCursor = i;
            }
            if (g_tab == 1 && (mk & KEY_X) && g_libCount > 0)
            {
                char key[220], keep[LINK_MAX_PATH];
                libFavKey(&g_lib[menuCursor], key, sizeof(key));
                snprintf(keep, sizeof(keep), "%s", g_lib[menuCursor].path);
                toggleFav(key);
                qsort(g_lib, g_libCount, sizeof(LibGame), cmpLib);
                int i = findLib(keep);
                if (i >= 0) menuCursor = i;
            }
            if (g_tab == 1 && (mk & KEY_Y) && native && g_tcpConnected)
            {
                LightLock_Lock(&g_lock);
                g_haveLibReqEv = true;   /* Azahar sends its list again */
                LightLock_Unlock(&g_lock);
            }
            if ((mk & KEY_A) && listCount > 0)
            {
                char base[256];
                if (g_tab == 1) snprintf(base, sizeof(base), "%s", g_lib[menuCursor].name);
                else
                {
                    snprintf(base, sizeof(base), "%s", g_games[menuCursor]);
                    size_t bl = strlen(base);
                    if (bl > 4) base[bl - 4] = 0;
                }
                if ((g_tab == 1) != native)
                {
                    /* the other emulator is connected: it cannot start this game
                       (the line under the list says which one is needed) */
                }
                else if (g_gameStarted && strcmp(base, g_currentGame) == 0)
                {
                    /* the game that runs anyway: just go back to it, never restart it */
                    g_menuOpen = false;
                    g_haveResumeEv = true;
                }
                else if (g_gameStarted)
                {
                    /* another game while one runs: unsaved progress would be lost */
                    g_switchIdx = menuCursor;
                    g_switchTab = g_tab;
                    g_localAsk = LA_SWITCH;
                }
                else if (g_tab == 1) requestLaunchLib(menuCursor);
                else
                {
                    requestLaunch(menuCursor);
                    saveConfig();      /* remembers the last game */
                }
            }
            else if ((mk & KEY_B) && g_gameStarted) { g_menuOpen = false; g_haveResumeEv = true; }
        }

        /* ---------------- top screen ---------------- */
        char status[128], game[128], xi[64], xi2[64];
        LightLock_Lock(&g_lock);
        snprintf(status, sizeof(status), "%s", g_statusText);
        snprintf(game, sizeof(game), "%s", g_currentGame);
        snprintf(xi, sizeof(xi), "%s", g_xferInfo);
        snprintf(xi2, sizeof(xi2), "%s", g_xferInfo2);
        LightLock_Unlock(&g_lock);

        {
            /* battery and WiFi top right (setting) */
            char st[100] = "";
            if (g_statusBar)
            {
                char bat[12] = "?", wifi[8] = "?";
                if (g_battery <= 100) snprintf(bat, sizeof(bat), "%u%%%s", (unsigned)g_battery, g_charging ? "+" : "");
                if (g_wifi <= 3)
                    for (int i = 0; i < 3; i++) wifi[i] = i < g_wifi ? (char)0xDB : (char)0xFA;   /* bars (code page 437) */
                if (g_wifi <= 3) wifi[3] = 0;
                const char* batColor = g_charging ? "\x1b[32m" : g_battery <= 20 ? "\x1b[31m" : g_battery <= 30 ? "\x1b[33m" : "\x1b[37m";
                int visible = (int)(strlen(T(S_BAT)) + 1 + strlen(bat) + 2 + strlen(T(S_WIFI)) + 1 + 3);
                snprintf(st, sizeof(st), "\x1b[1;%dH\x1b[90m%s %s%s \x1b[90m %s \x1b[37m%s", 51 - visible,
                         T(S_BAT), batColor, bat, T(S_WIFI), wifi);
            }
            setLine(1, "\x1b[36m%s\x1b[0m  \x1b[90mv%u%s", T(S_TITLE), (unsigned)LINK_VERSION, st);
        }
        setLine(3, "%-12s \x1b[33m%s", T(S_IP), inet_ntoa(ip));
        char emuLabel[16];
        snprintf(emuLabel, sizeof(emuLabel), "%s:", emulatorName());
        setLine(4, "%-12s %s%s", emuLabel, g_tcpConnected ? "\x1b[32m" : "\x1b[90m", g_tcpConnected ? T(S_CONNECTED) : T(S_WAITING));
        setLine(5, "%-12s %s%s\x1b[0m   %s %s%s", T(S_CONTROLS), in.haveUdpPeer ? "\x1b[32m" : "\x1b[90m",
                in.haveUdpPeer ? T(S_ACTIVE) : "-", T(S_PICTURE), haveVideo ? "\x1b[32m" : "\x1b[90m", haveVideo ? "ok" : "-");
        setLine(6, "%-12s %s", T(S_DISPLAY), zoom && !native ? T(S_ENLARGED) : T(S_ORIGINAL));
        setLine(7, "\x1b[90m%.49s", xi);
        setLine(8, "\x1b[33m%.49s", status);
        setLine(9, "\x1b[90m%.49s", xi2);

        if (askActive || localAsk != LA_NONE)
        {
            bool strongAsk = !askActive && localAsk != LA_RESUME;
            char lines[ASK_LINES][52], a[48], b[48];
            int count = 0;
            if (askActive)
            {
                /* a question from melonDS */
                LightLock_Lock(&g_lock);
                count = g_askLineCount;
                memcpy(lines, g_askLines, sizeof(lines));
                snprintf(a, sizeof(a), "%s", g_askA);
                snprintf(b, sizeof(b), "%s", g_askB);
                LightLock_Unlock(&g_lock);
            }
            else
            {
                char g[130], other[300];
                utf8ToCp437(g, sizeof(g), game);
                switch (localAsk)
                {
                case LA_RESUME:
                    snprintf(lines[count++], 52, "%s", T(S_D_RESUME_T));
                    snprintf(lines[count++], 52, "%.49s", g);
                    snprintf(a, sizeof(a), "%s", T(S_D_CONTINUE));
                    snprintf(b, sizeof(b), "%s", T(S_D_LIST));
                    break;
                case LA_SWITCH:
                    if (g_switchTab == 1)
                        utf8ToCp437(other, sizeof(other), g_switchIdx < g_libCount ? g_lib[g_switchIdx].name : "");
                    else
                    {
                        utf8ToCp437(other, sizeof(other), g_switchIdx < g_gameCount ? g_games[g_switchIdx] : "");
                        if (strlen(other) > 4) other[strlen(other) - 4] = 0;
                    }
                    snprintf(lines[count++], 52, "%s", T(S_D_SWITCH_T));
                    snprintf(lines[count++], 52, "%s %.40s", T(S_RUNNING), g);
                    snprintf(lines[count++], 52, "%.49s", T(S_D_LOST));
                    snprintf(a, sizeof(a), "%s: %.30s", T(S_D_START), other);
                    snprintf(b, sizeof(b), "%s", T(S_D_BACK));
                    break;
                case LA_STOP:
                    snprintf(lines[count++], 52, "%s", T(S_D_STOP_T));
                    snprintf(lines[count++], 52, "%.49s", g);
                    snprintf(lines[count++], 52, "%.49s", T(S_D_LOST));
                    snprintf(a, sizeof(a), "%s", T(S_D_STOP_A));
                    snprintf(b, sizeof(b), "%s", T(S_D_BACK));
                    break;
                case LA_RESTORE:
                    LightLock_Lock(&g_lock);
                    snprintf(lines[count++], 52, "%s", T(S_D_RESTORE_T));
                    snprintf(lines[count++], 52, "%.49s", g_restoreIdx < g_bakCount ? g_bakLines[g_restoreIdx] : "");
                    LightLock_Unlock(&g_lock);
                    snprintf(lines[count++], 52, "%.49s", T(S_D_RESTORE_1));
                    if (g_gameStarted) snprintf(lines[count++], 52, "%.49s", T(S_D_RESTORE_2));
                    snprintf(a, sizeof(a), "%s", T(S_D_RESTORE_A));
                    snprintf(b, sizeof(b), "%s", T(S_D_BACK));
                    break;
                default:
                    snprintf(lines[count++], 52, "%s", T(S_D_QUIT_T));
                    if (g_saveWriting) snprintf(lines[count++], 52, "\x1b[31m%.44s\x1b[0m", T(S_D_SAVING));
                    if (g_gameStarted) snprintf(lines[count++], 52, "%.49s", T(S_D_QUIT_1));
                    snprintf(a, sizeof(a), "%s", T(S_D_QUIT_A));
                    snprintf(b, sizeof(b), "%s", T(S_D_BACK));
                    break;
                }
            }
            for (int r = 0; r < 16; r++)
            {
                int row = 10 + r;
                if (r < count) setLine(row, r == 0 ? "\x1b[36m%.49s" : "%.49s", lines[r]);
                else if (r == count + 1 && strongAsk) setLine(row, "\x1b[47;30m L+R+A \x1b[0m %.40s", a);
                else if (r == count + 1) setLine(row, "\x1b[47;30m A \x1b[0m %.44s", a);
                else if (r == count + 2 && strongAsk) setLine(row, "\x1b[47;30m   B   \x1b[0m %.40s", b);
                else if (r == count + 2) setLine(row, "\x1b[47;30m B \x1b[0m %.44s", b);
                else setLine(row, "");
            }
            setLine(26, "\x1b[90m%s", strongAsk ? T(S_CONFIRM_HINT) : T(S_ASK_HINT));
        }
        else if (g_linkMenuOpen)
        {
            setLine(10, "\x1b[36m%s", T(S_MENU_TITLE));
            setLine(11, "");
            for (int i = 0; i < itemCount; i++)
            {
                char label[64];
                switch (items[i])
                {
                case MI_BACK: snprintf(label, sizeof(label), "%s", T(S_M_BACK)); break;
                case MI_GAMES: snprintf(label, sizeof(label), "%s", T(S_M_GAMES)); break;
                case MI_SHOT: snprintf(label, sizeof(label), "%s", T(S_M_SHOT)); break;
                case MI_SHOTMODE:
                    snprintf(label, sizeof(label), "%s: %s", T(S_M_SHOTMODE),
                             T(g_shotMode == LINK_SHOT_TOP ? S_SHOT_TOP : g_shotMode == LINK_SHOT_BOTTOM ? S_SHOT_BOTTOM :
                               g_shotMode == LINK_SHOT_SEPARATE ? S_SHOT_SEPARATE : S_SHOT_BOTH));
                    break;
                case MI_DISPLAY: snprintf(label, sizeof(label), "%s: %s", T(S_M_DISPLAY), zoom ? T(S_ENLARGED) : T(S_ORIGINAL)); break;
                case MI_BOTTOM: snprintf(label, sizeof(label), "%s: %s", T(S_M_BOTTOM), g_videoOff ? T(S_OFF) : T(S_ON)); break;
                case MI_TOPOFF: snprintf(label, sizeof(label), "%s", T(S_M_TOPOFF)); break;
                case MI_STATUSBAR: snprintf(label, sizeof(label), "%s: %s", T(S_M_STATUSBAR), g_statusBar ? T(S_ON) : T(S_OFF)); break;
                case MI_GLITCH: snprintf(label, sizeof(label), "%s: %s", T(S_M_GLITCH), g_allowGlitch ? T(S_GLITCH_ALLOW) : T(S_GLITCH_NEVER)); break;
                case MI_LANG: snprintf(label, sizeof(label), "%s: %s", T(S_M_LANG), T(S_LANGNAME)); break;
                case MI_STOP: snprintf(label, sizeof(label), "%s", T(S_M_STOP)); break;
                case MI_BACKUPS:
                {
                    char nm[300];
                    utf8ToCp437(nm, sizeof(nm), bakGame >= 0 ? g_games[bakGame] : "");
                    if (strlen(nm) > 4) nm[strlen(nm) - 4] = 0;
                    snprintf(label, sizeof(label), "%s: %.26s", T(S_M_BACKUPS), nm);
                    break;
                }
                default: snprintf(label, sizeof(label), "%s", T(S_M_QUIT)); break;
                }
                if (i == linkCursor) setLine(12 + i, "\x1b[47;30m> %-46.46s", label);
                else setLine(12 + i, "  %-46.46s", label);
            }
            for (int r = 12 + itemCount; r < 26; r++) setLine(r, "");
            setLine(26, "\x1b[90m%s", T(S_MENU_HINT));
        }
        else if (g_bakOpen)
        {
            char nm[300];
            LightLock_Lock(&g_lock);
            const char* file = strrchr(g_bakRom, '/');
            utf8ToCp437(nm, sizeof(nm), file ? file + 1 : g_bakRom);
            if (strlen(nm) > 4) nm[strlen(nm) - 4] = 0;
            setLine(10, "\x1b[36m%s\x1b[0m %.30s", T(S_M_BACKUPS), nm);
            for (int r = 0; r < 14; r++)
            {
                int row = 11 + r;
                if (g_bakWaiting) setLine(row, r == 0 ? "\x1b[90m  %s" : "", T(S_B_LOADING));
                else if (g_bakCount == 0) setLine(row, r == 0 ? "\x1b[33m  %s" : "", T(S_B_NONE));
                else if (r < g_bakCount && r == g_bakCursor) setLine(row, "\x1b[47;30m> %-46.46s", g_bakLines[r]);
                else if (r < g_bakCount) setLine(row, "  %-46.46s", g_bakLines[r]);
                else setLine(row, "");
            }
            LightLock_Unlock(&g_lock);
            setLine(25, "");
            setLine(26, "\x1b[90m%s", T(S_B_HINT));
        }
        else if (g_menuOpen)
        {
            /* the tabs: DS games (melonDS) and 3DS games (Azahar) */
            if (g_tab == 0)
                setLine(10, "\x1b[46;30m %s \x1b[0m \x1b[90m%s\x1b[0m  %d", T(S_TAB_DS), T(S_TAB_3DS), g_gameCount);
            else
                setLine(10, "\x1b[90m%s\x1b[0m \x1b[46;30m %s \x1b[0m  %d", T(S_TAB_DS), T(S_TAB_3DS), g_libCount);
            if (menuCursor < menuTop) menuTop = menuCursor;
            if (menuCursor >= menuTop + LIST_ROWS) menuTop = menuCursor - LIST_ROWS + 1;
            if (menuTop < 0) menuTop = 0;
            for (int r = 0; r < LIST_ROWS; r++)
            {
                int i = menuTop + r;
                if (i < listCount)
                {
                    char name[300];
                    bool fav, mark;
                    const char* markText;
                    if (g_tab == 1)
                    {
                        utf8ToCp437(name, sizeof(name), g_lib[i].name);
                        fav = libIsFav(&g_lib[i]);
                        mark = g_lib[i].on3ds;
                        markText = " 3DS";
                    }
                    else
                    {
                        utf8ToCp437(name, sizeof(name), g_games[i]);
                        size_t n0 = strlen(name);
                        if (n0 > 4 && strcasecmp(name + n0 - 4, ".nds") == 0) name[n0 - 4] = '\0';
                        fav = isFav(g_games[i]);
                        LightLock_Lock(&g_lock);
                        mark = isOnPcLocked(g_games[i]);
                        LightLock_Unlock(&g_lock);
                        markText = " PC";
                    }
                    size_t n = strlen(name);
                    const char* shown = name;
                    if (i == menuCursor && n > 43)
                    {
                        /* too long: the selected name scrolls (pauses at both ends) */
                        int steps = (int)(n - 43);
                        int t = (int)((osGetTime() / 200) % (u64)(steps + 12));
                        int off = t < 6 ? 0 : t < 6 + steps ? t - 6 : steps;
                        shown = name + off;
                    }
                    if (i == menuCursor) setLine(11 + r, "\x1b[47;30m>%c%-43.43s%-4s", fav ? '*' : ' ', shown, mark ? markText : "");
                    else if (fav) setLine(11 + r, "\x1b[33m*\x1b[0m %-43.43s\x1b[32m%s", shown, mark ? markText : "");
                    else setLine(11 + r, "  %-43.43s\x1b[32m%s", shown, mark ? markText : "");
                }
                else if (r == 0 && listCount == 0)
                    setLine(11 + r, "\x1b[31m  %s", T(g_tab == 1 ? S_NO_LIB : S_NO_GAMES));
                else setLine(11 + r, "");
            }
            if ((g_tab == 1) != native || !g_tcpConnected)
                setLine(25, "\x1b[33m%s", T(g_tab == 1 ? S_NEED_AZAHAR : S_NEED_MELONDS));
            else setLine(25, "\x1b[32m%s", T(g_tab == 1 ? S_ON3DS : S_PC_COPIED));
            setLine(26, "\x1b[90m%s", g_gameStarted ? T(S_LIST_HINT_BACK) : T(S_LIST_HINT));
        }
        else
        {
            char g[130];
            utf8ToCp437(g, sizeof(g), game);
            setLine(10, "\x1b[36m%s\x1b[0m %.40s", T(S_RUNNING), g);
            for (int r = 11; r <= 26; r++) setLine(r, "");
        }

        setLine(28, "\x1b[90m%s", T(S_HINT_MENU));
        setLine(29, "\x1b[90m%s", isNew3DS && !native ? T(S_HINT_CSTICK) : "");
        setLine(30, "\x1b[90m%s", g_tcpConnected ? "" : T(S_HINT_CONNECT));

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
        g_vblankTime = osGetTime();
    }

    g_running = false;
    close(g_listenFd);
    threadJoin(inp, U64_MAX);
    threadJoin(vid, U64_MAX);
    threadJoin(vrx, U64_MAX);
    /* a blocking send could still be waiting for the PC: give it 2 s, then
       socExit() below tears the sockets down anyway */
    threadJoin(srv, 2000000000LL);
    threadFree(inp);
    threadFree(vid);
    threadFree(vrx);
    threadFree(srv);
    close(g_udpFd);
    close(g_videoFd);
    socExit();
    if (g_mvdReady) mvdstdExit();
    irrstExit();
    setTopBacklight(true);
    setBottomBacklight(true);
    aptUnhook(&hookCookie);
    gfxExit();
    return 0;
}
