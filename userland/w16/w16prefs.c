// w16prefs.c -- the settings table w16 and Retro Launcher
// share (w16prefs.h).
#include "w16/w16prefs.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "lib/uconf.h"
#include "query_abi.h"
#include "rt/sys.h"

static const char *const SCALE_WORDS[] = { "auto", "1", "1.25", "1.5", "1.75", "2", "2.5", "3" };
const int w16_prefs_scale_q[W16_SCALE_COUNT] = { 0, 4, 5, 6, 7, 8, 10, 12 };
static const char *const METHOD_WORDS[] = { "sharp", "pixels", "smooth" };
static const char *const FRAME_WORDS[] = { "win31", "desktop" };
static const char *const VERSION_WORDS[] = { "3.10", "3.11", "3.00" };
static const char *const FAULT_WORDS[] = { "box", "report" };
static const char *const FONTS_WORDS[] = { "win311", "standin" };
static const char *const FROM_WORDS[] = { "run", "check", "ask", "show" };
static const char *const SOUNDS_WORDS[] = { "win311", "toyos", "off" };
static const char *const HELP_WORDS[] = { "viewer", "winhelp" };
static const char *const COLORS_WORDS[] = { "256", "16" };

static int is_abs_path(const char *v) { return v[0] == '/' || v[0] == 0; }

#define DRIVE_KEY(l, i) UPREF_TEXT_KEY("drive_" #l, struct w16_prefs, drive[i], "", is_abs_path)

// THE FIRST ELEVEN ARE A PROGRAM'S TO OVERRIDE, each its bit in
// w16_prefs_overridden() by its place here -- so a new one goes after
// them, never between; drives D: to Y: are for every program alike.
// Keep w16_prefs_program_keys in step.
const struct upref w16_prefs_keys[] = {
    UPREF_WORD_KEY("scale", struct w16_prefs, scale, SCALE_WORDS, W16_SCALE_COUNT, W16_SCALE_AUTO),
    UPREF_WORD_KEY("frame", struct w16_prefs, frame, FRAME_WORDS, 2, W16_FRAME_31),
    UPREF_WORD_KEY("version", struct w16_prefs, version, VERSION_WORDS, 3, W16_VER_310),
    UPREF_WORD_KEY("on_fault", struct w16_prefs, on_fault, FAULT_WORDS, 2, W16_FAULT_BOX),
    UPREF_BOOL_KEY("trace", struct w16_prefs, trace, 0),
    UPREF_TEXT_KEY("drive_c", struct w16_prefs, drive_c, "/home/Win16", is_abs_path),
    UPREF_WORD_KEY("method", struct w16_prefs, method, METHOD_WORDS, 3, W16_METHOD_SHARP),
    UPREF_WORD_KEY("fonts", struct w16_prefs, fonts, FONTS_WORDS, 2, W16_FONTS_WIN311),
    UPREF_WORD_KEY("sounds", struct w16_prefs, sounds, SOUNDS_WORDS, 3, W16_SOUNDS_WIN311),
    UPREF_WORD_KEY("help", struct w16_prefs, help, HELP_WORDS, 2, W16_HELP_VIEWER),
    UPREF_WORD_KEY("colors", struct w16_prefs, colors, COLORS_WORDS, 2, W16_COLORS_256),
    DRIVE_KEY(d, 0), DRIVE_KEY(e, 1), DRIVE_KEY(f, 2), DRIVE_KEY(g, 3), DRIVE_KEY(h, 4), DRIVE_KEY(i, 5),
    DRIVE_KEY(j, 6), DRIVE_KEY(k, 7), DRIVE_KEY(l, 8), DRIVE_KEY(m, 9), DRIVE_KEY(n, 10), DRIVE_KEY(o, 11),
    DRIVE_KEY(p, 12), DRIVE_KEY(q, 13), DRIVE_KEY(r, 14), DRIVE_KEY(s, 15), DRIVE_KEY(t, 16), DRIVE_KEY(u, 17),
    DRIVE_KEY(v, 18), DRIVE_KEY(w, 19), DRIVE_KEY(x, 20), DRIVE_KEY(y, 21),
    UPREF_WORD_KEY("from_files", struct w16_prefs, from_files, FROM_WORDS, 4, W16_FROM_CHECK),
    UPREF_BOOL_KEY("add_folder", struct w16_prefs, add_folder, 1),
    UPREF_BOOL_KEY("sound_drv", struct w16_prefs, sound_drv, 1),
};
const int w16_prefs_program_keys = 11;
const struct uprefs w16_prefs_defaults = {
    W16_CONF, w16_prefs_keys, (int)(sizeof w16_prefs_keys / sizeof w16_prefs_keys[0])
};

