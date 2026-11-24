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
