// The light autohinter: a glyph's horizontal features onto the pixel grid,
// vertically only (ttf.h, ttf_set_hinting()). Split from ttf.c because that
// file parses and fills and this one only moves points it was handed.
//
// THE SHAPE IS FREETYPE'S LATIN AUTOHINTER IN "LIGHT" MODE, cut down:
//   1. the x-height (its round letters' top) is rounded, up from 3/8 px,
//      and the whole outline
//      scaled about the baseline to match, so every lower-case letter's
//      top lands on a row;
//   2. EDGES are found: flat runs of the outline, and on-curve extrema
//      (a round top or foot), each knowing which side its ink is on;
//   3. an edge in a BLUE ZONE (ttf_open()'s) snaps to that zone's fitted
//      row -- its flat line, or its overshoot when the overshoot is half
//      a pixel or more, so a round 'o' stays a row taller only when it can;
//   4. a top edge and the bottom edge under it, close and overlapping, are
//      a STEM: it keeps its own thickness (light hinting) and follows a
//      blue edge it has, else is placed by FreeType's rule for a free one;
//   5. those fitted rows are ANCHORS, kept in order (one that would cross
//      another is dropped), and every point is moved by interpolating
//      between the anchors around it -- shifted with the nearest one past
//      the ends.
// Step 5 is one map for the whole glyph where FreeType interpolates per
// contour; for Latin, Greek and Cyrillic letters the two agree.
//
// Points only move in y and never cross, so a hinted outline is still a
// closed, correctly wound outline and the fill needs nothing new.
#include "ttf.h"
#include "ttf_internal.h"

static fx_t fx_absv(fx_t v) { return v < 0 ? -v : v; }
static fx_t fx_floorv(fx_t v) { return v & ~(fx_t)(FX_ONE - 1); }
static fx_t fx_roundv(fx_t v) { return fx_floorv(v + FX_ONE / 2); }

struct zone { fx_t ref, shoot, fref, fshoot; int top, ok; };

// Inserts the anchor (o -> fit) in order, refusing one that would put two
// anchors in the opposite order fitted than unfitted. 1 when it is there.
static int anchor_add(struct ttf_scratch *sc, fx_t o, fx_t fit) {
    int i = 0;
    while (i < sc->nanc && sc->anc_o[i] < o) i++;
    if (i < sc->nanc && sc->anc_o[i] == o) return sc->anc_f[i] == fit;
    if (i > 0 && sc->anc_f[i - 1] > fit) return 0;
    if (i < sc->nanc && sc->anc_f[i] < fit) return 0;
    if (sc->nanc >= TTF_MAX_HEDGES) return 0;
    for (int k = sc->nanc; k > i; k--) {
        sc->anc_o[k] = sc->anc_o[k - 1];
        sc->anc_f[k] = sc->anc_f[k - 1];
    }
    sc->anc_o[i] = o;
    sc->anc_f[i] = fit;
    sc->nanc++;
    return 1;
}

static void anchor_remove(struct ttf_scratch *sc, fx_t o) {
    for (int i = 0; i < sc->nanc; i++)
        if (sc->anc_o[i] == o) {
            for (int k = i; k + 1 < sc->nanc; k++) {
                sc->anc_o[k] = sc->anc_o[k + 1];
                sc->anc_f[k] = sc->anc_f[k + 1];
            }
            sc->nanc--;
            return;
        }
}

// Both ends of a stem, or neither: half a stem fitted squeezes or
// stretches it.
static int anchor_pair(struct ttf_scratch *sc, fx_t o1, fx_t f1, fx_t o2, fx_t f2) {
    int had1 = 0;
    for (int i = 0; i < sc->nanc; i++) if (sc->anc_o[i] == o1) had1 = 1;
    if (!anchor_add(sc, o1, f1)) return 0;
    if (anchor_add(sc, o2, f2)) return 1;
    if (!had1) anchor_remove(sc, o1);
    return 0;
}

