// sounds.c -- Options > Sound's table: each event drive C:'s WIN.INI
// names, what it plays under the chosen Sounds setting, and a Play cell
// that plays it here. WIN.INI is a Windows file and is read only by w16,
// in its sandbox (`w16 --sounds`); this reads w16's lines.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ulog.h"
#include "lib/usnd.h"
#include "ui/uui_table.h"
#include "w16/w16prefs.h"
#include "retrolauncher/retrolauncher.h"

struct sound_row {
    char *alias, *event, *ini, *host, *status, *saunaos;   // w16's fields, in one allocation each
};

static struct sound_row *g_rows;
static int g_nrows, g_mode;
static char *g_text;                         // w16's output; the rows point into it
static struct usnd_clip g_clip;
static int g_usnd;                           // 1 ready, -1 refused
struct uui_table g_retro_sounds;

static const struct uui_table_column COLS[] = {
    { "Event", 18, UUI_TALIGN_LEFT, 0, 0 }, { "WIN.INI [sounds]", 0, UUI_TALIGN_LEFT, 0, 0 },
    { "Plays", 16, UUI_TALIGN_LEFT, 0, 0 }, { "On drive C:", 12, UUI_TALIGN_LEFT, 0, 0 },
    { "", 7, UUI_TALIGN_LEFT, "play", UUI_TCOL_ACTION },
};

static const char *base(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

// Does the setting play drive C:'s own file for this row?
static int plays_own(const struct sound_row *r) {
    return g_mode == W16_SOUNDS_WIN311 && !strcmp(r->status, "found");
}

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    out[0] = 0;
    if (row < 0 || row >= g_nrows) return;
    const struct sound_row *r = &g_rows[row];
    switch (col) {
    case 0: snprintf(out, (size_t)cap, "%s", r->event); break;
    case 1:
        if (r->ini[0]) snprintf(out, (size_t)cap, "%s=%s", r->alias, r->ini);
        else snprintf(out, (size_t)cap, "%s (not named)", r->alias);
        break;
    case 2:
        if (g_mode == W16_SOUNDS_OFF) snprintf(out, (size_t)cap, "Nothing");
        else if (plays_own(r)) {
            snprintf(out, (size_t)cap, "%s", base(r->host));
            for (char *p = out; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        } else {
            char name[32];
            snprintf(name, sizeof name, "%s", base(r->saunaos));
            char *dot = strrchr(name, '.');
            if (dot) *dot = 0;
            snprintf(out, (size_t)cap, "SaunaOS's %s", name);
        }
        break;
    case 3:
        snprintf(out, (size_t)cap, "%s", !strcmp(r->status, "found") ? "Found"
                                       : !strcmp(r->status, "missing") ? "Missing" : "--");
        break;
    case 4: if (g_mode != W16_SOUNDS_OFF) snprintf(out, (size_t)cap, "Play"); break;
    default: break;
    }
}

void retro_sounds_init(void) {
    uui_table_init(&g_retro_sounds, 0, 0, 0, 0, COLS, (int)(sizeof COLS / sizeof COLS[0]), cell, 0);
}

void retro_sounds_load(int mode) {
    free(g_rows);
    free(g_text);
    g_rows = 0;
    g_nrows = 0;
    g_mode = mode;
    g_text = retro_sounds();
    int cap = 0;
    for (char *l = g_text; l && *l; ) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        // "sound<TAB>ALIAS<TAB>EVENT<TAB>INI<TAB>HOST<TAB>STATUS<TAB>SAUNAOS"
        char *f[7];
        int n = 0;
        for (char *p = l; n < 7; n++) {
            f[n] = p;
            char *t = strchr(p, '\t');
            if (!t) { n++; break; }
            *t = 0;
            p = t + 1;
        }
        if (n == 7 && !strcmp(f[0], "sound")) {
            if (g_nrows == cap) {
                int c = cap ? cap * 2 : 16;
                struct sound_row *g = realloc(g_rows, sizeof *g * (size_t)c);
                if (!g) break;
                g_rows = g;
                cap = c;
            }
            g_rows[g_nrows++] = (struct sound_row){ f[1], f[2], f[3], f[4], f[5], f[6] };
        }
        l = nl ? nl + 1 : 0;
    }
    uui_table_set_rows(&g_retro_sounds, g_nrows);
}

void retro_sounds_mode(int mode) { g_mode = mode; }

void retro_sounds_play(int row) {
    if (row < 0 || row >= g_nrows || g_mode == W16_SOUNDS_OFF) return;
    const struct sound_row *r = &g_rows[row];
    const char *path = plays_own(r) ? r->host : r->saunaos;
    ulogf("retrolauncher: play %d %s\n", row, path);
    if (!g_usnd) {
        g_usnd = usnd_init() == 0 ? 1 : -1;
        if (g_usnd < 0) ulogf("retrolauncher: no sound -- %s\n", usnd_last_error());
    }
    if (g_usnd < 0) return;
    usnd_clip_free(&g_clip);                     // stops the last one
    if (usnd_clip_load(path, &g_clip) != 0) {
        ulogf("retrolauncher: %s -- %s\n", path, usnd_last_error());
        return;
    }
    usnd_clip_play(&g_clip, 256);
}
