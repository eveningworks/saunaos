// A user id's name for a listing (uusername.h).
#include "base/uusername.h"
#include "base/uself.h"
#include "proc_info.h"
#include "query_abi.h"
#include "cred_abi.h"   // CRED_SEAT
#include "rt/sys.h"
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct entry {
    unsigned uid, width;
    char *text;
};

static struct entry *g_ent;
static int g_n, g_cap;

const char *uusername(unsigned uid, unsigned width) {
    for (int i = 0; i < g_n; i++)
        if (g_ent[i].uid == uid && g_ent[i].width == width) return g_ent[i].text;
    char num[12];
    snprintf(num, sizeof num, "%u", uid);
    struct passwd *pw = getpwuid(uid);
    const char *use = pw && (!width || strlen(pw->pw_name) < width) ? pw->pw_name : num;
    if (g_n == g_cap) {
        int nc = g_cap ? g_cap * 2 : 8;
        struct entry *nb = realloc(g_ent, (size_t)nc * sizeof *nb);
        if (!nb) return "?";
        g_ent = nb;
        g_cap = nc;
    }
    char *text = strdup(use);
    if (!text) return "?";
    g_ent[g_n++] = (struct entry){ uid, width, text };
    return text;
}

int uusername_desktop(unsigned *uid, unsigned *gid) {
    FILE *f = fopen("/etc/services.d/toywm", "r");
    if (!f) return 0;
    char line[160], name[64] = "";
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "User=", 5)) continue;
        size_t n = strcspn(line + 5, " \t\r\n");
        if (n && n < sizeof name) { memcpy(name, line + 5, n); name[n] = 0; }
    }
    fclose(f);
    // `User=@session` (users D2b): the desktop runs as whoever signed in --
    // the account its running compositor has, else the first person's.
    struct passwd *pw = 0;
    if (!strcmp(name, "@session")) {
        struct proc_info pi;
        char path[QUERY_PROCPATH_MAX];
        for (int i = 0; !pw && sys_proc_info(i, &pi) == 0; i++)
            if (pi.pid && pi.pid == pi.tgid && pi.uid >= 1000 && uproc_path(pi.pid, path, sizeof path) >= 0 &&
                !strcmp(path, "/bin/wm/system/toywm"))
                pw = getpwuid(pi.uid);
        if (!pw) {
            setpwent();
            while ((pw = getpwent()) && pw->pw_uid < 1000) { }
            endpwent();
        }
    } else if (name[0]) {
        pw = getpwnam(name);
    }
    if (!pw || pw->pw_uid == 0) return 0;
    *uid = (unsigned)pw->pw_uid;
    *gid = (unsigned)pw->pw_gid;
    return 1;
}

int uusername_session(unsigned *uid, unsigned *gid) {
    struct proc_info pi;
    for (int i = 0; sys_proc_info(i, &pi) == 0; i++) {
        if (!pi.pid || pi.uid < 1000 || !(pi.cred_flags & CRED_SEAT)) continue;
        struct passwd *pw = getpwuid(pi.uid);
        if (!pw) continue;
        *uid = (unsigned)pw->pw_uid;
        *gid = (unsigned)pw->pw_gid;
        return 1;
    }
    return 0;
}
