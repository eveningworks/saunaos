// crashlog -- the ring-3 crash reports in /var/crash: list them, or
// print one: a summary, the BACKTRACE recovered from its saved stack
// (base/ucrash.h -- the same reading the Crash Reports window shows), and
// its text header. The kernel writes a report on the way to killing a
// process that faulted (kernel/proc/crash_report.c); the raw stack is
// never printed, and tools/lib/panic_resolve.py --crash on the host adds
// source lines from DWARF.
//
// A KERNEL PANIC IS NOT IN THAT LIST. /var/crash holds processes that
// faulted while the kernel carried on; a panic is the system failing,
// so `--panics` is a list of its own: the dead boots whose log logd
// appended the panic store's record to (kernel/panic_store.h).
#include <stdint.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "base/ulistdir.h"
#include "base/cmd.h"
#include "base/dirsort.h"
#include "base/ucrash.h"
#include "base/uargs.h"
#include "base/ufile.h"
#include <string.h>
#include <stdio.h>

#define CRASH_DIR "/var/crash"
#define BOOT_DIR  "/var/log/boot"
// logd's line above a filed record (userland/services/logd/logd.c, file_panic()).
#define PANIC_MARK "logd: THIS BOOT PANICKED"

static int g_panics;
static const struct uargs_opt OPTS[] = {
    { "panics", 0, 0, "list the boots that panicked, or with BOOT show that one's last kernel lines", &g_panics, 0 },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "crashlog",
    .usage = "[REPORT]\n--panics [BOOT]",
    .summary = "List the crash reports in /var/crash, or print one; --panics does the same for "
               "kernel panics kept in the dead boots' logs.",
    .opts = OPTS,
};

static int list(void) {
    // WHOLE, or the newest report could be one the listing left out.
    struct ulist_entry *ents = 0;
    int n = ulistdir(CRASH_DIR, &ents);
    if (n < 0) {
        sys_print("crashlog: no reports (" CRASH_DIR " does not exist)\n");
        return 0;
    }
    // NEWEST FIRST, unlike the other listings here: the report anyone
    // wants is the one from the crash that just happened. dirsort's name
    // tie-break keeps it deterministic if two share a timestamp.
    dirsort(ents, n, DIRSORT_TIME, 0);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (ents[i].is_dir) continue;
        char line[128];
        snprintf(line, sizeof line, "%-40s %8u bytes\n", ents[i].name, (unsigned)ents[i].size);
        sys_print(line);
        shown++;
    }
    free(ents);
    if (!shown) sys_print("crashlog: no reports\n");
    return 0;
}

