// Folder options -- see ufolderopts.h.
#include "lib/ufolderopts.h"
#include <string.h>

static const char *const CLICK_WORDS[] = { "double", "single" };

// The defaults: what Explorer and Dolphin ship.
#define O struct ufolderopts
static const struct upref KEYS[] = {
    UPREF_WORD_KEY("click",          O, single_click,   CLICK_WORDS, 2, 0),
    UPREF_BOOL_KEY("thumbs",         O, thumbs,         1),
    UPREF_INT_KEY("thumb_max_mb",    O, thumb_max_mb,   1, 1024, 16),
    UPREF_BOOL_KEY("hidden",         O, hidden,         0),
    UPREF_BOOL_KEY("extensions",     O, extensions,     1),
    UPREF_BOOL_KEY("confirm_delete", O, confirm_delete, 1),
};
#undef O
const struct uprefs ufolderopts_prefs = { UFOLDEROPTS_CONF, KEYS, (int)(sizeof KEYS / sizeof KEYS[0]) };

void ufolderopts_load(struct ufolderopts *o) {
    memset(o, 0, sizeof *o);
    uprefs_load(&ufolderopts_prefs, o);
}

int ufolderopts_thumb_ok(const struct ufolderopts *o, uint64_t size) {
    return o->thumbs && size <= (uint64_t)o->thumb_max_mb * 1024 * 1024;
}
