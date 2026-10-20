// commdlg16.c -- COMMDLG, the common dialogs, in Windows 3.1's own look
// (the "Win16 Notepad" canvas's O1): Open and Save As -- a file list, the
// folders down to the current one, the file types and the drives --, Find,
// and a PrintDlg that says there is no printer. Windows shipped COMMDLG
// as a DLL of 16-bit code; this is w16's own, as Wine's is, built on the
// dialog manager and the LISTBOX and COMBOBOX classes.
//
// The dialogs see the files the program does (file16.c): drive C:, the
// Win16 folder, and Z:, all of toy-os. A program's hook procedure
// (OFN_ENABLEHOOK, FR_ENABLEHOOK) hears every message first; a custom
// template is refused, as CDERR_NOTEMPLATE.
#include "w16_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lib/ufile.h"
#include "ne.h"

#define OFN_READONLY        0x0001
#define OFN_OVERWRITEPROMPT 0x0002
#define OFN_HIDEREADONLY    0x0004
#define OFN_NOCHANGEDIR     0x0008
#define OFN_SHOWHELP        0x0010
#define OFN_ENABLEHOOK      0x0020
#define OFN_ENABLETEMPLATE  0x0040
#define OFN_ENABLETEMPLATEHANDLE 0x0080
#define OFN_PATHMUSTEXIST   0x0800
#define OFN_FILEMUSTEXIST   0x1000
#define OFN_EXTENSIONDIFFERENT 0x0400

#define FR_DOWN       0x00000001
#define FR_WHOLEWORD  0x00000002
#define FR_MATCHCASE  0x00000004
#define FR_FINDNEXT   0x00000008
#define FR_DIALOGTERM 0x00000040
#define FR_SHOWHELP   0x00000080
#define FR_ENABLEHOOK 0x00000100
#define FR_ENABLETEMPLATE 0x00000200
#define FR_NOUPDOWN   0x00000400
#define FR_NOMATCHCASE 0x00000800
#define FR_NOWHOLEWORD 0x00001000
#define FR_HIDEUPDOWN 0x00004000
#define FR_HIDEMATCHCASE 0x00008000
#define FR_HIDEWHOLEWORD 0x00010000

#define CDERR_NOTEMPLATE      0x0003
#define CDERR_MEMALLOCFAILURE 0x0009
#define PDERR_NODEFAULTPRN    0x1008
#define FNERR_BUFFERTOOSMALL  0x3003

// The controls' ids, COMMDLG's own (DLGS.H), so a hook finds them.
#define IDOK     1
#define IDCANCEL 2
#define PSH_HELP 1038
#define CHX1     1040
#define CHX2     1041
#define RAD1     1056
#define RAD2     1057
#define GRP1     1072
#define STC1     1088
#define STC2     1089
#define STC3     1090
#define STC4     1091
#define LST1     1120
#define LST2     1121
#define CMB1     1136
#define CMB2     1137
#define EDT1     1152

#define BS_PUSHBUTTON     0x00
#define BS_DEFPUSHBUTTON  0x01
#define BS_AUTOCHECKBOX   0x03
#define BS_GROUPBOX       0x07
#define BS_AUTORADIOBUTTON 0x09
#define ES_AUTOHSCROLL    0x0080
#define LBS_NOTIFY        0x0001
#define LBS_SORT          0x0002
#define LBS_DISABLENOSCROLL 0x1000
#define CBS_DROPDOWNLIST  0x0003
#define SS_NOPREFIX       0x0080
#define LB_ADDSTRING      (WM_USER + 1)
#define LB_RESETCONTENT   (WM_USER + 5)
#define LB_SETCURSEL      (WM_USER + 7)
#define LB_GETCURSEL      (WM_USER + 9)
#define LB_GETITEMDATA    (WM_USER + 26)
#define LB_SETITEMDATA    (WM_USER + 27)
#define BM_GETCHECK       (WM_USER + 0)
#define BM_SETCHECK       (WM_USER + 1)
#define EM_SETSEL         (WM_USER + 1)
#define EM_LIMITTEXT      (WM_USER + 21)
#define LBN_SELCHANGE     1
#define LBN_DBLCLK        2
#define CBN_SELCHANGE     1
#define EN_CHANGE         0x0300
#define MB_YESNO          0x0004
#define MB_ICONEXCLAMATION 0x0030
#define IDYES             6
#define COLOR_WINDOW      5

static uint16_t g_error;                          // CommDlgExtendedError's

// --- far memory ----------------------------------------------------------------------

static uint8_t *at(struct w16 *w, uint32_t far, unsigned n, int write) {
    return (far >> 16) ? cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, n, write) : 0;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }

static char *far_dup(struct w16 *w, uint32_t far) {
    if (!(far >> 16)) return strdup("");
    char *s = malloc(512);
    if (s && cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, s, 512) < 0) { free(s); return 0; }
    return s;
}

// A double-NUL list at a far pointer (lpstrFilter), as one allocation.
static char *far_list(struct w16 *w, uint32_t far, size_t *len) {
    *len = 0;
    if (!(far >> 16)) return 0;
    size_t n = 0;
    for (;;) {
        uint8_t *p = at(w, far + (uint32_t)n, 2, 0);
        if (!p) { w->cpu.fault = 0; break; }
        if (!p[0] && !p[1]) { n += 2; break; }
        n++;
        if (n > 4096) break;
    }
    uint8_t *p = n ? at(w, far, (unsigned)n, 0) : 0;
    char *out = p ? malloc(n) : 0;
    if (out) { memcpy(out, p, n); *len = n; }
    return out;
}

