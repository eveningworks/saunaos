// Places, one ordered list -- see upins.h.
#include "base/upins.h"
#include "base/uconf.h"
#include "base/udirs.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"
#include "kpath.h"
#include <stdlib.h>

#define VERSION_LINE "#places 2"   // the format's first line; a reader skips it as a comment

struct pin { char *entry; char label[UPINS_LABEL]; };   // entry: heap

// The "dev:" lines, read with the entries and written back with them, so
// no rewrite of the Places list drops the mounts' order.
static char g_dev[UPINS_DEV_MAX][64];
static int g_ndev;

// "Home" is a plain path, the person's home -- the third entry.
static const char *const DEFAULT_ENTRIES[] = {
    "quick:/", "recent:/", 0, "udir:desktop", "udir:documents", "udir:music",
    "udir:pictures", "udir:videos", "udir:downloads", "trash:/",
};
#define N_DEFAULTS ((int)(sizeof DEFAULT_ENTRIES / sizeof DEFAULT_ENTRIES[0]))
#define DEFAULTS(i) (DEFAULT_ENTRIES[i] ? DEFAULT_ENTRIES[i] : udir_home())

static int is_entry(const char *s) {
    return s[0] == '/' || !strcmp(s, "recent:/") || !strcmp(s, "trash:/") || !strcmp(s, "quick:/") ||
           !strncmp(s, "udir:", 5);
}

static int index_of(const struct pin *all, int n, const char *entry) {
    for (int i = 0; i < n; i++)
        if (!strcmp(all[i].entry, entry)) return i;
    return -1;
}

// Every entry of the file, existing folders or not: what a rewrite must
// not lose. No file, or one that holds nothing at all (write_all()
// truncates before it writes, so a cut there leaves 0 bytes): the
// defaults.
// `out` holds `cap` pins whose entries are this file's: freed and
// replaced here. A pin that cannot be allocated is left out.
static int read_all(struct pin *out, int cap) {
    for (int i = 0; i < cap; i++) { free(out[i].entry); out[i].entry = 0; }
    int n = 0, empty = 1;
    static struct pin old[UPINS_MAX];
    int nold = 0;
    g_ndev = 0;
    static char at[UCONF_USER_PATH_MAX];
    FILE *f = uconf_own_read_path(UPINS_FILE, at, sizeof at) ? fopen(at, "r") : 0;
    if (f) {
        char *line = 0;
        size_t lcap = 0;
        while (getline(&line, &lcap, f) > 0) {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
            if (line[0]) empty = 0;
            if (!strncmp(line, "dev:", 4)) {
                if (g_ndev < UPINS_DEV_MAX && line[4] == '/') strlcpy(g_dev[g_ndev++], line + 4, sizeof g_dev[0]);
                continue;
            }
            char *tab = strchr(line, '\t');
            if (tab) *tab++ = '\0';
            if (!is_entry(line) || nold >= UPINS_MAX) continue;   // blank, a comment, or not one
            if (!(old[nold].entry = strdup(line))) continue;
            strlcpy(old[nold].label, tab ? tab : "", UPINS_LABEL);
            nold++;
        }
        free(line);
        fclose(f);
    }
    if (empty) {
        for (int i = 0; i < N_DEFAULTS && n < cap; i++) {
            if (!(out[n].entry = strdup(DEFAULTS(i)))) continue;
            out[n++].label[0] = '\0';
        }
    }
    for (int i = 0; i < nold; i++) {
        if (n < cap && index_of(out, n, old[i].entry) < 0) out[n++] = old[i];
        else free(old[i].entry);
        old[i].entry = 0;
    }
    return n;
}

static int write_all(const struct pin *in, int n) {
    static char at[UCONF_USER_PATH_MAX];
    if (!uconf_own_write_path(UPINS_FILE, at, sizeof at)) return -ENAMETOOLONG;
    FILE *f = fopen(at, "w");
    if (!f) return -errno;
    fputs(VERSION_LINE "\n# Places, in order (the File Manager, the file dialog): entry[TAB name]\n", f);
    for (int i = 0; i < n; i++) {
        if (in[i].label[0]) fprintf(f, "%s\t%s\n", in[i].entry, in[i].label);
        else fprintf(f, "%s\n", in[i].entry);
    }
    if (g_ndev) fputs("# The mounts' order under This computer and Network\n", f);
    for (int i = 0; i < g_ndev; i++) fprintf(f, "dev:%s\n", g_dev[i]);
    return fclose(f) == 0 ? 0 : -EIO;
}

