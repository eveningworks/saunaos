// sysparam16.c -- USER's system-wide settings: SystemParametersInfo.
// Windows 3.1 read them from WIN.INI at start and kept them in USER; a
// program set one for the session, or for good with SPIF_UPDATEINIFILE.
// w16 keeps them in WIN.INI too, and only there: they are the emulated
// Windows' settings, never toy-os's (docs/decisions/gui.md, Wine's
// shape), and Control Panel is what changes them.
//
// THE KEYS AND DEFAULTS ARE 3.1's: a WIN.INI made from WIN.SRC holds a
// few of them, and the rest read as Windows did with the key absent.
#include "w16_internal.h"
#include "lib/udclick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPIF_UPDATEINIFILE    0x0001
#define SPIF_SENDWININICHANGE 0x0002
#define WM_WININICHANGE       0x001A

enum { K_INT, K_BOOL };

// The settings a number or a yes/no is enough for, by SPI_ action: the
// GET action reads into *lpvParam, its SET action (when not 0) takes uParam.
static const struct spi_key {
    unsigned get, set;
    const char *sec, *key;
    int dflt, kind;
} KEYS[] = {
    { 1, 2, "windows", "Beep", 1, K_BOOL },                     // SPI_GETBEEP / SETBEEP
    { 5, 6, "windows", "BorderWidth", 3, K_INT },               // SPI_GETBORDER / SETBORDER
    { 10, 11, "windows", "KeyboardSpeed", 31, K_INT },          // SPI_GETKEYBOARDSPEED / SET
    { 14, 15, "windows", "ScreenSaveTimeOut", 120, K_INT },     // SPI_GETSCREENSAVETIMEOUT / SET
    { 16, 17, "windows", "ScreenSaveActive", 0, K_BOOL },       // SPI_GETSCREENSAVEACTIVE / SET
    { 18, 19, "Desktop", "GridGranularity", 0, K_INT },         // SPI_GETGRIDGRANULARITY / SET
    { 22, 23, "windows", "KeyboardDelay", 2, K_INT },           // SPI_GETKEYBOARDDELAY / SET
    { 25, 26, "Desktop", "IconTitleWrap", 1, K_BOOL },          // SPI_GETICONTITLEWRAP / SET
    { 27, 28, "windows", "MenuDropAlignment", 0, K_BOOL },      // SPI_GETMENUDROPALIGNMENT / SET
    { 35, 36, "windows", "CoolSwitch", 1, K_BOOL },             // SPI_GETFASTTASKSWITCH / SET
    { 0, 29, "windows", "DoubleClickWidth", 4, K_INT },         // SPI_SETDOUBLECLKWIDTH
    { 0, 30, "windows", "DoubleClickHeight", 4, K_INT },        // SPI_SETDOUBLECLKHEIGHT
    { 0, 32, "windows", "DoubleClickSpeed", 452, K_INT },       // SPI_SETDOUBLECLICKTIME
    { 0, 33, "windows", "SwapMouseButtons", 0, K_BOOL },        // SPI_SETMOUSEBUTTONSWAP
};

#define SPI_GETMOUSE             3
#define SPI_SETMOUSE             4
#define SPI_LANGDRIVER           12
#define SPI_ICONHORIZONTALSPACING 13
#define SPI_SETDESKWALLPAPER     20
#define SPI_SETDESKPATTERN       21
#define SPI_ICONVERTICALSPACING  24
#define SPI_GETICONTITLELOGFONT  31
#define SPI_SETICONTITLELOGFONT  34

// The two settings asked on every press and pointer move (below), kept until WIN.INI or the
// session's values change.
static struct { int valid; uint64_t ver; int dclick, swap; } g_mouse;

// The session's value of a setting a program set without SPIF_UPDATEINIFILE:
// this program's alone, as w16 is one program -- and gone with it.
static struct { const char *key; int value; } g_session[sizeof KEYS / sizeof KEYS[0] + 4];
static int g_nsession;

static int session(const char *key, int *value) {
    for (int i = 0; i < g_nsession; i++)
        if (!strcmp(g_session[i].key, key)) { *value = g_session[i].value; return 1; }
    return 0;
}

static void session_set(const char *key, int value) {
    g_mouse.valid = 0;
    for (int i = 0; i < g_nsession; i++)
        if (!strcmp(g_session[i].key, key)) { g_session[i].value = value; return; }
    if (g_nsession < (int)(sizeof g_session / sizeof g_session[0]))
        g_session[g_nsession++] = (typeof(g_session[0])){ key, value };
}