// --- text in controls ------------------------------------------------------------------

static void set_text(struct w16 *w, struct w16_wnd *c, const char *t) {
    size_t n = strlen(t);
    uint16_t a = w16_sys_push(w, (unsigned)n + 1);
    if (w->cpu.fault) return;
    memcpy(w->sysmem + a, t, n + 1);
    w16_send(w, c->hwnd, WM_SETTEXT, 0, (uint32_t)w->sysdata << 16 | a);
    w16_sys_pop(w, a);
}

// The name field set to `t`, all of it selected so typing replaces it.
// 3.1's dialogs show names in lower case -- FAT's are upper -- and w16
// resolves a name without regard to case, so the lowered one still opens.
static const char *lowered(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (; s[n] && n + 1 < cap; n++) out[n] = s[n] >= 'A' && s[n] <= 'Z' ? (char)(s[n] + 32) : s[n];
    out[n] = 0;
    return out;
}

static void set_name(struct w16 *w, struct w16_wnd *e, const char *t) {
    char low[160];
    set_text(w, e, lowered(t, low, sizeof low));
    w16_send(w, e->hwnd, EM_SETSEL, 0, 0xFFFF0000u);
}

static char *get_text(struct w16 *w, struct w16_wnd *c) {
    return c && c->host == w16_edit_proc ? w16_edit_text(w, c) : strdup(c && c->text ? c->text : "");
}

static struct w16_wnd *item(struct w16 *w, struct w16_wnd *d, uint16_t id) { return w16_dlg_item(w, d->hwnd, id); }

// --- 3.11's own folder and drive pictures -------------------------------------------

// From the user's own COMMDLG.DLL in C:\WINDOWS\SYSTEM, when it is there:
// its bitmap 576, eight 16-by-16 pictures in a row -- open folder, the
// current folder, a shut one, then floppy, hard disk, network, CD and RAM
// drives -- blue (0000FF) where nothing is drawn. Read once, like any DLL,
// by w16's NE parser; no Microsoft picture is in toy-os itself.
#define ICON_SIZE 16
static uint32_t g_icon[W16_ICON_COUNT][ICON_SIZE * ICON_SIZE];
static int g_icon_state;                         // 0 not tried, 1 loaded, -1 none

static void load_icons(void) {
    g_icon_state = -1;
    static char path[1024];
    if (w16_dos_path("C:\\WINDOWS\\SYSTEM\\COMMDLG.DLL", path, sizeof path, 0, 0, 0) != 0) return;
    unsigned char *img = 0;
    size_t n = 0;
    if (ufile_slurp(path, 1u << 20, &img, &n) != UFILE_OK) return;
    static struct ne_module m;
    struct ne_resource r;
    struct gdi_dib dib;
    if (ne_parse(img, (uint32_t)n, &m) < 0 || ne_resource_find(&m, NE_RT_BITMAP, 0, 576, 0, &r) < 0 ||
        gdi_dib_header(img + r.off, r.len, 0, &dib) < 0 || dib.w < ICON_SIZE * W16_ICON_COUNT ||
        (dib.h < ICON_SIZE && -dib.h < ICON_SIZE)) {
        free(img);
        return;
    }
    int rows = dib.h < 0 ? -dib.h : dib.h;
    if (dib.header_len + (uint64_t)dib.stride * (uint64_t)rows > r.len) { free(img); return; }
    for (int y = 0; y < ICON_SIZE; y++) {
        const uint8_t *row = img + r.off + dib.header_len + (size_t)dib.stride * (size_t)(dib.h < 0 ? y : rows - 1 - y);
        for (int i = 0; i < W16_ICON_COUNT; i++)
            for (int x = 0; x < ICON_SIZE; x++) {
                uint32_t px = gdi_dib_px(&dib, row, i * ICON_SIZE + x) & 0xFFFFFF;
                g_icon[i][y * ICON_SIZE + x] = px == 0x0000FF ? 0 : 0xFF000000u | px;
            }
    }
    free(img);
    g_icon_state = 1;
}

const uint32_t *w16_commdlg_icon(int which) {
    if (!g_icon_state) load_icons();
    return g_icon_state > 0 && which >= 0 && which < W16_ICON_COUNT ? g_icon[which] : 0;
}

// --- the dialogs' shared machinery -------------------------------------------------------

// A dialog unit to pixels: x per 4, y per 8.
// In COMMDLG's own templates' units -- FONT 8, "Helv", 7 by 13 -- mapped
// from the template's origin as Windows maps them (g_org: Open's 36,24,
// Find's 30,73, from COMMDLG.DLL).
static int g_org_x, g_org_y;

static struct w16_wnd *add(struct w16 *w, struct w16_wnd *d, const char *cls, uint16_t id, uint32_t style,
                           int x, int y, int cw, int ch, const char *text) {
    return w16_dlg_add(w, d, cls, id, style | WS_VISIBLE, w16_du_x(g_org_x, x, 7), w16_du_y(g_org_y, y, 13),
                       w16_du_w(cw, 7), w16_du_h(ch, 13), text);
}

// The program's hook, when it has one: it hears the message first.
static int hook(struct w16 *w, struct w16_wnd *d, uint32_t fn, uint16_t msg, uint16_t wp, uint32_t lp) {
    if (!fn) return 0;
    return (w16_call_wndproc(w, (uint16_t)(fn >> 16), (uint16_t)fn, d->hwnd, msg, wp, lp) & 0xFFFF) != 0;
}

