// See uui_crtpick.h.
#include "ui/uui_crtpick.h"

#include <string.h>
#include "ui/uui_prefs.h"
#include "ui/utheme.h"

static const char *const LEVEL_OPTS[] = { "Off", "Low", "Medium", "High" };
static const char *const CURVE_OPTS[] = { "Off", "Subtle", "Strong" };
static const char *const MASK_OPTS[] = { "Off", "Aperture grille", "Slot mask" };
// Card 0 is the effect off; card 1 + i is ucrt_presets[i].
static const char *const LOOK_OPTS[UUI_CRTPICK_CARDS] = { "Off", "Subtle", "Classic CRT", "Curved" };

enum { ID_LOOKS, ID_SCAN, ID_GLOW, ID_VIG, ID_CURVE, ID_MASK, ID_FLICKER, ID_NOISE };

static void draw_card(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    struct uui_crtpick *p = ctx;
    if (i < 0 || i >= UUI_CRTPICK_CARDS) return;
    struct ucrt_look look = { 0 };
    if (i > 0) look = ucrt_presets[i - 1];
    struct ucrt *c = &p->card[i];
    c->look = look;
    if (p->draw_sample) p->draw_sample(s, x, y, w, h, &look, c, p->ctx);
    ucrt_apply(c, s, x, y, w, h);
}

static void to_controls(struct uui_crtpick *p) {
    const struct ucrt_look *l = &p->look;
    p->scan.selected = l->scanlines;
    p->glow.selected = l->glow;
    p->vig.selected = l->vignette;
    p->curve.selected = l->curve;
    p->mask.selected = l->mask;
    p->flicker.checked = l->flicker;
    p->noise.checked = l->noise;
    int k = ucrt_preset_of(l);
    p->looks.selected = !p->on ? 0 : k >= 0 ? 1 + k : -1;   // -1: a look of its own
}

static void from_controls(struct uui_crtpick *p) {
    struct ucrt_look *l = &p->look;
    l->scanlines = p->scan.selected > 0 ? p->scan.selected : 0;
    l->glow = p->glow.selected > 0 ? p->glow.selected : 0;
    l->vignette = p->vig.selected > 0 ? p->vig.selected : 0;
    l->curve = p->curve.selected > 0 ? p->curve.selected : 0;
    l->mask = p->mask.selected > 0 ? p->mask.selected : 0;
    l->flicker = p->flicker.checked;
    l->noise = p->noise.checked;
}

void uui_crtpick_init(struct uui_crtpick *p, int on, const struct ucrt_look *look,
                      void (*draw_sample)(struct ugfx_surface *, int, int, int, int,
                                          const struct ucrt_look *, struct ucrt *, void *),
                      void *ctx) {
    memset(p, 0, sizeof *p);
    p->draw_sample = draw_sample;
    p->ctx = ctx;
    uint32_t bg = UUI_ROLE(window_bg), fg = UUI_ROLE(text);   // roles: resolved at draw
    uui_gallery_init(&p->looks, LOOK_OPTS, UUI_CRTPICK_CARDS, 0);
    p->looks.draw_tile = draw_card;
    p->looks.ctx = p;
    for (int i = 0; i < UUI_CRTPICK_CARDS; i++) ucrt_init(&p->card[i]);
    uui_slider_init(&p->scan, LEVEL_OPTS, UCRT_LEVEL_MAX + 1);
    uui_slider_init(&p->glow, LEVEL_OPTS, UCRT_LEVEL_MAX + 1);
    uui_slider_init(&p->vig, LEVEL_OPTS, UCRT_LEVEL_MAX + 1);
    uui_segmented_init(&p->curve, CURVE_OPTS, UCRT_CURVE_COUNT, 0);
    uui_segmented_init(&p->mask, MASK_OPTS, UCRT_MASK_COUNT, 0);
    uui_checkbox_init(&p->flicker, 0, 0, 0, "Flicker", bg, fg);
    uui_checkbox_init(&p->noise, 0, 0, 0, "Static noise", bg, fg);
    uui_crtpick_set(p, on, look);
}

void uui_crtpick_set(struct uui_crtpick *p, int on, const struct ucrt_look *look) {
    p->on = on;
    p->look = *look;
    to_controls(p);
}

void uui_crtpick_rows(struct uui_crtpick *p, int page, int first_id) {
    p->first_id = first_id;
    uui_prefs_row(page, "Look:", &uui_gallery_ops, &p->looks, first_id + ID_LOOKS, "looks", UUI_FILL_W);
    // The sliders get a track a hand can travel: their natural width is
    // the labels', which makes four stops a flick of the wrist.
    static const char *const cap[] = { "Scanlines:", "Glow:", "Vignette:" };
    static const char *const nm[] = { "scanlines", "glow", "vignette" };
    struct uui_slider *sl[] = { &p->scan, &p->glow, &p->vig };
    for (int k = 0; k < 3; k++) {
        int r = uui_prefs_row(page, cap[k], &uui_slider_ops, sl[k], first_id + ID_SCAN + k, nm[k], 0);
        uui_prefs_widen(r, ugfx_char_advance('n') * 34);
    }
    uui_prefs_row(page, "Curvature:", &uui_segmented_ops, &p->curve, first_id + ID_CURVE, "curve", 0);
    uui_prefs_row(page, "Phosphor mask:", &uui_segmented_ops, &p->mask, first_id + ID_MASK, "mask", 0);
    int r = uui_prefs_row(page, "Animation:", &uui_checkbox_ops, &p->flicker, first_id + ID_FLICKER, "flicker", 0);
    uui_prefs_also(r, &uui_checkbox_ops, &p->noise, first_id + ID_NOISE, "noise");
}

int uui_crtpick_changed(struct uui_crtpick *p, int id) {
    int k = id - p->first_id;
    if (k < 0 || k >= UUI_CRTPICK_IDS) return 0;
    if (k == ID_LOOKS) {
        int c = p->looks.selected;
        if (c == 0) p->on = 0;
        else if (c > 0 && c < UUI_CRTPICK_CARDS) { p->on = 1; p->look = ucrt_presets[c - 1]; }
    } else {
        from_controls(p);
        p->on = ucrt_look_on(&p->look);
    }
    to_controls(p);
    return 1;
}

void uui_crtpick_free(struct uui_crtpick *p) {
    for (int i = 0; i < UUI_CRTPICK_CARDS; i++) ucrt_free(&p->card[i]);
}