void w16_prog_key(const char *exe, char *out, int cap) {
    int n = 0;
    for (const char *p = exe[0] == '/' ? exe + 1 : exe; *p && n < cap - 1; p++)
        out[n++] = *p == '/' ? '_' : *p;
    out[n] = 0;
}

static void prog_file(const char *dir, const char *exe, const char *ext, char *out, int cap) {
    char key[W16_PATH_MAX];
    w16_prog_key(exe, key, sizeof key);
    snprintf(out, (size_t)cap, "%s/%s%s", dir, key, ext);
}

void w16_prog_conf(const char *exe, char *out, int cap) { prog_file(W16_PROG_DIR, exe, ".conf", out, cap); }
void w16_prog_last(const char *exe, char *out, int cap) { prog_file(W16_STATE_DIR, exe, ".last", out, cap); }
void w16_trace_path(const char *exe, char *out, int cap) { prog_file(W16_STATE_DIR, exe, ".trace", out, cap); }

void w16_prefs_load(struct w16_prefs *p, const char *exe) {
    memset(p, 0, sizeof *p);
    uprefs_load(&w16_prefs_defaults, p);
    if (!exe) return;
    char path[W16_PATH_MAX + 32];
    w16_prog_conf(exe, path, sizeof path);
    const struct uprefs own = { path, w16_prefs_keys, w16_prefs_program_keys };
    uprefs_read(&own, p);
}

unsigned w16_prefs_overridden(const char *exe) {
    char path[W16_PATH_MAX + 32], v[W16_PATH_MAX];
    w16_prog_conf(exe, path, sizeof path);
    unsigned mask = 0;
    for (int i = 0; i < w16_prefs_program_keys; i++)
        if (uconf_get(path, w16_prefs_keys[i].key, v, sizeof v)) mask |= 1u << i;
    return mask;
}

int w16_prefs_save_program(const char *exe, const struct w16_prefs *p, unsigned mask) {
    char path[W16_PATH_MAX + 32];
    w16_prog_conf(exe, path, sizeof path);
    mkdir(W16_PROG_DIR, 0755);                   // shipped, but a parent is never assumed
    int ok = 1;
    for (int i = 0; i < w16_prefs_program_keys; i++) {
        if (mask & (1u << i)) {
            const struct uprefs one = { path, &w16_prefs_keys[i], 1 };
            if (uprefs_save(&one, p, 0) < 0) ok = 0;
        } else {
            uconf_unset(path, w16_prefs_keys[i].key);
        }
    }
    return ok ? 0 : -1;
}

// --- the launcher's folders ----------------------------------------------------------

#define FOLDER_PATH 1024                             // a path read back from the file

static int add_folder(struct w16_folder **f, int *n, const char *path, const char *name) {
    struct w16_folder *g = realloc(*f, sizeof **f * (size_t)(*n + 1));
    if (!g) return -1;
    *f = g;
    g[*n].path = strdup(path);
    g[*n].name = strdup(name);
    if (!g[*n].path || !g[*n].name) { free(g[*n].path); free(g[*n].name); return -1; }
    (*n)++;
    return 0;
}

