#ifndef ULIB_UBINMAP_H
#define ULIB_UBINMAP_H

// A BINARY FILE'S STRUCTURE MAP: what the bytes of an ELF program, a
// DOOM WAD, a TrueType font, a disk image's MBR or GPT, a FAT boot
// sector, a QOI or BMP picture ARE -- a flat outline of nodes, each a
// label, a value and the byte range it covers, at a depth. What a hex
// viewer's structure pane shows (ImHex's pattern view, 010 Editor's
// templates, HxD's data structures); pure code, no drawing.
//
// **THE FORMAT IS SNIFFED FROM THE BYTES, NEVER THE NAME**, through a
// registry table (ubinmap_formats[]): adding a format is adding a row
// and its decoder in a ubinmap_<kind>.c.
//
// **EVERY FIELD IS HOSTILE.** A decoder reads only through the source's
// callback, only inside the file's size, and trusts no length, count or
// offset it reads: a table that claims more than the file holds is
// listed as far as the file goes and the map's `note` says so; a node's
// range is CLIPPED to the file (UBINMAP_CLIPPED). Nodes are allocated to
// fit, up to UBINMAP_NODES_MAX -- past it they are counted in `dropped`
// and the caller logs that, since a crafted count must not cost the
// machine its memory.
#include <stddef.h>
#include <stdint.h>

// Where the bytes come from: `read` copies up to `n` bytes at `off` and
// returns how many it got -- short only at the end of the file
// (base/urange.h's urange_reader is one).
struct ubinmap_src {
    uint64_t size;
    size_t (*read)(void *ctx, uint64_t off, void *buf, size_t n);
    void *ctx;
};

#define UBINMAP_TINTS     8          // tint slots 1..8; 0 is none
#define UBINMAP_NODES_MAX 65536

enum { UBINMAP_CLIPPED = 1 };        // its declared range ran past the end of the file

struct ubinmap_node {
    uint64_t off, len;               // the bytes it covers, inside the file
    uint32_t label, value;           // offsets into the map's `text`
    int16_t depth;                   // 0 for a top-level node
    uint8_t tint;                    // a colour slot for its bytes, 0 for none
    uint8_t flags;                   // UBINMAP_*
};

struct ubinmap {
    char format[80];                 // "ELF64 executable, x86-64"; "" when nothing matched
    const char *kind;                // the registry row's name ("elf"), NULL when none
    struct ubinmap_node *nodes;
    int count, cap;
    char *text;                      // every label and value, NUL-separated
    size_t text_len, text_cap;
    int dropped;                     // nodes past UBINMAP_NODES_MAX, not listed
    int oom;                         // an allocation failed; the map is what fitted
    char note[128];                  // the first thing found wrong with the file, or ""
};

// Decodes `src` into `m` (which it initialises). 1 when a format
// matched -- the map may still be partial, see `note` -- 0 when none
// did, with `m` empty. Free with ubinmap_free() either way.
int  ubinmap_decode(const struct ubinmap_src *src, struct ubinmap *m);
void ubinmap_free(struct ubinmap *m);

const char *ubinmap_label(const struct ubinmap *m, int i);
const char *ubinmap_value(const struct ubinmap *m, int i);
// The node `i` sits under, or -1 at the top.
int  ubinmap_parent(const struct ubinmap *m, int i);
// The DEEPEST node whose bytes include `off` (the last such when two
// overlap), or -1.
int  ubinmap_deepest(const struct ubinmap *m, uint64_t off);
// That node's labels from the top: "ELF header > Entry". 0 when no
// node covers `off`, with `out` "".
int  ubinmap_path(const struct ubinmap *m, uint64_t off, char *out, size_t cap);

// THE TINTED BYTES as disjoint runs, sorted: where tinted nodes overlap
// the deepest wins (the later of two at one depth). `*out` is allocated
// to fit and the caller frees it; returns the count, or -1 for no memory.
struct ubinmap_span {
    uint64_t off, len;
    int tint;
    int node;
};
int  ubinmap_tints(const struct ubinmap *m, struct ubinmap_span **out);
// The run holding `off`, or -1.
int  ubinmap_span_at(const struct ubinmap_span *s, int n, uint64_t off);

// --- the registry ---------------------------------------------------------

struct ubinmap_ctx;
struct ubinmap_format {
    const char *name;                // "elf", "wad", ...
    // 1 when the bytes are this format and the map is filled; 0, having
    // added nothing, when they are not.
    int (*decode)(struct ubinmap_ctx *c);
};
extern const struct ubinmap_format ubinmap_formats[];
extern const int ubinmap_format_count;

#endif
