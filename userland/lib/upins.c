// Places, one ordered list -- see upins.h.
#include "lib/upins.h"
#include "lib/udirs.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"
#include "kpath.h"

#define VERSION_LINE "#places 2"

struct pin { char entry[UPINS_PATH]; char label[UPINS_LABEL]; };

static const char *const DEFAULTS[] = {
    "recent:/", "/home", "udir:desktop", "udir:documents", "udir:music",
    "udir:pictures", "udir:videos", "udir:downloads", "trash:/",
};
#define N_DEFAULTS ((int)(sizeof DEFAULTS / sizeof DEFAULTS[0]))

static int is_entry(const char *s) {
    return s[0] == '/' || !strcmp(s, "recent:/") || !strcmp(s, "trash:/") || !strncmp(s, "udir:", 5);
}

static int index_of(const struct pin *all, int n, const char *entry) {
    for (int i = 0; i < n; i++)
        if (!strcmp(all[i].entry, entry)) return i;
    return -1;
}

// Every entry of the file, existing folders or not: what a rewrite must
// not lose. No file, or the old pins-only one: the defaults come first.
static int read_all(struct pin *out, int cap) {
    int n = 0, versioned = 0;
    static struct pin old[UPINS_MAX];   // 6 KiB: never on a ring-3 frame
    int nold = 0;
    FILE *f = fopen(UPINS_FILE, "r");
    if (f) {
        char line[UPINS_PATH + UPINS_LABEL + 4];
        while (fgets(line, sizeof line, f)) {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
            if (!strcmp(line, VERSION_LINE)) { versioned = 1; continue; }
            char *tab = strchr(line, '\t');
            if (tab) *tab++ = '\0';
            if (!is_entry(line) || nold >= UPINS_MAX) continue;   // blank, a comment, or not one
            strlcpy(old[nold].entry, line, UPINS_PATH);
            strlcpy(old[nold].label, tab ? tab : "", UPINS_LABEL);
            nold++;
        }
        fclose(f);
    }
    if (!versioned)
        for (int i = 0; i < N_DEFAULTS && n < cap; i++) {
            strlcpy(out[n].entry, DEFAULTS[i], UPINS_PATH);
            out[n++].label[0] = '\0';
        }
    for (int i = 0; i < nold && n < cap; i++)
        if (index_of(out, n, old[i].entry) < 0) out[n++] = old[i];
    return n;
}

static int write_all(const struct pin *in, int n) {
    FILE *f = fopen(UPINS_FILE, "w");
    if (!f) return -errno;
    fputs(VERSION_LINE "\n# Places, in order (the File Manager, the file dialog): entry[TAB name]\n", f);
    for (int i = 0; i < n; i++) {
        if (in[i].label[0]) fprintf(f, "%s\t%s\n", in[i].entry, in[i].label);
        else fprintf(f, "%s\n", in[i].entry);
    }
    return fclose(f) == 0 ? 0 : -EIO;
}

// The callers' buffers are static: a ring-3 frame has 2 KiB, and the
// list is several.
static struct pin g_all[UPINS_MAX + 1];

int upins_resolve(const char *entry, char *path, int pcap, char *label, int lcap,
                  const char **icon) {
    const char *ic = "folder", *name = 0;
    int virt = 0;
    if (!strcmp(entry, "recent:/"))     { name = "Recent";      ic = "place-recent"; virt = 1; strlcpy(path, entry, (size_t)pcap); }
    else if (!strcmp(entry, "trash:/")) { name = "Recycle Bin"; ic = "place-trash";  virt = 1; strlcpy(path, entry, (size_t)pcap); }
    else if (!strncmp(entry, "udir:", 5)) {
        enum udir d = UDIR_COUNT;
        for (int k = 0; k < UDIR_COUNT; k++)
            if (!strcmp(udir_key((enum udir)k), entry + 5)) d = (enum udir)k;
        strlcpy(path, d < UDIR_COUNT ? udir_path(d) : "/home", (size_t)pcap);
        name = d < UDIR_COUNT ? udir_label(d) : entry + 5;
        ic = d < UDIR_COUNT ? udir_icon(d) : "folder";
    } else {
        strlcpy(path, entry, (size_t)pcap);
        if (!strcmp(entry, "/home")) { name = "Home"; ic = "place-home"; }
        else { const char *b = k_path_basename(entry); name = b[0] ? b : entry; }
    }
    if (label) {
        int n = read_all(g_all, UPINS_MAX), i = index_of(g_all, n, entry);
        strlcpy(label, i >= 0 && g_all[i].label[0] ? g_all[i].label : name, (size_t)lcap);
    }
    if (icon) *icon = ic;
    return virt;
}

int upins_load(char (*out)[UPINS_PATH], int cap) {
    static struct pin all[UPINS_MAX + 1];
    int n = read_all(all, UPINS_MAX), k = 0;
    for (int i = 0; i < n && k < cap; i++) {
        char path[UPINS_PATH];
        struct sys_stat st;
        int virt = upins_resolve(all[i].entry, path, sizeof path, 0, 0, 0);
        if (virt || (sys_stat(path, &st) == 0 && st.is_dir)) strlcpy(out[k++], all[i].entry, UPINS_PATH);
    }
    return k;
}

