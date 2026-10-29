#ifndef ETC_CONFIG_CASES_H
#define ETC_CONFIG_CASES_H

#include <stdint.h>
#include "etc_config.h"

// The parser's cases, COMPILED INTO BOTH RINGS and asserted from both.
//
// Same arrangement, and the same reason, as klineedit_cases.h:
// kernel/lib/etc_config.c is built twice -- once for the kernel image
// and once into libuapp.a, where /bin/netd, the File Manager and the WM
// read the same /etc documents the kernel does. A KTEST runs inside the
// kernel and cannot see that second build at all, so what these pin
// down is the LINK and the second compilation as much as the logic.
//
// Add a case once; both rings gain it.

struct etc_config_get_case {
    const char *name;
    const char *doc;      // the whole document
    const char *section;  // 0 = top level
    const char *key;
    const char *want;     // the value, or 0 if the lookup must MISS
};

struct etc_config_set_case {
    const char *name;
    const char *doc;
    const char *section;  // 0 = top level
    const char *key;
    const char *value;    // 0 = remove the key
    const char *want;     // the resulting document, or 0 if it must REFUSE
};

struct etc_config_sections_case {
    const char *name;
    const char *doc;
    const char *want;     // the distinct section names, '|'-joined
};

extern const struct etc_config_get_case      etc_get_cases[];
extern const int                             etc_get_case_count;
extern const struct etc_config_set_case      etc_set_cases[];
extern const int                             etc_set_case_count;
extern const struct etc_config_sections_case etc_sections_cases[];
extern const int                             etc_sections_case_count;

// Each runner takes the CALLER's allocator, which is the point of
// running them twice: a document is loaded at its own size through it,
// a rewrite is sized by etc_config_buf_set_room() and also made through
// etc_config_buf_set_alloc(), and each must agree with the case -- so
// the ring that did not write the bound still checks it.
//
// All three return 1 when the case held, and fill `got` either way so a
// failure can say what it actually produced.
int etc_get_case_run(const struct etc_config_get_case *c,
                     const struct etc_config_mem *mem, char *got, uint32_t cap);
int etc_set_case_run(const struct etc_config_set_case *c,
                     const struct etc_config_mem *mem, char *got, uint32_t got_cap);
int etc_sections_case_run(const struct etc_config_sections_case *c,
                          const struct etc_config_mem *mem, char *got, uint32_t cap);

// A document of at least `bytes` bytes, every line a `pad<N>=<N>` key so
// the parser walks all of it, ending in `last=end` -- the key a file read
// short would lose. Allocated from `mem`; NULL when it cannot be. *len is
// its length. What the 3/5/64 KiB tests in both rings build their file from.
char *etc_config_big_doc(uint32_t bytes, const struct etc_config_mem *mem, uint32_t *len);

#endif
