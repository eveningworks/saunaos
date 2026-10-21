#ifndef UUI_UWMF_DRAW_H
#define UUI_UWMF_DRAW_H

// uwmf_draw -- plays a 16-bit Windows metafile (lib/uwmf.h decodes it) into a box of a ugfx surface:
// the metafile's window (SetWindowOrg/Ext) is mapped onto the box, MM_ANISOTROPIC's way, so the
// picture fills the box whatever units it was recorded in. Help Viewer's pictures are the caller
// (WinHelp stores a metafile picture's suggested size beside it); an image viewer could be the next.
//
// WHAT IT DRAWS: lines, polylines, polygons, rectangles, ellipses, PatBlt, pixels, text (TextOut,
// ExtTextOut with its advances and opaque box) and the DIB records (StretchDIBits, the DIB blits,
// SetDIBitsToDevice), with pens, brushes and fonts made, selected and deleted through the object table,
// and SaveDC/RestoreDC. Palettes, regions, clipping records, escapes, arcs, pies and chords are skipped:
// a picture that needs them draws without those parts.
//
// UNTRUSTED INPUT: lib/uwmf checks every record; here every count is bounded by the record, a DIB's
// header is checked before its bits are used, and nothing is drawn outside the box (a nested clip).

#include <stddef.h>
#include <stdint.h>

struct ugfx_surface;
struct ugfx_font;

// A face for the metafile's font: its LOGFONT face name, a pixel size, bold, italic. NULL as the
// callback draws every font in the session's face.
typedef const struct ugfx_font *(*uwmf_font_fn)(void *ctx, const char *face, int px, int bold, int italic);

// Draws the metafile `data` (n bytes; a placeable header is allowed) into the w x h box at (x, y).
// The number of records played, or -1 when it is no metafile; a malformed record ends the drawing
// there, keeping what was drawn. Puts back the font it found.
int uwmf_draw(struct ugfx_surface *s, const void *data, size_t n, int x, int y, int w, int h,
              uwmf_font_fn font, void *ctx);

#endif
