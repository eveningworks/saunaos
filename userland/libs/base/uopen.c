// uopen -- see base/uopen.h for the two layers and why there is no
// daemon. The declaration scan moved here from the File Manager, which
// was the design doc's "the day a second caller wants one".
#include <stdlib.h>
#include "base/uopen.h"
#include "ulistdir.h"
#include "base/ulaunch.h"
#include "base/urecent.h"
#include "base/uconf.h"
#include "base/ucapture.h"
#include "base/ufileurl.h"
#include "etc_config.h"
#include "rt/sys.h"
#include "kpath.h"
#include "base/upath.h"
#include "base/ugrow.h"
#include <string.h>
#include <strings.h> // strncasecmp -- an extension is not case-sensitive
#include <stdio.h>
#include <ctype.h>

#define DESKTOP_ENTRY_DIR "/usr/wm/applications"

// The desktop entry file of stem `name`, allocated; NULL out of memory.
static char *entry_file(const char *name) {
    char *p = 0;
    return asprintf(&p, DESKTOP_ENTRY_DIR "/%s.desktop", name) < 0 ? 0 : p;
}

// A space/comma-separated list, matched whole -- ".md" must not match
// ".mdx", which a substring search would. See uopen.h.
int uopen_ext_matches(const char *list, const char *ext) {
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != ',') p++;
        int n = (int)(p - start);
        if (n > 0 && (int)strlen(ext) == n && strncasecmp(start, ext, (size_t)n) == 0)
            return 1;
    }
    return 0;
}

// An entry's key, at its length: NULL when absent.
static char *entry_key(const struct etc_config_buf *cfg, const char *key) {
    return uconf_buf_dup_in_or_top(cfg, UOPEN_ENTRY_SECTION, key);
}

// Handles= claims `ext`.
static int handles(const struct etc_config_buf *cfg, const char *ext, int *any) {
    char *list = entry_key(cfg, "Handles");
    int hit = list && uopen_ext_matches(list, ext);
    if (any) *any = list && list[0];
    free(list);
    return hit;
}

// One entry's Exec, by its name (the filename stem); NULL for none.
static char *entry_exec(const char *name) {
    char *entry = entry_file(name);
    if (!entry) return 0;
    struct etc_config_buf cfg;
    char *exec = uconf_load(entry, &cfg) ? entry_key(&cfg, "Exec") : 0;
    etc_config_buf_free(&cfg);
    free(entry);
    return exec;
}

// The declaration scan: the first entry whose Handles= claims `ext`.
static char *declared_exec(const char *ext) {
    struct ulist_entry *entries = 0;
    int n = ulistdir(DESKTOP_ENTRY_DIR, &entries);
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir) continue;
        char *entry = upath_join(DESKTOP_ENTRY_DIR, entries[i].name);
        if (!entry) continue;
        // Loaded ONCE and asked twice: etc_config_get() re-reads the
        // whole file per key (CLAUDE.md's 54-reads lesson).
        struct etc_config_buf cfg;
        char *exec = uconf_load(entry, &cfg) && handles(&cfg, ext, 0) ? entry_key(&cfg, "Exec") : 0;
        etc_config_buf_free(&cfg);
        free(entry);
        if (exec) { free(entries); return exec; }
    }
    free(entries);
    return 0;
}

// The override file's value for `ext`, allocated; NULL for none.
static char *override_for(const char *ext) {
    struct etc_config_buf conf;
    char *val = uconf_load(UOPEN_CONF, &conf) ? uconf_buf_dup_in_or_top(&conf, UOPEN_CONF_SECTION, ext) : 0;
    etc_config_buf_free(&conf);
    return val;
}

