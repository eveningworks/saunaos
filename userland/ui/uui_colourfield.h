#ifndef UUI_COLOURFIELD_H
#define UUI_COLOURFIELD_H

// uui_colourfield -- PICKING A COLOUR BY EYE: a square of saturation
// (left to right) and brightness (top to bottom) at one hue, or the hue
// strip under it -- GTK's custom colour editor, KDE's and Krita's
// selectors. An app uses one of each over ONE struct ucolour_hsv of its
// own, so moving the strip recolours the square.
//
// It holds no colour of its own: `hsv` is the caller's, and a press, a
// drag or an arrow key writes it and reports the change as any control
// does (on_widget). The square's picture is cached and drawn again only
// when its hue or size changes.

#include <stdint.h>
#include "ui/uui_widget.h"
#include "lib/ucolour.h"

enum uui_colourfield_kind { UUI_CF_SQUARE, UUI_CF_HUE };

struct uui_colourfield {
    int x, y, w, h;
    enum uui_colourfield_kind kind;
    struct ucolour_hsv *hsv;   // the caller's, shared by its square and strip
    int dragging, focused;     // OWNED

    // OWNED: the square at `cache_hue`, cache_w x cache_h, its corners
    // transparent so it sits inside a rounded edge.
    uint32_t *cache;
    int cache_w, cache_h;
    float cache_hue;
};

void uui_colourfield_init(struct uui_colourfield *f, enum uui_colourfield_kind kind, struct ucolour_hsv *hsv);
void uui_colourfield_free(struct uui_colourfield *f);

extern const struct uui_widget_ops uui_colourfield_ops;

#endif
