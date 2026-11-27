// /bin/ls -- list a directory.
//
// A real ring-3 program over SYS_LISTDIR, taking ordinary C argc/argv.
// argv[0] is whatever path the shell invoked it as; argv[1..] are flags
// and at most one positional directory path.
//
// WITH NO ARGUMENT IT LISTS THE CURRENT DIRECTORY, which is what every
// ls does and what this one did not: it defaulted to `/`, so a bare
// `ls` in /docs listed the root. Nothing noticed because the kernel
// shell had a BUILTIN WRAPPER whose only remaining job was to resolve
// the cwd and pass it in -- one command with two halves in two rings,
// where the ring-3 half was wrong on its own and could not be run on
// its own. Fixing the default deleted the wrapper.
//
// It asks SYS_GETCWD rather than passing "." down, deliberately. The
// cwd is the KERNEL's and every path syscall already resolves against
// it, so "." would work for the listing itself -- but the path is also
// printed in -R headers and joined onto child names, and "./docs:" is
// not what a reader wants where "/docs:" was. Resolving once, up
// front, keeps every later use absolute.
//
// COLOUR IS AN ESCAPE SEQUENCE NOW, NOT A SYSCALL. This used to call
// sys_set_color(), which reaches around the byte stream and changes the
// console's state directly -- so `ls > out.txt` recoloured the console
// while its bytes went to the file, and `ls | cat` coloured whatever the
// console was printing instead. `ESC[36m` travels IN the stream and
// lands wherever the output lands, which is what every real terminal
// does and what makes `--color=never` a thing worth offering.
//
// `--color=auto` IS THE DEFAULT, and it is what a terminal being a real
// object bought. "Colour to a terminal, plain to a pipe" needs to know
// whether fd 1 IS one; that used to be unanswerable here and is now one
// call (sys_isatty(), over SYS_FSTAT's SYS_STAT_TTY). So `ls` on a
// terminal is coloured, `ls > out.txt` and `ls | cat` are not, and
// nobody has to remember a flag -- which is the whole reason real ls
// defaults to auto. `never` and `always` override it.
//
// WHAT IS NOT COLOURED: executables. Real ls colours them from the mode
// bits, which a listing does not carry (struct ulist_entry: a name, a
// size, is_dir and a timestamp) -- only -l stats each entry for them.
//
// -l IS GNU's LONG FORM LESS THE LINK COUNT: mode, owner, group, size,
// time, name. Tests read it through tools/lib/ls_long.py, one parser.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include "base/dirsort.h"
#include "base/ulistdir.h"
#include <stdlib.h>
#include "base/udate.h"
#include "base/unum.h"
#include "base/uargs.h"
#include "base/upath.h"  // upath_join -- the -R queue
#include <locale.h>
#include "kpath.h"    // k_path_basename, for a path that names a file
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

// The output flags, gathered so the recursion below can pass one thing.
struct opts {
    int long_form;  // -l
    int columns;    // -C -- multi-column; one per line otherwise
    int human;      // -h
    int recurse;    // -R
    int reverse;    // -r
    int sort;       // 'n' name (default), 't' time, 'S' size
    int color;
};

static void put(const char *s) { write(1, s, strlen(s)); }

// ---- colour ----------------------------------------------------------
//
// Only two, deliberately: see this file's top comment on why an
// executable cannot be told apart from a plain file here.
#define C_DIR   "\033[1;36m" // bright cyan
#define C_RESET "\033[0m"

static void put_name(const struct opts *o, const struct ulist_entry *e) {
    if (o->color && e->is_dir) put(C_DIR);
    put(e->name);
    if (o->color && e->is_dir) put(C_RESET);
    // The trailing slash is NOT gated on colour: it is the one piece of
    // type information that survives being piped into another program,
    // which is exactly when colour has been turned off.
    if (e->is_dir) put("/");
}

// ---- sizes -----------------------------------------------------------