// The list as the file holds it; one more slot for an add. Its entries
// are heap strings, freed by the next read_all().
static struct pin g_all[UPINS_MAX + 1];

// Where an entry goes, without touching the file: what lookups loop on.
static const char *target(const char *entry, const char **name, const char **icon, int *virt) {
    const char *ic = "folder", *nm = 0, *path = entry;
    *virt = 0;
    if (!strcmp(entry, "recent:/"))     { nm = "Recent";       ic = "place-recent"; *virt = 1; }
    else if (!strcmp(entry, "quick:/")) { nm = "Quick access"; ic = "place-quick";  *virt = 1; }
    else if (!strcmp(entry, "trash:/")) { nm = "Recycle Bin";  ic = "place-trash";  *virt = 1; }
    else if (!strncmp(entry, "udir:", 5)) {
        enum udir d = UDIR_COUNT;
        for (int k = 0; k < UDIR_COUNT; k++)
            if (!strcmp(udir_key((enum udir)k), entry + 5)) d = (enum udir)k;
        path = d < UDIR_COUNT ? udir_path(d) : udir_home();
        nm = d < UDIR_COUNT ? udir_label(d) : entry + 5;
        ic = d < UDIR_COUNT ? udir_icon(d) : "folder";
    } else {
        if (!strcmp(entry, udir_home())) { nm = "Home"; ic = "place-home"; }
        else { const char *b = k_path_basename(entry); nm = b[0] ? b : entry; }
    }
    if (name) *name = nm;
    if (icon) *icon = ic;
    return path;
}

int upins_resolve(const char *entry, char **path, char *label, int lcap, const char **icon) {
    const char *name;
    int virt;
    const char *to = target(entry, &name, icon, &virt);
    if (path) *path = strdup(to);
    if (label) {
        int n = read_all(g_all, UPINS_MAX), i = index_of(g_all, n, entry);
        strlcpy(label, i >= 0 && g_all[i].label[0] ? g_all[i].label : name, (size_t)lcap);
    }
    return virt;
}

void upins_free(char **entries, int n) {
    for (int i = 0; i < n; i++) free(entries[i]);
}

int upins_load(char **out, int cap) {
    int n = read_all(g_all, UPINS_MAX), k = 0;
    for (int i = 0; i < n && k < cap; i++) {
        struct sys_stat st;
        int virt;
        const char *path = target(g_all[i].entry, 0, 0, &virt);
        if (!virt && (sys_stat(path, &st) != 0 || !st.is_dir)) continue;
        if ((out[k] = strdup(g_all[i].entry))) k++;
    }
    return k;
}

char *upins_entry_of(const char *path) {
    int n = read_all(g_all, UPINS_MAX), virt;
    for (int i = 0; i < n; i++)
        if (!strcmp(target(g_all[i].entry, 0, 0, &virt), path) || !strcmp(g_all[i].entry, path))
            return strdup(g_all[i].entry);
    return 0;
}

int upins_has(const char *path) {
    char *e = upins_entry_of(path);
    free(e);
    return e != 0;
}

// The pin `entry` names in g_all, freshly read: its index, or -1. `*n`
// is the list's length.
static int find(const char *entry, int *n) {
    *n = 0;
    char *e = upins_entry_of(entry);
    if (!e) return -1;
    *n = read_all(g_all, UPINS_MAX);
    int i = index_of(g_all, *n, e);
    free(e);
    return i;
}

int upins_add_before(const char *path, const char *before) {
    struct sys_stat st;
    if (!path || path[0] != '/' || sys_stat(path, &st) != 0 || !st.is_dir) return -EINVAL;
    if (upins_has(path)) return -EEXIST;
    // `before` may be a path the list holds as an entry (/home/user/Music is
    // udir:music): asked for as the tree shows it.
    char *b = before ? upins_entry_of(before) : 0;
    char *copy = strdup(path);
    int n = read_all(g_all, UPINS_MAX);
    int at = b ? index_of(g_all, n, b) : -1;
    free(b);
    if (!copy) return -ENOMEM;
    if (n >= UPINS_MAX) { free(copy); return -ENOSPC; }
    if (at < 0) at = n;
    memmove(&g_all[at + 1], &g_all[at], (size_t)(n - at) * sizeof g_all[0]);
    g_all[at].entry = copy;
    g_all[at].label[0] = '\0';
    return write_all(g_all, n + 1);
}

int upins_add(const char *path) { return upins_add_before(path, 0); }