int w16_folders_load(struct w16_folder **out) {
    struct w16_folder *f = 0;
    int n = 0;
    char *v = malloc(FOLDER_PATH), *name = malloc(FOLDER_PATH), key[32];
    if (!v || !name) {
        free(v); free(name);
        *out = 0;
        return 0;
    }
    if (uconf_get(W16_CONF, "folders", v, FOLDER_PATH)) {
        int count = atoi(v);
        for (int i = 1; i <= count; i++) {
            snprintf(key, sizeof key, "folder%d", i);
            if (!uconf_get(W16_CONF, key, v, FOLDER_PATH) || v[0] != '/') continue;
            snprintf(key, sizeof key, "folder%d_name", i);
            if (!uconf_get(W16_CONF, key, name, FOLDER_PATH)) name[0] = 0;
            add_folder(&f, &n, v, name);
        }
    } else {
        // Before the count: eight keys, the first two with defaults.
        static const char *const DEF[] = { "/usr/share/w16", "/home/Win16" };
        for (int i = 1; i <= 8; i++) {
            snprintf(key, sizeof key, "folder%d", i);
            if (!uconf_get(W16_CONF, key, v, FOLDER_PATH)) snprintf(v, FOLDER_PATH, "%s", i <= 2 ? DEF[i - 1] : "");
            if (v[0] == '/') add_folder(&f, &n, v, "");
        }
    }
    free(v);
    free(name);
    *out = f;
    return n;
}

int w16_folders_save(const struct w16_folder *f, int n) {
    char v[16], key[32];
    int was = uconf_get(W16_CONF, "folders", v, sizeof v) ? atoi(v) : 8, ok = 1;
    snprintf(v, sizeof v, "%d", n);
    if (!uconf_set(W16_CONF, "folders", v)) ok = 0;
    for (int i = 1; i <= n; i++) {
        snprintf(key, sizeof key, "folder%d", i);
        if (!uconf_set(W16_CONF, key, f[i - 1].path)) ok = 0;
        snprintf(key, sizeof key, "folder%d_name", i);
        if (f[i - 1].name[0] ? !uconf_set(W16_CONF, key, f[i - 1].name) : (uconf_unset(W16_CONF, key), 0)) ok = 0;
    }
    for (int i = n + 1; i <= was; i++) {
        snprintf(key, sizeof key, "folder%d", i);
        uconf_unset(W16_CONF, key);
        snprintf(key, sizeof key, "folder%d_name", i);
        uconf_unset(W16_CONF, key);
    }
    return ok ? 0 : -1;
}

void w16_folders_free(struct w16_folder *f, int n) {
    for (int i = 0; i < n; i++) { free(f[i].path); free(f[i].name); }
    free(f);
}

int w16_prefs_auto_q(void) {
    struct query_display d;
    if (sys_query_record(QUERY_DISPLAY, 0, &d, sizeof d) < (int)sizeof d || !d.width) return 4;
    int want = (int)(d.width * 4 + 640) / 1280, best = 4;
    for (int i = W16_SCALE_1; i < W16_SCALE_COUNT; i++) {
        int q = w16_prefs_scale_q[i];
        if (abs(q - want) < abs(best - want)) best = q;
    }
    return best;
}

uint16_t w16_prefs_winver(const struct w16_prefs *p) {
    switch (p->version) {
    case W16_VER_311: return 0x0B03;             // GetVersion's AX: minor in the high byte
    case W16_VER_300: return 0x0003;
    default:          return 0x0A03;
    }
}

char *w16_short_title(const char *d) {
    for (int again = 1; again; ) {
        again = 0;
        static const char *const PREFIX[] = { "Microsoft ", "Windows " };
        for (int i = 0; i < 2; i++) {
            size_t n = strlen(PREFIX[i]);
            if (!strncmp(d, PREFIX[i], n) && d[n]) { d += n; again = 1; }
        }
    }
    char *t = strdup(d);
    if (!t) return 0;
    for (char *c = t; *c; c++)
        if (c > t && c[-1] == ' ' &&
            (!strncmp(c, "(c)", 3) || !strncmp(c, "(C)", 3) || !strncmp(c, "Copyright", 9) ||
             (c[0] == 'v' && c[1] >= '0' && c[1] <= '9'))) {
            c[-1] = 0;
            break;
        }
    return t;
}
