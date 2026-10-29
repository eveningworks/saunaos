// Shared name=value config-file reader/writer for /etc -- see
// etc_config.h for the public API. This replaces two independent
// hand-rolled single-purpose parsers (tz.c's and font_config.c's,
// see the git history for the build that did the replacing) with one engine
// any current or future /etc file can go through.
//
// File format: one "key=value" per line. '#' starts a comment that
// runs to the end of the line -- a whole-line comment or a trailing
// one after a real "key=value" are both stripped before parsing.
// Blank lines are ignored. Leading/trailing spaces/tabs around the key
// and the value are trimmed. A `[name]` line opens a SECTION and every
// key after it belongs to it; a key before any header is at top level,
// which is what every file predating sections is made of. There is no
// quoting and no escaping, so a value runs to the end of its line.
//
// Stateless by design: every call re-reads and re-parses the whole
// file, there's no open/parsed-in-memory handle to manage. Given how
// this project's config files are actually used -- read once at boot,
// written rarely (a shell command), and tiny (a handful of settings) --
// the redundant work is cheap and not worth the extra state a real
// handle-based API would need to manage correctly with no heap
// allocator to lean on.
//
// One file, multiple settings: as of the build that added this,
// /etc/timezone and /etc/fontsize's content both moved into a single
// shared /etc/saunaos.conf (see tz.c/font_config.c). That's a default,
// not a rule this file enforces -- `path` is just a parameter, so a
// future setting with enough of its own keys to be unwieldy sharing a
// file (imagine a GUI app with a dozen preferences) can just pass its
// own /etc/<name>.conf path instead. Nothing here favors one file over
// many; that choice belongs to each caller.
#include "etc_config.h"
#include "etc_renamed.h"
#include "string.h"

// k_isblank() is string.h's now -- line-oriented, so NOT k_isspace(),
// whose '\n' would run this parser into the next line.
#define is_space(c) k_isblank(c)

// Narrows [*start, end) by trimming leading/trailing spaces/tabs and
// returns the trimmed length. Doesn't touch the underlying bytes (no
// NUL written) -- callers copy the [*start, *start + len) slice out
// themselves.
static uint32_t trim(const char **start, const char *end) {
    const char *s = *start;
    while (s < end && is_space(*s)) s++;
    while (end > s && is_space(end[-1])) end--;
    *start = s;
    return (uint32_t)(end - s);
}

// Yields the next line [*line_start, *line_end) (end exclusive, not
// including the '\n') starting at *pos, and advances *pos past it.
// Returns 0 once *pos has reached data_len (no more lines).
static int next_line(const char *data, uint32_t data_len, uint32_t *pos,
                      const char **line_start, const char **line_end) {
    if (*pos >= data_len) return 0;
    const char *p = data + *pos;
    const char *end = data + data_len;
    *line_start = p;
    while (p < end && *p != '\n') p++;
    *line_end = p;
    *pos = (uint32_t)((p < end ? p + 1 : p) - data);
    return 1;
}

// Narrows line_end to strip a '#'-started comment, if any.
static const char *strip_comment(const char *line_start, const char *line_end) {
    for (const char *p = line_start; p < line_end; p++) {
        if (*p == '#') return p;
    }
    return line_end;
}

// Parses one already comment-stripped-free line as "key=value" (after
// stripping its own comment here). Returns 1 and fills the four out
// params on a real "key=value" line with a non-empty trimmed key; 0
// for a blank/comment-only line or one with no '='.
static int parse_kv(const char *line_start, const char *line_end,
                     const char **key_start, uint32_t *key_len,
                     const char **val_start, uint32_t *val_len) {
    const char *end = strip_comment(line_start, line_end);
    const char *eq = 0;
    for (const char *p = line_start; p < end; p++) {
        if (*p == '=') { eq = p; break; }
    }
    if (!eq) return 0;

    const char *ks = line_start, *ke = eq;
    *key_len = trim(&ks, ke);
    if (*key_len == 0) return 0;
    *key_start = ks;

    const char *vs = eq + 1, *ve = end;
    *val_len = trim(&vs, ve);
    *val_start = vs;
    return 1;
}

// Is this line a `[section]` header? Fills the trimmed name if so.
// A name that is empty or longer than the cap is NOT a header -- the
// line is ignored like any other malformed one, which is what lets a
// walk hand every name it reports straight back to a lookup.
static int parse_section(const char *line_start, const char *line_end,
                         const char **name_start, uint32_t *name_len) {
    const char *s = line_start, *e = strip_comment(line_start, line_end);
    uint32_t len = trim(&s, e);
    if (len < 2 || s[0] != '[' || s[len - 1] != ']') return 0;

    const char *ns = s + 1, *ne = s + len - 1;
    uint32_t n = trim(&ns, ne);
    if (n == 0 || n >= ETC_CONFIG_SECTION_MAX) return 0;
    *name_start = ns;
    *name_len = n;
    return 1;
}