// 1.2K / 4.0M, one decimal, the coreutils `-h` shape. Integer only --
// this kernel has no floating point (-mno-sse), so the tenth is
// computed from the remainder rather than by dividing.
static void put_human(uint32_t n) {
    static const char unit[] = { 'B', 'K', 'M', 'G' };
    int u = 0;
    uint32_t whole = n, rem = 0;
    while (whole >= 1024 && u < 3) {
        rem = whole % 1024;
        whole /= 1024;
        u++;
    }
    char buf[16];
    if (u == 0) snprintf(buf, sizeof buf, "%u", whole);
    else if (whole >= 10) snprintf(buf, sizeof buf, "%u%c", whole, unit[u]);
    else snprintf(buf, sizeof buf, "%u.%u%c", whole, (rem * 10) / 1024, unit[u]);
    unum_localize(buf, sizeof buf, 0);

    // Right-aligned in a fixed field, like the plain size column.
    for (int i = (int)strlen(buf); i < 6; i++) put(" ");
    put(buf);
}

static void put_size(const struct opts *o, uint32_t n) {
    if (o->human) { put_human(n); return; }
    char buf[16];
    snprintf(buf, sizeof buf, "%u", n);
    for (int i = (int)strlen(buf); i < 10; i++) put(" ");
    put(buf);
}

// In the locale's spelling, PADDED to the widest date it can write --
// "1.1.2026" and "31.12.2026" differ in width, and the name column
// after it must not move from row to row.
static void put_timestamp(const struct rtc_time *t) {
    static int width = -1;
    char buf[48];
    if (width < 0) {
        struct tm widest = { .tm_sec = 59, .tm_min = 59, .tm_hour = 22, .tm_mday = 28,
                             .tm_mon = 11, .tm_year = 100, .tm_wday = 4 };
        udate_format_tm(buf, sizeof buf, &widest, UDATE_DATE | UDATE_TIME | UDATE_SECONDS);
        width = (int)strlen(buf);
    }
    udate_format(buf, sizeof buf, t, UDATE_DATE | UDATE_TIME | UDATE_SECONDS);
    put(buf);
    for (int i = (int)strlen(buf); i < width; i++) put(" ");
}

// ---- the long form's mode and owner ---------------------------------

// What -l prints before the size, from one SYS_STATX per entry.
struct lmeta {
    int ok;
    uint32_t mode, uid, gid;
};

// `drwsr-xr-x`: the type, then rwx thrice, with setuid/setgid/sticky in
// the x places (s/S, t/T: upper case when the x under it is not set).
static void mode_str(char out[11], int is_dir, uint32_t m) {
    static const char rwx[] = "rwxrwxrwx";
    out[0] = is_dir ? 'd' : '-';
    for (int i = 0; i < 9; i++) out[1 + i] = (m & (0400u >> i)) ? rwx[i] : '-';
    if (m & 04000) out[3] = (m & 0100) ? 's' : 'S';
    if (m & 02000) out[6] = (m & 0010) ? 's' : 'S';
    if (m & 01000) out[9] = (m & 0001) ? 't' : 'T';
    out[10] = '\0';
}

// An account's name, or its number when it has none. The last answer is
// kept: a folder's entries are mostly one owner's, and getpwuid() reads
// /etc/passwd each time.
static const char *owner_name(uint32_t uid, int group) {
    static uint32_t last[2] = { (uint32_t)-1, (uint32_t)-1 };
    static char name[2][64];
    if (last[group] == uid) return name[group];
    const char *n = 0;
    if (group) { struct group *g = getgrgid(uid); n = g ? g->gr_name : 0; }
    else { struct passwd *pw = getpwuid(uid); n = pw ? pw->pw_name : 0; }
    if (n && strlen(n) < sizeof name[0]) strlcpy(name[group], n, sizeof name[group]);
    else snprintf(name[group], sizeof name[group], "%u", (unsigned)uid);
    last[group] = uid;
    return name[group];
}

static void put_padded(const char *s, int width) {
    put(s);
    for (int i = (int)strlen(s); i < width; i++) put(" ");
}

// Each entry's mode and owner, and the owner and group columns' widths.
// NULL when it cannot be allocated: the long form then prints without them.
static struct lmeta *long_meta(const char *path, const struct ulist_entry *ents, int count,
                               int one, int *ow, int *gw) {
    struct lmeta *m = calloc(count ? (size_t)count : 1, sizeof *m);
    *ow = *gw = 1;
    if (!m) return 0;
    for (int i = 0; i < count; i++) {
        char *full = one ? 0 : upath_join(path, ents[i].name);
        struct sys_statx sx;
        if ((one || full) && sys_statx(one ? path : full, &sx) == 0) {
            m[i].ok = 1;
            m[i].mode = sx.st.mode & 07777;
            m[i].uid = sx.uid;
            m[i].gid = sx.gid;
            int a = (int)strlen(owner_name(sx.uid, 0)), b = (int)strlen(owner_name(sx.gid, 1));
            if (a > *ow) *ow = a;
            if (b > *gw) *gw = b;
        }
        free(full);
    }
    return m;
}