// --- Open and Save As -------------------------------------------------------------------

struct ofn {
    uint32_t far;                 // the program's OPENFILENAME
    uint32_t flags, hook;
    int save;
    char *filter;                 // its pairs: "Text Files (*.TXT)\0*.TXT\0..."
    size_t filter_len;
    int ntypes, type;             // how many pairs, which one is chosen (0-based)
    char dir[160];                // the folder shown, a full DOS path
    char pattern[64];             // what the file list shows: "*.TXT;*.DOC"
    char *defext;
};

// The `i`th string of the filter list (0-based), or NULL.
static const char *filter_str(const struct ofn *o, int i) {
    const char *p = o->filter;
    if (!p) return 0;
    for (; i > 0 && (size_t)(p - o->filter) < o->filter_len; i--) p += strlen(p) + 1;
    return (size_t)(p - o->filter) < o->filter_len && *p ? p : 0;
}

static int matches_one(const char *pat, const char *name) {
    for (; *pat; pat++, name++) {
        if (*pat == '*') {
            if (!strcmp(pat, "*.*") || !strcmp(pat, "*")) return 1;
            for (const char *n = name; ; n++) {
                if (matches_one(pat + 1, n)) return 1;
                if (!*n) return 0;
            }
        }
        char a = *pat >= 'a' && *pat <= 'z' ? (char)(*pat - 32) : *pat;
        char b = *name >= 'a' && *name <= 'z' ? (char)(*name - 32) : *name;
        if (!*name || (a != '?' && a != b)) return 0;
    }
    return !*name;
}

// One of the ';'-separated patterns.
static int matches(const char *patterns, const char *name) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s", patterns);
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(0, ";")) {
        while (*tok == ' ') tok++;
        if (matches_one(tok, name)) return 1;
    }
    return 0;
}

static int has_wild(const char *s) { return strchr(s, '*') || strchr(s, '?'); }

// Drive `d`'s row in the drive box, which lists every drive C: to Z: in order.
static int drive_row(char d) {
    int row = 0;
    d = (char)(d & ~0x20);
    for (char k = 'C'; k < d && k <= 'Z'; k++) row += w16_drive_root(k) != 0;
    return row;
}

static void fill_lists(struct w16 *w, struct w16_wnd *d) {
    struct ofn *o = d->dlgdata;
    struct w16_wnd *files = item(w, d, LST1), *dirs = item(w, d, LST2), *label = item(w, d, STC1);
    static char host[1024], path[1400];          // the task thread's alone
    if (!files || !dirs) return;
    w16_send(w, files->hwnd, LB_RESETCONTENT, 0, 0);
    w16_send(w, dirs->hwnd, LB_RESETCONTENT, 0, 0);
    char lower[160];
    size_t n = 0;
    for (; o->dir[n] && n < sizeof lower - 1; n++) lower[n] = o->dir[n] >= 'A' && o->dir[n] <= 'Z' ? (char)(o->dir[n] + 32) : o->dir[n];
    lower[n] = 0;
    if (label) set_text(w, label, lower);
    // The drive box follows the folder, however it was reached.
    struct w16_wnd *drives = item(w, d, CMB2), *dl = drives ? w16_combo_list(w, drives) : 0;
    if (dl) {
        w16_send(w, dl->hwnd, LB_SETCURSEL, (uint16_t)drive_row(lower[0]), 0);
        w16_combo_changed(w, drives);
    }
    // The folders down to this one, open, each a step further in.
    int depth = 0;
    char part[160];
    snprintf(part, sizeof part, "%.2s\\", lower);
    int idx = (int)w16_listbox_str(w, dirs, LB_ADDSTRING, 0, part);
    w16_send(w, dirs->hwnd, LB_SETITEMDATA, (uint16_t)idx, 1);
    for (char *p = lower + 3; *p; depth++) {
        char *end = strchr(p, '\\');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        snprintf(part, sizeof part, "%.*s", (int)len, p);
        idx = (int)w16_listbox_str(w, dirs, LB_ADDSTRING, 0, part);
        w16_send(w, dirs->hwnd, LB_SETITEMDATA, (uint16_t)idx, (uint32_t)(depth + 1) << 8 | 1);
        p += len;
        while (*p == '\\') p++;
    }
    // The last open folder is the current one, pictured as 3.11 did (kind 4).
    uint32_t last = (uint32_t)w16_send(w, dirs->hwnd, LB_GETITEMDATA, (uint16_t)idx, 0);
    w16_send(w, dirs->hwnd, LB_SETITEMDATA, (uint16_t)idx, (last & ~0xFFu) | 4);
    // No selection: 3.11 lists the open folders without lighting the current
    // one (a press or the arrows choose).
    // What is in it: folders after the open ones, files that match.
    if (w16_dos_path(o->dir, host, sizeof host, 0, 0, 0) != 0) return;
    DIR *dir = opendir(host);
    struct dirent *de;
    char **subs = 0;
    int nsubs = 0;
    while (dir && (de = readdir(dir))) {
        if (de->d_name[0] == '.') continue;
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", strcmp(host, "/") ? host : "", de->d_name);
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            char **g = realloc(subs, sizeof *g * (size_t)(nsubs + 1));
            char low[256];
            if (g && (g[nsubs] = strdup(lowered(de->d_name, low, sizeof low)))) { subs = g; nsubs++; }
            else if (g) subs = g;
        } else if (matches(o->pattern, de->d_name)) {
            char low[256];
            w16_listbox_str(w, files, LB_ADDSTRING, 0, lowered(de->d_name, low, sizeof low));
        }
    }
    if (dir) closedir(dir);
    // Sorted without case, after the open folders.
    for (int i = 1; i < nsubs; i++)
        for (int j = i; j > 0 && strcasecmp(subs[j], subs[j - 1]) < 0; j--) {
            char *t = subs[j]; subs[j] = subs[j - 1]; subs[j - 1] = t;
        }
    for (int i = 0; i < nsubs; i++) {
        idx = (int)w16_listbox_str(w, dirs, 0x402 /* LB_INSERTSTRING */, 0xFFFF, subs[i]);
        w16_send(w, dirs->hwnd, LB_SETITEMDATA, (uint16_t)idx, (uint32_t)(depth + 1) << 8 | 2);
        free(subs[i]);
    }
    free(subs);
}

