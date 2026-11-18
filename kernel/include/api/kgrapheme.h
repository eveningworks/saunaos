#ifndef KGRAPHEME_H
#define KGRAPHEME_H

// GRAPHEME CLUSTERS: what a person calls one character, for BOTH RINGS --
// a letter with its accents, a family emoji (three people joined by U+200D),
// a flag (two regional indicators), a skin-toned thumb. The caret steps over
// one, the terminal gives one its cells, a measurement counts one.
//
// UAX #29's EXTENDED grapheme clusters (kgrapheme_table.h, generated), rules
// GB3-GB13 -- the shape ICU's BreakIterator and Pango's log attrs give --
// with one deliberate difference: GB9c, the Indic conjunct rule, is NOT
// applied, so a Devanagari conjunct is two clusters, as SaunaOS does no
// Indic shaping to draw it as one. kernel/lib/kgrapheme.c; compiled into
// the kernel and libbase, like klineedit.c, which steps with it.
//
// Every index is a BYTE offset into UTF-8 on a character boundary, decoded
// with the transition rule (api/kutf8.h): a stray byte is a character of
// its own and of class Other.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum kgrapheme_class {
    KGB_CR, KGB_LF, KGB_CONTROL, KGB_EXTEND, KGB_ZWJ, KGB_RI, KGB_PREPEND, KGB_SPACINGMARK,
    KGB_L, KGB_V, KGB_T, KGB_EP,      // EP: Extended_Pictographic (and GCB Other)
    KGB_LV, KGB_LVT, KGB_OTHER,
};

enum kgrapheme_class kgrapheme_class(uint32_t cp);

// Where the cluster STARTING at byte `i` ends. `n` bounds the buffer; < 0
// means NUL-terminated. Returns `i` only at the end.
int kgrapheme_next(const char *s, int n, int i);

// Where the cluster ENDING at byte `i` starts -- the inverse of
// kgrapheme_next(), agreeing with it on every input. 0 at the start. Reads
// no byte at or past `i`, so the buffer need not be NUL-terminated there.
int kgrapheme_prev(const char *s, int i);

// Where BACKSPACE at `i` deletes back to (Pango's backspace_deletes_character
// rule): an EMOJI cluster -- a ZWJ sequence, a flag, a skin tone, a keycap, a
// tag sequence, anything with U+FE0F, or one starting with a pictograph --
// goes whole, and any other cluster loses only its last code point, so
// "e + U+0301" backspaces to a plain "e".
int kgrapheme_backspace(const char *s, int i);

// 1 when the cluster [i, end) is an emoji cluster in kgrapheme_backspace()'s
// sense: what a terminal gives two cells and a picker lists as one emoji.
int kgrapheme_is_emoji(const char *s, int i, int end);

#ifdef __cplusplus
}
#endif

#endif