// ---- ordering --------------------------------------------------------
//
// Sorting lives in userland/libs/base/dirsort.c, NOT here, because Notepad's
// file dialog has to produce the same order -- see that header. Default
// is BY NAME, which is new: this used to print whatever order fs_list()
// happened to walk the directory in, so the same directory could list
// differently on two machines.

// ---- one directory ---------------------------------------------------

// -R's work list. Breadth-first, so one directory's listing is freed
// before the next is read rather than one held per level. Each entry is
// a heap path and the list grows to the tree: neither a deep path nor a
// wide tree is cut. A push that cannot get memory is counted and said.
static char **g_queue;
static int g_qhead, g_qtail, g_qcap, g_qdropped;

static void queue_push(const char *dir, const char *name) {
    if (g_qtail == g_qcap) {
        int cap = g_qcap ? g_qcap * 2 : 64;
        char **q = realloc(g_queue, (size_t)cap * sizeof *q);
        if (!q) { g_qdropped++; return; }
        g_queue = q;
        g_qcap = cap;
    }
    char *path = upath_join(dir, name);
    if (!path) { g_qdropped++; return; }
    g_queue[g_qtail++] = path;
}

// Lists one directory. Returns 0 on success, 1 if the path could not be
// read at all.
// A PATH THAT IS A FILE IS NOT AN ERROR, it is a listing of one. Real
// ls stats a non-directory argument and prints that single entry, and
// this did not: ulistdir() answers ENOTDIR for a file, which came
// out as `ls: cannot access '/bin/ls': not a directory` -- for a file
// that plainly exists. Found on the bare-metal laptop while checking
// the size of a binary that had just been copied there, where the one
// command anybody reaches for reported the file missing.
//
// Synthesised into a dirent so there is ONE printing path: the columns,
// the sort, the colouring and -l all keep working without knowing that
// this entry did not come from a directory read.
static int stat_one(const char *path, struct ulist_entry *out) {
    struct sys_stat st;
    if (sys_stat(path, &st) < 0) return 0;
    memset(out, 0, sizeof *out);
    strlcpy(out->name, k_path_basename(path), sizeof out->name);
    out->size = (uint32_t)st.size;
    out->is_dir = st.is_dir;
    out->modified = st.modified;
    return 1;
}

static int list_one(const struct opts *o, const char *path, int with_header) {
    struct ulist_entry *ents = 0, one;
    int count = ulistdir(path, &ents);
    if (count < 0) {
        if (stat_one(path, &one)) {
            ents = &one;
            count = 1;
        } else {
            put("ls: cannot access '");
            put(path);
            put("': ");
            put(strerror(sys_errno()));
            put("\n");
            return 1;
        }
    }

    if (with_header) { put("\n"); put(path); put(":\n"); }

    dirsort(ents, count,
            o->sort == 'S' ? DIRSORT_SIZE : o->sort == 't' ? DIRSORT_TIME : DIRSORT_NAME,
            o->reverse);

    // After the sort, so each entry's mode stays beside it.
    int ow = 1, gw = 1;
    struct lmeta *meta = o->long_form ? long_meta(path, ents, count, ents == &one, &ow, &gw) : 0;

    for (int i = 0; i < count; i++) {
        struct ulist_entry *e = &ents[i];

        if (o->recurse && e->is_dir) queue_push(path, e->name);

        if (o->long_form) {
            char ms[11];
            if (meta && meta[i].ok) {
                mode_str(ms, e->is_dir, meta[i].mode);
                put(ms); put(" ");
                put_padded(owner_name(meta[i].uid, 0), ow); put(" ");
                put_padded(owner_name(meta[i].gid, 1), gw); put(" ");
            } else {
                put(e->is_dir ? "d?????????" : "-?????????"); put(" ");
                put_padded("?", ow); put(" ");
                put_padded("?", gw); put(" ");
            }
            if (e->is_dir) put(o->human ? "      " : "          ");
            else put_size(o, e->size);
            put(" ");
            put_timestamp(&e->modified);
            put("  ");
        }
        put_name(o, e);
        // ONE PER LINE BY DEFAULT, columns only when asked. Real ls
        // columnises to a TERMINAL and prints one per line when its
        // output is a pipe -- and with no isatty() here (see this file's
        // top comment) it cannot tell, so the default is the one that is
        // safe to parse. Making columns the default broke Notepad's
        // dialog test, which reads `ls /` a line at a time, the moment
        // it was tried.
        put(o->columns && !o->long_form ? "  " : "\n");
    }
    if (o->columns && !o->long_form && count > 0) put("\n");
    free(meta);
    if (ents != &one) free(ents);
    return 0;
}

