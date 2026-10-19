// ne.h -- the NE ("new executable") format of Windows 3.x programs and
// libraries, read for w16 (docs/w16-design.md). PARSES UNTRUSTED INPUT:
// every offset is checked against the image before it is read, and a
// malformed file is REJECTED with a reason, never guessed at. Nothing
// here allocates; the caller owns the image and the struct.
#ifndef W16_NE_H
#define W16_NE_H

#include <stddef.h>
#include <stdint.h>

#define NE_MAX_SEGMENTS 255   // a relocation names its segment in a byte
#define NE_MAX_MODULES  64

// Segment flags.
#define NE_SEG_DATA     0x0001
#define NE_SEG_MOVABLE  0x0010
#define NE_SEG_PRELOAD  0x0040
#define NE_SEG_RELOCS   0x0100

// Relocation source types: what the site holds.
#define NE_SRC_LOBYTE   0
#define NE_SRC_SEGMENT  2
#define NE_SRC_FARPTR   3
#define NE_SRC_OFFSET   5

// Relocation target kinds (the low two bits of its flags), plus ADDITIVE:
// without it, the word at the site is the next site of a chain, 0xFFFF
// ending it.
#define NE_REL_INTERNAL    0
#define NE_REL_IMPORT_ORD  1
#define NE_REL_IMPORT_NAME 2
#define NE_REL_OSFIXUP     3
#define NE_REL_ADDITIVE    4

struct ne_segment {
    uint32_t file_off;     // 0: no data in the file, the segment starts zeroed
    uint32_t length;       // bytes in the file (the header's 0 means 64 KiB)
    uint32_t minalloc;     // bytes to allocate (0 means 64 KiB)
    uint16_t flags;
    uint32_t reloc_off;    // where its relocation records start, if NE_SEG_RELOCS
    uint16_t nrelocs;
};

struct ne_reloc {
    uint8_t src;           // NE_SRC_*
    uint8_t flags;         // NE_REL_* | NE_REL_ADDITIVE
    uint16_t site;         // offset in the segment
    uint16_t a;            // internal: segment (low byte); import: module index (1-based)
    uint16_t b;            // internal: offset or entry ordinal; import: ordinal or name offset
};

struct ne_module {
    const uint8_t *img;
    uint32_t size;
    uint32_t ne;           // file offset of the NE header
    char name[64];         // the module name (resident names, ordinal 0)
    uint16_t flags;        // program flags (low byte) and application flags
    uint16_t autodata;     // the automatic data segment (DGROUP), 1-based; 0 none
    uint16_t heap, stack;  // initial local heap and stack sizes
    uint16_t entry_cs, entry_ip;   // CS is a segment number
    uint16_t ss, sp;
    uint16_t expected_win; // 0x030A for Windows 3.10
    uint16_t align_shift;
    uint16_t nseg;
    struct ne_segment seg[NE_MAX_SEGMENTS];
    uint16_t nmod;
    uint16_t modref[NE_MAX_MODULES];   // offsets into the imported-names table
    uint32_t imported;     // file offsets of the tables
    uint32_t entry_table;
    uint32_t entry_len;
    uint32_t resident;
    uint32_t resources;    // the resource table; 0 when there is none
    uint16_t res_shift;    // its alignment shift
    uint16_t nres;         // resources in it, all types
};

// Resource types (a type ID with 0x8000 set is one of these).
#define NE_RT_CURSOR       1
#define NE_RT_BITMAP       2
#define NE_RT_ICON         3
#define NE_RT_MENU         4
#define NE_RT_DIALOG       5
#define NE_RT_STRING       6
#define NE_RT_ACCELERATOR  9
#define NE_RT_GROUP_CURSOR 12
#define NE_RT_GROUP_ICON   14

// One resource. A type or ID is a NUMBER when its name is "" (the file's
// 0x8000 bit), else a NAME, upper-cased as Windows compares them.
struct ne_resource {
    uint16_t type, id;
    char type_name[32], name[32];
    uint32_t off, len;     // in the file, checked against it
};

// Resource i (0-based, every type in table order). 0, or a negative NE_E_*.
int ne_resource_at(const struct ne_module *m, unsigned i, struct ne_resource *out);

// The resource of `type` and `id`: a number when the name is NULL (or
// "#123"), else a name compared without case. Its index, or NE_E_RANGE.
int ne_resource_find(const struct ne_module *m, uint16_t type, const char *type_name,
                     uint16_t id, const char *name, struct ne_resource *out);

// 0, or a negative NE_E_* code; ne_strerror() says which check refused it.
int ne_parse(const uint8_t *img, uint32_t size, struct ne_module *m);
const char *ne_strerror(int err);

// Relocation i of segment seg (1-based). 0, or a negative NE_E_*.
int ne_reloc_at(const struct ne_module *m, unsigned seg, unsigned i, struct ne_reloc *out);

// Module reference idx (1-based) as a NUL-terminated name. 0 or negative.
int ne_module_name(const struct ne_module *m, unsigned idx, char *out, size_t cap);

// Entry ordinal -> segment and offset (a movable entry's segment too). 0
// or negative: no such entry.
int ne_entry(const struct ne_module *m, unsigned ordinal, uint16_t *seg, uint16_t *off);

#define NE_E_SHORT     (-1)   // shorter than the headers say
#define NE_E_NOT_MZ    (-2)
#define NE_E_NOT_NE    (-3)
#define NE_E_TABLE     (-4)   // a table outside the file
#define NE_E_SEGMENTS  (-5)   // too many segments, or one outside the file
#define NE_E_MODULES   (-6)
#define NE_E_RELOC     (-7)   // a relocation table outside the file
#define NE_E_NAME      (-8)   // a name runs past its table
#define NE_E_RANGE     (-9)   // an index out of range
#define NE_E_ENTRY     (-10)  // the entry table is malformed or lacks the ordinal
#define NE_E_RESOURCE  (-11)  // the resource table is malformed

#endif
