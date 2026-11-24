// owners -- puts the system volume's files in the hands /etc/owners.conf
// says (users stage C, docs/users-design.md), at every boot before the
// desktop starts: an old disk is root's throughout (an inode from before
// stage C reads owner 0), and a file a root service makes in /etc must
// still be the desktop's to rewrite. systemd-tmpfiles' `z`/`Z` lines, in
// a format with an explicit scope and a most-specific-rule-wins order.
//
// WHAT A BOOT COSTS: one stat per file the every-boot rules cover (/etc,
// a few hundred), and a chown only where an owner is wrong. A `once`
// rule's tree (/home) is walked when the file changes -- its hash is in
// /var/lib/owners -- or with --all.
//
// It never follows a symlink, never enters another mount, and never
// changes a file no rule covers.
#include "rt/sys.h"
#include "base/uargs.h"
#include "query_abi.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MARKER "/var/lib/owners"

static int opt_all, opt_dry, opt_verbose;
static const char *opt_config;

static const struct uargs_opt OPTS[] = {
    { "all",     'a', 0, "walk every rule's tree, the `once` ones too", &opt_all, 0 },
    { "dry-run", 'n', 0, "say what would change; change nothing", &opt_dry, 0 },
    { "verbose", 'v', 0, "say each file changed", &opt_verbose, 0 },
    { "config",  'c', "FILE", "read FILE instead of /etc/owners.conf", 0, &opt_config },
    { 0, 0, 0, 0, 0, 0 },
};

static const struct uargs_prog PROG = {
    "owners", "[OPTION]...",
    "Give the system volume's files the owners and modes /etc/owners.conf says.",
    OPTS, 0, 0, 0, 0, 0,
};

enum { SC_SELF = 1, SC_BELOW = 2 };

struct rule {
    char *path;
    size_t len;
    unsigned uid, gid;
    int mode;        // -1: keep
    int add_dirs;    // mode is `+BITS`: OR them into folders only
    int scope;       // SC_*
    int once;
};

static struct rule *g_rules;
static int g_nrules, g_caprules;
static char **g_mounts;   // other mounts' points, never entered
static int g_nmounts;
static unsigned g_changed, g_failed, g_seen;

static int add_rule(const struct rule *r) {
    if (g_nrules == g_caprules) {
        int nc = g_caprules ? g_caprules * 2 : 32;
        struct rule *nr = realloc(g_rules, (size_t)nc * sizeof *nr);
        if (!nr) return -1;
        g_rules = nr;
        g_caprules = nc;
    }
    g_rules[g_nrules++] = *r;
    return 0;
}

static int name_uid(const char *s, unsigned *out) {
    struct passwd *pw = getpwnam(s);
    if (pw) { *out = (unsigned)pw->pw_uid; return 0; }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (!*s || *end) return -1;
    *out = (unsigned)v;
    return 0;
}

static int name_gid(const char *s, unsigned *out) {
    struct group *gr = getgrnam(s);
    if (gr) { *out = (unsigned)gr->gr_gid; return 0; }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (!*s || *end) return -1;
    *out = (unsigned)v;
    return 0;
}

// A line that does not parse is REFUSED with its number, and the whole
// file with it: a half-read policy would give some files to the wrong
// account.
static int load(const char *file, uint32_t *hash) {
    FILE *f = fopen(file, "r");
    if (!f) { fprintf(stderr, "owners: %s: %s\n", file, strerror(errno)); return -1; }
    char line[512];
    int lineno = 0;
    *hash = 2166136261u;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        for (char *p = line; *p; p++) *hash = (*hash ^ (uint8_t)*p) * 16777619u;
        char *hashc = strchr(line, '#');
        if (hashc) *hashc = 0;
        char *w[7];
        int n = 0;
        for (char *t = strtok(line, " \t\r\n"); t && n < 7; t = strtok(0, " \t\r\n")) w[n++] = t;
        if (!n) continue;
        struct rule r;
        memset(&r, 0, sizeof r);
        int bad = n < 5 || n > 6 || w[0][0] != '/';
        if (!bad) bad = name_uid(w[1], &r.uid) || name_gid(w[2], &r.gid);
        if (!bad) {
            const char *m = w[3];
            r.add_dirs = m[0] == '+';
            if (r.add_dirs) m++;
            if (!strcmp(m, "-") && !r.add_dirs) r.mode = -1;
            else { char *e; r.mode = (int)strtol(m, &e, 8); bad = !*m || *e || r.mode < 0 || r.mode > 07777; }
        }
        if (!bad) {
            r.scope = !strcmp(w[4], "self") ? SC_SELF : !strcmp(w[4], "below") ? SC_BELOW
                    : !strcmp(w[4], "tree") ? (SC_SELF | SC_BELOW) : 0;
            bad = !r.scope;
        }
        if (!bad && n == 6) { r.once = !strcmp(w[5], "once"); bad = !r.once; }
        if (bad) {
            fprintf(stderr, "owners: %s:%d: not PATH OWNER GROUP MODE SCOPE [once]\n", file, lineno);
            fclose(f);
            return -1;
        }
        r.len = strlen(w[0]);
        while (r.len > 1 && w[0][r.len - 1] == '/') w[0][--r.len] = 0;
        r.path = strdup(w[0]);
        if (!r.path || add_rule(&r) < 0) { fclose(f); fprintf(stderr, "owners: out of memory\n"); return -1; }
    }
    fclose(f);
    return 0;
}

