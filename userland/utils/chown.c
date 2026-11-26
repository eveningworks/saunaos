// chown -- change who owns a file (users stage C, docs/users-design.md).
//
// GNU coreutils' shape: OWNER, OWNER:GROUP, :GROUP or OWNER: (the
// owner's own group), each a name from /etc/passwd and /etc/group or a
// number. -R walks a folder. Giving a file away is root's (`sudo chown`);
// an owner may only move it into a group it is in.
#include "base/uargs.h"
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int o_recursive, o_verbose;

static const struct uargs_opt OPTS[] = {
    { "recursive", 'R', 0, "change folders and everything in them", &o_recursive, 0 },
    { "verbose",   'v', 0, "say each file changed", &o_verbose, 0 },
    { 0, 0, 0, 0, 0, 0 },
};

static const struct uargs_prog PROG = {
    "chown", "[OPTION]... OWNER[:[GROUP]] FILE...\n:GROUP FILE...",
    "Change the owner and/or group of each FILE to OWNER and/or GROUP.\n"
    "OWNER: alone sets the owner's own group. Only root may give a file away.",
    OPTS, 0, 0, 0, 0, 0, 0,
};

// A name, or a number when nothing has that name. Refuses an empty or
// unknown word rather than guess.
static int parse_user(const char *s, uid_t *out, gid_t *primary) {
    struct passwd *pw = getpwnam(s);
    if (pw) { *out = pw->pw_uid; *primary = pw->pw_gid; return 0; }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (!*s || *end || v >= 0xFFFFFFFFul) return -1;
    *out = (uid_t)v;
    pw = getpwuid(*out);
    *primary = pw ? pw->pw_gid : (gid_t)-1;
    return 0;
}

static int parse_group(const char *s, gid_t *out) {
    struct group *gr = getgrnam(s);
    if (gr) { *out = gr->gr_gid; return 0; }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (!*s || *end || v >= 0xFFFFFFFFul) return -1;
    *out = (gid_t)v;
    return 0;
}

static int change(const char *path, uid_t uid, gid_t gid) {
    if (chown(path, uid, gid) != 0) {
        if (errno == ENOTSUP)
            fprintf(stderr, "chown: %s: this filesystem stores no owners "
                            "(its files belong to whoever it was mounted for)\n", path);
        else
            fprintf(stderr, "chown: changing ownership of '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (o_verbose) printf("ownership of '%s' changed\n", path);
    return 0;
}

// Recurses by what the LISTING says an entry is, so a symlink is never
// walked through -- GNU's -P default; following one would hand whatever
// it names (a link to /etc in someone's folder) to the new owner.
static int walk(const char *path, uid_t uid, gid_t gid, int is_dir) {
    int failed = change(path, uid, gid);
    if (!o_recursive || !is_dir) return failed;
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "chown: cannot read folder '%s': %s\n", path, strerror(errno)); return 1; }
    size_t plen = strlen(path);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || e->d_type == DT_LNK) continue;
        size_t n = plen + 1 + strlen(e->d_name) + 1;
        char *child = malloc(n);
        if (!child) { closedir(d); fprintf(stderr, "chown: out of memory\n"); return 1; }
        snprintf(child, n, "%s%s%s", path, plen && path[plen - 1] == '/' ? "" : "/", e->d_name);
        failed |= walk(child, uid, gid, e->d_type == DT_DIR);
        free(child);
    }
    closedir(d);
    return failed;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc < 1) return uargs_error(&PROG, "missing operand");
    if (a.argc < 2) return uargs_error(&PROG, "missing operand after '%s'", a.argv[0]);

    char *spec = strdup(a.argv[0]);
    if (!spec) { fprintf(stderr, "chown: out of memory\n"); return 1; }
    char *colon = strchr(spec, ':');
    if (!colon) colon = strchr(spec, '.') && !getpwnam(spec) ? strchr(spec, '.') : 0;   // GNU's old OWNER.GROUP
    if (colon) *colon = '\0';
    const char *owner = spec, *group = colon ? colon + 1 : 0;

    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1, primary = (gid_t)-1;
    if (*owner && parse_user(owner, &uid, &primary) != 0) {
        fprintf(stderr, "chown: invalid user: '%s'\n", a.argv[0]);
        return 1;
    }
    if (group && *group && parse_group(group, &gid) != 0) {
        fprintf(stderr, "chown: invalid group: '%s'\n", a.argv[0]);
        return 1;
    }
    if (group && !*group) {
        // OWNER: -- the owner's login group, as GNU does.
        if (!*owner || primary == (gid_t)-1) {
            fprintf(stderr, "chown: invalid spec: '%s'\n", a.argv[0]);
            return 1;
        }
        gid = primary;
    }
    if (!*owner && !group) return uargs_error(&PROG, "invalid spec: '%s'", a.argv[0]);

    int failed = 0;
    for (int i = 1; i < a.argc; i++) {
        struct stat st;
        failed |= walk(a.argv[i], uid, gid, stat(a.argv[i], &st) == 0 && S_ISDIR(st.st_mode));
    }
    free(spec);
    return failed;
}
