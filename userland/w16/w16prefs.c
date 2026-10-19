// w16prefs.c -- the settings table w16 and the Win16 Programs launcher
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

static int is_abs_path(const char *v) { return v[0] == '/' || v[0] == 0; }

#define FOLDER_KEY(n, d) UPREF_TEXT_KEY("folder" #n, struct w16_prefs, folder[n - 1], d, is_abs_path)

// THE FIRST SEVEN ARE A PROGRAM'S TO OVERRIDE, each its bit in
// w16_prefs_overridden() by its place here -- so a new one goes after
// them, never between; the folders are the launcher's alone. Keep
// w16_prefs_program_keys in step.
const struct upref w16_prefs_keys[] = {
    UPREF_WORD_KEY("scale", struct w16_prefs, scale, SCALE_WORDS, W16_SCALE_COUNT, W16_SCALE_AUTO),
    UPREF_WORD_KEY("frame", struct w16_prefs, frame, FRAME_WORDS, 2, W16_FRAME_31),
    UPREF_WORD_KEY("version", struct w16_prefs, version, VERSION_WORDS, 3, W16_VER_310),
    UPREF_WORD_KEY("on_fault", struct w16_prefs, on_fault, FAULT_WORDS, 2, W16_FAULT_BOX),
    UPREF_BOOL_KEY("trace", struct w16_prefs, trace, 0),
    UPREF_TEXT_KEY("drive_c", struct w16_prefs, drive_c, "/home/Win16", is_abs_path),
    UPREF_WORD_KEY("method", struct w16_prefs, method, METHOD_WORDS, 3, W16_METHOD_SHARP),
    FOLDER_KEY(1, "/usr/share/w16"), FOLDER_KEY(2, "/home/Win16"), FOLDER_KEY(3, ""),
    FOLDER_KEY(4, ""), FOLDER_KEY(5, ""), FOLDER_KEY(6, ""), FOLDER_KEY(7, ""), FOLDER_KEY(8, ""),
};
const int w16_prefs_program_keys = 7;
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