char *uopen_resolve(const char *path) {
    const char *dot = strrchr(k_path_basename(path), '.');
    if (!dot || !dot[1]) return 0;

    // The override file's keys are stored lowercased; matching them
    // case-insensitively at both ends is what makes ".JPG" one type.
    char ext[16];
    int n = 0;
    for (; dot[n] && n < (int)sizeof ext - 1; n++)
        ext[n] = (char)tolower((unsigned char)dot[n]);
    ext[n] = '\0';

    // `[Default Applications]` first, then the TOP LEVEL: a
    // mimeapps.list copied off a Linux box carries the header, ours does
    // not, and `open -s` keeps writing the flat form (uopen.h). Asking
    // only the section would stop resolving every override already on
    // disk. Loaded once rather than asked twice, since each ask is a
    // whole-file read.
    char *val = override_for(ext);
    if (val && val[0] && strcmp(val, "-") != 0) {
        if (val[0] == '/') return val;   // the literal-path escape hatch
        char *exec = entry_exec(val);
        if (exec) { free(val); return exec; }
        // A dangling override falls through to the declarations.
    }
    free(val);
    return declared_exec(ext);
}

int uopen_decide(const char *path, struct uopen_decision *d) {
    memset(d, 0, sizeof *d);
    if (ulaunch_classify(path, &d->info) && d->info.kind != ULAUNCH_NONE) {
        // An app opens at once; anything that cannot run AS IT IS (no
        // execute bit, no interpreter) is asked about, so the card can
        // say why.
        int act = !d->info.runnable || !d->info.interp_found ? ULAUNCH_ASK
                  : d->info.kind == ULAUNCH_APP ? ULAUNCH_RUN : ulaunch_policy(d->info.kind);
        d->act = act;
        return d->how = act == ULAUNCH_ASK ? UOPEN_ASK : UOPEN_RUN;
    }
    if ((d->exec = uopen_resolve(path))) return d->how = UOPEN_WITH;
    // A directory opens where directories live -- Explorer's rule.
    struct sys_stat st;
    if (sys_stat(path, &st) == 0 && st.is_dir) return d->how = UOPEN_FOLDER;
    return d->how = UOPEN_NOTHING;
}

void uopen_decision_free(struct uopen_decision *d) {
    free(d->exec);
    d->exec = 0;
}

// The path is always ONE argument: the string form of a spawn splits on
// whitespace, which cut "my notes.txt" to "my".
int uopen_decision_argv(const char *path, const struct uopen_decision *d, char **argv) {
    switch (d->how) {
    case UOPEN_RUN:
        return ulaunch_argv(path, d->info.kind, d->act, argv);
    case UOPEN_ASK:
        argv[0] = ULAUNCH_ASK_EXEC;
        break;
    case UOPEN_WITH:
        argv[0] = (char *)d->exec;
        break;
    case UOPEN_FOLDER:
        argv[0] = "/bin/wm/apps/files";
        break;
    default:
        return 0;
    }
    argv[1] = (char *)path;
    argv[2] = 0;
    return 1;
}

int uopen_spawn(const char *path) {
    struct uopen_decision d;
    char *argv[5];
    if (!uopen_decide(path, &d) || !uopen_decision_argv(path, &d, argv)) { uopen_decision_free(&d); return -1; }
    int pid = sys_spawn_argv(argv[0], argv);
    if (pid >= 0 && d.how == UOPEN_WITH) {   // a file opened for a person: Recent's (base/urecent.h)
        char app[URECENT_APP];
        urecent_app_of_exec(d.exec, app, sizeof app);
        urecent_add(path, app);
    }
    uopen_decision_free(&d);
    return pid;
}

int uopen_is_web_url(const char *url) {
    return url && (!strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8));
}

int uopen_url(const char *url) {
    if (!url) return -1;
    if (uopen_is_web_url(url)) {
        char *argv[] = { (char *)UOPEN_BROWSER, (char *)url, 0 };
        return sys_spawn_argv(argv[0], argv);
    }
    size_t cap = strlen(url) + 1;   // decoding never lengthens it
    char *path = malloc(cap);
    int pid = -1;
    if (path && ufileurl_to_path(url, path, cap) == 0) pid = uopen_spawn(path);
    free(path);
    return pid;
}

// The extension, lowercased, as the override file keys it. 0 for none.
static int ext_of(const char *path, char *ext, int cap) {
    const char *dot = strrchr(k_path_basename(path), '.');
    if (!dot || !dot[1]) return 0;
    int n = 0;
    for (; dot[n] && n < cap - 1; n++) ext[n] = (char)tolower((unsigned char)dot[n]);
    ext[n] = '\0';
    return 1;
}

