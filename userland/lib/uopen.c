// uopen -- see lib/uopen.h for the two layers and why there is no
// daemon. The declaration scan moved here from the File Manager, which
// was the design doc's "the day a second caller wants one".
#include <stdlib.h>
#include "lib/uopen.h"
#include "ulistdir.h"
#include "lib/ulaunch.h"
#include "lib/urecent.h"
#include "lib/uconf.h"
#include "lib/ucapture.h"
#include "etc_config.h"
#include "rt/sys.h"
#include "kpath.h"
#include <string.h>
#include <strings.h> // strncasecmp -- an extension is not case-sensitive
#include <stdio.h>
#include <ctype.h>

#define UOPEN_PATH_MAX 256 // NOT FS_PATH_MAX (4096): this file's own bound
#define DESKTOP_ENTRY_DIR "/usr/wm/applications"

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

// One entry's Exec, by its name (the filename stem). 1 = found.
static int entry_exec(const char *name, char *out, int cap) {
    char entry[UOPEN_PATH_MAX];
    if (snprintf(entry, sizeof entry, DESKTOP_ENTRY_DIR "/%s.desktop", name)
        >= (int)sizeof entry)
        return 0;
    struct etc_config_buf cfg;
    int rc = uconf_load(entry, &cfg)
             && etc_config_buf_get_in_or_top(&cfg, UOPEN_ENTRY_SECTION, "Exec",
                                             out, (uint32_t)cap);
    etc_config_buf_free(&cfg);
    return rc ? 1 : 0;
}

// The declaration scan: the first entry whose Handles= claims `ext`.
static int declared_exec(const char *ext, char *out, int cap) {
    struct sys_dirent *entries = 0;
    int n = ulistdir(DESKTOP_ENTRY_DIR, &entries);
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir) continue;
        char entry[UOPEN_PATH_MAX];
        if (!k_path_join(DESKTOP_ENTRY_DIR, entries[i].name, entry, sizeof entry))
            continue;
        // Loaded ONCE and asked twice: etc_config_get() re-reads the
        // whole file per key (CLAUDE.md's 54-reads lesson).
        struct etc_config_buf cfg;
        char list[128];   // a handful of extensions, not a document
        int hit = uconf_load(entry, &cfg)
                  && etc_config_buf_get_in_or_top(&cfg, UOPEN_ENTRY_SECTION,
                                                  "Handles", list, sizeof list)
                  && uopen_ext_matches(list, ext)
                  && etc_config_buf_get_in_or_top(&cfg, UOPEN_ENTRY_SECTION,
                                                  "Exec", out, (uint32_t)cap);
        etc_config_buf_free(&cfg);
        if (hit) { free(entries); return 1; }
    }
    free(entries);
    return 0;
}

