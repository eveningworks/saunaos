// The personal folders -- see udirs.h.
#include "lib/udirs.h"
#include "ulistdir.h"
#include "lib/uconf.h"
#include "etc_config.h"
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *key, *conf, *label, *icon, *def; } DIRS[UDIR_COUNT] = {
    [UDIR_DESKTOP]     = { "desktop",     "DESKTOP",     "Desktop",     "place-desktop",     UDIR_DEFAULT_DESKTOP },
    [UDIR_DOCUMENTS]   = { "documents",   "DOCUMENTS",   "Documents",   "place-documents",   "/home/Documents" },
    [UDIR_MUSIC]       = { "music",       "MUSIC",       "Music",       "place-music",       "/home/Music" },
    [UDIR_PICTURES]    = { "pictures",    "PICTURES",    "Pictures",    "place-pictures",    "/home/Pictures" },
    [UDIR_VIDEOS]      = { "videos",      "VIDEOS",      "Videos",      "place-videos",      "/home/Videos" },
    [UDIR_DOWNLOADS]   = { "downloads",   "DOWNLOAD",    "Downloads",   "place-downloads",   "/home/Downloads" },
    [UDIR_SCREENSHOTS] = { "screenshots", "SCREENSHOTS", "Screenshots", "place-pictures",    UDIR_DEFAULT_SCREENSHOTS },
};

static char g_path[UDIR_COUNT][128];
static int g_loaded;

// "$HOME/Music" and quotes are XDG's spelling; the value here is a path.
static void load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    struct etc_config_buf doc, *buf = &doc;   // the file, read once
    int ok = uconf_load(UDIRS_CONF, buf);
    for (int d = 0; d < UDIR_COUNT; d++) {
        char v[128] = "";
        char xdg[40];
        snprintf(xdg, sizeof xdg, "XDG_%s_DIR", DIRS[d].conf);
        if (ok && (!etc_config_buf_get(buf, DIRS[d].conf, v, sizeof v) || !v[0]))
            etc_config_buf_get(buf, xdg, v, sizeof v);
        char *p = v;
        size_t n = strlen(p);
        if (n >= 2 && p[0] == '"' && p[n - 1] == '"') { p[n - 1] = '\0'; p++; }
        if (!strncmp(p, "$HOME/", 6)) snprintf(g_path[d], sizeof g_path[d], "/home/%s", p + 6);
        else if (p[0] == '/') snprintf(g_path[d], sizeof g_path[d], "%s", p);
        else snprintf(g_path[d], sizeof g_path[d], "%s", DIRS[d].def);   // a parser rejects rather than guesses
    }
    etc_config_buf_free(buf);
}

const char *udir_path(enum udir d)  { if ((unsigned)d >= UDIR_COUNT) return "/home"; load(); return g_path[d]; }
const char *udir_label(enum udir d) { return (unsigned)d < UDIR_COUNT ? DIRS[d].label : ""; }
const char *udir_key(enum udir d)   { return (unsigned)d < UDIR_COUNT ? DIRS[d].key : ""; }
const char *udir_icon(enum udir d)  { return (unsigned)d < UDIR_COUNT ? DIRS[d].icon : "folder"; }

int udir_of(const char *path) {
    for (int d = 0; d < UDIR_COUNT; d++)
        if (!strcmp(udir_path((enum udir)d), path)) return d;
    return -1;
}

// mkdir -p: each component in turn, an existing one is no error.
static void mkdirs(const char *path) {
    char p[128];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        sys_mkdir(p);
        *s = '/';
    }
    sys_mkdir(p);
}

static int exists(const char *p) {
    struct sys_stat st;
    return sys_stat(p, &st) == 0;
}

void udir_create_all(void) {
    for (int d = 0; d < UDIR_COUNT; d++)
        if (d != UDIR_SCREENSHOTS) mkdirs(udir_path((enum udir)d));
}

int udir_make(enum udir d) {
    mkdirs(udir_path(d));
    return exists(udir_path(d)) ? 0 : -1;
}

int udir_migrate(void) {
    static const struct { const char *from; enum udir to; } OLD[] = {
        { "/home/desktop",     UDIR_DESKTOP },
        { "/home/screenshots", UDIR_SCREENSHOTS },
    };
    int moved = 0;
    for (unsigned i = 0; i < sizeof OLD / sizeof OLD[0]; i++) {
        const char *to = udir_path(OLD[i].to);
        if (!exists(OLD[i].from)) continue;
        if (!exists(to)) {
            char parent[128];
            snprintf(parent, sizeof parent, "%s", to);
            char *slash = strrchr(parent, '/');
            if (slash && slash != parent) { *slash = '\0'; mkdirs(parent); }
            if (sys_rename(OLD[i].from, to) == 0) moved++;
            continue;
        }
        // BOTH ALREADY THERE: the update that brought this code seeded the
        // new folder (the desktop's launchers) before this first boot. The
        // OLD one is the person's: its entries win a clash, and a seeded
        // launcher it lacks is one they had deleted, so it goes again.
        // Each listing is read WHOLE before anything in it moves, so a
        // rename or an unlink cannot shift a page still to be read.
        struct sys_dirent *ents = 0;
        char a[192], b[192];
        int n = ulistdir(to, &ents);
        for (int k = 0; k < n; k++) {
            snprintf(a, sizeof a, "%s/%s", OLD[i].from, ents[k].name);
            snprintf(b, sizeof b, "%s/%s", to, ents[k].name);
            size_t l = strlen(ents[k].name);
            int launcher = !ents[k].is_dir && l > 8 && !strcmp(ents[k].name + l - 8, ".desktop");
            if (!ents[k].is_dir && (exists(a) || launcher)) sys_unlink(b);
        }
        free(ents);
        ents = 0;
        n = ulistdir(OLD[i].from, &ents);
        for (int k = 0; k < n; k++) {
            snprintf(a, sizeof a, "%s/%s", OLD[i].from, ents[k].name);
            snprintf(b, sizeof b, "%s/%s", to, ents[k].name);
            if (!exists(b) && sys_rename(a, b) == 0) moved++;
        }
        free(ents);
        sys_unlink(OLD[i].from);   // refused while anything is left in it
    }
    // Screenshot's Options may name the old folder outright
    // (userland/screenshot/screenshot.h's SHOT_CONF).
    char v[128];
    if (uconf_get("/etc/screenshot.conf", "folder", v, sizeof v) && !strcmp(v, "/home/screenshots"))
        uconf_set("/etc/screenshot.conf", "folder", udir_path(UDIR_SCREENSHOTS));
    return moved;
}
