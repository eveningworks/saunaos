// utf8migrate -- the one-time move of an old disk from Latin-1 to UTF-8
// (docs/utf8-design.md, step 3): every NAME on a writable TFS3 volume that
// is not valid UTF-8 is renamed to its UTF-8 spelling, and the CONTENTS of
// the text files under /etc, and of those under /home with a text
// extension, are converted the same way. Everything it does goes to
// /var/log/utf8migrate.log; /var/lib/utf8migrate marks it done.
//
// convmv's shape, with its safety rule and not its default: convmv only
// SHOWS what it would do until given --notest; this ACTS, because it runs
// once at boot as Debian's one-shot maintainer scripts do (the update that
// ships it runs the previous updater -- data/etc/services.d/bootmigrate),
// and --dry-run is the convmv default for a person at a prompt.
//
// WHAT MAKES A SECOND RUN HARMLESS: a name or file that is ALREADY valid
// UTF-8 is never touched, so --force over a converted tree changes
// nothing. The conversion keeps every valid sequence and turns each byte
// that starts none into its Latin-1 letter (kutf8_from_latin1()) -- the
// result spells exactly what the transition rule drew before.
//
// WHAT IT LEAVES ALONE, each logged:
//  * w16's drives (C:, and D:-Y: when mapped; /etc/win16.conf): their
//    files are Windows-1252 by contract and w16 hands names to a program
//    as bytes, so renaming one there would hide it from the program that
//    uses it -- names and contents both;
//  * a file with a NUL byte (binary) and one with more than one link (a
//    rewrite would split it from its other names);
//  * a name whose UTF-8 is past TFS3's 255 bytes, and one too long for a
//    listing to carry whole (struct sys_dirent);
//  * any mount that is not TFS3 or is read-only (/boot, /tmp, a share).
// A converted file keeps its mode; its times are new, as there is no call
// that sets them.
#include "rt/sys.h"
#include "lib/uargs.h"
#include "lib/ulistdir.h"
#include "lib/ufileop.h"
#include "lib/uconf.h"
#include "kutf8.h"
#include "query_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MARKER   "/var/lib/utf8migrate"
#define LOGFILE  "/var/log/utf8migrate.log"
#define TMPNAME  ".utf8migrate.tmp"
#define NAME_MAX_BYTES 255     // TFS3's, in bytes (FS_NAME_MAX)
#define DIRENT_NAME    64      // struct sys_dirent's name, NUL included

static int opt_dry, opt_force;
static const char *opt_root;

static const struct uargs_opt OPTS[] = {
    { "root", 0, "DIR", "work on the tree under DIR as if it were / (its etc, home, var)", 0, &opt_root },
    { "dry-run", 'n', 0, "list what would change; change nothing, write no log or marker", &opt_dry, 0 },
    { "force", 0, 0, "run even though the marker says it has run", &opt_force, 0 },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "utf8migrate",
    .usage = "[--root DIR] [--dry-run] [--force]",
    .summary = "Convert Latin-1 file names, and the text files under /etc and /home,\n"
               "to UTF-8 -- once. Runs at boot as a service; names and files already\n"
               "in UTF-8 are never touched.",
    .opts = OPTS,
    .notes = "Every change and every skip goes to /var/log/utf8migrate.log, and\n"
             "/var/lib/utf8migrate marks the conversion done.",
};

static char g_root[1024];       // "" for /, else the --root directory
static FILE *g_log;
static unsigned g_renamed, g_converted, g_skipped, g_failed;

// --- the mounts ---------------------------------------------------------

struct mnt { char point[64]; int ok; char fs[32]; };
static struct mnt *g_mnt;
static int g_nmnt;

static void load_mounts(void) {
    struct query_fsinfo fs;
    for (int i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++) {
        if (!(fs.flags & QUERY_FS_MOUNTED)) continue;
        struct mnt *m = realloc(g_mnt, sizeof *m * (size_t)(g_nmnt + 1));
        if (!m) return;
        g_mnt = m;
        m = &g_mnt[g_nmnt++];
        snprintf(m->point, sizeof m->point, "%s", fs.point);
        snprintf(m->fs, sizeof m->fs, "%s", fs.name);
        m->ok = !strcmp(fs.name, "tfs3") &&
                !(fs.flags & (QUERY_FS_RDONLY | QUERY_FS_REMOTE | QUERY_FS_GONE));
    }
}

// The mount whose point is exactly `path`, or NULL.
static const struct mnt *mount_at(const char *path) {
    for (int i = 0; i < g_nmnt; i++)
        if (!strcmp(g_mnt[i].point, path)) return &g_mnt[i];
    return 0;
}

