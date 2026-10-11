// smb -- read a Windows or Samba share from the command line: list a
// folder, copy a file out, print one, or say what the server agreed to.
//
// The protocol is /lib/libsmb.so (<usmb.h>); this is the front end. A
// share you use often is better MOUNTED (/etc/mounts.d, smbfsd), where
// every program sees its files; this is for a look, a one-off copy, and
// for telling a sign-in problem from a mount problem.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <locale.h>
#include <time.h>
#include <unistd.h>

#include "rt/sys.h"
#include "tty_abi.h"
#include "lib/uargs.h"
#include "lib/udate.h"
#include "lib/human.h"
#include "usmb.h"
#include "userfs_abi.h"
#include "lib/unetmount.h"

static const char *f_user, *f_password, *f_domain, *f_timeout;
static int f_remember;

static const struct uargs_opt OPTS[] = {
    { "user",     'u', "USER", "sign in as USER (\"guest\" for the guest account; none: anonymous)", 0, &f_user },
    { "password", 'p', "PW",   "the password; asked for when -u is given without it", 0, &f_password },
    { "domain",   'W', "DOM",  "the domain or workgroup (default: the server's own)", 0, &f_domain },
    { "timeout",  't', "SECS", "how long to wait for each answer (default 10)", 0, &f_timeout },
    { "remember", 'r', 0,      "login: save the password (as a hash) for the next boot", &f_remember, 0 },
    { 0 },
};

static const struct uargs_cmd CMDS[] = {
    { "ls",   "URL",        "list a folder" },
    { "get",  "URL [FILE]", "copy a file here, as FILE or under its own name" },
    { "cat",  "URL",        "print a file" },
    { "stat", "URL",        "one file's or folder's size and dates" },
    { "info", "URL",        "what the server agreed to, and the share's size" },
    { "login", "NAME",      "sign in to a mounted share: /net/NAME, or any mount point" },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "smb",
    .usage = "[OPTION]... COMMAND //SERVER/SHARE[/PATH] [FILE]\n-u USER [-r] login NAME",
    .summary = "Read files from a Windows or Samba share (SMB 2 and 3, read-only).",
    .opts = OPTS,
    .cmds = CMDS,
    .notes = "A URL is //server/share/path, \\\\server\\share\\path or smb://server:port/share/path.\n"
             "The server is a name or an address; the port defaults to 445.",
};

// Read a password from the terminal with echo off.
static void ask_password(char *out, size_t cap) {
    struct tty_termios saved, quiet;
    int tty = sys_tcgetattr(0, &saved) == 0;
    if (tty) {
        quiet = saved;
        quiet.lflag &= ~(uint32_t)TTY_ECHO;
        sys_tcsetattr(0, &quiet);
    }
    fputs("Password: ", stdout);
    fflush(stdout);
    size_t n = 0;
    char c;
    while (n + 1 < cap && read(0, &c, 1) == 1 && c != '\n' && c != '\r') out[n++] = c;
    out[n] = '\0';
    if (tty) { sys_tcsetattr(0, &saved); fputs("\n", stdout); }
}

static void when(char *out, size_t cap, uint64_t t) {
    time_t e = (time_t)t;
    struct tm tm;
    localtime_r(&e, &tm);
    udate_format_tm(out, cap, &tm, UDATE_DATE | UDATE_TIME);
}

static int print_entry(void *ctx, const char *name, const struct usmb_stat *st) {
    (void)ctx;
    char size[16], date[40];
    if (st->is_dir) snprintf(size, sizeof size, "-");
    else human_size(size, sizeof size, st->size);
    when(date, sizeof date, st->modified);
    printf("%8s  %-16s  %s%s\n", size, date, name, st->is_dir ? "/" : "");
    return 0;
}

static int fail(const char *what, int rc) {
    fprintf(stderr, "smb: %s: %s\n", what, usmb_strerror(rc));
    return 1;
}

// Copy `path` to `fd`.
static int copy_out(struct usmb *s, const char *path, int fd) {
    struct usmb_file f;
    int rc = usmb_open_file(s, path, &f);
    if (rc) return fail(path, rc);
    char *buf = malloc(64 * 1024);
    if (!buf) { usmb_close_file(s, &f); return fail(path, USMB_E_NOMEM); }
    int status = 0;
    for (uint64_t off = 0; off < f.size;) {
        int64_t n = usmb_read(s, &f, off, buf, 64 * 1024);
        if (n < 0) { status = fail(path, (int)n); break; }
        if (n == 0) break;
        if (write(fd, buf, (size_t)n) != n) { fprintf(stderr, "smb: write failed\n"); status = 1; break; }
        off += (uint64_t)n;
    }
    free(buf);
    usmb_close_file(s, &f);
    return status;
}

