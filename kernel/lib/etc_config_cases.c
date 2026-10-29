// The shared case table -- see api/etc_config_cases.h for why it is
// shared rather than living in the KTEST.
#include "etc_config_cases.h"
#include "string.h"

// 70 characters, over ETC_CONFIG_SECTION_MAX: a name this long is not a
// header, so the key after it stays at top level.
#define LONG_NAME "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

const struct etc_config_get_case etc_get_cases[] = {
    { "an unsectioned file is all top level",
      "a=1\nb=2\n", 0, "b", "2" },
    { "top level stops at the first header",
      "a=1\n[s]\nb=2\n", 0, "b", 0 },
    { "a key inside its section",
      "a=1\n[s]\nb=2\n", "s", "b", "2" },
    { "a top-level key is not visible from a section",
      "a=1\n[s]\nb=2\n", "s", "a", 0 },
    { "a section that is not there is a miss",
      "a=1\n[s]\nb=2\n", "other", "b", 0 },
    { "a repeated header is ONE section",
      "[s]\na=1\n[t]\nx=0\n[s]\nb=2\n", "s", "b", "2" },
    { "a header may carry spaces and a comment",
      "[ Desktop Entry ] # the only section\nName=About\n",
      "Desktop Entry", "Name", "About" },
    { "a commented-out header is not a header",
      "#[s]\na=1\n", 0, "a", "1" },
    { "brackets in a value are not a header",
      "a=[x]\nb=2\n", 0, "b", "2" },
    { "an empty header is not a header",
      "[]\na=1\n", 0, "a", "1" },
    { "a value keeps its interior spaces",
      "[s]\nk =  a b  \n", "s", "k", "a b" },
    { "a section name over the cap is not a header",
      "[" LONG_NAME "]\na=1\n", 0, "a", "1" },
    { "a section holds keys of the same name as another's",
      "[a]\nx=1\n[b]\nx=2\n", "b", "x", "2" },
    // The dotted names (api/etc_renamed.h): font.size was font_size.
    { "a renamed key is read under its old name when the new is absent",
      "font_size=16\n", 0, "font.size", "16" },
    { "...and the new key wins when both are there",
      "font_size=16\nfont.size=20\n", 0, "font.size", "20" },
    { "an old name is not an alias in the other direction",
      "font.size=20\n", 0, "font_size", 0 },
};
const int etc_get_case_count = (int)(sizeof etc_get_cases / sizeof etc_get_cases[0]);

const struct etc_config_set_case etc_set_cases[] = {
    { "a new key lands at the end of its own section",
      "[a]\nx=1\n\n[b]\ny=2\n", "a", "z", "3",
      "[a]\nx=1\nz=3\n\n[b]\ny=2\n" },
    { "a trailing comment block stays with the header below it",
      "[a]\nx=1\n# about b\n[b]\ny=2\n", "a", "z", "3",
      "[a]\nx=1\nz=3\n# about b\n[b]\ny=2\n" },
    { "an existing key is replaced where it stands",
      "[a]\nx=1\ny=2\n", "a", "x", "9", "[a]\nx=9\ny=2\n" },
    { "the same key in another section is untouched",
      "[a]\nx=1\n[b]\nx=2\n", "b", "x", "9", "[a]\nx=1\n[b]\nx=9\n" },
    { "a missing section is appended with its header",
      "[a]\nx=1\n", "b", "y", "2", "[a]\nx=1\n\n[b]\ny=2\n" },
    { "a section is created in an empty document",
      "", "a", "x", "1", "[a]\nx=1\n" },
    { "a document with no trailing newline still gets a blank line",
      "a=1", "b", "y", "2", "a=1\n\n[b]\ny=2\n" },
    { "a top-level key goes ABOVE the first header",
      "a=1\n[s]\nb=2\n", 0, "c", "3", "a=1\nc=3\n[s]\nb=2\n" },
    { "top level in a document that opens with a header",
      "[s]\nb=2\n", 0, "c", "3", "c=3\n[s]\nb=2\n" },
    { "an unsectioned document behaves as it did before sections",
      "a=1\n", 0, "b", "2", "a=1\nb=2\n" },
    { "comments and blank lines elsewhere survive",
      "# top\n\n[a]\nx=1\n", "a", "y", "2", "# top\n\n[a]\nx=1\ny=2\n" },
    { "a key is removed from its section",
      "[a]\nx=1\ny=2\n", "a", "x", 0, "[a]\ny=2\n" },
    { "removing the last key leaves the header",
      "[a]\nx=1\n", "a", "x", 0, "[a]\n" },
    { "removing a key that is not there refuses",
      "[a]\nx=1\n", "a", "z", 0, 0 },
    { "removing from a section that is not there refuses",
      "[a]\nx=1\n", "b", "x", 0, 0 },
    { "a section name that could not be read back is refused",
      "[a]\nx=1\n", "a]b", "z", "3", 0 },
    { "a write replaces a renamed key's OLD line where it stands",
      "a=1\nfont_size=16\nb=2\n", 0, "font.size", "20", "a=1\nfont.size=20\nb=2\n" },
    { "...and drops the old line when the new one is there too",
      "font_size=16\nfont.size=18\n", 0, "font.size", "20", "font.size=20\n" },
    { "removing a renamed key removes its old line",
      "a=1\nfont_size=16\n", 0, "font.size", 0, "a=1\n" },
};
const int etc_set_case_count = (int)(sizeof etc_set_cases / sizeof etc_set_cases[0]);