static int f_one, f_cols, f_long, f_human, f_recurse, f_reverse, f_time, f_size, f_all, f_classify;
static const char *f_color;

static const struct uargs_opt OPTS[] = {
    { 0, '1', 0, "one name per line (the default)", &f_one, 0 },
    { 0, 'C', 0, "names in columns; ignored with -l", &f_cols, 0 },
    { 0, 'l', 0, "long form: mode, owner, group, size and modification time", &f_long, 0 },
    { 0, 'h', 0, "sizes as 4.0K, 12M rather than bytes", &f_human, 0 },
    { 0, 't', 0, "newest first", &f_time, 0 },
    { 0, 'S', 0, "largest first", &f_size, 0 },
    { 0, 'r', 0, "reverse the order", &f_reverse, 0 },
    { 0, 'R', 0, "list subdirectories too", &f_recurse, 0 },
    { 0, 'a', 0, "accepted and ignored: there are no hidden files", &f_all, 0 },
    { 0, 'F', 0, "accepted and ignored: a directory always ends in /", &f_classify, 0 },
    { "color", 0, "WHEN", "never, always, or auto (colour on a terminal)", 0, &f_color },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "ls",
    .usage = "[OPTION]... [DIR]",
    .summary = "List a directory, the current one by default, sorted by name.",
    .opts = OPTS,
};

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    // The last field is `color`, and its default is AUTO -- resolved
    // here rather than carried as a third state, because everything
    // below only ever asks "colour or not".
    struct opts o = { 0, 0, 0, 0, 0, 'n', sys_isatty(1) };
    // The default, if no positional argument arrives. `/` is the
    // fallback for a caller with no scheduler slot: SYS_GETCWD needs a
    // process, and the legacy `run` loader is not one (see its ABI
    // comment), so a bare `ls` under `run` still lists something rather
    // than failing.
    char *cwd = getcwd(0, 0);   // sized to fit; lives until exit
    const char *path = cwd && cwd[0] ? cwd : "/";
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) sys_exit(a.status);
    if (a.argc > 1) sys_exit(uargs_error(&PROG, "only one directory at a time"));
    if (a.argc == 1) path = a.argv[0];
    // Of each pair, the one typed LAST wins (a flag holds its position).
    o.long_form = f_long != 0;
    o.columns = f_cols > f_one;
    o.human = f_human != 0;
    o.recurse = f_recurse != 0;
    o.reverse = f_reverse != 0;
    if (f_time || f_size) o.sort = f_time > f_size ? 't' : 'S';
    if (f_color) {
        if (!strcmp(f_color, "never")) o.color = 0;
        else if (!strcmp(f_color, "always")) o.color = 1;
        else if (!strcmp(f_color, "auto")) o.color = sys_isatty(1);
        else sys_exit(uargs_error(&PROG, "--color takes never, always or auto, not '%s'", f_color));
    }

    int rc = list_one(&o, path, 0);

    // -R, iteratively. Each directory found is appended and processed in
    // turn, and freed once listed.
    while (o.recurse && g_qhead < g_qtail) {
        char *next = g_queue[g_qhead];
        g_queue[g_qhead++] = 0;
        if (list_one(&o, next, 1)) rc = 1;
        free(next);
    }
    if (g_qdropped) {
        put("ls: out of memory -- some subdirectories were not listed\n");
        rc = 1;
    }

    sys_exit(rc);
}