static int key_matches(const char *key_start, uint32_t key_len, const char *key) {
    uint32_t klen = (uint32_t)k_strlen(key);
    if (klen != key_len) return 0;
    for (uint32_t i = 0; i < klen; i++) {
        if (key_start[i] != key[i]) return 0;
    }
    return 1;
}

// A NULL or empty section means TOP LEVEL -- the keys before the first
// header. Every unsuffixed entry point passes it, so a file with no
// headers at all behaves exactly as it did before sections existed.
#define WANT_TOP(sec) (!(sec) || !(sec)[0])

// The shared scan. Both the read-per-call and the read-once entry
// points below are this function plus a way of getting at the bytes --
// one parser, so the two can never disagree about what a line means.
//
// Every run of a repeated header is searched, in file order: a name is
// a section, not a position, so `[a] x=1 ... [a] y=2` answers both.
static int find_key(const char *data, uint32_t size, const char *section,
                    const char *key, char *out, uint32_t out_size) {
    uint32_t pos = 0;
    const char *ls, *le;
    int want_top = WANT_TOP(section);
    int in_scope = want_top;

    while (next_line(data, size, &pos, &ls, &le)) {
        const char *ns, *ks, *vs;
        uint32_t nlen, klen, vlen;

        if (parse_section(ls, le, &ns, &nlen)) {
            in_scope = !want_top && key_matches(ns, nlen, section);
            continue;
        }
        if (!in_scope) continue;
        if (!parse_kv(ls, le, &ks, &klen, &vs, &vlen)) continue;
        if (!key_matches(ks, klen, key)) continue;

        if (vlen >= out_size) vlen = out_size - 1; // truncate to fit
        k_memcpy(out, vs, vlen);
        out[vlen] = '\0';
        return 1;
    }
    return 0;
}

int etc_config_buf_get_in(const struct etc_config_buf *buf, const char *section,
                          const char *key, char *out, uint32_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!buf || !buf->valid || buf->size == 0) return 0;
    if (find_key(buf->data, buf->size, section, key, out, out_size)) return 1;
    // A file from before the dotted names says it under the old key.
    const char *was = etc_renamed_was(key);
    return was && find_key(buf->data, buf->size, section, was, out, out_size);
}

int etc_config_buf_get(const struct etc_config_buf *buf, const char *key,
                       char *out, uint32_t out_size) {
    return etc_config_buf_get_in(buf, 0, key, out, out_size);
}

int etc_config_buf_get_in_or_top(const struct etc_config_buf *buf,
                                 const char *section, const char *key,
                                 char *out, uint32_t out_size) {
    if (etc_config_buf_get_in(buf, section, key, out, out_size)) return 1;
    return etc_config_buf_get_in(buf, 0, key, out, out_size);
}

// ---- walking the sections -------------------------------------------
//
// Distinct names in first-appearance order. The dedup is a re-scan of
// the headers already passed rather than a table: a config file holds
// a handful of sections, and a table here would be either a fixed cap
// on how many sections a file may have or an allocation in a function
// that has never needed one.
static int section_at(const char *data, uint32_t size, int index,
                      const char **name, uint32_t *name_len) {
    uint32_t pos = 0;
    const char *ls, *le;
    int seen = 0;

    while (next_line(data, size, &pos, &ls, &le)) {
        const char *ns;
        uint32_t nlen;
        if (!parse_section(ls, le, &ns, &nlen)) continue;

        int dup = 0;
        uint32_t back = 0;
        const char *bs, *be;
        while (back < (uint32_t)(ls - data) && next_line(data, size, &back, &bs, &be)) {
            const char *ps;
            uint32_t plen;
            if (!parse_section(bs, be, &ps, &plen) || plen != nlen) continue;
            uint32_t i = 0;
            while (i < nlen && ps[i] == ns[i]) i++;
            if (i == nlen) { dup = 1; break; }
        }
        if (dup) continue;

        if (seen == index) { *name = ns; *name_len = nlen; return 1; }
        seen++;
    }
    return 0;
}

int etc_config_section_count(const struct etc_config_buf *buf) {
    if (!buf || !buf->valid || buf->size == 0) return 0;
    int n = 0;
    const char *name;
    uint32_t len;
    while (section_at(buf->data, buf->size, n, &name, &len)) n++;
    return n;
}