const struct etc_config_sections_case etc_sections_cases[] = {
    { "a file with no headers declares no sections", "a=1\nb=2\n", "" },
    { "headers in file order", "[a]\nx=1\n[b]\n", "a|b" },
    { "a repeated name is reported once",
      "[a]\nx=1\n[b]\ny=2\n[a]\nz=3\n", "a|b" },
    { "a name is trimmed", "[ Desktop Entry ]\n", "Desktop Entry" },
    { "a commented-out header is not counted", "#[x]\n[y]\n", "y" },
};
const int etc_sections_case_count =
    (int)(sizeof etc_sections_cases / sizeof etc_sections_cases[0]);

// A document is loaded rather than read: these cases are about the
// PARSER, and going through a file would test the filesystem as well
// and could not run before one is mounted.
int etc_get_case_run(const struct etc_config_get_case *c,
                     const struct etc_config_mem *mem, char *got, uint32_t cap) {
    struct etc_config_buf buf;
    if (!etc_config_buf_copy(&buf, c->doc, (uint32_t)k_strlen(c->doc), mem)) {
        k_strlcpy(got, "(no memory)", cap);
        return 0;
    }
    int found = etc_config_buf_get_in(&buf, c->section, c->key, got, cap);
    etc_config_buf_free(&buf);
    if (!c->want) return !found;
    return found && k_strcmp(got, c->want) == 0;
}

// Both writers: the sized call into exactly etc_config_buf_set_room()
// bytes -- a bound one byte short refuses some case here -- and the
// allocating one, which must produce the same document.
int etc_set_case_run(const struct etc_config_set_case *c,
                     const struct etc_config_mem *mem, char *got, uint32_t got_cap) {
    uint32_t in_len = (uint32_t)k_strlen(c->doc);
    uint32_t room = etc_config_buf_set_room(in_len, c->section, c->key, c->value);
    char *out = room ? mem->alloc(room) : 0;
    if (!out) {
        k_strlcpy(got, "(no memory)", got_cap);
        return 0;
    }
    uint32_t n = etc_config_buf_set_in(c->doc, in_len, c->section, c->key, c->value,
                                       out, room);
    k_strlcpy(got, n ? out : "", got_cap);
    int sized_ok = !c->want ? n == 0
                 : n == (uint32_t)k_strlen(c->want) && k_strcmp(out, c->want) == 0;
    mem->free(out);

    uint32_t an = 0;
    char *a = etc_config_buf_set_alloc(c->doc, in_len, c->section, c->key, c->value,
                                       mem, &an);
    int alloc_ok = !c->want ? a == 0
                 : a && an == (uint32_t)k_strlen(c->want) && k_strcmp(a, c->want) == 0;
    if (a && sized_ok && !alloc_ok) k_strlcpy(got, a, got_cap);
    if (a) mem->free(a);
    return sized_ok && alloc_ok;
}

int etc_sections_case_run(const struct etc_config_sections_case *c,
                          const struct etc_config_mem *mem, char *got, uint32_t cap) {
    if (cap == 0) return 0;
    got[0] = '\0';
    struct etc_config_buf buf;
    if (!etc_config_buf_copy(&buf, c->doc, (uint32_t)k_strlen(c->doc), mem)) return 0;

    char name[ETC_CONFIG_SECTION_MAX];
    uint32_t len = 0;
    int ok = 1;
    int n = etc_config_section_count(&buf);
    for (int i = 0; i < n && ok; i++) {
        if (!etc_config_section_name(&buf, i, name, sizeof name)) { ok = 0; break; }
        uint32_t nl = (uint32_t)k_strlen(name);
        if (len + (i ? 1u : 0u) + nl + 1 > cap) { ok = 0; break; }
        if (i) got[len++] = '|';
        k_memcpy(got + len, name, nl);
        len += nl;
        got[len] = '\0';
    }
    etc_config_buf_free(&buf);
    return ok && k_strcmp(got, c->want) == 0;
}

static uint32_t put_u32(char *p, uint32_t v) {
    char tmp[10];
    uint32_t n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (uint32_t i = 0; i < n; i++) p[i] = tmp[n - 1 - i];
    return n;
}

char *etc_config_big_doc(uint32_t bytes, const struct etc_config_mem *mem, uint32_t *len) {
    // A line is at most "pad" + 10 digits + "=" + 10 digits + "\n".
    uint32_t cap = bytes + 64;
    char *d = mem && mem->alloc ? mem->alloc(cap) : 0;
    if (!d) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; n < bytes; i++) {
        k_memcpy(d + n, "pad", 3);
        n += 3;
        n += put_u32(d + n, i);
        d[n++] = '=';
        n += put_u32(d + n, i);
        d[n++] = '\n';
    }
    k_memcpy(d + n, "last=end\n", 9);
    n += 9;
    d[n] = '\0';
    if (len) *len = n;
    return d;
}