int uopen_resolve(const char *path, char *exec, int cap) {
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
    char val[UOPEN_PATH_MAX];
    val[0] = '\0';
    struct etc_config_buf conf;
    if (uconf_load(UOPEN_CONF, &conf))
        etc_config_buf_get_in_or_top(&conf, UOPEN_CONF_SECTION, ext, val, sizeof val);
    etc_config_buf_free(&conf);
    if (val[0] && strcmp(val, "-") != 0) {
        if (val[0] == '/') { // the literal-path escape hatch
            strlcpy(exec, val, (size_t)cap);
            return 1;
        }
        if (entry_exec(val, exec, cap)) return 1;
        // A dangling override falls through to the declarations.
    }
    return declared_exec(ext, exec, cap);
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
    if (uopen_resolve(path, d->exec, sizeof d->exec)) return d->how = UOPEN_WITH;
    // A directory opens where directories live -- Explorer's rule.
    struct sys_dirent probe;
    if (sys_listdir(path, &probe, 1) >= 0) return d->how = UOPEN_FOLDER;
    return d->how = UOPEN_NOTHING;
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
    if (!uopen_decide(path, &d) || !uopen_decision_argv(path, &d, argv)) return -1;
    int pid = sys_spawn_argv(argv[0], argv);
    if (pid >= 0 && d.how == UOPEN_WITH) {   // a file opened for a person: Recent's (lib/urecent.h)
        char app[URECENT_APP];
        urecent_app_of_exec(d.exec, app, sizeof app);
        urecent_add(path, app);
    }
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

int uopen_apps_for(const char *path, struct uopen_app *out, int max, int *current) {
    struct sys_dirent *entries = 0;
    char ext[16], now[UOPEN_PATH_MAX] = "";
    int count = 0;
    *current = -1;
    if (!ext_of(path, ext, sizeof ext)) return 0;
    if (!uopen_resolve(path, now, sizeof now)) now[0] = '\0';
    struct etc_config_buf doc = {0}, *cfg = &doc;   // one entry at a time, freed before the next
    int n = ulistdir(DESKTOP_ENTRY_DIR, &entries);
    // Two passes, Windows' "Open with" order: the apps that claim this
    // extension, then every other app that opens files at all.
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < n && count < max; i++) {
        const char *nm = entries[i].name;
        size_t len = strlen(nm);
        if (entries[i].is_dir || len < 9 || strcmp(nm + len - 8, ".desktop")) continue;
        char entry[UOPEN_PATH_MAX], list[128];
        struct uopen_app *a = &out[count];
        etc_config_buf_free(cfg);
        if (!k_path_join(DESKTOP_ENTRY_DIR, nm, entry, sizeof entry) || !uconf_load(entry, cfg)) continue;
        if (!etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Handles", list, sizeof list) ||
            !list[0] || uopen_ext_matches(list, ext) != (pass == 0) ||
            !etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Exec", a->exec, sizeof a->exec))
            continue;
        if (!etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Name", a->name, sizeof a->name))
            strlcpy(a->name, k_path_basename(a->exec), sizeof a->name);
        snprintf(a->entry, sizeof a->entry, "%.*s", (int)(len - 8), nm);
        if (now[0] && !strcmp(now, a->exec)) *current = count;
        count++;
    }
    etc_config_buf_free(cfg);
    free(entries);
    // An override naming a program no entry claims still opens the file:
    // list it, or the choice would vanish from the list showing it.
    if (now[0] && *current < 0 && count < max) {
        struct uopen_app *a = &out[count];
        strlcpy(a->exec, now, sizeof a->exec);
        strlcpy(a->name, k_path_basename(now), sizeof a->name);
        strlcpy(a->entry, now, sizeof a->entry);
        *current = count++;
    }
    return count;
}

int uopen_set_default(const char *path, const char *entry) {
    char ext[16];
    if (!ext_of(path, ext, sizeof ext)) return -1;
    return uconf_set(UOPEN_CONF, ext, entry) ? 0 : -1;
}


// --- what the opening app offers besides opening ---------------------------

// The desktop entry (its file) that opens `path`, by uopen_resolve()'s
// two layers; 0 for none, or for an override naming a literal program.
static int opening_entry(const char *path, char *entry, int cap) {
    char ext[16], val[UOPEN_PATH_MAX] = "", list[128];
    if (!ext_of(path, ext, sizeof ext)) return 0;
    struct etc_config_buf doc, *cfg = &doc;
    if (uconf_load(UOPEN_CONF, cfg)) etc_config_buf_get_in_or_top(cfg, UOPEN_CONF_SECTION, ext, val, sizeof val);
    etc_config_buf_free(cfg);   // reused below, one entry at a time
    struct sys_stat st;
    int hit = 0;
    if (val[0] == '/') return 0;
    if (val[0] && strcmp(val, "-") && snprintf(entry, (size_t)cap, DESKTOP_ENTRY_DIR "/%s.desktop", val) < cap &&
        sys_stat(entry, &st) == 0)
        hit = 1;
    struct sys_dirent *ents = 0;
    int n = hit ? 0 : ulistdir(DESKTOP_ENTRY_DIR, &ents);
    for (int i = 0; i < n && !hit; i++) {
        hit = !ents[i].is_dir && k_path_join(DESKTOP_ENTRY_DIR, ents[i].name, entry, (size_t)cap) &&
              uconf_load(entry, cfg) &&
              etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Handles", list, sizeof list) &&
              uopen_ext_matches(list, ext);
        etc_config_buf_free(cfg);
    }
    free(ents);
    return hit;
}

int uopen_actions(const char *path, struct uopen_action **out) {
    *out = 0;
    char entry[UOPEN_PATH_MAX], list[256];
    if (!opening_entry(path, entry, sizeof entry)) return 0;
    struct etc_config_buf doc, *cfg = &doc;
    int n = 0;
    if (uconf_load(entry, cfg) && etc_config_buf_get_in_or_top(cfg, UOPEN_ENTRY_SECTION, "Actions", list, sizeof list)) {
        char *save = 0;
        for (char *id = strtok_r(list, ";", &save); id; id = strtok_r(0, ";", &save)) {
            char sec[96], name[96], exec[UOPEN_PATH_MAX];
            snprintf(sec, sizeof sec, "Desktop Action %s", id);
            if (!etc_config_buf_get_in(cfg, sec, "Name", name, sizeof name) ||
                !etc_config_buf_get_in(cfg, sec, "Exec", exec, sizeof exec))
                continue;
            struct uopen_action *g = realloc(*out, sizeof **out * (size_t)(n + 1));
            if (!g) break;
            *out = g;
            g[n].name = strdup(name);
            g[n].exec = strdup(exec);
            if (!g[n].name || !g[n].exec) { free(g[n].name); free(g[n].exec); break; }
            n++;
        }
    }
    etc_config_buf_free(cfg);
    return n;
}

void uopen_actions_free(struct uopen_action *a, int n) {
    for (int i = 0; i < n; i++) { free(a[i].name); free(a[i].exec); }
    free(a);
}

// An Exec line's words, `%f` made `path` -- ONE argument, spaces and all.
// No quoting: an entry's own lines name programs and flags, not paths.
static char **exec_argv(const char *line, const char *path, char **words) {
    *words = strdup(line);
    if (!*words) return 0;
    int cap = 4, n = 0;
    char **argv = malloc(sizeof *argv * (size_t)cap), *save = 0;
    for (char *w = argv ? strtok_r(*words, " \t", &save) : 0; w; w = strtok_r(0, " \t", &save)) {
        if (n + 2 > cap) {
            char **g = realloc(argv, sizeof *argv * (size_t)(cap *= 2));
            if (!g) { free(argv); argv = 0; break; }
            argv = g;
        }
        argv[n++] = !strcmp(w, "%f") ? (char *)path : w;
    }
    if (argv) argv[n] = 0;
    if (!argv || !n) { free(argv); free(*words); return 0; }
    return argv;
}

int uopen_action_run(const struct uopen_action *a, const char *path) {
    char *words, **argv = exec_argv(a->exec, path, &words);
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
    char ext[16], entry[UOPEN_PATH_MAX], line[UOPEN_PATH_MAX] = "";
    if (!ext_of(path, ext, sizeof ext)) return 0;
    for (int i = 0; i < g_nxd; i++)
        if (!strcmp(g_xd[i].ext, ext)) return g_xd[i].line[0] ? g_xd[i].line : 0;
    if (opening_entry(path, entry, sizeof entry)) {
        struct etc_config_buf cfg;
        if (uconf_load(entry, &cfg))
            etc_config_buf_get_in_or_top(&cfg, UOPEN_ENTRY_SECTION, "X-Details", line, sizeof line);
        etc_config_buf_free(&cfg);
    }
    char *copy = strdup(line);
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
    char *words, **argv = exec_argv(line, path, &words);
    if (!argv) return 0;
    char *out = ucapture(argv[0], argv);
    free(argv);
    free(words);
    return out;
}