// A setting as the program sees it: what it set this session, else WIN.INI's, else 3.1's default.
int w16_sysparam(const char *sec, const char *key, int dflt) {
    int v;
    if (session(key, &v)) return v;
    char s[32];
    if (!w16_ini_get("WIN.INI", sec, key, s, sizeof s)) return dflt;
    if (!strcasecmp(s, "yes") || !strcasecmp(s, "true") || !strcasecmp(s, "on")) return 1;
    if (!strcasecmp(s, "no") || !strcasecmp(s, "false") || !strcasecmp(s, "off")) return 0;
    return atoi(s);
}

static void mouse_settings(void) {
    uint64_t ver = w16_ini_version("WIN.INI");
    if (g_mouse.valid && g_mouse.ver == ver) return;
    g_mouse.dclick = w16_sysparam("windows", "DoubleClickSpeed", -1);   // -1: toy-os's, asked live
    g_mouse.swap = w16_sysparam("windows", "SwapMouseButtons", 0) != 0;
    g_mouse.ver = ver;
    g_mouse.valid = 1;
}

// The double-click time the program's windows go by: WIN.INI's DoubleClickSpeed (WIN.SRC sets 3.1's
// 452) or SetDoubleClickTime's, else toy-os's own -- a tree with no WIN.INI has no Windows setting.
int w16_dclick_ms(void) {
    mouse_settings();
    return g_mouse.dclick > 0 ? g_mouse.dclick : udclick_ms();
}

uint64_t w16_dclick_ticks(void) { return (uint64_t)w16_dclick_ms() * USER_HZ / 1000; }

// `buttons` (WIN_MOUSE_* bits) as the program hears them: left and right exchanged while
// SwapMouseButtons is set -- in its windows only, never toy-os's.
unsigned w16_mouse_swap(unsigned buttons) {
    mouse_settings();
    if (!g_mouse.swap) return buttons;
    return (buttons & ~3u) | (buttons & 1) << 1 | (buttons & 2) >> 1;
}

static void put(const char *sec, const char *key, int value, int kind, unsigned flags) {
    session_set(key, value);
    if (!(flags & SPIF_UPDATEINIFILE)) return;
    char v[16];
    if (kind == K_BOOL) snprintf(v, sizeof v, "%s", value ? "yes" : "no");
    else snprintf(v, sizeof v, "%d", value);
    w16_ini_set("WIN.INI", sec, key, v);
}

static int write_word(struct w16 *w, uint32_t far, int value) {
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 2, 1);
    if (!p) return 0;
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    return 1;
}

// The icon titles' font, as [Desktop] names it: 3.1's MS Sans Serif 8 point, at the VGA's 96 dpi.
static int icon_title_font(struct w16 *w, uint32_t far, unsigned size) {
    if (size < 50) return 0;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 50, 1);
    if (!p) return 0;
    char face[32];
    if (!w16_ini_get("WIN.INI", "Desktop", "IconTitleFaceName", face, sizeof face))
        snprintf(face, sizeof face, "MS Sans Serif");
    int points = w16_sysparam("Desktop", "IconTitleSize", 8);
    int height = -(points * 96 + 36) / 72;
    memset(p, 0, 50);
    p[0] = (uint8_t)height;
    p[1] = (uint8_t)(height >> 8);
    p[8] = 400 & 0xFF;                                   // FW_NORMAL
    p[9] = 400 >> 8;
    p[17] = 0x22;                                        // VARIABLE_PITCH | FF_SWISS
    for (int i = 0; i < 31 && face[i]; i++) p[18 + i] = (uint8_t)face[i];
    return 1;
}

static void tell(struct w16 *w, unsigned flags) {
    if ((flags & SPIF_SENDWININICHANGE) && w->win.hwnd)
        w16_send(w, w->win.hwnd, WM_WININICHANGE, 0, 0);   // a NULL section: any may have changed
}

