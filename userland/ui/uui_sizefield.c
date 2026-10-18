// sizefield -- see ui/uui_sizefield.h. The two parts keep their own
// ops; this routes to them and keeps the number's range in step with
// the unit.
#include "ui/uui_sizefield.h"
#include "ui/uui_describe.h"
#include "keyboard.h"
#include "knum.h"

#define MIB_PER_GIB 1024u
// The spinbox's parse refuses more than a million (ui/uui_spinbox.c).
#define NUM_MAX 1000000u

static const char *const UNITS[] = { "MiB", "GiB" };

static uint64_t per(int unit) { return unit == 1 ? MIB_PER_GIB : 1; }

static int unit_fits(const struct uui_sizefield *f, int unit) {
    return f->max_mib / per(unit) >= 1;
}

// The number's bounds in `unit`: whole units inside min..max.
static void apply_range(struct uui_sizefield *f, int unit) {
    uint64_t p = per(unit);
    uint64_t lo = (f->min_mib + p - 1) / p, hi = f->max_mib / p;
    if (lo < 1) lo = 1;
    if (hi > NUM_MAX) hi = NUM_MAX;
    if (hi < lo) hi = lo;
    f->num.min = (int)lo;
    f->num.max = (int)hi;
}

// Shows `mib` in `unit`, rounded to the nearest whole unit and clamped.
static void show(struct uui_sizefield *f, int unit, uint64_t mib) {
    f->unit.selected = unit;
    apply_range(f, unit);
    uint64_t p = per(unit), n = (mib + p / 2) / p;
    if (n > NUM_MAX) n = NUM_MAX;
    uui_spinbox_set_value(&f->num, (int)n);
}

void uui_sizefield_init(struct uui_sizefield *f, uint64_t mib, uint64_t min_mib, uint64_t max_mib) {
    f->x = f->y = f->w = f->h = 0;
    f->part = 0;
    f->disabled = 0;
    uui_spinbox_init(&f->num, 1, 1, 2, 1, 0);
    uui_segmented_init(&f->unit, UNITS, 2, 0);
    f->min_mib = min_mib ? min_mib : 1;
    f->max_mib = max_mib > f->min_mib ? max_mib : f->min_mib;
    uui_sizefield_set_mib(f, mib);
}

void uui_sizefield_set_mib(struct uui_sizefield *f, uint64_t mib) {
    if (mib < f->min_mib) mib = f->min_mib;
    if (mib > f->max_mib) mib = f->max_mib;
    // Past what the number can hold in MiB (a terabyte gap), GiB it is.
    int gib = unit_fits(f, 1) && mib >= MIB_PER_GIB &&
              (mib % MIB_PER_GIB == 0 || mib > NUM_MAX);
    show(f, gib, mib);
}

void uui_sizefield_set_range(struct uui_sizefield *f, uint64_t min_mib, uint64_t max_mib) {
    uint64_t was = uui_sizefield_mib(f);
    f->min_mib = min_mib ? min_mib : 1;
    f->max_mib = max_mib > f->min_mib ? max_mib : f->min_mib;
    int unit = f->unit.selected == 1 && unit_fits(f, 1) ? 1 : 0;
    if (was < f->min_mib) was = f->min_mib;
    if (was > f->max_mib) was = f->max_mib;
    show(f, unit, was);
}

uint64_t uui_sizefield_mib(const struct uui_sizefield *f) {
    return (uint64_t)uui_spinbox_value(&f->num) * per(f->unit.selected);
}

int uui_sizefield_commit(struct uui_sizefield *f) { return uui_spinbox_commit(&f->num); }

// The unit was CLICKED: convert what is shown. One the range cannot
// hold goes back.
static void unit_clicked(struct uui_sizefield *f, int was) {
    int now = f->unit.selected;
    if (now == was) return;
    if (!unit_fits(f, now)) { f->unit.selected = was; return; }
    uint64_t mib = (uint64_t)uui_spinbox_value(&f->num) * per(was);
    show(f, now, mib);
}

// A unit letter TYPED: the digits in the field are in the new unit. A
// number the new unit cannot take is refused whole -- the unit stays
// and the field goes back, the spinbox's revert-not-clamp rule.
static int unit_typed(struct uui_sizefield *f, int unit) {
    uint64_t n;
    int was = f->unit.selected;
    int active = f->num.field.active;
    if (unit_fits(f, unit) && k_parse_u64(f->num.field.buf, &n)) {
        f->unit.selected = unit;
        apply_range(f, unit);
        if (n >= (uint64_t)f->num.min && n <= (uint64_t)f->num.max) {
            uui_spinbox_commit(&f->num);
            if (active) f->num.field.active = 1;
            return 1;
        }
        f->unit.selected = was;
        apply_range(f, was);
    }
    uui_spinbox_set_value(&f->num, uui_spinbox_value(&f->num));
    if (active) f->num.field.active = 1;
    return 1;
}

// --- geometry -------------------------------------------------------------

static int gap(void) { return ugfx_char_w() / 2 + 2; }

// Measured over the widest number the spinbox will take, not the current
// range, so the field does not change width when the range does.
void uui_sizefield_natural_size(const struct uui_sizefield *f, int *out_w, int *out_h) {
    struct uui_spinbox probe = f->num;
    probe.min = 1;
    probe.max = 999999;
    probe.unit = 0;
    int sw, sh, uw, uh;
    uui_spinbox_natural_size(&probe, &sw, &sh);
    uui_segmented_natural_size(&f->unit, &uw, &uh);
    if (out_w) *out_w = sw + gap() + uw;
    if (out_h) *out_h = sh > uh ? sh : uh;
}