// --- w16's drives ---------------------------------------------------------

static char **g_w16;            // real paths, UTF-8 spelling
static int g_nw16;

static void add_w16(const char *conf_path) {
    if (!conf_path[0] || conf_path[0] != '/') return;
    static char conv[1024], full[2048];
    if (kutf8_from_latin1(conf_path, -1, conv, sizeof conv) < 0) return;
    size_t n = strlen(conv);
    while (n > 1 && conv[n - 1] == '/') conv[--n] = 0;
    snprintf(full, sizeof full, "%s%s", g_root, conv);
    char **g = realloc(g_w16, sizeof *g * (size_t)(g_nw16 + 1));
    if (!g) return;
    g_w16 = g;
    g_w16[g_nw16++] = strdup(full);
}

// drive_c (default /home/Win16, w16prefs.c) and drive_d..drive_y from
// /etc/win16.conf, and every per-program drive_c in /etc/win16/.
static void load_w16(void) {
    static char conf[1100], v[1024];
    snprintf(conf, sizeof conf, "%s/etc/win16.conf", g_root);
    add_w16(uconf_get(conf, "drive_c", v, sizeof v) ? v : "/home/Win16");
    for (char d = 'd'; d <= 'y'; d++) {
        char key[] = "drive_?";
        key[6] = d;
        if (uconf_get(conf, key, v, sizeof v)) add_w16(v);
    }
    static char dir[1100];
    snprintf(dir, sizeof dir, "%s/etc/win16", g_root);
    struct sys_dirent *e;
    int n = ulistdir(dir, &e);
    for (int i = 0; i < n; i++) {
        snprintf(conf, sizeof conf, "%s/%s", dir, e[i].name);
        if (!e[i].is_dir && uconf_get(conf, "drive_c", v, sizeof v)) add_w16(v);
    }
    if (n > 0) free(e);
}

static int is_w16_drive(const char *path) {
    for (int i = 0; i < g_nw16; i++)
        if (!strcmp(g_w16[i], path)) return 1;
    return 0;
}

// --- the log --------------------------------------------------------------

// A path for the log: valid UTF-8 as itself, a stray byte as \xHH, so the
// log is itself UTF-8 and the old name can still be read.
static void put_path(FILE *f, const char *s) {
    for (int i = 0; s[i];) {
        int len;
        uint32_t cp = kutf8_decode(s + i, -1, &len);
        if (len == 1 && cp >= 0x80) fprintf(f, "\\x%02X", cp);
        else fwrite(s + i, 1, (size_t)len, f);
        i += len;
    }
}

// One line: "<what> <path>[ -> <to>][: <why>]" to the log, and to stdout
// on a dry run (where there is no log).
static void note(const char *what, const char *path, const char *to, const char *why) {
    FILE *outs[2] = { g_log, opt_dry ? stdout : 0 };
    for (int k = 0; k < 2; k++) {
        FILE *f = outs[k];
        if (!f) continue;
        fprintf(f, "%-8s", what);
        put_path(f, path + strlen(g_root));
        if (to) { fputs(" -> ", f); put_path(f, to + strlen(g_root)); }
        if (why) fprintf(f, ": %s", why);
        fputc('\n', f);
    }
}

static void skip(const char *path, const char *why) { g_skipped++; note("skip", path, 0, why); }

// --- contents -------------------------------------------------------------

static int has_text_ext(const char *name) {
    static const char *const EXT[] = { ".txt", ".md", ".conf", ".desktop", ".ini" };
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; i++) {
        size_t k = strlen(EXT[i]);
        if (n > k && !strcasecmp(name + n - k, EXT[i])) return 1;
    }
    return 0;
}

