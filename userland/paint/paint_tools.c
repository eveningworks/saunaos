// Paint's tools: what a press, a drag and a release do to the picture.
// One of Paint's units -- see paint.h.
//
// EVERY CHANGE IS ONE UNDO STEP, begun on the press and committed on the
// release with the rect the stroke touched (lib/upixundo.h) -- the
// granularity of every paint program's history.
#include "paint.h"
#include "ui/ulog.h"

static int g_stroke;          // a stroke of ours is under way
static int g_slot;            // which colour it paints
static int g_lx, g_ly;        // where it last was
static struct upix_rect g_dirty;
static struct upix_stroke g_cov;   // a soft, highlighter or soft-eraser stroke's coverage
static int g_spraying;
static uint32_t g_seed = 0x2545F491u;

int tools_busy(void) { return g_stroke; }

static uint32_t ink(int slot) {
    // THE ERASER PAINTS THE SECOND COLOUR on the bottom layer -- MS
    // Paint's, whose eraser "rubs out to the background"; the right
    // button swaps the two. On any layer above it, it makes holes to the
    // layers below (Paint.NET's, Krita's).
    if (g_tool == TOOL_ERASER) return layers_active() > 0 ? 0x00000000u : g_colour[!slot];
    return g_colour[slot];
}

static int size_of(void) {
    if (g_tool == TOOL_PENCIL) return 1;
    return g_tool == TOOL_ERASER ? g_erase_size : g_brush_size[g_brush_kind];
}

// A brush painted from its coverage (lib/upixel.h's stroke) rather than
// by stamping exact pixels: soft edges, the highlighter, a soft eraser.
static int by_coverage(void) {
    if (g_tool == TOOL_ERASER) return g_erase_hard < 100;
    return g_tool == TOOL_BRUSH && (g_brush_kind == BRUSH_SOFT || g_brush_kind == BRUSH_HIGHLIGHT);
}

static void begin(int slot) {
    g_stroke = 1;
    g_slot = slot;
    g_dirty = (struct upix_rect){ 0, 0, 0, 0 };
    upixundo_begin(&g_undo, g_layer);
}

static void end(void) {
    g_stroke = 0;
    upixundo_commit(&g_undo, g_layer, &g_dirty);
    paint_changed(&g_dirty);
}

// One piece of a stroke, from (x0, y0) to (x1, y1); `first` is the press.
static void segment(int x0, int y0, int x1, int y1, int first, struct upix_rect *seg) {
    int size = size_of();
    if (g_cov.cov) {
        if (g_tool == TOOL_BRUSH && g_brush_kind == BRUSH_HIGHLIGHT)
            upix_stroke_chisel(&g_cov, x0, y0, x1, y1, size / 4 > 1 ? size / 4 : 1, size, seg);
        else
            upix_stroke_round(&g_cov, x0 + 0.5f, y0 + 0.5f, x1 + 0.5f, y1 + 0.5f, (float)size,
                              g_tool == TOOL_ERASER ? g_erase_hard : g_soft_hard, seg);
        return;
    }
    enum upix_shape shape = g_tool == TOOL_BRUSH && g_brush_kind == BRUSH_ROUND ? UPIX_ROUND : UPIX_SQUARE;
    if (first) upix_stamp(g_layer, x1, y1, size, shape, ink(g_slot), seg);
    else upix_line(g_layer, x0, y0, x1, y1, size, shape, ink(g_slot), 0, seg);
}

// SPRAY: dots in proportion to the disc's area, each tick while held.
static void spray(struct upix_rect *seg) {
    int size = g_brush_size[BRUSH_SPRAY];
    int n = size * size * 785 / 1000 * g_spray_density / 2000;
    upix_spray(g_layer, g_lx, g_ly, size, n > 0 ? n : 1, ink(g_slot), &g_seed, seg);
}