void uui_sizefield_set_geometry(struct uui_sizefield *f, int x, int y, int w, int h) {
    f->x = x; f->y = y; f->w = w; f->h = h;
    int uw, uh;
    uui_segmented_natural_size(&f->unit, &uw, &uh);
    int sw = w - gap() - uw;
    if (sw < ugfx_char_w() * 3) sw = ugfx_char_w() * 3;
    uui_spinbox_set_geometry(&f->num, x, y, sw, h);
    uui_segmented_set_geometry(&f->unit, x + sw + gap(), y + (h - uh) / 2);
}

void uui_sizefield_draw(struct ugfx_surface *s, const struct uui_sizefield *f) {
    struct uui_sizefield *m = (struct uui_sizefield *)f;
    m->num.disabled = m->unit.disabled = f->disabled;
    uui_spinbox_draw(s, &f->num);
    uui_segmented_draw(s, &f->unit);
}

// --- ops ------------------------------------------------------------------

static int in(const void *w, int cx, int cy, int x, int y, int ww, int hh) {
    (void)w;
    return cx >= x && cx < x + ww && cy >= y && cy < y + hh;
}

static int part_at(const struct uui_sizefield *f, int cx, int cy) {
    if (uui_spinbox_ops.hit(&f->num, cx, cy)) return 1;
    if (uui_segmented_ops.hit(&f->unit, cx, cy)) return 2;
    return 0;
}

static void op_natural(const void *w, int *ow, int *oh) { uui_sizefield_natural_size(w, ow, oh); }
static void op_geometry(void *w, int x, int y, int ww, int hh) { uui_sizefield_set_geometry(w, x, y, ww, hh); }
static void op_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_sizefield *f = w;
    *x = f->x; *y = f->y; *ow = f->w; *oh = f->h;
}
static void op_draw(struct ugfx_surface *s, const void *w) { uui_sizefield_draw(s, w); }
static int op_hit(const void *w, int cx, int cy) {
    const struct uui_sizefield *f = w;
    return in(w, cx, cy, f->x, f->y, f->w, f->h) && part_at(f, cx, cy) != 0;
}

static int op_press(void *w, int cx, int cy, unsigned mods) {
    struct uui_sizefield *f = w;
    if (f->disabled) return 0;
    f->part = part_at(f, cx, cy);
    if (f->part == 1) return uui_spinbox_ops.press(&f->num, cx, cy, mods);
    if (f->part == 2) {
        // Leaving the field adopts what was typed, before the unit moves.
        if (f->num.field.active) { f->num.field.active = 0; uui_spinbox_commit(&f->num); }
        return uui_segmented_ops.press(&f->unit, cx, cy, mods);
    }
    return 0;
}
static int op_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_sizefield *f = w;
    if (f->disabled) return 0;
    int a = uui_spinbox_ops.motion(&f->num, cx, cy, buttons);
    int b = uui_segmented_ops.motion(&f->unit, cx, cy, buttons);
    return a || b;
}
static int op_release(void *w, int cx, int cy) {
    struct uui_sizefield *f = w;
    int part = f->part;
    f->part = 0;
    if (f->disabled) return 0;
    if (part == 1) return uui_spinbox_ops.release(&f->num, cx, cy);
    if (part == 2) {
        int was = f->unit.selected;
        uui_segmented_ops.release(&f->unit, cx, cy);
        unit_clicked(f, was);
        return 1;
    }
    return 0;
}
static int op_wheel(void *w, int notches) {
    struct uui_sizefield *f = w;
    return f->disabled ? 0 : uui_spinbox_ops.wheel(&f->num, notches);
}
static int op_key(void *w, int key, unsigned mods) {
    struct uui_sizefield *f = w;
    if (f->disabled) return 0;
    if (key == 'g' || key == 'G') return unit_typed(f, 1);
    if (key == 'm' || key == 'M') return unit_typed(f, 0);
    return uui_spinbox_ops.key(&f->num, key, mods);
}
static void op_set_focused(void *w, int focused) {
    struct uui_sizefield *f = w;
    uui_spinbox_ops.set_focused(&f->num, focused);
}
static int op_accepts_focus(const void *w) { return !((const struct uui_sizefield *)w)->disabled; }
static int op_edit_target(void *w, int cx, int cy, struct uui_edit_target *out) {
    struct uui_sizefield *f = w;
    return uui_spinbox_ops.edit_target(&f->num, cx, cy, out);
}
static void op_describe(const void *w, const struct uui_describe *d) {
    const struct uui_sizefield *f = w;
    uui_describe_rect(d, "number", f->num.x, f->num.y, f->num.w, f->num.h);
    uui_segmented_ops.describe(&f->unit, d);
    uui_describe_int(d, "mib", (int)uui_sizefield_mib(f));
}

const struct uui_widget_ops uui_sizefield_ops = {
    .natural_size  = op_natural,
    .set_geometry  = op_geometry,
    .bounds        = op_bounds,
    .draw          = op_draw,
    .hit           = op_hit,
    .press         = op_press,
    .motion        = op_motion,
    .release       = op_release,
    .wheel         = op_wheel,
    .key           = op_key,
    .set_focused   = op_set_focused,
    .accepts_focus = op_accepts_focus,
    .edit_target   = op_edit_target,
    .describe      = op_describe,
};
