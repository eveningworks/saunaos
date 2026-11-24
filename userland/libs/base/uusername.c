// A user id's name for a listing (uusername.h).
#include "base/uusername.h"
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
    struct passwd *pw = name[0] ? getpwnam(name) : 0;
    if (!pw || pw->pw_uid == 0) return 0;
    *uid = (unsigned)pw->pw_uid;
    *gid = (unsigned)pw->pw_gid;
    return 1;
}
