// The emoji list and the recently used ones (base/uemojidata.h).
#include "base/uemojidata.h"
#include "base/ufile.h"
#include "base/usetting.h"
#include "base/uutf8.h"
#include "kgrapheme.h"

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
    free(d->arena);
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
    // A code point's UTF-8 is never longer than its hex and a space, so the
    // sequences fit in the file's own size plus a NUL each.
    out->arena = malloc((size_t)len + 2u * (size_t)lines);
    char *ar = out->arena;
    if (!out->entries || !out->groups || !ar) {
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
            // "CPS;name" or "CPS;name;TEMPLATE": hex code points, one space
            // apart, '~' in the template where a tone goes. Anything else is
            // skipped, not guessed.
            char *q = p, *seq = ar;
            int n = 0, k = 0;
            uint32_t first = 0, second = 0;
            for (uint32_t cp; (cp = hex_cp(&q)) != 0;) {
                if (!k) first = cp;
                if (k == 1) second = cp;
                k++;
                n += uutf8_encode(cp, seq + n);
                if (*q != ' ') break;
                q++;
            }
            char *semi = *q == ';' ? strchr(q + 1, ';') : 0;
            if (k && n && *q == ';' && q[1] && (!semi || semi[1])) {
                seq[n] = '\0';
                ar += n + 1;
                struct uemoji_entry *e = &out->entries[out->count];
                e->cp = first;
                e->vs16 = k == 2 && second == 0xFE0F;
                e->group = out->ngroups - 1;
                e->name = q + 1;
                e->seq = seq;
                e->toned = 0;
                if (semi) {
                    *semi = '\0';
                    char *t = semi + 1, *tpl = ar;
                    int m = 0;
                    while (*t) {
                        if (*t == '~') { tpl[m++] = UEMOJI_TONE_SLOT; t++; }
                        else { uint32_t cp = hex_cp(&t); if (!cp) break; m += uutf8_encode(cp, tpl + m); }
                        if (*t == ' ') t++;
                    }
                    tpl[m] = '\0';
                    ar += m + 1;
                    if (m) e->toned = tpl;
                }
                out->count++;
                out->groups[out->ngroups - 1].count++;
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

int uemojidata_toned(const struct uemoji_entry *e, int tone, char *out, int cap) {
    if (!e || !out || cap <= 0) return 0;
    if (tone == 0) {
        int n = (int)strlen(e->seq);
        if (n >= cap) return 0;
        memcpy(out, e->seq, (size_t)n + 1);
        return n;
    }
    if (!e->toned || tone < 1 || tone > UEMOJI_TONES) return 0;
    char u[4];
    int t = uutf8_encode(0x1F3FA + (uint32_t)tone, u), o = 0;
    for (const char *p = e->toned; *p; p++) {
        int need = *p == UEMOJI_TONE_SLOT ? t : 1;
        if (o + need >= cap) return 0;
        if (*p == UEMOJI_TONE_SLOT) { memcpy(out + o, u, (size_t)t); o += t; }
        else out[o++] = *p;
    }
    out[o] = '\0';
    return o;
}

// Whether `a` (`n` bytes) spells `b`, a U+FE0F in either being optional --
// a pasted or typed emoji often lacks the one the list writes.
static int same_emoji(const char *a, int n, const char *b) {
    int i = 0, j = 0, bn = (int)strlen(b);
    while (i < n || j < bn) {
        if (i + 3 <= n && !memcmp(a + i, "\xEF\xB8\x8F", 3)) { i += 3; continue; }
        if (j + 3 <= bn && !memcmp(b + j, "\xEF\xB8\x8F", 3)) { j += 3; continue; }
        if (i >= n || j >= bn || a[i] != b[j]) return 0;
        i++, j++;
    }
    return 1;
}

int uemojidata_find_seq(const struct uemojidata *d, const char *utf8, int n) {
    if (!utf8 || n <= 0) return -1;
    char toned[64];
    for (int i = 0; i < d->count; i++) {
        const struct uemoji_entry *e = &d->entries[i];
        if (same_emoji(utf8, n, e->seq)) return i;
        for (int t = 1; e->toned && t <= UEMOJI_TONES; t++)
            if (uemojidata_toned(e, t, toned, sizeof toned) && same_emoji(utf8, n, toned)) return i;
    }
    return -1;
}

int uemojidata_search(const struct uemojidata *d, const char *q, int *out, int cap) {
    int qn = q ? (int)strlen(q) : 0, found = 0;
    if (!qn) return 0;
    // THE EMOJI ITSELF, pasted or completed into the field -- one whole
    // grapheme cluster: that entry alone.
    if ((unsigned char)q[0] >= 0x80 && kgrapheme_next(q, qn, 0) == qn) {
        int e = uemojidata_find_seq(d, q, qn);
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
    for (int i = 0, nx; i < len && n < cap && (nx = kgrapheme_next(value, len, i)) > i; i = nx) {
        int e = uemojidata_find_seq(d, value + i, nx - i);
        if (e >= 0) out[n++] = e;
    }
    return n;
}

int uemojidata_recent_push(const char *value, const char *seq, char *out, int cap) {
    if (cap <= 0) return 0;
    int o = 0, kept = 0, len = value ? (int)strlen(value) : 0, sn = seq ? (int)strlen(seq) : 0;
    if (sn > 0 && sn < cap) { memcpy(out, seq, (size_t)sn); o = sn; kept = 1; }
    for (int i = 0, nx; i < len && kept < UEMOJI_RECENT_MAX && (nx = kgrapheme_next(value, len, i)) > i; i = nx) {
        // A duplicate, or a selector left over from an older value, is
        // dropped; what does not fit ends the list, whole emoji only.
        int k = nx - i;
        if ((sn && same_emoji(value + i, k, seq)) || (k == 3 && !memcmp(value + i, "\xEF\xB8", 2))) continue;
        if (o + k > cap - 1) break;
        memcpy(out + o, value + i, (size_t)k);
        o += k;
        kept++;
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
    uemojidata_recent_push(was, d->entries[entry].seq, now, sizeof now);
    return usetting_set(UEMOJI_RECENT_SET, now) > 0;
}
