// The emoji list and the recently used ones (base/uemojidata.h).
#include "base/uemojidata.h"
#include "base/ufile.h"
#include "base/usetting.h"
#include "base/uutf8.h"

#include <stdlib.h>
#include <string.h>

#define DATA_MAX (1u << 20)

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// One hex code point at `*p`, advancing past it; 0 when there is none.
static uint32_t hex_cp(char **p) {
    uint32_t v = 0;
    int n = 0;
    while (n < 6 && hexval(**p) >= 0) v = v * 16 + (uint32_t)hexval(*(*p)++), n++;
    return n ? v : 0;
}

void uemojidata_free(struct uemojidata *d) {
    free(d->text);
    free(d->entries);
    free(d->groups);
    memset(d, 0, sizeof *d);
}

int uemojidata_parse(char *text, int len, struct uemojidata *out) {
    memset(out, 0, sizeof *out);
    out->text = text;
    if (!text || len <= 0) return 1;
    // SIZED TO THE FILE: one entry per line at most, one group per '@'.
    int lines = 1, ats = 0;
    for (int i = 0; i < len; i++) {
        lines += text[i] == '\n';
        ats += text[i] == '@';
    }
    out->entries = malloc(sizeof *out->entries * (size_t)lines);
    out->groups = malloc(sizeof *out->groups * (size_t)(ats + 1));
    if (!out->entries || !out->groups) {
        uemojidata_free(out);
        return 0;
    }
    for (char *p = text; p < text + len && *p;) {
        char *nl = memchr(p, '\n', (size_t)(text + len - p));
        if (nl) *nl = '\0';
        if (*p == '@' && p[1]) {
            struct uemoji_group *g = &out->groups[out->ngroups++];
            g->name = p + 1;
            int n = 0;
            while (p[1 + n] && p[1 + n] != ' ' && n < (int)sizeof g->tab - 1) n++;
            memcpy(g->tab, p + 1, (size_t)n);
            g->tab[n] = '\0';
            g->first = out->count;
            g->count = 0;
        } else if (*p != '#' && out->ngroups) {
            // "1F600;name" or "2764 FE0F;name". Anything else -- a sequence
            // the generator should not have written -- is skipped, not guessed.
            char *q = p;
            uint32_t cp = hex_cp(&q), sel = 0;
            if (*q == ' ') { q++; sel = hex_cp(&q); }
            if (cp && *q == ';' && q[1] && (!sel || sel == 0xFE0F)) {
                struct uemoji_entry *e = &out->entries[out->count];
                char u[4];
                int n = uutf8_encode(cp, u);
                if (n > 0) {
                    e->cp = cp;
                    e->vs16 = sel == 0xFE0F;
                    e->group = out->ngroups - 1;
                    e->name = q + 1;
                    memcpy(e->seq, u, (size_t)n);
                    if (e->vs16) n += uutf8_encode(0xFE0F, e->seq + n);
                    e->seq[n] = '\0';
                    out->count++;
                    out->groups[out->ngroups - 1].count++;
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 1;
}

const struct uemojidata *uemojidata(void) {
    static struct uemojidata d;
    static int loaded;
    if (loaded) return &d;
    loaded = 1;
    uint8_t *raw;
    size_t n;
    if (ufile_slurp(UEMOJI_DATA_PATH, DATA_MAX, &raw, &n) != UFILE_OK) return &d;
    char *text = malloc(n + 1);
    if (text) {
        memcpy(text, raw, n);
        text[n] = '\0';
        if (!uemojidata_parse(text, (int)n, &d)) memset(&d, 0, sizeof d);
    }
    free(raw);
    return &d;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int holds(const char *name, const char *q, int qn) {
    for (const char *s = name; *s; s++) {
        int k = 0;
        while (k < qn && s[k] && lower(s[k]) == lower(q[k])) k++;
        if (k == qn) return 1;
    }
    return 0;
}

int uemojidata_search(const struct uemojidata *d, const char *q, int *out, int cap) {
    int qn = q ? (int)strlen(q) : 0, found = 0;
    if (!qn) return 0;
    // THE EMOJI ITSELF, pasted or completed into the field: that entry alone.
    int k;
    uint32_t c = uutf8_decode(q, qn, &k);
    if (c >= 0x80 && (k == qn || (qn - k == 3 && !memcmp(q + k, "\xEF\xB8\x8F", 3)))) {
        int e = uemojidata_find(d, c);
        if (e >= 0) {
            if (cap > 0) out[0] = e;
            return 1;
        }
        // Not an emoji: a letter like 'n with tilde', searched in the names.
    }
    for (int i = 0; i < d->count; i++) {
        if (!holds(d->entries[i].name, q, qn)) continue;
        if (found < cap) out[found] = i;
        found++;
    }
    return found;
}

int uemojidata_find(const struct uemojidata *d, uint32_t cp) {
    for (int i = 0; i < d->count; i++)
        if (d->entries[i].cp == cp) return i;
    return -1;
}

int uemojidata_recent_decode(const struct uemojidata *d, const char *value, int *out, int cap) {
    int n = 0, len = value ? (int)strlen(value) : 0;
    for (int i = 0; i < len && n < cap;) {
        int k;
        uint32_t cp = uutf8_decode(value + i, len - i, &k);
        i += k > 0 ? k : 1;
        int e = uemojidata_find(d, cp);
        if (e >= 0) out[n++] = e;
    }
    return n;
}

int uemojidata_recent_push(const char *value, uint32_t cp, char *out, int cap) {
    if (cap <= 0) return 0;
    int o = 0, kept = 0, len = value ? (int)strlen(value) : 0;
    char u[4];
    int n = uutf8_encode(cp, u);
    if (n > 0 && n < cap) { memcpy(out, u, (size_t)n); o = n; kept = 1; }
    for (int i = 0; i < len && kept < UEMOJI_RECENT_MAX;) {
        int k;
        uint32_t c = uutf8_decode(value + i, len - i, &k);
        if (k <= 0) k = 1;
        // A selector or a duplicate is dropped; what does not fit ends the
        // list, whole characters only.
        if (c != cp && c != 0xFE0F && c != 0xFE0E) {
            if (o + k > cap - 1) break;
            memcpy(out + o, value + i, (size_t)k);
            o += k;
            kept++;
        }
        i += k;
    }
    out[o] = '\0';
    return o;
}

int uemojidata_recents(int *out, int cap) {
    char v[SETTING_ABI_VALUE_MAX];
    if (!usetting_get(UEMOJI_RECENT_SET, v, sizeof v)) return 0;
    return uemojidata_recent_decode(uemojidata(), v, out, cap);
}

int uemojidata_recent_add(int entry) {
    const struct uemojidata *d = uemojidata();
    if (entry < 0 || entry >= d->count) return 0;
    char was[SETTING_ABI_VALUE_MAX], now[SETTING_ABI_VALUE_MAX];
    if (!usetting_get(UEMOJI_RECENT_SET, was, sizeof was)) was[0] = '\0';
    uemojidata_recent_push(was, d->entries[entry].cp, now, sizeof now);
    return usetting_set(UEMOJI_RECENT_SET, now) > 0;
}
