// The font fallback list (base/ufontfallback.h).
#include "base/ufontfallback.h"
#include "base/ufile.h"
#include "base/ulog.h"
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// What a machine without /etc/fonts/fallback.conf gets: data/etc/fonts/
// fallback.conf's own list, so the two agree on a fresh install.
static const char DEFAULT_LIST[] =
    "noto-sans\n"
    "noto-sans-symbols2\n"
    "noto-sans-symbols\n"
    "noto-sans-math\n"
    "emoji twemoji\n"
    "dejavu-sans-mono\n";

static int stem_ok(const char *s, int n) {
    if (n <= 0 || n > 64) return 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return s[0] != '.';   // a stem, never a path or a hidden file
}

int ufont_fallback_parse(const char *text, int (*exists)(const char *path),
                         struct ufont_fallback *out) {
    out->faces = NULL;
    out->n = 0;
    int cap = 0, said = 0;
    for (const char *p = text; p && *p;) {
        const char *eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);
        const char *line = p;
        p = eol ? eol + 1 : NULL;
        while (len && (line[0] == ' ' || line[0] == '\t')) { line++; len--; }
        while (len && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r')) len--;
        if (!len || line[0] == '#') continue;
        int emoji = 0;
        if (len > 6 && !strncmp(line, "emoji", 5) && (line[5] == ' ' || line[5] == '\t')) {
            emoji = 1;
            line += 6;
            len -= 6;
            while (len && (line[0] == ' ' || line[0] == '\t')) { line++; len--; }
        }
        if (!stem_ok(line, len)) {
            if (!said++) ulogf("fonts: %s: skipping a line that is not a face name\n", UFONT_FALLBACK_CONF);
            continue;
        }
        char path[160];
        snprintf(path, sizeof path, "%s/%.*s.ttf", UFONT_FALLBACK_DIR, len, line);
        if (!exists(path)) snprintf(path, sizeof path, "%s/%.*s.ttf", UFONT_DIR, len, line);
        if (out->n == cap) {
            int ncap = cap ? cap * 2 : 8;
            struct ufont_fallback_face *nf = realloc(out->faces, (size_t)ncap * sizeof *nf);
            if (!nf) return 0;
            out->faces = nf;
            cap = ncap;
        }
        char *dup = strdup(path);
        if (!dup) return 0;
        out->faces[out->n].path = dup;
        out->faces[out->n].emoji = emoji;
        out->n++;
    }
    return 1;
}

static int file_exists(const char *path) {
    struct sys_stat st;
    return sys_stat(path, &st) == 0;
}

const struct ufont_fallback *ufont_fallback(void) {
    static struct ufont_fallback list;
    static int loaded;
    if (loaded) return &list;
    loaded = 1;
    uint8_t *data = NULL;
    size_t len = 0;
    char *text = NULL;
    // ufile_slurp() hands back bytes, not a string: the parser gets a copy
    // with its terminator.
    if (ufile_slurp(UFONT_FALLBACK_CONF, 64 * 1024, &data, &len) == UFILE_OK &&
        (text = malloc(len + 1)) != NULL) {
        memcpy(text, data, len);
        text[len] = 0;
    }
    free(data);
    if (!ufont_fallback_parse(text ? text : DEFAULT_LIST, file_exists, &list))
        ulogf("fonts: no memory for the fallback list\n");
    free(text);
    return &list;
}