// Is the file at `rel` (its path below the root) one whose contents are
// converted? Everything under /etc; under /home, the text extensions.
static int wants_contents(const char *rel, const char *name) {
    if (!strncmp(rel, "/etc/", 5)) return 1;
    return !strncmp(rel, "/home/", 6) && has_text_ext(name);
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        long w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void convert_file(const char *dir, const char *path) {
    struct sys_stat st;
    if (sys_stat(path, &st) != 0) { g_failed++; note("fail", path, 0, "cannot stat it"); return; }
    if (st.size == 0) return;
    char *buf = malloc((size_t)st.size);
    if (!buf) { skip(path, "too large to read into memory"); return; }
    int fd = open(path, O_RDONLY);
    size_t got = 0;
    if (fd >= 0) {
        while (got < st.size) {
            long r = read(fd, buf + got, (size_t)(st.size - got));
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
    }
    if (fd < 0 || got != st.size) { free(buf); g_failed++; note("fail", path, 0, "cannot read it"); return; }
    if (kutf8_valid(buf, (int)got)) { free(buf); return; }     // already UTF-8: untouched
    if (memchr(buf, 0, got)) { free(buf); skip(path, "binary (it has a NUL byte)"); return; }
    if (st.nlink > 1) {
        char why[64];
        snprintf(why, sizeof why, "it has %u links, and a rewrite would split them", st.nlink);
        free(buf);
        skip(path, why);
        return;
    }
    size_t cap = got * 2 + 1;
    char *out = malloc(cap);
    int n = out ? kutf8_from_latin1(buf, (int)got, out, (int)cap) : -1;
    free(buf);
    if (n < 0) { free(out); skip(path, "too large to convert in memory"); return; }
    if (opt_dry) { g_converted++; note("convert", path, 0, 0); free(out); return; }

    // Atomically: a temporary beside it, flushed, then the replacing rename.
    static char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s/%s", dir, TMPNAME);
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, st.mode);
    int ok = fd >= 0 && write_all(fd, out, (size_t)n) == 0 && fsync(fd) >= 0;
    if (fd >= 0) close(fd);
    free(out);
    if (ok && (st.flags & SYS_STAT_MODE)) sys_chmod(tmp, st.mode);
    errno = EIO;
    if (!ok || sys_rename2(tmp, path, RENAME2_REPLACE) != 0) {
        int e = errno;
        unlink(tmp);
        g_failed++;
        note("fail", path, 0, e == ENOTSUP ? "no atomic replace on this filesystem"
                                           : "could not write the converted copy");
        return;
    }
    g_converted++;
    note("convert", path, 0, 0);
}

// --- the walk -------------------------------------------------------------

struct entry { char *name; int is_dir; };

static char *join(const char *dir, const char *name) {
    size_t n = strlen(dir) + strlen(name) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
    return p;
}

// Renames `e` if its name is not UTF-8; on return e->name is the name it
// has now. A clash takes ufileop_unique_name()'s " (1)", as a copy does.
static void fix_name(const char *dir, struct entry *e) {
    if (kutf8_valid(e->name, -1)) return;
    char *old = join(dir, e->name);
    if (!old) return;
    char conv[NAME_MAX_BYTES * 2 + 1];
    int n = kutf8_from_latin1(e->name, -1, conv, sizeof conv);
    if (n < 0 || n > NAME_MAX_BYTES) { skip(old, "its UTF-8 name would pass 255 bytes"); free(old); return; }
    char final[NAME_MAX_BYTES + 1];
    snprintf(final, sizeof final, "%s", conv);
    char *to = join(dir, final);
    struct sys_stat st;
    int clash = to && sys_stat(to, &st) == 0;
    if (clash) {
        free(to);
        to = 0;
        if (ufileop_unique_name(dir, conv, final, sizeof final) && strlen(final) <= NAME_MAX_BYTES)
            to = join(dir, final);
        if (!to) { g_failed++; note("fail", old, 0, "its UTF-8 name is taken, and no free name fits"); free(old); return; }
    }
    if (!opt_dry && sys_rename(old, to) != 0) {
        g_failed++;
        note("fail", old, to, "the rename was refused");
    } else {
        g_renamed++;
        note("rename", old, to, clash ? "the UTF-8 name was taken" : 0);
        if (!opt_dry) { free(e->name); e->name = strdup(final); }
    }
    free(old);
    free(to);
}

static void walk(const char *dir) {
    struct sys_dirent *de;
    int n = ulistdir(dir, &de);
    if (n < 0) { g_failed++; note("fail", dir, 0, "cannot list it"); return; }
    // COLLECT FIRST, then rename: renaming while the directory is being
    // listed can repeat or skip an entry (readdir()'s hazard).
    struct entry *e = n ? calloc((size_t)n, sizeof *e) : 0;
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(de[i].name, TMPNAME)) continue;
        e[m].name = strdup(de[i].name);
        e[m].is_dir = (int)de[i].is_dir;
        if (e[m].name) m++;
    }
    free(de);

    for (int i = 0; i < m; i++) {
        // A name a listing had to cut cannot be named back.
        if (strlen(e[i].name) >= DIRENT_NAME - 1) {
            char *p = join(dir, e[i].name);
            struct sys_stat st;
            if (p && sys_stat(p, &st) != 0) {
                skip(p, "the name is longer than a listing carries");
                free(e[i].name);
                e[i].name = 0;
            }
            free(p);
        }
        if (e[i].name) fix_name(dir, &e[i]);
    }

    for (int i = 0; i < m; i++) {
        if (!e[i].name) continue;
        char *p = join(dir, e[i].name);
        if (!p) continue;
        const char *prel = p + strlen(g_root);
        if (e[i].is_dir) {
            const struct mnt *mt = mount_at(p);
            if (is_w16_drive(p)) skip(p, "w16's drive -- Windows-1252 by contract");
            else if (mt && !mt->ok) note("leave", p, 0, mt->fs);
            else walk(p);
        } else if (wants_contents(prel, e[i].name)) {
            convert_file(dir, p);
        }
        free(p);
    }
    for (int i = 0; i < m; i++) free(e[i].name);
    free(e);
}

