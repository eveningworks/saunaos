#ifndef ULIB_UHLP_H
#define ULIB_UHLP_H

#include <stddef.h>
#include <stdint.h>

// uhlp -- WINDOWS HELP FILES (.HLP, the WinHelp 3.0/3.1 format) read in
// ring 3: the internal directory, the topics, their text and formatting,
// the pictures, and the tables that find a topic (context hashes, map
// numbers, keywords). The reader is `/bin/hlpview`; `uui_helpview` draws
// what this returns.
//
// **IT PARSES UNTRUSTED INPUT.** Every offset and length read from the
// file is checked against the file before use, a B+ tree walk is bounded
// by the page count, and the decompressors write to a buffer sized from
// the stream they read, never from a size the file claims. The caller
// pledges before the first byte (docs/conventions/kernel.md).
//
// **NOTHING IS CAPPED.** Topics, fonts, links, runs and pictures are
// grown to fit what the file holds.
//
// A topic is turned into a DOCUMENT (struct uhlp_doc): paragraphs of
// runs, with the links, pictures and table layout resolved, in the
// file's own units (HALF-POINTS: 144 to the inch). Drawing, wrapping and
// hit-testing are the widget's.
//
// What is not here: the 3.1 "MVB" and "new font" tables (a file written
// by a later compiler; opened with fonts defaulted -- none of Windows 3.11's
// own files, English or Finnish, has one, so there is nothing to check a
// reader against), and the Windows 95 phrase and full-text formats beyond
// |Phrases / |PhrIndex.

#define UHLP_LINK_JUMP   0
#define UHLP_LINK_POPUP  1
#define UHLP_LINK_MACRO  2

#define UHLP_FONT_BOLD      0x01
#define UHLP_FONT_ITALIC    0x02
#define UHLP_FONT_UNDERLINE 0x04
#define UHLP_FONT_STRIKE    0x08
#define UHLP_FONT_DOUBLE    0x10
#define UHLP_FONT_SMALLCAPS 0x20

// FF_* of a font's family byte, as the file stores it.
#define UHLP_FAMILY_MODERN  1   // fixed pitch
#define UHLP_FAMILY_ROMAN   2   // serif
#define UHLP_FAMILY_SWISS   3   // sans
#define UHLP_FAMILY_SCRIPT  4
#define UHLP_FAMILY_DECOR   5

struct uhlp;

struct uhlp_font {
    char face[32];
    int half_pts;          // the size, in half-points
    uint8_t flags;         // UHLP_FONT_*
    uint8_t family;        // UHLP_FAMILY_*
    uint32_t rgb;          // 0xRRGGBB
};

// A window the file names (|SYSTEM record 6): where its topics open.
struct uhlp_window {
    char name[16];
    char caption[51];
    int x, y, w, h;        // 0..1023 of the screen; -1 = the viewer's choice
    uint32_t sr_rgb;       // the scrolling region's background, 0xFFFFFFFF = none
    uint32_t nsr_rgb;      // the non-scrolling region's
};

struct uhlp_link {
    uint8_t kind;          // UHLP_LINK_*
    uint8_t colour;        // 0 when the link is drawn in the text's own colour (a hidden hotspot)
    uint32_t hash;         // JUMP / POPUP: a context hash (or a topic offset when `by_offset`)
    uint8_t by_offset;
    int file;              // doc->pool offset of an EXTERNAL file name, -1 = this file
    int window;            // index into uhlp_windows(), -1 = the current one
    int macro;             // MACRO: doc->pool offset of the macro text
};

enum { UHLP_RUN_TEXT, UHLP_RUN_LINEBREAK, UHLP_RUN_TAB, UHLP_RUN_PICTURE, UHLP_RUN_HYPHEN, UHLP_RUN_NBSP };

struct uhlp_run {
    uint8_t kind;          // UHLP_RUN_*
    uint8_t place;         // PICTURE: 0 inline, 1 left (text wraps), 2 right
    uint16_t font;         // index into uhlp_fonts()
    int link;              // index into doc->link, or -1
    int text;              // TEXT: doc->pool offset, NUL-terminated
    int len;               // TEXT: bytes
    int pic;               // PICTURE: index into doc->pic
};

struct uhlp_tab { int pos; uint8_t kind; };   // kind 0 left, 1 right, 2 centre

struct uhlp_para {
    int run0, nrun;
    int left, right, first;      // indents; `first` is added to `left` on the first line
    int before, after;           // space above and below
    int spacing;                 // line spacing, 0 = the font's
    uint8_t align;               // 0 left, 1 right, 2 centre
    uint8_t border;              // 0x01 box 0x02 top 0x04 left 0x08 bottom 0x10 right 0x20 thick 0x40 double
    int border_w;
    int tab0, ntab;              // into doc->tab
    int table;                   // index into doc->table, -1 outside a table
    int column;                  // in a table: the column
    int row;                     // in a table: the row (counted from 0)
    uint8_t scrolls;             // 0 = in the non-scrolling region (stays on top)
    uint8_t keep;
};