int tools_tick(void) {
    if (!g_spraying || !g_stroke) return 0;
    struct upix_rect seg = { 0, 0, 0, 0 };
    spray(&seg);
    upix_rect_add(&g_dirty, g_layer, seg.x, seg.y, seg.w, seg.h);
    paint_changed(&seg);
    return 1;
}

static void brush_pointer(enum uui_paint_phase phase, int x, int y, int slot) {
    struct upix_rect seg = { 0, 0, 0, 0 };
    if (phase == UUI_PAINT_DOWN) {
        begin(slot);
        g_lx = x;
        g_ly = y;
        if (g_tool == TOOL_BRUSH && g_brush_kind == BRUSH_SPRAY) {
            g_spraying = 1;
            paint_timer(1);
            spray(&seg);
        } else {
            if (by_coverage()) {
                enum upix_blend b = g_tool != TOOL_ERASER ? (g_brush_kind == BRUSH_HIGHLIGHT ? UPIX_BLEND_MULTIPLY
                                                                                            : UPIX_BLEND_OVER)
                                    : layers_active() > 0 ? UPIX_BLEND_ERASE : UPIX_BLEND_OVER;
                if (upix_stroke_begin(&g_cov, g_layer, ink(slot), b))
                    ulogf("paint: no memory for a %dx%d stroke\n", g_layer->w, g_layer->h);
            }
            segment(x, y, x, y, 1, &seg);
        }
        if (g_tool != TOOL_ERASER) paint_colour_used(slot);
    } else if (phase == UUI_PAINT_MOVE && g_stroke) {
        if (!g_spraying) segment(g_lx, g_ly, x, y, 0, &seg);
        g_lx = x;
        g_ly = y;
    } else if (phase == UUI_PAINT_UP && g_stroke) {
        if (g_spraying) paint_timer(0);
        g_spraying = 0;
        upix_stroke_end(&g_cov);
        end();
        return;
    } else {
        return;
    }
    upix_rect_add(&g_dirty, g_layer, seg.x, seg.y, seg.w, seg.h);
    paint_changed(&seg);
}

void tools_pointer(enum uui_paint_phase phase, int x, int y, int slot, unsigned mods) {
    if (!g_layer->px) return;
    // A TEXT LAYER IS ITS WORDS until it is made pixels, and a brush on it
    // would be lost on its next edit: ask first, GIMP's "discard the text
    // information?".
    if (phase == UUI_PAINT_DOWN && layers_text(layers_active()) &&
        (g_tool == TOOL_PENCIL || g_tool == TOOL_BRUSH || g_tool == TOOL_ERASER || g_tool == TOOL_FILL ||
         (g_tool == TOOL_SHAPES && !shapes_active()))) {
        paint_ask_rasterize();
        return;
    }
    switch (g_tool) {
    case TOOL_PENCIL: case TOOL_BRUSH: case TOOL_ERASER:
        brush_pointer(phase, x, y, slot);
        return;
    case TOOL_FILL:
        if (phase != UUI_PAINT_DOWN) return;
        if (x < 0 || y < 0 || x >= g_layer->w || y >= g_layer->h) return;
        begin(slot);
        int n = upix_fill(g_layer, x, y, g_colour[slot], 0, &g_dirty);
        ulogf("paint: fill at %d,%d -> %d pixels\n", x, y, n);
        paint_colour_used(slot);
        end();
        return;
    case TOOL_TEXT:
        text_pointer(phase, x, y, mods);
        return;
    case TOOL_SHAPES:
        shapes_pointer(phase, x, y, mods);
        return;
    case TOOL_PICK:
        // WHAT SHOWS, every layer together -- Windows 11 Paint's picker.
        if (phase != UUI_PAINT_DOWN || x < 0 || y < 0 || x >= g_layer->w || y >= g_layer->h) return;
        g_colour[slot] = upix_get(layers_view(), x, y) | 0xFF000000u;
        paint_colour_picked(slot);
        // HANDED BACK, MS Paint's picker: one pick, then the tool it
        // was picked for.
        g_tool = g_tool_before_pick;
        paint_tool_changed();
        return;
    default:
        return;
    }
}