// --- main -----------------------------------------------------------------

static void stamp(char *out, size_t cap) {
    time_t t = time(0);
    struct tm tm;
    if (!localtime_r(&t, &tm) || !strftime(out, cap, "%Y-%m-%d %H:%M", &tm))
        snprintf(out, cap, "(no clock)");
}

static void mkdir_p(const char *path) {
    static char p[1100];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc) return uargs_error(&PROG, "unexpected '%s'", a.argv[0]);

    if (opt_root) {
        snprintf(g_root, sizeof g_root, "%s", opt_root);
        size_t n = strlen(g_root);
        while (n > 0 && g_root[n - 1] == '/') g_root[--n] = 0;
        struct sys_stat st;
        if (sys_stat(opt_root, &st) != 0 || !st.is_dir) {
            fprintf(stderr, "utf8migrate: %s: not a directory\n", opt_root);
            return 1;
        }
    }
    static char marker[1100], logp[1100], top[1100];
    snprintf(marker, sizeof marker, "%s%s", g_root, MARKER);
    snprintf(logp, sizeof logp, "%s%s", g_root, LOGFILE);
    snprintf(top, sizeof top, "%s", g_root[0] ? g_root : "/");

    struct sys_stat st;
    if (!opt_force && !opt_dry && sys_stat(marker, &st) == 0) {
        char line[128] = "";
        FILE *f = fopen(marker, "r");
        if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
        line[strcspn(line, "\n")] = 0;
        fprintf(stderr, "utf8migrate: nothing to do -- %s\n", line[0] ? line : "already converted");
        return 0;
    }

    load_mounts();
    // The volume the walk starts on: the mount whose point is the longest
    // prefix of `top`.
    const struct mnt *home = 0;
    for (int i = 0; i < g_nmnt; i++) {
        size_t k = strlen(g_mnt[i].point);
        int under = !strcmp(g_mnt[i].point, "/") ||
                    (!strncmp(top, g_mnt[i].point, k) && (top[k] == '/' || !top[k]));
        if (under && (!home || k > strlen(home->point))) home = &g_mnt[i];
    }
    if (home && !home->ok) {
        fprintf(stderr, "utf8migrate: nothing to do -- %s is %s, not a writable TFS3 volume\n",
                home->point, home->fs);
        return 0;
    }
    load_w16();

    char when[32];
    stamp(when, sizeof when);
    if (!opt_dry) {
        static char d[1100];
        snprintf(d, sizeof d, "%s/var/log", g_root);
        mkdir_p(d);
        g_log = fopen(logp, "a");
        if (!g_log) fprintf(stderr, "utf8migrate: cannot write %s -- converting without a log\n", logp);
        else fprintf(g_log, "utf8migrate %s, root %s\n", when, top);
        fprintf(stderr, "utf8migrate: converting Latin-1 names and text files to UTF-8, once\n");
    }

    walk(top);

    char sum[160];
    snprintf(sum, sizeof sum, "%u renamed, %u converted, %u skipped, %u failed",
             g_renamed, g_converted, g_skipped, g_failed);
    if (opt_dry) {
        printf("utf8migrate: would do: %s (dry run)\n", sum);
        return 0;
    }
    if (g_log) { fprintf(g_log, "done: %s\n", sum); fclose(g_log); }
    static char d[1100];
    snprintf(d, sizeof d, "%s/var/lib", g_root);
    mkdir_p(d);
    FILE *f = fopen(marker, "w");
    if (f) {
        fprintf(f, "converted %s: %s\n", when, sum);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    fprintf(stderr, "utf8migrate: %s -- see %s\n", sum, LOGFILE);
    if (!opt_root) printf("utf8migrate: %s\n", sum);
    return g_failed ? 1 : 0;
}