// One app's three strings in one block, `entry` first.
static int app_set(struct uopen_app *a, const char *entry, const char *name, const char *exec) {
    size_t e = strlen(entry) + 1, n = strlen(name) + 1, x = strlen(exec) + 1;
    char *m = malloc(e + n + x);
    if (!m) return 0;
    memcpy(m, entry, e);
    memcpy(m + e, name, n);
    memcpy(m + e + n, exec, x);
    a->entry = m;
    a->name = m + e;
    a->exec = m + e + n;
    return 1;
}

void uopen_apps_free(struct uopen_app *apps, int n) {
    for (int i = 0; i < n; i++) free(apps[i].entry);
    free(apps);
}

int uopen_apps_for(const char *path, struct uopen_app **out, int *current) {
    struct ulist_entry *entries = 0;
    char ext[16];
    struct uopen_app *apps = 0;
    int count = 0, cap = 0;
    *current = -1;
    *out = 0;
    if (!ext_of(path, ext, sizeof ext)) return 0;
    char *now = uopen_resolve(path);
    struct etc_config_buf doc = {0}, *cfg = &doc;   // one entry at a time, freed before the next
    int n = ulistdir(DESKTOP_ENTRY_DIR, &entries);
    // Two passes, Windows' "Open with" order: the apps that claim this
    // extension, then every other app that opens files at all.
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < n; i++) {
        const char *nm = entries[i].name;
        size_t len = strlen(nm);
        if (entries[i].is_dir || len < 9 || strcmp(nm + len - 8, ".desktop")) continue;
        etc_config_buf_free(cfg);
        char *entry = upath_join(DESKTOP_ENTRY_DIR, nm);
        int loaded = entry && uconf_load(entry, cfg);
        free(entry);
        if (!loaded) continue;
        int any;
        if (handles(cfg, ext, &any) != (pass == 0) || !any) continue;
        char *exec = entry_key(cfg, "Exec"), *name = exec ? entry_key(cfg, "Name") : 0;
        char *stem = strndup(nm, len - 8);
        int ok = exec && stem && UGROW(apps, cap, count + 1) &&
                 app_set(&apps[count], stem, name ? name : k_path_basename(exec), exec);
        if (ok && now && !strcmp(now, exec)) *current = count;
        count += ok;
        free(exec);
        free(name);
        free(stem);
    }
    etc_config_buf_free(cfg);
    free(entries);
    // An override naming a program no entry claims still opens the file:
    // list it, or the choice would vanish from the list showing it.
    if (now && *current < 0 && UGROW(apps, cap, count + 1) &&
        app_set(&apps[count], now, k_path_basename(now), now))
        *current = count++;
    free(now);
    if (!count) { free(apps); apps = 0; }
    *out = apps;
    return count;
}

int uopen_set_default(const char *path, const char *entry) {
    char ext[16];
    if (!ext_of(path, ext, sizeof ext)) return -1;
    return uconf_set(UOPEN_CONF, ext, entry) ? 0 : -1;
}


// --- what the opening app offers besides opening ---------------------------

// The desktop entry (its file, allocated) that opens `path`, by
// uopen_resolve()'s two layers; NULL for none, for an override naming a
// literal program, or out of memory.
static char *opening_entry(const char *path) {
    char ext[16];
    if (!ext_of(path, ext, sizeof ext)) return 0;
    struct etc_config_buf doc, *cfg = &doc;
    char *val = override_for(ext);
    struct sys_stat st;
    char *entry = 0;
    if (val && val[0] == '/') { free(val); return 0; }
    if (val && val[0] && strcmp(val, "-") && (entry = entry_file(val)) && sys_stat(entry, &st) != 0) {
        free(entry);
        entry = 0;
    }
    free(val);
    struct ulist_entry *ents = 0;
    int n = entry ? 0 : ulistdir(DESKTOP_ENTRY_DIR, &ents);
    for (int i = 0; i < n && !entry; i++) {
        if (ents[i].is_dir || !(entry = upath_join(DESKTOP_ENTRY_DIR, ents[i].name))) continue;
        int hit = uconf_load(entry, cfg) && handles(cfg, ext, 0);
        etc_config_buf_free(cfg);
        if (!hit) { free(entry); entry = 0; }
    }
    free(ents);
    return entry;
}

