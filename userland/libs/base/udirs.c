// The personal folders -- see udirs.h.
#include "base/udirs.h"
#include "base/uusername.h"
#include <pwd.h>
#include "base/uconf.h"
#include "etc_config.h"
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *key, *conf, *label, *icon, *def; } DIRS[UDIR_COUNT] = {
    [UDIR_DESKTOP]     = { "desktop",     "DESKTOP",     "Desktop",     "place-desktop",     "Desktop" },
    [UDIR_DOCUMENTS]   = { "documents",   "DOCUMENTS",   "Documents",   "place-documents",   "Documents" },
    [UDIR_MUSIC]       = { "music",       "MUSIC",       "Music",       "place-music",       "Music" },
    [UDIR_PICTURES]    = { "pictures",    "PICTURES",    "Pictures",    "place-pictures",    "Pictures" },
    [UDIR_VIDEOS]      = { "videos",      "VIDEOS",      "Videos",      "place-videos",      "Videos" },
    [UDIR_DOWNLOADS]   = { "downloads",   "DOWNLOAD",    "Downloads",   "place-downloads",   "Downloads" },
    [UDIR_SCREENSHOTS] = { "screenshots", "SCREENSHOTS", "Screenshots", "place-pictures",    "Pictures/Screenshots" },
};

static char g_path[UDIR_COUNT][256];
static int g_loaded;

const char *udir_home(void) {
    static char *home;
    if (home) return home;
    const char *h = getenv("HOME");
    unsigned uid, gid;
    struct passwd *pw;
    if (h && h[0] == '/' && h[1]) home = strdup(h);
    else if (uusername_desktop(&uid, &gid) && (pw = getpwuid(uid)) && pw->pw_dir[0] == '/' && pw->pw_dir[1])
        home = strdup(pw->pw_dir);
    if (!home) return "/";   // none, or no memory: asked again next time
    size_t n = strlen(home);
    while (n > 1 && home[n - 1] == '/') home[--n] = '\0';   // "/home/user/" + "/Music"
    return home;
}

// A home of "/" joins as "/Music", not "//Music".
static void in_home(char *out, size_t cap, const char *rel) {
    const char *h = udir_home();
    snprintf(out, cap, "%s/%s", strcmp(h, "/") ? h : "", rel);
}

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
        if (!strncmp(p, "$HOME/", 6)) in_home(g_path[d], sizeof g_path[d], p + 6);
        else if (p[0] == '/') snprintf(g_path[d], sizeof g_path[d], "%s", p);
        else in_home(g_path[d], sizeof g_path[d], DIRS[d].def);   // a parser rejects rather than guesses
    }
    etc_config_buf_free(buf);
}

const char *udir_path(enum udir d)  { if ((unsigned)d >= UDIR_COUNT) return udir_home(); load(); return g_path[d]; }
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
    char p[256];
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
