#ifndef ULIB_UWMF_H
#define ULIB_UWMF_H
#include <stddef.h>
#include <stdint.h>
// Windows metafiles (MS-WMF): a 16-bit metafile's records, decoded and
// checked, one at a time, for a caller that draws them -- w16's
// PlayMetaFile into a GDI DC, or an image viewer into a canvas. The
// placeable header (0x9AC6CDD7, a .WMF file's) is read when there.
//
// PARSES UNTRUSTED INPUT: every record's size is checked against the
// bytes given, every argument a record is read for lies inside it, and a
// count is checked before what it counts. It allocates only the decoded
// record's arrays, freed by uwmf_close().
//
// THE ARGUMENTS ARE IN GDI'S ORDER. A record stores its words last
// argument first; `v` has them as the GDI call takes them, so a
// META_RECTANGLE's v is left, top, right, bottom.
//
// THE OBJECT TABLE IS MS-WMF's: each Create record takes the lowest free
// slot (`slot`), SelectObject and DeleteObject name one. The caller keeps
// what it made for each slot.

#define UWMF_SETBKCOLOR           0x0201
#define UWMF_SETBKMODE            0x0102
#define UWMF_SETMAPMODE           0x0103
#define UWMF_SETROP2              0x0104
#define UWMF_SETRELABS            0x0105
#define UWMF_SETPOLYFILLMODE      0x0106
#define UWMF_SETSTRETCHBLTMODE    0x0107
#define UWMF_SETTEXTCHAREXTRA     0x0108
#define UWMF_SETTEXTCOLOR         0x0209
#define UWMF_SETTEXTJUSTIFICATION 0x020A
#define UWMF_SETWINDOWORG         0x020B
#define UWMF_SETWINDOWEXT         0x020C
#define UWMF_SETVIEWPORTORG       0x020D
#define UWMF_SETVIEWPORTEXT       0x020E
#define UWMF_OFFSETWINDOWORG      0x020F
#define UWMF_SCALEWINDOWEXT       0x0410
#define UWMF_OFFSETVIEWPORTORG    0x0211
#define UWMF_SCALEVIEWPORTEXT     0x0412
#define UWMF_LINETO               0x0213
#define UWMF_MOVETO               0x0214
#define UWMF_EXCLUDECLIPRECT      0x0415
#define UWMF_INTERSECTCLIPRECT    0x0416
#define UWMF_ARC                  0x0817
#define UWMF_ELLIPSE              0x0418
#define UWMF_FLOODFILL            0x0419
#define UWMF_PIE                  0x081A
#define UWMF_RECTANGLE            0x041B
#define UWMF_ROUNDRECT            0x061C
#define UWMF_PATBLT               0x061D
#define UWMF_SAVEDC               0x001E
#define UWMF_SETPIXEL             0x041F
#define UWMF_OFFSETCLIPRGN        0x0220
#define UWMF_TEXTOUT              0x0521
#define UWMF_POLYGON              0x0324
#define UWMF_POLYLINE             0x0325
#define UWMF_ESCAPE               0x0626
#define UWMF_RESTOREDC            0x0127
#define UWMF_SELECTCLIPREGION     0x012C
#define UWMF_SELECTOBJECT         0x012D
#define UWMF_SETTEXTALIGN         0x012E
#define UWMF_CHORD                0x0830
#define UWMF_SETMAPPERFLAGS       0x0231
#define UWMF_EXTTEXTOUT           0x0A32
#define UWMF_SETDIBTODEV          0x0D33
#define UWMF_SELECTPALETTE        0x0234
#define UWMF_REALIZEPALETTE       0x0035
#define UWMF_POLYPOLYGON          0x0538
#define UWMF_DIBBITBLT            0x0940
#define UWMF_DIBSTRETCHBLT        0x0B41
#define UWMF_DIBCREATEPATTERNBRUSH 0x0142
#define UWMF_STRETCHDIB           0x0F43
#define UWMF_DELETEOBJECT         0x01F0
#define UWMF_CREATEPALETTE        0x00F7
#define UWMF_CREATEPATTERNBRUSH   0x01F9
#define UWMF_CREATEPENINDIRECT    0x02FA
#define UWMF_CREATEFONTINDIRECT   0x02FB
#define UWMF_CREATEBRUSHINDIRECT  0x02FC
#define UWMF_CREATEREGION         0x06FF

struct uwmf_rec {
    unsigned func;                 // UWMF_*; one this header does not name is passed on with its words
    int16_t v[16];                 // the plain word arguments, GDI's order
    int nv;
    uint32_t color;                // a COLORREF (SetBkColor, SetTextColor, SetPixel, a pen, a brush)
    uint32_t rop;                  // a ROP3 (PatBlt, the DIB blits) or the ROP2 / mode in v[0]
    int slot;                      // the object table's slot: made, selected or deleted; -1 none
    // CREATEPENINDIRECT: style, width (v); CREATEBRUSHINDIRECT: style, hatch (v)
    // CREATEFONTINDIRECT: a LOGFONT's 18 bytes of fields as v (height, width,
    // escapement, orientation, weight, then italic.. pitch_family one each) and its face
    char face[32];
    const int16_t *pt;             // POLYGON, POLYLINE, POLYPOLYGON: x, y pairs...
    int npt;                       // ...how many points
    const uint16_t *counts;        // POLYPOLYGON: the points each polygon takes
    int ncounts;
    const char *text;              // TEXTOUT, EXTTEXTOUT: the characters (no NUL)...
    int ntext;
    const int16_t *dx;             // ...and EXTTEXTOUT's advances, NULL when the record has none
    int16_t rect[4];               // EXTTEXTOUT's clip or opaque rectangle, when its options ask for one
    const uint8_t *dib;            // the DIB records: a BITMAPINFO and its bits, as stored...
    uint32_t ndib;                 // ...and their bytes; the caller checks the header
    const uint8_t *words;          // the record's argument bytes as stored, for a record not decoded
    uint32_t nwords;
};

struct uwmf {
    const uint8_t *p;
    size_t n, at;
    int placeable;                 // a placeable header was there:
    int16_t bounds[4];             // its bounding box, in the metafile's units...
    unsigned inch;                 // ...and how many of them make an inch
    unsigned nobjects;             // the header's object count: the table's size
    uint8_t *used;                 // the object table, a byte a slot
    void *scratch;                 // the decoded record's arrays (aligned copies)
    size_t scratch_cap;
};

// 0, or -1 when `data` is no metafile (or its header lies).
int uwmf_open(struct uwmf *m, const void *data, size_t n);
// The next record into *r: 1, 0 at the end (META_EOF, or the bytes run
// out exactly), -1 for a malformed record -- stop drawing there.
int uwmf_next(struct uwmf *m, struct uwmf_rec *r);
void uwmf_close(struct uwmf *m);
#endif