int uopen_actions(const char *path, struct uopen_action **out) {
    *out = 0;
    char *entry = opening_entry(path);
    if (!entry) return 0;
    struct etc_config_buf doc, *cfg = &doc;
    int n = 0;
    char *list = uconf_load(entry, cfg) ? entry_key(cfg, "Actions") : 0;
    char *save = 0;
    for (char *id = list ? strtok_r(list, ";", &save) : 0; id; id = strtok_r(0, ";", &save)) {
        char *sec = 0, *name = 0, *exec = 0;
        if (asprintf(&sec, "Desktop Action %s", id) < 0) break;
        name = uconf_buf_dup(cfg, sec, "Name");
        exec = name ? uconf_buf_dup(cfg, sec, "Exec") : 0;
        free(sec);
        struct uopen_action *g = name && exec ? realloc(*out, sizeof **out * (size_t)(n + 1)) : 0;
        if (g) {
            *out = g;
            g[n].name = name;
            g[n].exec = exec;
            n++;
        } else {
            free(name);
            free(exec);
        }
    }
    free(list);
    etc_config_buf_free(cfg);
    free(entry);
    return n;
}

void uopen_actions_free(struct uopen_action *a, int n) {
    for (int i = 0; i < n; i++) { free(a[i].name); free(a[i].exec); }
    free(a);
}

char **uopen_exec_argv(const char *line, const char *path, char **words) {
    *words = line ? strdup(line) : 0;
    if (!*words) return 0;
    int cap = 4, n = 0, placed = 0;
    char **argv = malloc(sizeof *argv * (size_t)cap), *save = 0;
    for (char *w = argv ? strtok_r(*words, " \t", &save) : 0; w; w = strtok_r(0, " \t", &save)) {
        if (n + 3 > cap) {
            char **g = realloc(argv, sizeof *argv * (size_t)(cap *= 2));
            if (!g) { free(argv); argv = 0; break; }
            argv = g;
        }
        if (strcmp(w, "%f")) argv[n++] = w;
        else if (path && !placed++) argv[n++] = (char *)path;
    }
    if (argv && n && path && !placed) argv[n++] = (char *)path;
    if (argv) argv[n] = 0;
    if (!argv || !n) { free(argv); free(*words); return 0; }
    return argv;
}

int uopen_action_run(const struct uopen_action *a, const char *path) {
    char *words, **argv = uopen_exec_argv(a->exec, path, &words);
    if (!argv) return -1;
    int pid = sys_spawn_argv(argv[0], argv);
    free(argv);
    free(words);
    return pid;
}

// Each extension's X-Details line, asked of the entries once a process:
// the details pane asks on every selection, and a scan of every desktop
// entry per keypress is what made scrolling /bin paint half frames.
static struct { char ext[16]; char *line; } *g_xd;
static int g_nxd;

static const char *details_line(const char *path) {
    char ext[16];
    if (!ext_of(path, ext, sizeof ext)) return 0;
    for (int i = 0; i < g_nxd; i++)
        if (!strcmp(g_xd[i].ext, ext)) return g_xd[i].line[0] ? g_xd[i].line : 0;
    char *entry = opening_entry(path), *copy = 0;
    if (entry) {
        struct etc_config_buf cfg;
        if (uconf_load(entry, &cfg)) copy = entry_key(&cfg, "X-Details");
        etc_config_buf_free(&cfg);
        free(entry);
    }
    if (!copy) copy = strdup("");
    void *g = copy ? realloc(g_xd, sizeof *g_xd * (size_t)(g_nxd + 1)) : 0;
    if (!g) { free(copy); return 0; }
    g_xd = g;
    strlcpy(g_xd[g_nxd].ext, ext, sizeof g_xd[g_nxd].ext);
    g_xd[g_nxd++].line = copy;
    return copy[0] ? copy : 0;
}

char *uopen_details(const char *path) {
    const char *line = details_line(path);
    if (!line) return 0;
    char *words, **argv = uopen_exec_argv(line, path, &words);
    if (!argv) return 0;
    char *out = ucapture(argv[0], argv);
    free(argv);
    free(words);
    return out;
}