int upins_remove(const char *entry) {
    int n, i = find(entry, &n);
    if (i < 0) return -ENOENT;
    char *gone = g_all[i].entry;
    memmove(&g_all[i], &g_all[i + 1], (size_t)(n - i - 1) * sizeof g_all[0]);
    g_all[n - 1].entry = 0;
    free(gone);
    return write_all(g_all, n - 1);
}

void upins_label(const char *entry, char *out, int cap) {
    upins_resolve(entry, 0, out, cap, 0);
}

int upins_set_label(const char *entry, const char *label) {
    if (strpbrk(label, "\t\r\n")) return -EINVAL;
    char def[UPINS_LABEL];
    int n, i = find(entry, &n), virt;
    if (i < 0) return -ENOENT;
    const char *e = g_all[i].entry;
    g_all[i].label[0] = '\0';
    // Its default name is no name of its own.
    const char *b = k_path_basename(target(e, 0, 0, &virt));
    strlcpy(def, !strcmp(e, "recent:/") ? "Recent" : !strcmp(e, "trash:/") ? "Recycle Bin"
                 : !strcmp(e, "quick:/") ? "Quick access"
                 : !strcmp(e, udir_home()) ? "Home" : b, sizeof def);
    if (!strncmp(e, "udir:", 5))
        for (int k = 0; k < UDIR_COUNT; k++)
            if (!strcmp(udir_key((enum udir)k), e + 5)) strlcpy(def, udir_label((enum udir)k), sizeof def);
    strlcpy(g_all[i].label, strcmp(label, def) ? label : "", UPINS_LABEL);
    return write_all(g_all, n);
}

static int move_to(int n, int i, int j) {
    struct pin p = g_all[i];
    if (j < i) memmove(&g_all[j + 1], &g_all[j], (size_t)(i - j) * sizeof p);
    else if (j > i) memmove(&g_all[i], &g_all[i + 1], (size_t)(j - i) * sizeof p);
    g_all[j] = p;
    return write_all(g_all, n);
}

int upins_move(const char *entry, int delta) {
    int n, i = find(entry, &n);
    if (i < 0) return -ENOENT;
    int j = i + delta;
    if (j < 0) j = 0;
    if (j >= n) j = n - 1;
    if (j == i) return -ERANGE;
    return move_to(n, i, j);
}

int upins_move_before(const char *entry, const char *before) {
    char *b = before ? upins_entry_of(before) : 0;
    int n, i = find(entry, &n);
    int k = b ? index_of(g_all, n, b) : n;
    free(b);
    if (i < 0) return -ENOENT;
    if (k < 0) k = n;
    // Removing it first shifts everything after it up by one.
    int j = k > i ? k - 1 : k;
    if (j == i) return 0;
    return move_to(n, i, j);
}

int upins_missing_defaults(void) {
    int n = read_all(g_all, UPINS_MAX), miss = 0;
    for (int i = 0; i < N_DEFAULTS; i++) if (index_of(g_all, n, DEFAULTS(i)) < 0) miss++;
    return miss;
}

int upins_restore_defaults(void) {
    int n = read_all(g_all, UPINS_MAX), back = 0;
    for (int i = 0; i < N_DEFAULTS && n < UPINS_MAX; i++) {
        if (index_of(g_all, n, DEFAULTS(i)) >= 0) continue;
        if (!(g_all[n].entry = strdup(DEFAULTS(i)))) continue;
        g_all[n++].label[0] = '\0';
        back++;
    }
    if (!back) return 0;
    int rc = write_all(g_all, n);
    return rc < 0 ? rc : back;
}

int upins_dev_order(char (*out)[64], int cap) {
    read_all(g_all, UPINS_MAX);
    int n = g_ndev < cap ? g_ndev : cap;
    for (int i = 0; i < n; i++) strlcpy(out[i], g_dev[i], sizeof out[0]);
    return n;
}

int upins_dev_set_order(char (*points)[64], int n) {
    int np = read_all(g_all, UPINS_MAX);
    static char merged[UPINS_DEV_MAX][64];
    int m = 0;
    for (int i = 0; i < n && m < UPINS_DEV_MAX; i++) strlcpy(merged[m++], points[i], sizeof merged[0]);
    for (int i = 0; i < g_ndev && m < UPINS_DEV_MAX; i++) {
        int seen = 0;
        for (int k = 0; k < n; k++) if (!strcmp(points[k], g_dev[i])) seen = 1;
        if (!seen) strlcpy(merged[m++], g_dev[i], sizeof merged[0]);
    }
    memcpy(g_dev, merged, sizeof merged[0] * (size_t)m);
    g_ndev = m;
    return write_all(g_all, np);
}
