#ifndef UUI_SCALESAMPLE_H
#define UUI_SCALESAMPLE_H
// A window's corner and a piece of the taskbar drawn at a DISPLAY SCALE
// that is not the one in effect -- what a Scale picker shows under its
// steps (System Settings' Display page; First-boot setup and the
// installer can offer the same). Drawn at the real size the desktop
// would have, cropped to the box, so 200% shows as big as it will be.
//
// THE TEXT IS THE SESSION FACE at text size x scale, rasterized here
// through uglyph -- the session atlas exists at one size only, today's.
// A `builtin` face has no file and is drawn at its one size.
#include "ui/ugfx.h"
#include "ui/uui_fontsample.h"

struct uui_scalesample {
    struct uui_fontface face;   // the session face, opened by _open()
    int have_face;
};

// Opens the session's UI face (`system.font.face`); 0 for `builtin`,
// which still draws. Close it with _close().
int  uui_scalesample_open(struct uui_scalesample *ss);
void uui_scalesample_close(struct uui_scalesample *ss);

// The sample in the box at `scale_pct` (100..200) of `text_px`, the text
// size before the scale (`system.font.size`).
void uui_scalesample_draw(struct ugfx_surface *s, struct uui_scalesample *ss,
                          int x, int y, int w, int h, int scale_pct, int text_px);

// The scale to recommend for a screen `width_px` wide, `width_mm` across
// (EDID's; 0 when unknown), in quarter steps, 100..200. From the DPI over
// 96 when the size is known -- times 0.85, for a screen seen from further
// than a desk's 96 DPI assumes, which lands a 13" 1080p laptop and a 27"
// 4K monitor on 150% as Windows recommends -- else the width over 1280.
int uui_scale_recommended(int width_px, int width_mm);

#endif