// The folder the folder list's item `i` names.
static void folder_of(struct w16 *w, struct w16_wnd *d, int i, char *out, size_t cap) {
    struct ofn *o = d->dlgdata;
    struct w16_wnd *dirs = item(w, d, LST2);
    uint32_t data = (uint32_t)w16_send(w, dirs->hwnd, LB_GETITEMDATA, (uint16_t)i, 0);
    int depth = (int)(data >> 8 & 0xFF), open = (data & 0xFF) == 1 || (data & 0xFF) == 4;
    const char *name = w16_listbox_text(dirs, i);
    if (!name) { snprintf(out, cap, "%s", o->dir); return; }
    if (!open) {                                 // a folder inside the current one
        size_t n = strlen(o->dir);
        snprintf(out, cap, "%s%s%s", o->dir, n && o->dir[n - 1] == '\\' ? "" : "\\", name);
        return;
    }
    // An open one: the current path cut after `depth` components.
    snprintf(out, cap, "%.2s\\", o->dir);
    const char *p = o->dir + 3;
    for (int k = 0; k < depth && *p; k++) {
        const char *end = strchr(p, '\\');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        size_t at = strlen(out);
        snprintf(out + at, cap - at, "%s%.*s", at > 3 ? "\\" : "", (int)len, p);
        p += len;
        while (*p == '\\') p++;
    }
}

