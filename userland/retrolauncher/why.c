// why.c -- Why? on a stand-in call: its section of the stand-ins page,
// in a window of its own. The page is toy-os's own Markdown, also read
// whole in Help; a call with no section says so rather than nothing
// (tools/check_docs.py refuses a stand-in with none).
#include "retrolauncher/retrolauncher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/umd.h"
#include "ui/ulog.h"
#include "ui/uui_docdialog.h"
#include "w16/w16prefs.h"

static char *slurp(const char *path, int *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char *t = 0;
    size_t n = 0, cap = 0, got;
    do {
        if (n + 4096 > cap) {
            char *g = realloc(t, cap = cap * 2 + 8192);
            if (!g) { free(t); fclose(f); return 0; }
            t = g;
        }
        got = fread(t + n, 1, cap - n, f);
        n += got;
    } while (got);
    fclose(f);
    *len = (int)n;
    return t;
}

void retro_why_open(struct uapp *a, const char *module, unsigned ordinal, const char *name) {
    char key[48], title[96];
    snprintf(key, sizeof key, "%s.%u", module, ordinal);
    snprintf(title, sizeof title, "%s - partly there", name);
    int len = 0, off = 0, n = 0;
    char *doc = slurp(W16_STANDINS_DOC, &len);
    if (doc) n = umd_section_find(doc, len, key, &off);
    ulogf("retrolauncher: why %s %s\n", key, n ? "shown" : "no section");
    if (n) uui_docdialog_open(a, title, doc + off, n);
    else {
        char text[256];
        int k = snprintf(text, sizeof text, "## %s %s\n\nNo explanation is written for this call yet. "
                         "What w16 does is in the list's last column.\n", key, name);
        uui_docdialog_open(a, title, text, k);
    }
    free(doc);
}