static int show(const char *name) {
    char path[64];
    if (name[0] == '/') snprintf(path, sizeof path, "%s", name);
    else                snprintf(path, sizeof path, CRASH_DIR "/%s", name);
    int fd = sys_open(path, 0);
    if (fd < 0) { cmd_fail("crashlog", path); return 1; }
    // Read up to the stack marker and no further: what follows is binary.
    static char buf[16384];
    int total = 0;
    while (total < (int)sizeof buf - 1) {
        int n = sys_read(fd, buf + total, (int)sizeof buf - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    sys_close(fd);
    buf[total] = 0;
    char *mark = strstr(buf, "---- stack ----");
    if (mark) *mark = 0;
    static struct ucrash r;
    if (ucrash_load(&r, path) == 0) {
        char line[192], where[96], by[24];
        ucrash_backtrace(&r);
        ucrash_where(&r, where, sizeof where);
        ucrash_sender(&r, by, sizeof by);
        snprintf(line, sizeof line, "%s (pid %d): %s%s%s\nwhere: %s\n\nbacktrace, found by scanning the stack:\n",
                 r.program, r.pid, r.fault, by[0] ? ", sent by " : "", by, where);
        if (r.message[0]) {
            sys_print("message:\n");
            for (const char *m = r.message; *m; ) {
                const char *nl = strchr(m, '\n');
                int n = nl ? (int)(nl - m) : (int)strlen(m);
                char ml[UCRASH_MESSAGE + 4];
                snprintf(ml, sizeof ml, "  %.*s\n", n, m);
                sys_print(ml);
                m += n + (nl ? 1 : 0);
            }
            sys_print("\n");
        }
        sys_print(line);
        for (int i = 0; i < r.nframe; i++) {
            const struct ucrash_frame *f = &r.frame[i];
            if (f->func[0])
                snprintf(line, sizeof line, "  #%-2d 0x%llx  %s +0x%llx  (%s)\n", i,
                         (unsigned long long)f->addr, f->func, (unsigned long long)f->off, f->module);
            else
                snprintf(line, sizeof line, "  #%-2d 0x%llx  %s +0x%llx\n", i,
                         (unsigned long long)f->addr, f->module, (unsigned long long)f->off);
            sys_print(line);
        }
        if (!r.nframe) sys_print("  (none: no code addresses on the saved stack)\n");
        if (r.stale) sys_print("  ** a binary is newer than this report: the names may be wrong **\n");
        sys_print("\n");
    }
    sys_print(buf);
    return 0;
}

// A boot's log, whole (a dead boot's file is bounded by logd's size cap).
static char *boot_log(const char *name) {
    char path[96];
    snprintf(path, sizeof path, BOOT_DIR "/%s", name);
    uint8_t *buf;
    size_t len;
    if (ufile_slurp(path, 0, &buf, &len) != UFILE_OK) return 0;
    char *t = realloc(buf, len + 1);
    if (!t) { free(buf); return 0; }
    // A BOOT THAT DIED MID-WRITE LEAVES NUL BYTES in its file, ahead of the
    // record logd appended -- a string search would stop at the first.
    for (size_t i = 0; i < len; i++)
        if (!t[i]) t[i] = '?';
    t[len] = 0;
    return t;
}

// The line holding `at`, copied into `out` without its newline.
static void line_at(const char *text, const char *at, char *out, size_t cap) {
    while (at > text && at[-1] != '\n') at--;
    const char *nl = strchr(at, '\n');
    snprintf(out, cap, "%.*s", nl ? (int)(nl - at) : (int)strlen(at), at);
}

static int boot_number(const char *name) {
    int n = 0, digits = 0;
    for (; *name >= '0' && *name <= '9'; name++, digits++) n = n * 10 + (*name - '0');
    return digits && !strcmp(name, ".log") ? n : -1;
}

static int panics_list(void) {
    struct ulist_entry *ents = 0;
    int n = ulistdir(BOOT_DIR, &ents);
    if (n < 0) { sys_print("crashlog: no boot logs (" BOOT_DIR " does not exist)\n"); return 0; }
    dirsort(ents, n, DIRSORT_TIME, 0);   // newest first, as the report list
    int shown = 0, read = 0, unread = 0;
    for (int i = 0; i < n; i++) {
        int boot = boot_number(ents[i].name);
        if (ents[i].is_dir || boot < 0) continue;
        char *t = boot_log(ents[i].name);
        if (t) read++; else unread++;
        const char *mark = t ? strstr(t, PANIC_MARK) : 0;
        if (mark) {
            const char *what = strstr(mark, "PANIC:");
            char why[120] = "(no PANIC: line in the record)";
            if (what) line_at(mark, what, why, sizeof why);
            const char *w = strstr(why, "PANIC:");
            char row[200];
            snprintf(row, sizeof row, "boot %-5d %s\n", boot, w ? w : why);
            sys_print(row);
            shown++;
        }
        free(t);
    }
    free(ents);
    if (!shown) {
        char msg[128];
        snprintf(msg, sizeof msg, "crashlog: no boot in " BOOT_DIR " panicked (%d log(s) read%s)\n",
                 read, unread ? ", some could not be" : "");
        sys_print(msg);
    }
    return 0;
}

// The file for boot `boot` -- logd zero-pads the number (0099.log).
static int find_boot(int boot, char *name, size_t cap) {
    struct ulist_entry *ents = 0;
    int n = ulistdir(BOOT_DIR, &ents), found = 0;
    for (int i = 0; i < n && !found; i++)
        if (!ents[i].is_dir && boot_number(ents[i].name) == boot) {
            snprintf(name, cap, "%s", ents[i].name);
            found = 1;
        }
    free(ents);
    return found;
}

static int panics_show(const char *arg) {
    char name[64];
    int boot = boot_number(arg);
    if (boot < 0) {
        snprintf(name, sizeof name, "%s.log", arg);
        boot = boot_number(name);
    }
    if (boot < 0) return uargs_error(&PROG, "'%s' is not a boot number", arg);
    if (!find_boot(boot, name, sizeof name)) {
        char msg[96];
        snprintf(msg, sizeof msg, "crashlog: no log for boot %d in " BOOT_DIR " (log --list)\n", boot);
        sys_print(msg);
        return 1;
    }
    char *t = boot_log(name);
    if (!t) { cmd_fail("crashlog", BOOT_DIR); return 1; }
    const char *mark = strstr(t, PANIC_MARK);
    if (!mark) {
        char msg[96];
        snprintf(msg, sizeof msg, "crashlog: boot %d did not panic (log -p reads its log)\n", boot);
        sys_print(msg);
        free(t);
        return 1;
    }
    while (mark > t && mark[-1] != '\n') mark--;
    sys_print(mark);
    free(t);
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc > 1) return uargs_error(&PROG, "takes at most one argument");
    if (g_panics) return a.argc ? panics_show(a.argv[0]) : panics_list();
    return a.argc ? show(a.argv[0]) : list();
}