// The rule for `p`: the longest path that covers it, and of two at one
// path the `self` one for the path itself.
static const struct rule *rule_for(const char *p) {
    size_t pl = strlen(p);
    const struct rule *best = 0;
    size_t best_len = 0;
    int best_exact = 0;
    for (int i = 0; i < g_nrules; i++) {
        const struct rule *r = &g_rules[i];
        int exact = (r->scope & SC_SELF) && pl == r->len && !memcmp(p, r->path, pl);
        int below = (r->scope & SC_BELOW) && pl > r->len && !memcmp(p, r->path, r->len) &&
                    (r->len == 1 || p[r->len] == '/');
        if (!exact && !below) continue;
        if (!best || r->len > best_len || (r->len == best_len && exact && !best_exact)) {
            best = r;
            best_len = r->len;
            best_exact = exact;
        }
    }
    return best;
}

static void apply(const char *p) {
    const struct rule *r = rule_for(p);
    if (!r) return;
    g_seen++;
    struct stat st;
    if (stat(p, &st) != 0) return;
    int own = st.st_uid != r->uid || st.st_gid != r->gid;
    int want = r->add_dirs ? (S_ISDIR(st.st_mode) ? (int)(st.st_mode & 07777) | r->mode : -1) : r->mode;
    int mod = want >= 0 && (int)(st.st_mode & 07777) != want;
    if (!own && !mod) return;
    if (opt_dry || opt_verbose)
        printf("owners: %s%s -> %u:%u%s\n", opt_dry ? "would change " : "", p, r->uid, r->gid,
               mod ? " (and its mode)" : "");
    if (opt_dry) { g_changed++; return; }
    int bad = own && chown(p, r->uid, r->gid) != 0;
    if (!bad && mod) bad = chmod(p, (mode_t)want) != 0;
    if (bad) {
        if (g_failed++ < 8) fprintf(stderr, "owners: %s: %s\n", p, strerror(errno));
    } else {
        g_changed++;
    }
}

static int is_other_mount(const char *p) {
    for (int i = 0; i < g_nmounts; i++)
        if (!strcmp(g_mounts[i], p)) return 1;
    return 0;
}

static void walk(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    size_t dl = strlen(dir);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || e->d_type == DT_LNK) continue;
        size_t n = dl + 1 + strlen(e->d_name) + 1;
        char *p = malloc(n);
        if (!p) break;
        snprintf(p, n, "%s%s%s", dir, dl == 1 ? "" : "/", e->d_name);
        if (!is_other_mount(p)) {
            apply(p);
            if (e->d_type == DT_DIR) walk(p);
        }
        free(p);
    }
    closedir(d);
}

static void load_mounts(void) {
    struct query_fsinfo fs;
    for (int i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++) {
        if (!(fs.flags & QUERY_FS_MOUNTED) || !strcmp(fs.point, "/")) continue;
        char **nm = realloc(g_mounts, (size_t)(g_nmounts + 1) * sizeof *nm);
        if (!nm) return;
        g_mounts = nm;
        g_mounts[g_nmounts] = strdup(fs.point);
        if (g_mounts[g_nmounts]) g_nmounts++;
    }
}

static int marker_matches(uint32_t hash) {
    FILE *f = fopen(MARKER, "r");
    if (!f) return 0;
    unsigned long v = 0;
    int ok = fscanf(f, "%lx", &v) == 1 && (uint32_t)v == hash;
    fclose(f);
    return ok;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc) return uargs_error(&PROG, "extra operand '%s'", a.argv[0]);
    if (geteuid() != 0 && !opt_dry) {
        fprintf(stderr, "owners: only root can give files away (try sudo, or --dry-run)\n");
        return 1;
    }
    uint32_t hash;
    if (load(opt_config ? opt_config : "/etc/owners.conf", &hash) < 0) return 1;
    load_mounts();
    int all = opt_all || !marker_matches(hash);

    for (int i = 0; i < g_nrules; i++) {
        const struct rule *r = &g_rules[i];
        if (r->once && !all) continue;
        apply(r->path);
        if (r->scope & SC_BELOW) {
            // A rule's tree once: two rules at one path walk it once.
            int dup = 0;
            for (int j = 0; j < i && !dup; j++)
                dup = (g_rules[j].scope & SC_BELOW) && (!g_rules[j].once || all) &&
                      !strcmp(g_rules[j].path, r->path);
            if (!dup) walk(r->path);
        }
    }
    printf("owners: %u file(s) checked, %u changed%s, %u failed%s\n", g_seen, g_changed,
           opt_dry ? " (dry run)" : "", g_failed, all ? " -- every tree walked" : "");
    if (all && !opt_dry && !g_failed) {
        mkdir("/var/lib", 0755);
        FILE *f = fopen(MARKER, "w");
        if (f) { fprintf(f, "%08x\n", hash); fclose(f); }
    }
    return g_failed ? 1 : 0;
}