int upins_entry_of(const char *path, char *out, int cap) {
    static struct pin all[UPINS_MAX + 1];
    int n = read_all(all, UPINS_MAX);
    for (int i = 0; i < n; i++) {
        char p[UPINS_PATH];
        upins_resolve(all[i].entry, p, sizeof p, 0, 0, 0);
        if (!strcmp(p, path) || !strcmp(all[i].entry, path)) { strlcpy(out, all[i].entry, (size_t)cap); return 1; }
    }
    return 0;
}

int upins_has(const char *path) {
    char e[UPINS_PATH];
    return upins_entry_of(path, e, sizeof e);
}

int upins_add_before(const char *path, const char *before) {
    struct sys_stat st;
    if (!path || path[0] != '/' || strlen(path) >= UPINS_PATH ||
        sys_stat(path, &st) != 0 || !st.is_dir) return -EINVAL;
    if (upins_has(path)) return -EEXIST;
    // `before` may be a path the list holds as an entry (/home/Music is
    // udir:music): asked for as the tree shows it.
    char b[UPINS_PATH];
    int have_b = before && upins_entry_of(before, b, sizeof b);
    int n = read_all(g_all, UPINS_MAX);
    if (n >= UPINS_MAX) return -ENOSPC;
    int at = have_b ? index_of(g_all, n, b) : -1;
    if (at < 0) at = n;
    memmove(&g_all[at + 1], &g_all[at], (size_t)(n - at) * sizeof g_all[0]);
    strlcpy(g_all[at].entry, path, UPINS_PATH);
    g_all[at].label[0] = '\0';
    return write_all(g_all, n + 1);
}

int upins_add(const char *path) { return upins_add_before(path, 0); }

int upins_remove(const char *entry) {
    char e[UPINS_PATH];
    if (!upins_entry_of(entry, e, sizeof e)) return -ENOENT;
    int n = read_all(g_all, UPINS_MAX), i = index_of(g_all, n, e);
    if (i < 0) return -ENOENT;
    memmove(&g_all[i], &g_all[i + 1], (size_t)(n - i - 1) * sizeof g_all[0]);
    return write_all(g_all, n - 1);
}

void upins_label(const char *entry, char *out, int cap) {
    char p[UPINS_PATH];
    upins_resolve(entry, p, sizeof p, out, cap, 0);
}

int upins_set_label(const char *entry, const char *label) {
    if (strpbrk(label, "\t\r\n")) return -EINVAL;
    char e[UPINS_PATH], p[UPINS_PATH], def[UPINS_LABEL];
    if (!upins_entry_of(entry, e, sizeof e)) return -ENOENT;
    int n = read_all(g_all, UPINS_MAX), i = index_of(g_all, n, e);
    if (i < 0) return -ENOENT;
    g_all[i].label[0] = '\0';
    upins_resolve(e, p, sizeof p, 0, 0, 0);
    // Its default name is no name of its own.
    const char *b = k_path_basename(p);
    strlcpy(def, !strcmp(e, "recent:/") ? "Recent" : !strcmp(e, "trash:/") ? "Recycle Bin"
                 : !strcmp(e, "/home") ? "Home" : b, sizeof def);
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
    char e[UPINS_PATH];
    if (!upins_entry_of(entry, e, sizeof e)) return -ENOENT;
    int n = read_all(g_all, UPINS_MAX), i = index_of(g_all, n, e);
    if (i < 0) return -ENOENT;
    int j = i + delta;
    if (j < 0) j = 0;
    if (j >= n) j = n - 1;
    if (j == i) return -ERANGE;
    return move_to(n, i, j);
}

int upins_move_before(const char *entry, const char *before) {
    char e[UPINS_PATH], b[UPINS_PATH];
    if (!upins_entry_of(entry, e, sizeof e)) return -ENOENT;
    int n = read_all(g_all, UPINS_MAX), i = index_of(g_all, n, e);
    if (i < 0) return -ENOENT;
    int k = before && upins_entry_of(before, b, sizeof b) ? index_of(g_all, n, b) : n;
    if (k < 0) k = n;
    // Removing it first shifts everything after it up by one.
    int j = k > i ? k - 1 : k;
    if (j == i) return 0;
    return move_to(n, i, j);
}

int upins_missing_defaults(void) {
    int n = read_all(g_all, UPINS_MAX), miss = 0;
    for (int i = 0; i < N_DEFAULTS; i++) if (index_of(g_all, n, DEFAULTS[i]) < 0) miss++;
    return miss;
}

int upins_restore_defaults(void) {
    int n = read_all(g_all, UPINS_MAX), back = 0;
    for (int i = 0; i < N_DEFAULTS && n < UPINS_MAX; i++) {
        if (index_of(g_all, n, DEFAULTS[i]) >= 0) continue;
        strlcpy(g_all[n].entry, DEFAULTS[i], UPINS_PATH);
        g_all[n++].label[0] = '\0';
        back++;
    }
    if (!back) return 0;
    int rc = write_all(g_all, n);
    return rc < 0 ? rc : back;
}