// SystemParametersInfo(uAction, uParam, lpvParam, fuWinIni) -> BOOL
// Windows: Reads or sets one of Windows' settings -- the screen saver, the icon spacing, the
// keyboard speed -- by its SPI_ number; SPIF_UPDATEINIFILE writes the change to WIN.INI as well,
// and SPIF_SENDWININICHANGE tells every window WIN.INI changed.
// w16: The same for each of 3.1's actions, from WIN.INI with 3.1's defaults: a change lasts the
// program's run, and WIN.INI's when asked. They are the emulated Windows' settings -- toy-os's own
// keyboard, mouse and screen saver are System Settings' -- and only the double-click time, the
// border width and the beep change what w16 does; the screen saver, the wallpaper and the icon
// spacing have no desktop of Windows' to act on. WM_WININICHANGE goes to the program's own windows.
static int api_SystemParametersInfo(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    unsigned action = w16_arg16(a), uparam = w16_arg16(a);
    uint32_t lpv = w16_arg32(a);
    unsigned flags = w16_arg16(a);
    int ok = 0;
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0] && !ok; i++) {
        const struct spi_key *k = &KEYS[i];
        if (action == k->get && k->get) ok = lpv && write_word(w, lpv, w16_sysparam(k->sec, k->key, k->dflt));
        else if (action == k->set) {
            put(k->sec, k->key, k->kind == K_BOOL ? uparam != 0 : (int)uparam, k->kind, flags);
            ok = 1;
        }
    }
    if (!ok) {
        switch (action) {
        case SPI_GETMOUSE:                               // three words: the two thresholds and the speed
            ok = lpv && write_word(w, lpv, w16_sysparam("windows", "MouseThreshold1", 6)) &&
                 write_word(w, lpv + 2, w16_sysparam("windows", "MouseThreshold2", 10)) &&
                 write_word(w, lpv + 4, w16_sysparam("windows", "MouseSpeed", 1));
            break;
        case SPI_SETMOUSE: {
            const uint8_t *p = lpv ? cpu16_ptr(&w->cpu, (uint16_t)(lpv >> 16), (uint16_t)lpv, 6, 0) : 0;
            if (!p) break;
            put("windows", "MouseThreshold1", (int16_t)(p[0] | p[1] << 8), K_INT, flags);
            put("windows", "MouseThreshold2", (int16_t)(p[2] | p[3] << 8), K_INT, flags);
            put("windows", "MouseSpeed", (int16_t)(p[4] | p[5] << 8), K_INT, flags);
            ok = 1;
            break;
        }
        case SPI_ICONHORIZONTALSPACING:
        case SPI_ICONVERTICALSPACING: {
            // A NULL lpvParam sets the spacing to uParam; otherwise uParam is ignored and the
            // spacing is read into *lpvParam.
            const char *key = action == SPI_ICONHORIZONTALSPACING ? "IconSpacing" : "IconVerticalSpacing";
            int dflt = action == SPI_ICONHORIZONTALSPACING ? 77 : 72;
            if (lpv) ok = write_word(w, lpv, w16_sysparam("Desktop", key, dflt));
            else { put("Desktop", key, (int)uparam, K_INT, flags); ok = 1; }
            break;
        }
        case SPI_GETICONTITLELOGFONT:
            ok = lpv && icon_title_font(w, lpv, uparam);
            break;
        case SPI_SETICONTITLELOGFONT:
        case SPI_LANGDRIVER:
            ok = 1;                                      // accepted; nothing of w16's draws icon titles
            break;
        case SPI_SETDESKWALLPAPER:
        case SPI_SETDESKPATTERN: {
            char v[128] = "";
            if (lpv && cpu16_str(&w->cpu, (uint16_t)(lpv >> 16), (uint16_t)lpv, v, sizeof v) < 0) break;
            // A NULL pattern means "read WIN.INI's again" -- nothing here holds one apart from it.
            if (lpv && (flags & SPIF_UPDATEINIFILE))
                w16_ini_set("WIN.INI", "Desktop", action == SPI_SETDESKWALLPAPER ? "Wallpaper" : "Pattern", v);
            ok = 1;
            break;
        }
        default:
            break;
        }
    }
    if (ok) tell(w, flags);
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 10;
}

// SetDoubleClickTime(wCount) -> void
// GetDoubleClickTime() -> WORD
// Windows: Sets, and reads, the milliseconds two clicks may be apart and still be one double click,
// for every program; 0 sets the default, 500. Control Panel's Mouse writes WIN.INI's
// DoubleClickSpeed itself.
// w16: The same for the program's own windows, for its run: WIN.INI's DoubleClickSpeed when the
// key is there, else toy-os's double-click setting (Settings > Mouse), which this never changes.
static int api_DoubleClickTime(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    if (ordinal == 21) {
        w->cpu.r[R_AX] = (uint16_t)w16_dclick_ms();
        return 0;
    }
    unsigned ms = w16_arg16(a);
    session_set("DoubleClickSpeed", ms ? (int)ms : 500);
    return 2;
}

// SwapMouseButton(fSwap) -> BOOL
// Windows: Exchanges the left and right buttons for every program, or puts them back; whether they
// were exchanged before. Control Panel's Mouse writes WIN.INI's SwapMouseButtons itself.
// w16: The same in the program's own windows, for its run; toy-os's buttons stay as they are.
static int api_SwapMouseButton(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int on = w16_arg16(a) != 0;
    w->cpu.r[R_AX] = (uint16_t)(w16_sysparam("windows", "SwapMouseButtons", 0) != 0);
    session_set("SwapMouseButtons", on);
    return 2;
}

const struct w16_api g_sysparam_api[] = {
    { 20, api_DoubleClickTime },
    { 21, api_DoubleClickTime },
    { 186, api_SwapMouseButton },
    { 483, api_SystemParametersInfo },
};
const unsigned g_sysparam_api_n = sizeof g_sysparam_api / sizeof g_sysparam_api[0];