// `login`: hand the share's server (smbfsd) a user and password, through
// the kernel (USERFS_CTL) -- the same message the File Manager's
// Sign-in dialog sends.
static __attribute__((noinline)) int login(const char *name) {
    if (!f_user || !f_user[0]) return uargs_error(&PROG, "login needs -u USER");
    struct userfs_ctl *c = calloc(1, sizeof *c);
    if (!c) return 1;
    if (name[0] == '/') snprintf(c->point, sizeof c->point, "%s", name);
    else unetmount_point(name, c->point, sizeof c->point);
    char pw[256];
    if (f_password) snprintf(pw, sizeof pw, "%s", f_password);
    else ask_password(pw, sizeof pw);
    int n = snprintf((char *)c->data, sizeof c->data, "%s%c%s%c%s%c%d", f_user, 0,
                     f_domain ? f_domain : "", 0, pw, 0, f_remember ? 1 : 0);
    memset(pw, 0, sizeof pw);
    if (n < 0 || n + 1 > (int)sizeof c->data) { free(c); return uargs_error(&PROG, "too long"); }
    c->code = USERFS_CTL_LOGIN;
    c->len = (uint32_t)n + 1;
    int64_t rc = sys_userfs(USERFS_CTL, (uint64_t)(uintptr_t)c, 0);
    int e = rc < 0 ? sys_errno() : 0;
    memset(c, 0, sizeof *c);
    free(c);
    if (!rc) { printf("Signed in to %s%s\n", name, f_remember ? "; the password is saved" : ""); return 0; }
    fprintf(stderr, "smb: login %s: %s\n", name,
            e == ENOENT ? "no network share is mounted there"
            : e == EACCES ? "wrong user name or password"
            : e == ETIMEDOUT ? "the server did not answer" : strerror(e));
    return 1;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc < 2) return uargs_error(&PROG, "missing URL");
    const char *cmd = a.argv[0], *url = a.argv[1];
    if (!strcmp(cmd, "login")) return a.argc == 2 ? login(url) : uargs_error(&PROG, "too many arguments");
    int is_get = !strcmp(cmd, "get");
    if (a.argc > (is_get ? 3 : 2)) return uargs_error(&PROG, "too many arguments");

    char server[256], share[256], path[768];
    uint16_t port;
    if (usmb_parse_url(url, server, sizeof server, &port, share, sizeof share, path, sizeof path))
        return uargs_error(&PROG, "'%s' is not //server/share", url);

    unsigned timeout = f_timeout ? (unsigned)atoi(f_timeout) * 1000u : 10000u;
    struct usmb_auth auth;
    memset(&auth, 0, sizeof auth);
    char pw[256];
    if (f_user && f_user[0]) {
        auth.user = f_user;
        if (!f_password && strcmp(f_user, "guest")) ask_password(pw, sizeof pw);
        else snprintf(pw, sizeof pw, "%s", f_password ? f_password : "");
        usmb_nt_hash(pw, auth.nt_hash);
        memset(pw, 0, sizeof pw);
    }
    auth.domain = f_domain;

    struct usmb_io io;
    int rc = usmb_dial(server, port, timeout, &io);
    if (rc) return fail(server, rc);
    struct usmb *s;
    rc = usmb_open(&s, &io, server, share, &auth, timeout);
    if (rc) return fail(url, rc);

    int status = 0;
    if (!strcmp(cmd, "ls")) {
        rc = usmb_list(s, path, print_entry, NULL);
        if (rc) status = fail(url, rc);
    } else if (!strcmp(cmd, "cat")) {
        status = copy_out(s, path, 1);
    } else if (is_get) {
        const char *base = strrchr(path, '/');
        const char *dest = a.argc > 2 ? a.argv[2] : (base ? base + 1 : path);
        if (!dest[0]) { usmb_close(s); return uargs_error(&PROG, "get needs a file, not a share"); }
        int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror(dest); usmb_close(s); return 1; }
        status = copy_out(s, path, fd);
        close(fd);
    } else if (!strcmp(cmd, "stat")) {
        struct usmb_stat st;
        rc = usmb_stat(s, path, &st);
        if (rc) status = fail(url, rc);
        else {
            char c[40], m[40];
            when(c, sizeof c, st.created);
            when(m, sizeof m, st.modified);
            printf("  Type: %s\n", st.is_dir ? "folder" : "file");
            if (!st.is_dir) printf("  Size: %llu bytes\n", (unsigned long long)st.size);
            printf("Created: %s\nModified: %s\n", c, m);
        }
    } else if (!strcmp(cmd, "info")) {
        struct usmb_info in;
        usmb_get_info(s, &in);
        printf("  Server: %s, share %s\n", server, share);
        printf(" Dialect: SMB %u.%u%s\n", in.dialect >> 8, (in.dialect >> 4) & 0xf,
               (in.dialect & 0xf) ? (in.dialect == 0x0311 ? ".1" : ".2") : "");
        printf(" Signing: %s\n", in.signing ? "every message" : "none");
        printf(" Sign-in: %s\n", in.guest ? "guest" : auth.user ? auth.user : "anonymous");
        uint64_t total = 0, avail = 0;
        if (!usmb_fs_size(s, &total, &avail)) {
            char t[16], f[16];
            human_size(t, sizeof t, total);
            human_size(f, sizeof f, avail);
            printf("    Size: %s, %s free\n", t, f);
        }
    }
    usmb_close(s);
    return status;
}