struct uhlp_column { int gap, width; };

struct uhlp_table {
    int ncol;
    int width;                   // 32767 = the whole window
    int type;
    int col0;                    // into doc->col
    int nrow;
};

#define UHLP_PIC_DIB 0           // `argb`, w * h, top row first
#define UHLP_PIC_WMF 1           // `wmf`, a Windows metafile (no placeable header)

struct uhlp_hot { int x, y, w, h; int link; };

struct uhlp_pic {
    uint8_t kind;
    int w, h;                    // pixels (DIB) or the picture's own extent (WMF)
    int mapmode;                 // WMF: the mapping mode
    uint32_t *argb;
    uint8_t *wmf;
    size_t wmf_len;
    int transparent;             // DIB: the last palette entry is see-through
    int hot0, nhot;              // into doc->hot
};

struct uhlp_doc {
    int topic;                   // the topic this is
    char *title;
    char *pool;                  // every string: text runs, macro text, file names
    size_t pool_len;
    struct uhlp_run *run; int nrun;
    struct uhlp_para *para; int npara;
    struct uhlp_link *link; int nlink;
    struct uhlp_tab *tab; int ntab;
    struct uhlp_table *table; int ntable;
    struct uhlp_column *col; int ncolumn;
    struct uhlp_pic *pic; int npic;
    struct uhlp_hot *hot; int nhot;
    char **macro; int nmacro;    // the topic's own macros, run when it opens
};

// Opens a help file already in memory. The bytes are COPIED. Returns NULL
// when it is not a help file this reader understands; `err`, if given,
// gets a static reason.
struct uhlp *uhlp_open(const void *data, size_t len, const char **err);
void uhlp_close(struct uhlp *h);

const char *uhlp_title(const struct uhlp *h);        // "" when it has none
const char *uhlp_copyright(const struct uhlp *h);    // NULL when it has none
int uhlp_version(const struct uhlp *h);              // the minor: 15 / 16 is Windows 3.0, 21 is 3.1

int uhlp_nfonts(const struct uhlp *h);
const struct uhlp_font *uhlp_font(const struct uhlp *h, int i);
int uhlp_nwindows(const struct uhlp *h);
const struct uhlp_window *uhlp_window(const struct uhlp *h, int i);

// The macros the file runs when it opens (|SYSTEM record 4), by number.
int uhlp_nmacros(const struct uhlp *h);
const char *uhlp_macro(const struct uhlp *h, int i);

// --- topics -----------------------------------------------------------------
//
// Topics are numbered 0..n-1 in FILE ORDER, which is the browse order
// of a file with no browse sequences. -1 is "no such topic".

int uhlp_ntopics(const struct uhlp *h);
const char *uhlp_topic_title(const struct uhlp *h, int topic);     // may be ""
int uhlp_contents(const struct uhlp *h);                           // the contents topic, or 0
int uhlp_topic_by_hash(const struct uhlp *h, uint32_t hash);       // a jump's target
int uhlp_topic_by_map(const struct uhlp *h, uint32_t map);         // WinHelp(HELP_CONTEXT)'s number
int uhlp_topic_by_offset(const struct uhlp *h, uint32_t offset, uint32_t *rel);
int uhlp_topic_prev(const struct uhlp *h, int topic);              // the browse sequence; -1 at the end
int uhlp_topic_next(const struct uhlp *h, int topic);
uint32_t uhlp_hash(const char *context_id);                        // HASH of a context string
// A context hash that names `topic` (0 when none does), and a string of lower-case letters that
// HASHES to `hash`: WinHelp's command line takes a context ID, so a topic reached by number or by
// keyword -- which the file only stores as a hash -- is named to Microsoft's own WINHELP.EXE by a
// string it hashes back to the same place. 1 on success.
uint32_t uhlp_topic_hash(const struct uhlp *h, int topic);
int uhlp_id_for_hash(uint32_t hash, char *out, size_t cap);

// --- keywords (the Index) -------------------------------------------------------

int uhlp_nkeywords(const struct uhlp *h);
const char *uhlp_keyword(const struct uhlp *h, int i);             // in the file's sorted order
// The topics a keyword names (a pointer into the reader's own array).
const int *uhlp_keyword_topics(const struct uhlp *h, int i, int *n);

// One topic as a document. NULL on a bad topic or no memory.
struct uhlp_doc *uhlp_render(const struct uhlp *h, int topic);
void uhlp_doc_free(struct uhlp_doc *d);
const char *uhlp_doc_str(const struct uhlp_doc *d, int pool_off);

// The plain text of a topic (runs joined, paragraphs separated by '\n'),
// allocated, for Find. NULL on failure.
char *uhlp_topic_text(const struct uhlp *h, int topic);

#endif
