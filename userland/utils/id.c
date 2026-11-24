// id -- who a process is: its user, group and supplementary groups.
//
// GNU coreutils' output and options: `uid=1000(user) gid=1000(user)
// groups=1000(user),10(wheel)`, or one field with -u/-g/-G, names with
// -n. With USER, that account's ids from /etc/passwd and /etc/group
// instead of this process's.
#include "base/uargs.h"
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int o_user, o_group, o_groups, o_name, o_real;

static const struct uargs_opt OPTS[] = {
    { "user",   'u', 0, "only the effective user id", &o_user, 0 },
    { "group",  'g', 0, "only the effective group id", &o_group, 0 },
    { "groups", 'G', 0, "only the group ids, every one", &o_groups, 0 },
    { "name",   'n', 0, "with -u, -g or -G: the name instead of the number", &o_name, 0 },
    { "real",   'r', 0, "with -u, -g or -G: the real id instead of the effective one", &o_real, 0 },
    { 0, 0, 0, 0, 0, 0 },
};

static const struct uargs_prog PROG = {
    "id", "[OPTION]... [USER]",
    "Print the user and group ids of USER, or of this process.",
    OPTS, 0, 0, 0, 0, 0,
};

static void put_user(uid_t u) {
    struct passwd *pw = getpwuid(u);
    if (pw) printf("%u(%s)", (unsigned)u, pw->pw_name);
    else printf("%u", (unsigned)u);
}

static void put_group(gid_t g) {
    struct group *gr = getgrgid(g);
    if (gr) printf("%u(%s)", (unsigned)g, gr->gr_name);
    else printf("%u", (unsigned)g);
}

static int put_one_user(uid_t u) {
    if (!o_name) { printf("%u", (unsigned)u); return 0; }
    struct passwd *pw = getpwuid(u);
    if (!pw) { fprintf(stderr, "id: cannot find name for user ID %u\n", (unsigned)u); printf("%u", (unsigned)u); return 1; }
    printf("%s", pw->pw_name);
    return 0;
}

static int put_one_group(gid_t g) {
    if (!o_name) { printf("%u", (unsigned)g); return 0; }
    struct group *gr = getgrgid(g);
    if (!gr) { fprintf(stderr, "id: cannot find name for group ID %u\n", (unsigned)g); printf("%u", (unsigned)g); return 1; }
    printf("%s", gr->gr_name);
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc > 1) return uargs_error(&PROG, "extra operand '%s'", a.argv[1]);
    if (!!o_user + !!o_group + !!o_groups > 1)   // a flag holds its POSITION, not 1
        return uargs_error(&PROG, "cannot print \"only\" of more than one choice");
    if ((o_name || o_real) && !o_user && !o_group && !o_groups)
        return uargs_error(&PROG, "cannot print only names or real IDs in default format");

    uid_t ruid, euid;
    gid_t rgid, egid;
    gid_t *groups = 0;
    int ng = 0;
    if (a.argc == 1) {
        struct passwd *pw = getpwnam(a.argv[0]);
        if (!pw) { fprintf(stderr, "id: '%s': no such user\n", a.argv[0]); return 1; }
        ruid = euid = pw->pw_uid;
        rgid = egid = pw->pw_gid;
        getgrouplist(a.argv[0], rgid, 0, &ng);
        groups = malloc((size_t)ng * sizeof *groups);
        if (!groups || getgrouplist(a.argv[0], rgid, groups, &ng) < 0) { perror("id"); return 1; }
    } else {
        ruid = getuid(); euid = geteuid();
        rgid = getgid(); egid = getegid();
        int n = getgroups(0, 0);
        if (n < 0) n = 0;
        // The effective group first, then the supplementary ones -- GNU's order.
        groups = malloc((size_t)(n + 1) * sizeof *groups);
        if (!groups) { perror("id"); return 1; }
        groups[0] = egid;
        int got = n ? getgroups(n, groups + 1) : 0;
        ng = 1;
        for (int i = 0; i < got; i++)
            if (groups[1 + i] != egid) groups[ng++] = groups[1 + i];
    }

    int rc = 0;
    if (o_user) rc = put_one_user(o_real ? ruid : euid);
    else if (o_group) rc = put_one_group(o_real ? rgid : egid);
    else if (o_groups) {
        for (int i = 0; i < ng; i++) {
            if (i) putchar(' ');
            rc |= put_one_group(groups[i]);
        }
    } else {
        printf("uid="); put_user(ruid);
        printf(" gid="); put_group(rgid);
        if (euid != ruid) { printf(" euid="); put_user(euid); }
        if (egid != rgid) { printf(" egid="); put_group(egid); }
        printf(" groups=");
        for (int i = 0; i < ng; i++) {
            if (i) putchar(',');
            put_group(groups[i]);
        }
    }
    putchar('\n');
    free(groups);
    return rc;
}