static void edge_add(struct ttf_scratch *sc, fx_t y, fx_t x0, fx_t x1, int ink, fx_t eps) {
    if (x1 < x0) { fx_t t = x0; x0 = x1; x1 = t; }
    // The run before this one, continued: three collinear points are one edge.
    if (sc->nhedges) {
        struct ttf_hedge *p = &sc->hedges[sc->nhedges - 1];
        if (p->ink == ink && fx_absv(p->y - y) <= eps && x0 <= p->xmax + eps && x1 >= p->xmin - eps) {
            if (x0 < p->xmin) p->xmin = x0;
            if (x1 > p->xmax) p->xmax = x1;
            p->len = p->xmax - p->xmin;
            return;
        }
    }
    if (sc->nhedges >= TTF_MAX_HEDGES) return;
    struct ttf_hedge *e = &sc->hedges[sc->nhedges++];
    e->y = y;
    e->xmin = x0;
    e->xmax = x1;
    e->len = x1 - x0;
    e->ink = (int8_t)ink;
    e->used = 0;
}

// TrueType winds outer contours clockwise (font y up): the ink is on the
// right of travel, so a run travelling +x has its ink below it. A font
// wound the other way (some converted ones are) flips every answer.
static int orientation(const struct ttf_scratch *sc) {
    int64_t area = 0;
    int first = 0;
    for (int c = 0; c < sc->ncontours; c++) {
        int last = sc->contour_end[c];
        if (last >= sc->npoints) break;
        for (int i = first; i <= last; i++) {
            int j = i == last ? first : i + 1;
            area += (int64_t)sc->px[i] * sc->py[j] - (int64_t)sc->px[j] * sc->py[i];
        }
        first = last + 1;
    }
    return area >= 0 ? 1 : -1; // pixel space flips y: clockwise reads positive
}

static void find_edges(struct ttf_scratch *sc, fx_t em) {
    fx_t eps = em / 256, minlen = em / 64;
    int orient = orientation(sc);
    sc->nhedges = 0;
    int first = 0;
    for (int c = 0; c < sc->ncontours; c++) {
        int last = sc->contour_end[c];
        if (last >= sc->npoints) break;
        int n = last - first + 1;
        for (int k = 0; n >= 2 && k < n; k++) {
            int i = first + k, j = first + (k + 1) % n, h = first + (k + n - 1) % n;
            fx_t dx = sc->px[j] - sc->px[i];
            if (fx_absv(sc->py[j] - sc->py[i]) <= eps && fx_absv(dx) >= minlen)
                edge_add(sc, (sc->py[i] + sc->py[j]) / 2, sc->px[i], sc->px[j],
                         (dx > 0 ? 1 : -1) * orient, eps);
            // A round top or foot: an ON-curve point both neighbours are
            // past. (An off-curve one is the control polygon's corner, not
            // the curve's extreme.)
            if (!sc->on[i]) continue;
            int top = sc->py[i] < sc->py[h] && sc->py[i] < sc->py[j];
            int foot = sc->py[i] > sc->py[h] && sc->py[i] > sc->py[j];
            fx_t through = sc->px[j] - sc->px[h];
            if ((top || foot) && through)
                edge_add(sc, sc->py[i], sc->px[i], sc->px[i], (through > 0 ? 1 : -1) * orient, eps);
        }
        first = last + 1;
    }
}

// Shell sort by y (Ciura's gaps): no allocation, no recursion, and close
// to n log n on the few hundred to few thousand edges a glyph has.
static void sort_edges(struct ttf_scratch *sc) {
    static const int gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    for (unsigned g = 0; g < sizeof gaps / sizeof gaps[0]; g++) {
        int gap = gaps[g];
        for (int i = gap; i < sc->nhedges; i++) {
            struct ttf_hedge t = sc->hedges[i];
            int j = i;
            for (; j >= gap && sc->hedges[j - gap].y > t.y; j -= gap) sc->hedges[j] = sc->hedges[j - gap];
            sc->hedges[j] = t;
        }
    }
}