int etc_config_section_name(const struct etc_config_buf *buf, int index,
                            char *out, uint32_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!buf || !buf->valid || buf->size == 0 || index < 0) return 0;
    // Under the cap the name cannot be truncated, so a caller that
    // sizes its buffer by it never has to ask whether it was.
    if (out_size < ETC_CONFIG_SECTION_MAX) return 0;

    const char *name;
    uint32_t len;
    if (!section_at(buf->data, buf->size, index, &name, &len)) return 0;
    k_memcpy(out, name, len);
    out[len] = '\0';
    return 1;
}

// Rewrites `in` with `key` set to `value`, or with `key` REMOVED when
// `value` is NULL, inside `[section]` (NULL or "" = top level).
// Returns the new length, or 0 if it would not fit or (for a removal)
// the key was not there.
//
// Buffer to buffer, with no idea where either came from: that is what
// makes it usable from ring 3, where the file I/O around it is libsys
// rather than fs.h. It is also the only copy of this logic now --
// etc_config_set() and etc_config_unset() each had their own, differing
// only in whether the matched line was re-emitted, and two copies of a
// file rewriter is two chances to drop somebody's comments.
//
// Every line that is not the key's own is copied VERBATIM, so comments
// and ordering elsewhere survive. A comment on the key's own line does
// not: the line is replaced wholesale.
//
// Two passes, because where a NEW key lands cannot be known until the
// whole document has been read: pass 1 finds the key's line, or else
// the end of its section; pass 2 copies and splices. See the header for
// where that end is and why it is not the end of the file.
static int emit_bytes(char *out, uint32_t *len, uint32_t cap,
                      const char *b, uint32_t n) {
    if (*len + n + 1 > cap) return 0;   // +1: the terminating NUL
    k_memcpy(out + *len, b, n);
    *len += n;
    return 1;
}

static int emit_kv(char *out, uint32_t *len, uint32_t cap,
                   const char *key, const char *value) {
    return emit_bytes(out, len, cap, key, (uint32_t)k_strlen(key))
        && emit_bytes(out, len, cap, "=", 1)
        && emit_bytes(out, len, cap, value, (uint32_t)k_strlen(value))
        && emit_bytes(out, len, cap, "\n", 1);
}

// A section this writer would not be able to READ BACK is refused here
// rather than written: `[` or `]` inside the name, a newline, a '#'
// that would comment the header out, or a name over the cap all parse
// as "not a header", so the key would land in whatever section came
// before it.
static int section_writable(const char *section) {
    uint32_t n = (uint32_t)k_strlen(section);
    if (n == 0 || n >= ETC_CONFIG_SECTION_MAX) return 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = section[i];
        if (c == '[' || c == ']' || c == '#' || c == '\n' || c == '\r') return 0;
    }
    return 1;
}

// The rewrite itself: the new length, or -1 for a refusal -- which a
// length of 0 cannot say, because removing a file's only key is an
// EMPTY document and not a failure.
static int64_t set_core(const char *in, uint32_t in_len, const char *section,
                        const char *key, const char *value,
                        char *out, uint32_t out_cap) {
    if (!key || !*key || !out || out_cap == 0) return -1;
    int want_top = WANT_TOP(section);
    if (!want_top && !section_writable(section)) return -1;
    if (!in) in_len = 0;

    // ---- pass 1: find the key, or the end of its section ------------
    int idx = 0, total = 0;
    int match = -1;       // the key's own line, if it is already there
    int header = -1;      // the section's opening line
    int last_kv = -1;     // its last key=value line -- the insert point
    int started = want_top, ended = 0, in_scope = want_top;
    uint32_t pos = 0;
    const char *ls, *le;
    // The key's name before the dotted names (api/etc_renamed.h): its
    // line is REPLACED where it stands when the new key is not there yet,
    // and dropped otherwise, so a file migrates by being written.
    const char *was = etc_renamed_was(key);
    int old_line = -1;

    while (next_line(in, in_len, &pos, &ls, &le)) {
        const char *ns, *ks, *vs;
        uint32_t nlen, klen, vlen;

        if (parse_section(ls, le, &ns, &nlen)) {
            if (in_scope && started) ended = 1;   // the first run closes here
            in_scope = !want_top && key_matches(ns, nlen, section);
            if (in_scope && !started) { started = 1; header = idx; }
        } else if (in_scope && parse_kv(ls, le, &ks, &klen, &vs, &vlen)) {
            if (!ended) last_kv = idx;
            if (match < 0 && key_matches(ks, klen, key)) match = idx;
            if (old_line < 0 && was && key_matches(ks, klen, was)) old_line = idx;
        }
        idx++;
    }
    total = idx;
    if (match < 0 && old_line >= 0) { match = old_line; old_line = -1; }

    // Removing a key that was not there changes nothing, and saying so
    // matters: the caller must not rewrite the file identically and
    // must not claim to have removed anything.
    if (!value && match < 0) return -1;

    int insert_at = -1;    // insert BEFORE this line index
    int make_section = 0;  // ...or append the section itself
    if (value && match < 0) {
        if (!started) make_section = 1;
        else insert_at = (last_kv >= 0 ? last_kv : header) + 1;
    }

    // ---- pass 2: copy, splicing at the point pass 1 found -----------
    uint32_t out_len = 0;
    idx = 0;
    pos = 0;
    while (next_line(in, in_len, &pos, &ls, &le)) {
        if (idx == insert_at && !emit_kv(out, &out_len, out_cap, key, value))
            return -1;
        if (idx == match) {
            if (value && !emit_kv(out, &out_len, out_cap, key, value)) return -1;
        } else if (idx == old_line) {
            // the old key's line, beside the new one: gone
        } else {
            uint32_t n = (uint32_t)(le - ls);
            if (!emit_bytes(out, &out_len, out_cap, ls, n)) return -1;
            if (!emit_bytes(out, &out_len, out_cap, "\n", 1)) return -1;
        }
        idx++;
    }
    if (insert_at >= total && !emit_kv(out, &out_len, out_cap, key, value))
        return -1;

    if (make_section) {
        // A blank line above the header, so an appended section is
        // readable rather than jammed against what precedes it.
        if (out_len > 0 && !(out_len >= 2 && out[out_len - 2] == '\n')
            && !emit_bytes(out, &out_len, out_cap, "\n", 1)) return -1;
        if (!emit_bytes(out, &out_len, out_cap, "[", 1)) return -1;
        if (!emit_bytes(out, &out_len, out_cap, section, (uint32_t)k_strlen(section)))
            return -1;
        if (!emit_bytes(out, &out_len, out_cap, "]\n", 2)) return -1;
        if (!emit_kv(out, &out_len, out_cap, key, value)) return -1;
    }

    out[out_len] = '\0';
    return out_len;
}