static void chdir_to(struct w16 *w, struct w16_wnd *d, const char *dos) {
    struct ofn *o = d->dlgdata;
    char host[1024], full[160];
    struct stat st;
    if (w16_dos_path(dos, host, sizeof host, 0, full, sizeof full) != 0 || stat(host, &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    snprintf(o->dir, sizeof o->dir, "%s", full);
    fill_lists(w, d);
}

static void set_type(struct w16 *w, struct w16_wnd *d, int t) {
    struct ofn *o = d->dlgdata;
    const char *pat = filter_str(o, 2 * t + 1);
    o->type = t;
    snprintf(o->pattern, sizeof o->pattern, "%s", pat ? pat : "*.*");
    struct w16_wnd *e = item(w, d, EDT1);
    char *cur = e ? get_text(w, e) : 0;
    if (e && (!cur || !cur[0] || has_wild(cur))) set_name(w, e, o->pattern);
    free(cur);
    fill_lists(w, d);
}

// The program's buffers filled from the chosen file. 0 when its buffer is
// too small (FNERR_BUFFERTOOSMALL, the size it needs in its first word).
static int give_back(struct w16 *w, struct ofn *o, const char *full) {
    uint8_t *of = at(w, o->far, 72, 1);
    if (!of) return 0;
    uint32_t file = rd32(of + 24), max = rd32(of + 28), title = rd32(of + 32), tmax = rd32(of + 36);
    size_t n = strlen(full);
    if (n + 1 > max) {
        uint8_t *p = at(w, file, 2, 1);
        if (p) wr16(p, (uint16_t)(n + 1));
        g_error = FNERR_BUFFERTOOSMALL;
        return 0;
    }
    uint8_t *p = at(w, file, (unsigned)n + 1, 1);
    if (!p) return 0;
    memcpy(p, full, n + 1);
    const char *base = strrchr(full, '\\');
    base = base ? base + 1 : full + 2;
    const char *dot = strrchr(base, '.');
    of = at(w, o->far, 72, 1);                   // still where it was: nothing moves the program's memory
    if (!of) return 0;
    wr16(of + 52, (uint16_t)(base - full));
    wr16(of + 54, (uint16_t)(dot ? dot + 1 - full : (const char *)full + n - full));
    wr32(of + 20, (uint32_t)o->type + 1);
    if (title >> 16 && tmax) {
        size_t bn = strlen(base);
        if (bn > tmax - 1) bn = tmax - 1;
        uint8_t *t = at(w, title, (unsigned)bn + 1, 1);
        if (t) { memcpy(t, base, bn); t[bn] = 0; }
    }
    return 1;
}

// OK: a pattern shows those files, a folder opens, a file ends the dialog.
static void ok(struct w16 *w, struct w16_wnd *d) {
    struct ofn *o = d->dlgdata;
    struct w16_wnd *e = item(w, d, EDT1);
    char *name = e ? get_text(w, e) : 0;
    if (!name) return;
    char *s = name;
    while (*s == ' ') s++;
    for (size_t n = strlen(s); n && s[n - 1] == ' '; n--) s[n - 1] = 0;
    if (!*s) { free(name); return; }
    if (has_wild(s)) {
        snprintf(o->pattern, sizeof o->pattern, "%s", strrchr(s, '\\') ? strrchr(s, '\\') + 1 : s);
        fill_lists(w, d);
        set_name(w, e, o->pattern);
        free(name);
        return;
    }
    // Relative to the folder shown.
    static char dos[300], host[1024], full[300], msg[400];
    if (s[1] == ':' || s[0] == '\\') snprintf(dos, sizeof dos, "%s", s);
    else snprintf(dos, sizeof dos, "%s%s%s", o->dir, o->dir[strlen(o->dir) - 1] == '\\' ? "" : "\\", s);
    struct stat st;
    int err = w16_dos_path(dos, host, sizeof host, 1, full, sizeof full);
    if (!err && stat(host, &st) == 0 && S_ISDIR(st.st_mode)) {
        chdir_to(w, d, full);
        set_name(w, e, o->pattern);
        free(name);
        return;
    }
    // No extension typed: the default one.
    const char *base = strrchr(full, '\\');
    if (!err && o->defext && o->defext[0] && !strchr(base ? base : full, '.')) {
        size_t n = strlen(full);
        snprintf(full + n, sizeof full - n, ".%s", o->defext);
        err = w16_dos_path(full, host, sizeof host, 1, 0, 0);
    }
    int exists = !err && stat(host, &st) == 0;
    if (err || (!exists && (o->flags & OFN_FILEMUSTEXIST))) {
        snprintf(msg, sizeof msg, "%s\nFile not found.\nPlease verify the correct filename was given.", s);
        w16_message_box(w, d->hwnd, msg, o->save ? "Save As" : "Open", MB_ICONEXCLAMATION);
        free(name);
        return;
    }
    if (o->save && exists && (o->flags & OFN_OVERWRITEPROMPT)) {
        snprintf(msg, sizeof msg, "%s already exists.\nDo you want to replace it?", full);
        if (w16_message_box(w, d->hwnd, msg, "Save As", MB_YESNO | MB_ICONEXCLAMATION) != IDYES) {
            free(name);
            return;
        }
    }
    free(name);
    if (!give_back(w, o, full)) { d->result = 0; d->ended = 1; return; }
    if (!(o->flags & OFN_NOCHANGEDIR)) w16_dos_chdir(o->dir);
    d->result = 1;
    d->ended = 1;
}

static int ofn_proc(struct w16 *w, struct w16_wnd *d, uint16_t msg, uint16_t wp, uint32_t lp) {
    struct ofn *o = d->dlgdata;
    if (!o) return 0;
    if (hook(w, d, o->hook, msg, wp, lp) && msg != WM_INITDIALOG) return 1;
    if (msg == WM_INITDIALOG) return 1;
    if (msg != WM_COMMAND) return 0;
    uint16_t code = (uint16_t)(lp >> 16);
    switch (wp) {
    case IDOK: ok(w, d); return 1;
    case IDCANCEL: d->result = 0; d->ended = 1; return 1;
    case PSH_HELP: {                             // the owner hears it, as COMMDLG's HELPMSGSTRING
        uint16_t m = w16_atom_add(w, "commdlg_help");
        if (m && d->parent) w16_send(w, d->parent, m, d->hwnd, o->far);
        return 1;
    }
    case LST1:
        if (code == LBN_SELCHANGE || code == LBN_DBLCLK) {
            struct w16_wnd *l = item(w, d, LST1), *e = item(w, d, EDT1);
            int i = l ? (int16_t)w16_send(w, l->hwnd, LB_GETCURSEL, 0, 0) : -1;
            const char *t = l ? w16_listbox_text(l, i) : 0;
            if (t && e) set_text(w, e, t);
            if (code == LBN_DBLCLK && t) ok(w, d);
        }
        return 1;
    case LST2:
        if (code == LBN_DBLCLK) {
            struct w16_wnd *l = item(w, d, LST2);
            int i = l ? (int16_t)w16_send(w, l->hwnd, LB_GETCURSEL, 0, 0) : -1;
            char to[200];
            if (i >= 0) { folder_of(w, d, i, to, sizeof to); chdir_to(w, d, to); }
        }
        return 1;
    case CMB1:
        if (code == CBN_SELCHANGE) {
            struct w16_wnd *c = item(w, d, CMB1), *l = c ? w16_combo_list(w, c) : 0;
            int i = l ? (int16_t)w16_send(w, l->hwnd, LB_GETCURSEL, 0, 0) : -1;
            if (i >= 0) set_type(w, d, i);
        }
        return 1;
    case CMB2:
        if (code == CBN_SELCHANGE) {
            struct w16_wnd *c = item(w, d, CMB2), *l = c ? w16_combo_list(w, c) : 0;
            int i = l ? (int16_t)w16_send(w, l->hwnd, LB_GETCURSEL, 0, 0) : -1;
            const char *t = l ? w16_listbox_text(l, i) : 0;
            if (t) {
                char root[4] = { t[0], ':', '\\', 0 };
                chdir_to(w, d, root);
            }
        }
        return 1;
    default:
        return 0;
    }
}

static int file_dialog(struct w16 *w, struct w16_args *a, int save) {
    uint32_t far = w16_arg32(a);
    struct cpu16 *c = &w->cpu;
    c->r[R_AX] = 0;
    g_error = 0;
    uint8_t *of = at(w, far, 72, 0);
    if (!of) return -1;
    struct ofn *o = calloc(1, sizeof *o);
    if (!o) { g_error = CDERR_MEMALLOCFAILURE; return 4; }
    o->far = far;
    o->save = save;
    o->flags = rd32(of + 48);
    o->hook = o->flags & OFN_ENABLEHOOK ? rd32(of + 64) : 0;
    uint16_t owner = rd16(of + 4);
    uint32_t file = rd32(of + 24), init = rd32(of + 40), title = rd32(of + 44), def = rd32(of + 56);
    uint32_t index = rd32(of + 20), filter = rd32(of + 8);
    if (o->flags & (OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) {
        free(o);
        g_error = CDERR_NOTEMPLATE;
        return 4;
    }
    o->filter = far_list(w, filter, &o->filter_len);
    for (const char *p = o->filter; p && (size_t)(p - o->filter) < o->filter_len && *p; p += strlen(p) + 1) o->ntypes++;
    o->ntypes /= 2;
    o->type = index >= 1 && (int)index <= o->ntypes ? (int)index - 1 : 0;
    char *initial = far_dup(w, file), *dir = init >> 16 ? far_dup(w, init) : 0, *cap = title >> 16 ? far_dup(w, title) : 0;
    o->defext = def >> 16 ? far_dup(w, def) : 0;
    snprintf(o->dir, sizeof o->dir, "%s", w16_dos_cwd());
    if (dir && dir[0]) {
        char host[1024], full[160];
        if (w16_dos_path(dir, host, sizeof host, 0, full, sizeof full) == 0) snprintf(o->dir, sizeof o->dir, "%s", full);
    }
    const char *pat = filter_str(o, 2 * o->type + 1);
    snprintf(o->pattern, sizeof o->pattern, "%s", pat ? pat : "*.*");

    g_org_x = 36;                                // FILEOPENORD's place
    g_org_y = 24;
    struct w16_wnd *d = w16_dlg_new(w, owner, 0, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_DLGFRAME,
                                    w16_du_w(264, 7), w16_du_h(134, 13), cap && cap[0] ? cap : save ? "Save As" : "Open", 0, ofn_proc);
    if (!d) { g_error = CDERR_MEMALLOCFAILURE; goto out; }
    w16_dlg_set_font(d, 8);
    d->dlgdata = o;
    add(w, d, "STATIC", STC3, 0, 6, 6, 76, 9, "File &Name:");
    struct w16_wnd *e = add(w, d, "EDIT", EDT1, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 6, 16, 90, 12, "");
    struct w16_wnd *files = add(w, d, "LISTBOX", LST1, WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_SORT |
                                LBS_DISABLENOSCROLL, 6, 32, 90, 68, "");
    add(w, d, "STATIC", -1, 0, 110, 6, 92, 9, "&Directories:");
    add(w, d, "STATIC", STC1, SS_NOPREFIX, 110, 18, 92, 9, "");
    struct w16_wnd *dirs = add(w, d, "LISTBOX", LST2, WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY |
                               LBS_DISABLENOSCROLL, 110, 32, 92, 68, "");
    add(w, d, "STATIC", STC2, 0, 6, 104, 90, 9, save ? "Save File as &Type:" : "List Files of &Type:");
    struct w16_wnd *types = add(w, d, "COMBOBOX", CMB1, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 6, 114, 90, 68, "");
    add(w, d, "STATIC", STC4, 0, 110, 104, 92, 9, "Dri&ves:");
    struct w16_wnd *drives = add(w, d, "COMBOBOX", CMB2, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 110, 114, 92, 68, "");
    add(w, d, "BUTTON", IDOK, WS_TABSTOP | BS_DEFPUSHBUTTON, 208, 6, 50, 14, "OK");
    add(w, d, "BUTTON", IDCANCEL, WS_TABSTOP, 208, 24, 50, 14, "Cancel");
    if (o->flags & OFN_SHOWHELP) add(w, d, "BUTTON", PSH_HELP, WS_TABSTOP, 208, 46, 50, 14, "&Help");
    if (!(o->flags & OFN_HIDEREADONLY))
        add(w, d, "BUTTON", CHX1, WS_TABSTOP | BS_AUTOCHECKBOX, 208, 68, 50, 12, "&Read Only");
    if (!e || !files || !dirs || !types || !drives || c->fault) {
        if (!c->fault) g_error = CDERR_MEMALLOCFAILURE;
        d->dlgdata = 0;
        w16_wnd_destroy(w, d);
        goto out;
    }
    w16_listbox_folders(dirs, 1);
    w16_send(w, e->hwnd, EM_LIMITTEXT, 127, 0);
    struct w16_wnd *tl = w16_combo_list(w, types), *dl = w16_combo_list(w, drives);
    for (int i = 0; tl && i < o->ntypes; i++) w16_listbox_str(w, tl, LB_ADDSTRING, 0, filter_str(o, 2 * i));
    if (tl) w16_send(w, tl->hwnd, LB_SETCURSEL, (uint16_t)o->type, 0);
    w16_combo_changed(w, types);
    if (dl) {
        // 3.1's "letter: volume label", a drive icon before it; C: is the Win16
        // folder, D: to Y: named for theirs, and Z: the whole toy-os tree.
        w16_listbox_folders(dl, 1);
        for (char d = 'C'; d <= 'Z'; d++) {
            const char *root = w16_drive_root(d);
            if (!root) continue;
            char label[48], low[48];
            const char *base = strrchr(root, '/');
            snprintf(label, sizeof label, "%c: %s", d, d == 'C' ? "win16" : d == 'Z' ? "toy-os" : base ? base + 1 : root);
            w16_send(w, dl->hwnd, LB_SETITEMDATA,
                     (uint16_t)w16_listbox_str(w, dl, LB_ADDSTRING, 0, lowered(label, low, sizeof low)), 3);
        }
        w16_send(w, dl->hwnd, LB_SETCURSEL, (uint16_t)drive_row(o->dir[0]), 0);
    }
    w16_combo_changed(w, drives);
    {
        char low[160];
        set_text(w, e, lowered(initial && initial[0] ? initial : o->pattern, low, sizeof low));
    }
    fill_lists(w, d);
    {
        struct w16_wnd *ro = item(w, d, CHX1);
        if (ro && (o->flags & OFN_READONLY)) w16_send(w, ro->hwnd, BM_SETCHECK, 1, 0);
    }
    // The dialog's own start: the name field focused, all of it selected.
    uint16_t h = d->hwnd, eh = e->hwnd;
    w16_dlg_show(w, d, 1, far);
    if (w16_wnd_get(w, eh)) {
        w16_set_focus(w, eh);
        w16_send(w, eh, EM_SETSEL, 0, 0xFFFF0000u);
    }
    d = w16_wnd_get(w, h);
    if (d) {
        struct w16_wnd *ro = item(w, d, CHX1);
        int checked = ro ? (int)w16_send(w, ro->hwnd, BM_GETCHECK, 0, 0) : 0;
        int r = w16_dlg_run(w, d);
        of = at(w, far, 72, 1);
        if (of && r == 1) {
            uint32_t fl = rd32(of + 48);
            wr32(of + 48, checked ? fl | OFN_READONLY : fl & ~(uint32_t)OFN_READONLY);
        }
        c->r[R_AX] = r == 1;
    }
out:
    free(o->filter);
    free(o->defext);
    free(o);
    free(initial);
    free(dir);
    free(cap);
    return c->fault ? -1 : 4;
}

// GetOpenFileName(lpofn), GetSaveFileName(lpofn)
static int api_GetOpenFileName(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return file_dialog(w, a, ordinal == 2);
}

// --- Find --------------------------------------------------------------------------------

struct fr {
    uint32_t far;                 // the program's FINDREPLACE, which outlives the call
    uint32_t hook;
    uint16_t owner, msg;
};

// The FINDREPLACE told what the dialog says now, and its owner sent the
// registered message ("commdlg_FindReplace") with it.
static void find_report(struct w16 *w, struct w16_wnd *d, uint32_t verb) {
    struct fr *f = d->dlgdata;
    uint8_t *p = at(w, f->far, 36, 1);
    if (!p) return;
    uint32_t flags = rd32(p + 8) & ~(uint32_t)(FR_DOWN | FR_WHOLEWORD | FR_MATCHCASE | FR_FINDNEXT | FR_DIALOGTERM);
    struct w16_wnd *ww = item(w, d, CHX1), *mc = item(w, d, CHX2), *down = item(w, d, RAD2);
    if (ww && w16_send(w, ww->hwnd, BM_GETCHECK, 0, 0)) flags |= FR_WHOLEWORD;
    if (mc && w16_send(w, mc->hwnd, BM_GETCHECK, 0, 0)) flags |= FR_MATCHCASE;
    if (!down || w16_send(w, down->hwnd, BM_GETCHECK, 0, 0)) flags |= FR_DOWN;
    flags |= verb;
    if (verb == FR_FINDNEXT) {
        struct w16_wnd *e = item(w, d, EDT1);
        char *t = e ? get_text(w, e) : 0;
        uint32_t what = rd32(p + 12);
        uint16_t len = rd16(p + 20);
        if (t && len) {
            size_t n = strlen(t);
            if (n > (size_t)len - 1) n = (size_t)len - 1;
            uint8_t *q = at(w, what, (unsigned)n + 1, 1);
            if (q) { memcpy(q, t, n); q[n] = 0; }
        }
        free(t);
    }
    p = at(w, f->far, 36, 1);
    if (!p) return;
    wr32(p + 8, flags);
    w16_send(w, f->owner, f->msg, 0, f->far);
}

static int find_proc(struct w16 *w, struct w16_wnd *d, uint16_t msg, uint16_t wp, uint32_t lp) {
    struct fr *f = d->dlgdata;
    if (!f) return 0;
    if (msg == WM_NCDESTROY) {                   // modeless: the dialog owns its state
        free(f);
        d->dlgdata = 0;
        return 0;
    }
    if (hook(w, d, f->hook, msg, wp, lp) && msg != WM_INITDIALOG) return 1;
    if (msg == WM_INITDIALOG) return 1;
    if (msg != WM_COMMAND) return 0;
    uint16_t code = (uint16_t)(lp >> 16);
    switch (wp) {
    case IDOK:
        find_report(w, d, FR_FINDNEXT);
        return 1;
    case IDCANCEL: {
        uint16_t h = d->hwnd;
        find_report(w, d, FR_DIALOGTERM);
        if ((d = w16_wnd_get(w, h))) w16_wnd_destroy(w, d);
        return 1;
    }
    case EDT1:
        if (code == EN_CHANGE) {                 // Find Next waits for something to find
            struct w16_wnd *e = item(w, d, EDT1), *b = item(w, d, IDOK);
            char *t = e ? get_text(w, e) : 0;
            if (b) {
                pthread_mutex_lock(&w->lock);
                if (t && t[0]) b->style &= ~WS_DISABLED;
                else b->style |= WS_DISABLED;
                pthread_mutex_unlock(&w->lock);
                w16_invalidate(w, b, 0);
            }
            free(t);
        }
        return 1;
    default:
        return 0;
    }
}

// FindText(lpfr): the Find dialog, modeless; its hwnd
static int api_FindText(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t far = w16_arg32(a);
    struct cpu16 *c = &w->cpu;
    c->r[R_AX] = 0;
    g_error = 0;
    uint8_t *p = at(w, far, 36, 0);
    if (!p) return -1;
    uint32_t flags = rd32(p + 8);
    if (flags & FR_ENABLETEMPLATE) { g_error = CDERR_NOTEMPLATE; return 4; }
    struct fr *f = calloc(1, sizeof *f);
    char *what = far_dup(w, rd32(p + 12));
    if (!f || !what) { free(f); free(what); g_error = CDERR_MEMALLOCFAILURE; return what ? 4 : -1; }
    f->far = far;
    f->owner = rd16(p + 4);
    f->hook = flags & FR_ENABLEHOOK ? rd32(p + 28) : 0;
    f->msg = w16_atom_add(w, "commdlg_FindReplace");
    g_org_x = 30;                                // FINDDLGORD's place
    g_org_y = 73;
    struct w16_wnd *d = w16_dlg_new(w, f->owner, 0, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_DLGFRAME, w16_du_w(236, 7),
                                    w16_du_h(62, 13), "Find", 0, find_proc);
    if (!d) { free(f); free(what); g_error = CDERR_MEMALLOCFAILURE; return 4; }
    w16_dlg_set_font(d, 8);
    d->dlgdata = f;
    add(w, d, "STATIC", -1, 0, 4, 8, 42, 8, "Fi&nd What:");
    struct w16_wnd *e = add(w, d, "EDIT", EDT1, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 47, 7, 128, 12, what);
    if (!(flags & FR_HIDEWHOLEWORD))
        add(w, d, "BUTTON", CHX1, WS_TABSTOP | BS_AUTOCHECKBOX | ((flags & FR_NOWHOLEWORD) ? WS_DISABLED : 0), 4, 26, 100,
            12, "Match &Whole Word Only");
    if (!(flags & FR_HIDEMATCHCASE))
        add(w, d, "BUTTON", CHX2, WS_TABSTOP | BS_AUTOCHECKBOX | ((flags & FR_NOMATCHCASE) ? WS_DISABLED : 0), 4, 42, 64,
            12, "Match &Case");
    if (!(flags & FR_HIDEUPDOWN)) {
        uint32_t dis = (flags & FR_NOUPDOWN) ? WS_DISABLED : 0;
        add(w, d, "BUTTON", GRP1, BS_GROUPBOX, 107, 26, 68, 28, "Direction");
        add(w, d, "BUTTON", RAD1, WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | dis, 111, 38, 20, 12, "&Up");
        add(w, d, "BUTTON", RAD2, BS_AUTORADIOBUTTON | dis, 138, 38, 30, 12, "&Down");
    }
    struct w16_wnd *next = add(w, d, "BUTTON", IDOK, WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON | (what[0] ? 0 : WS_DISABLED),
                               182, 5, 50, 14, "&Find Next");
    add(w, d, "BUTTON", IDCANCEL, WS_TABSTOP, 182, 23, 50, 14, "Cancel");
    if (flags & FR_SHOWHELP) add(w, d, "BUTTON", PSH_HELP, WS_TABSTOP, 182, 45, 50, 14, "&Help");
    free(what);
    if (!e || !next || c->fault) {
        if (!c->fault) g_error = CDERR_MEMALLOCFAILURE;
        w16_wnd_destroy(w, d);
        return c->fault ? -1 : 4;
    }
    struct w16_wnd *ww = item(w, d, CHX1), *mc = item(w, d, CHX2), *dir = item(w, d, (flags & FR_DOWN) ? RAD2 : RAD1);
    if (ww && (flags & FR_WHOLEWORD)) w16_send(w, ww->hwnd, BM_SETCHECK, 1, 0);
    if (mc && (flags & FR_MATCHCASE)) w16_send(w, mc->hwnd, BM_SETCHECK, 1, 0);
    if (dir) w16_send(w, dir->hwnd, BM_SETCHECK, 1, 0);
    uint16_t h = d->hwnd, eh = e->hwnd;
    w16_dlg_show(w, d, 0, far);
    if (w16_wnd_get(w, eh)) {
        w16_set_focus(w, eh);
        w16_send(w, eh, EM_SETSEL, 0, 0xFFFF0000u);
    }
    c->r[R_AX] = w16_wnd_get(w, h) ? h : 0;
    return c->fault ? -1 : 4;
}

// PrintDlg(lppd): there is no printer, and the program is told so as
// Windows told it with none installed
static int api_PrintDlg(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a);
    g_error = PDERR_NODEFAULTPRN;
    w->cpu.r[R_AX] = 0;
    return 4;
}

// CommDlgExtendedError: why the last common dialog returned FALSE (0: Cancel)
static int api_CommDlgExtendedError(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    w->cpu.r[R_AX] = g_error;
    w->cpu.r[R_DX] = 0;
    return 0;
}

const struct w16_api g_commdlg_api[] = {
    { 1, api_GetOpenFileName },
    { 2, api_GetOpenFileName },
    { 11, api_FindText },
    { 20, api_PrintDlg },
    { 26, api_CommDlgExtendedError },
};
const unsigned g_commdlg_api_n = sizeof g_commdlg_api / sizeof g_commdlg_api[0];
