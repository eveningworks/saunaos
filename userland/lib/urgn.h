#ifndef ULIB_URGN_H
#define ULIB_URGN_H

// A GROWABLE REGION: any number of disjoint rectangles, with the set
// operations Windows' GDI regions have -- intersect, union, subtract, xor
// -- and offset, equality, containment. w16's HRGNs and a DC's clip are
// these (GDI kept RGNDATA, pixman's region32 is the same shape). Unlike
// lib/uregion.h, which the compositor clips with on its hot path, this one
// ALLOCATES and is exact: no cap, no superset on overflow -- a GDI clip
// that errs large draws over what a program excluded.
//
// Rectangles are half-open (x, y, w, h) and never empty. An operation's
// result is coalesced (neighbours with a shared edge and the same span
// merged), so a shape built row by row -- a polygon's spans -- stays a
// few rectangles, not one per row. Every call that grows a region
// answers -1 on ENOMEM and leaves its output EMPTY, never half-made.

#include "uregion.h"   // struct urect

struct urgn {
    int n, cap;
    struct urect *r;
};

void urgn_init(struct urgn *g);                 // empty, nothing allocated
void urgn_free(struct urgn *g);                 // back to empty
int  urgn_set_rect(struct urgn *g, int x, int y, int w, int h);
int  urgn_copy(struct urgn *dst, const struct urgn *src);

// dst = a op b. dst may be a or b.
int  urgn_intersect(struct urgn *dst, const struct urgn *a, const struct urgn *b);
int  urgn_union(struct urgn *dst, const struct urgn *a, const struct urgn *b);
int  urgn_subtract(struct urgn *dst, const struct urgn *a, const struct urgn *b);
int  urgn_xor(struct urgn *dst, const struct urgn *a, const struct urgn *b);

// One rectangle added (unioned in) or cut out.
int  urgn_add_rect(struct urgn *g, int x, int y, int w, int h);
int  urgn_cut_rect(struct urgn *g, int x, int y, int w, int h);

// A rectangle appended as it is, the CALLER promising it is disjoint from the rest -- a scan's
// spans, row by row -- then urgn_tidy() once to coalesce them. Cheaper than urgn_add_rect() per row.
int  urgn_append(struct urgn *g, int x, int y, int w, int h);
void urgn_tidy(struct urgn *g);

void urgn_offset(struct urgn *g, int dx, int dy);
int  urgn_is_empty(const struct urgn *g);
int  urgn_bbox(const struct urgn *g, struct urect *out);   // 0 and a zero rect when empty
long long urgn_area(const struct urgn *g);
int  urgn_contains(const struct urgn *g, int x, int y);
int  urgn_overlaps(const struct urgn *g, int x, int y, int w, int h);   // any of the rect in it
int  urgn_equal(const struct urgn *a, const struct urgn *b);           // the same pixels

#endif