// The first edge whose y is past `y` (edges sorted by y).
static int first_below(const struct ttf_scratch *sc, fx_t y) {
    int lo = 0, hi = sc->nhedges;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (sc->hedges[mid].y <= y) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void ttf_hint_outline(const struct ttf_font *f, fx_t scale, fx_t baseline,
                      struct ttf_scratch *sc) {
    if (!(f->blue_ok & (1u << TTF_BLUE_X)) || sc->npoints == 0) return;
    fx_t em = fx_mul(fx_from_int(f->units_per_em), scale);
    if (em < fx_from_int(4)) return;

    // 1. The x-height onto a row, rounding up from 3/8 px; the outline
    // scaled about the baseline to match. Measured at the round letters'
    // OVERSHOOT, as FreeType does: it is what rounds up at small sizes,
    // where a taller x-height is what keeps 'e' and 'a' legible.
    fx_t xh = fx_mul(fx_from_int(f->blue_shoot[TTF_BLUE_X]), scale);
    if (xh < FX_ONE) return;
    fx_t xh_fit = fx_floorv(xh + FX_ONE * 5 / 8);
    if (xh_fit < FX_ONE) xh_fit = FX_ONE;
    fx_t k = fx_div(xh_fit, xh);
    for (int i = 0; i < sc->npoints; i++)
        sc->py[i] = baseline - fx_mul(baseline - sc->py[i], k);
    fx_t s2 = fx_mul(scale, k);

    // 3's zones at this size, as pixel rows.
    struct zone z[TTF_BLUES];
    for (int i = 0; i < TTF_BLUES; i++) {
        z[i].ok = (f->blue_ok >> i) & 1;
        z[i].top = i >= TTF_BLUE_X;
        if (!z[i].ok) continue;
        fx_t ref = fx_mul(fx_from_int(f->blue_ref[i]), s2);
        fx_t shoot = fx_mul(fx_from_int(f->blue_shoot[i]), s2);
        fx_t fref = fx_roundv(ref);
        fx_t over = fx_absv(shoot - ref);
        fx_t fover = over < FX_ONE / 2 ? 0 : fx_roundv(over);
        if (over >= FX_ONE / 2 && fover < FX_ONE) fover = FX_ONE;
        fx_t fshoot = z[i].top ? fref + fover : fref - fover;
        z[i].ref = baseline - ref;
        z[i].shoot = baseline - shoot;
        z[i].fref = baseline - fref;
        z[i].fshoot = baseline - fshoot;
    }

    // 2. Sorted by y, so a stem's partner is found in the window below an
    // edge rather than by a scan of every edge (the arrays are sized for
    // TTF_MAX_POINTS, thousands, and pairing would otherwise be quadratic).
    find_edges(sc, em);
    sort_edges(sc);
    sc->nanc = 0;

    // 3. Blue edges: their fitted row is in the anchors; `used` 2 = blue.
    fx_t fuzz = em / 32;
    for (int e = 0; e < sc->nhedges; e++) {
        struct ttf_hedge *ed = &sc->hedges[e];
        int best = -1;
        fx_t best_d = 0, best_fit = 0;
        for (int i = 0; i < TTF_BLUES; i++) {
            if (!z[i].ok) continue;
            // A top zone takes edges with ink below them, a bottom zone above.
            if ((z[i].top && ed->ink != 1) || (!z[i].top && ed->ink != -1)) continue;
            fx_t dr = fx_absv(ed->y - z[i].ref), ds = fx_absv(ed->y - z[i].shoot);
            fx_t lo = z[i].ref < z[i].shoot ? z[i].ref : z[i].shoot;
            fx_t hi = z[i].ref < z[i].shoot ? z[i].shoot : z[i].ref;
            if (ed->y < lo - fuzz || ed->y > hi + fuzz) continue;
            fx_t d = dr < ds ? dr : ds;
            if (best < 0 || d < best_d) {
                best = i;
                best_d = d;
                best_fit = dr <= ds ? z[i].fref : z[i].fshoot;
            }
        }
        if (best >= 0 && anchor_add(sc, ed->y, best_fit)) ed->used = 2;
    }

    // 4. Stems: each top edge with the nearest bottom edge below it.
    // Those with a blue end first -- they are the ones already pinned.
    fx_t maxw = em / 4, tol = em / 8;
    for (int pass = 0; pass < 2; pass++) {
        for (int e = 0; e < sc->nhedges; e++) {
            struct ttf_hedge *top = &sc->hedges[e];
            if (top->ink != 1 || top->used == 1) continue;
            int best = -1;
            fx_t best_d = 0;
            for (int g = first_below(sc, top->y); g < sc->nhedges; g++) {
                struct ttf_hedge *b = &sc->hedges[g];
                fx_t d = b->y - top->y;
                if (d > maxw) break;                 // sorted: nothing nearer follows
                if (b->ink != -1 || b->used == 1) continue;
                if (b->xmin - tol > top->xmax || top->xmin - tol > b->xmax) continue;
                if (best < 0 || d < best_d) { best = g; best_d = d; }
            }
            if (best < 0) continue;
            struct ttf_hedge *bot = &sc->hedges[best];
            int pinned = top->used == 2 || bot->used == 2;
            if (pass == 0 && !pinned) continue;
            if (top->used == 2 && bot->used == 2) { top->used = bot->used = 1; continue; }
            // LIGHT: the stem keeps its own thickness; only its pinned edge
            // is on the grid, so a 1.3 px bar is one solid row and a light
            // one rather than two solid rows.
            fx_t w = best_d;
            fx_t ft, fb;
            if (top->used == 2) {
                ft = 0;
                for (int i = 0; i < sc->nanc; i++) if (sc->anc_o[i] == top->y) ft = sc->anc_f[i];
                fb = ft + w;
            } else if (bot->used == 2) {
                fb = 0;
                for (int i = 0; i < sc->nanc; i++) if (sc->anc_o[i] == bot->y) fb = sc->anc_f[i];
                ft = fb - w;
            } else {
                // A free stem, FreeType's rule, worked in height above the
                // baseline: under 1.5 px its centre goes to the nearer of
                // two spots around the nearest grid line (half a pixel
                // either side when it is a pixel or less, so it fills one
                // row); thicker, its foot is rounded.
                fx_t lo = baseline - bot->y;
                fx_t at;
                if (w < FX_ONE * 3 / 2) {
                    fx_t up = w <= FX_ONE ? FX_ONE / 2 : FX_ONE * 38 / 64;
                    fx_t dn = w <= FX_ONE ? FX_ONE / 2 : FX_ONE * 26 / 64;
                    fx_t c = lo + w / 2, g = fx_roundv(c);
                    fx_t c1 = g - up, c2 = g + dn;
                    at = (fx_absv(c - c1) < fx_absv(c - c2) ? c1 : c2) - w / 2;
                } else {
                    at = fx_roundv(lo);
                }
                fb = baseline - at;
                ft = fb - w;
            }
            if (anchor_pair(sc, top->y, ft, bot->y, fb)) top->used = bot->used = 1;
        }
    }
    if (sc->nanc == 0) return;

    // 5. Every point between the anchors around it.
    for (int p = 0; p < sc->npoints; p++) {
        fx_t y = sc->py[p];
        if (y <= sc->anc_o[0]) { sc->py[p] = y + (sc->anc_f[0] - sc->anc_o[0]); continue; }
        int last = sc->nanc - 1;
        if (y >= sc->anc_o[last]) { sc->py[p] = y + (sc->anc_f[last] - sc->anc_o[last]); continue; }
        int i = 0;
        while (i + 1 < sc->nanc && sc->anc_o[i + 1] < y) i++;
        fx_t o0 = sc->anc_o[i], o1 = sc->anc_o[i + 1];
        fx_t f0 = sc->anc_f[i], f1 = sc->anc_f[i + 1];
        sc->py[p] = f0 + (fx_t)((int64_t)(y - o0) * (f1 - f0) / (o1 - o0));
    }
}
