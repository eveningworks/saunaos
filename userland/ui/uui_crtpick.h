// uui_crtpick.h -- a CRT screen effect's controls (ui/ucrt.h) as rows of an
// Options window (ui/uui_prefs.h): the Look gallery -- Off, then each
// preset, every card the app's own sample run through that look -- and
// under it the look's parts: scanlines, glow, vignette, curvature, the
// phosphor mask, flicker and noise. The Terminal's and Retro Launcher's.
//
// A card is a whole look: Off turns the effect off and KEEPS the look for
// next time; a preset is copied into the controls. Moving any control is
// choosing a look of one's own, on -- the gallery then selects no card.
#ifndef UUI_CRTPICK_H
#define UUI_CRTPICK_H

#include "ui/ucrt.h"
#include "ui/ugfx.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_gallery.h"
#include "ui/uui_segmented.h"
#include "ui/uui_slider.h"

#define UUI_CRTPICK_CARDS (1 + UCRT_PRESET_COUNT)
#define UUI_CRTPICK_IDS 8          // the widget ids it takes, from `first_id` on

struct uui_crtpick {
    int on;                        // the effect, as edited
    struct ucrt_look look;         // its look, kept while it is off

    // Paints the app's sample into the card's rect before `look` runs
    // over it, and sets c->period and c->bezel to suit it. Keep content
    // ucrt_margin() in from the edges, or the curve cuts it.
    void (*draw_sample)(struct ugfx_surface *s, int x, int y, int w, int h,
                        const struct ucrt_look *look, struct ucrt *c, void *ctx);
    void *ctx;

    // The picker's own.
    int first_id;
    struct uui_gallery looks;
    struct uui_slider scan, glow, vig;
    struct uui_segmented curve, mask;
    struct uui_checkbox flicker, noise;
    struct ucrt card[UUI_CRTPICK_CARDS];
};

void uui_crtpick_init(struct uui_crtpick *p, int on, const struct ucrt_look *look,
                      void (*draw_sample)(struct ugfx_surface *, int, int, int, int,
                                          const struct ucrt_look *, struct ucrt *, void *),
                      void *ctx);

// Its rows on page `page` of the Options window being built (between
// uui_prefs_begin() and uui_prefs_open()), ids first_id .. + UUI_CRTPICK_IDS - 1,
// layout names "looks", "scanlines", "glow", "vignette", "curve", "mask",
// "flicker", "noise".
void uui_crtpick_rows(struct uui_crtpick *p, int page, int first_id);

// From the window's on_widget: 1 when `id` was the picker's, and `on` and
// `look` now say what it shows.
int uui_crtpick_changed(struct uui_crtpick *p, int id);

// Shows `on` and `look` (the window's Defaults).
void uui_crtpick_set(struct uui_crtpick *p, int on, const struct ucrt_look *look);

// The cards' effect buffers, from the window's on_close.
void uui_crtpick_free(struct uui_crtpick *p);

#endif