uint32_t etc_config_buf_set_in(const char *in, uint32_t in_len,
                               const char *section,
                               const char *key, const char *value,
                               char *out, uint32_t out_cap) {
    int64_t n = set_core(in, in_len, section, key, value, out, out_cap);
    return n < 0 ? 0 : (uint32_t)n;
}

uint32_t etc_config_buf_set(const char *in, uint32_t in_len,
                            const char *key, const char *value,
                            char *out, uint32_t out_cap) {
    return etc_config_buf_set_in(in, in_len, 0, key, value, out, out_cap);
}

// Pass 2 copies every line plus a '\n' (so at most in_len + 1: only the
// last line can lack one), and adds at most one key line -- replacing
// one, inserting one, or opening a section with "\n[" name "]\n" before
// it. A renamed key's old line only ever goes away. Then the NUL.
uint32_t etc_config_buf_set_room(uint32_t in_len, const char *section,
                                 const char *key, const char *value) {
    uint64_t room = (uint64_t)in_len + 1
                  + (key ? k_strlen(key) : 0) + 1 + (value ? k_strlen(value) : 0) + 1
                  + (section ? k_strlen(section) + 4 : 0)
                  + 1;
    return room > 0xFFFFFFFFull ? 0 : (uint32_t)room;
}

char *etc_config_buf_set_alloc(const char *in, uint32_t in_len,
                               const char *section,
                               const char *key, const char *value,
                               const struct etc_config_mem *mem,
                               uint32_t *out_len) {
    if (out_len) *out_len = 0;
    if (!mem || !mem->alloc || !mem->free) return 0;
    if (!in) in_len = 0;
    uint32_t room = etc_config_buf_set_room(in_len, section, key, value);
    char *out = room ? mem->alloc(room) : 0;
    if (!out) return 0;
    int64_t n = set_core(in, in_len, section, key, value, out, room);
    if (n < 0) {
        mem->free(out);
        return 0;
    }
    if (out_len) *out_len = (uint32_t)n;
    return out;
}

// ---- the document's memory -------------------------------------------

void etc_config_buf_free(struct etc_config_buf *buf) {
    if (!buf) return;
    if (buf->data && buf->mem && buf->mem->free) buf->mem->free(buf->data);
    buf->data = 0;
    buf->size = 0;
    buf->valid = 0;
    buf->mem = 0;
}

int etc_config_buf_copy(struct etc_config_buf *buf, const char *doc, uint32_t len,
                        const struct etc_config_mem *mem) {
    if (!buf) return 0;
    buf->data = 0;
    buf->size = 0;
    buf->valid = 0;
    buf->mem = 0;
    if (!mem || !mem->alloc || (!doc && len)) return 0;
    char *d = mem->alloc((unsigned long)len + 1);
    if (!d) return 0;
    if (len) k_memcpy(d, doc, len);
    d[len] = '\0';
    buf->data = d;
    buf->size = len;
    buf->valid = 1;
    buf->mem = mem;
    return 1;
}
