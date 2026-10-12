#ifndef BROWSER_NS_BITMAP_H
#define BROWSER_NS_BITMAP_H

// NetSurf's `struct bitmap`, for the Web Browser: 0xAARRGGBB words in
// lib/uimg.h's layout, plus one cached copy at the size it was last
// drawn, since a page draws an image at the size its box says.
#include <stdbool.h>
#include <stdint.h>
#include "lib/uimg.h"

struct bitmap {
    int w, h;
    uint32_t *px;
    bool opaque;
    uint32_t *scaled;   // px at sw x sh, or NULL
    int sw, sh;
};

struct gui_bitmap_table;
extern struct gui_bitmap_table *ns_bitmap_table;

// Tells NetSurf's decoders to write that layout. Once, before any image.
void ns_bitmap_set_format(void);
// The pixels at w x h -- `px` itself, or the cached scaled copy, made
// now if the size changed. NULL when scaling failed (out of memory).
const uint32_t *ns_bitmap_at_size(struct bitmap *b, int w, int h);

#endif
