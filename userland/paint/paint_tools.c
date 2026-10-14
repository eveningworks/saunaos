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

int tools_busy(void) { return g_stroke; }

static uint32_t ink(int slot) {
    // THE ERASER PAINTS THE SECOND COLOUR on the bottom layer -- MS
    // Paint's, whose eraser "rubs out to the background"; the right
    // button swaps the two. On any layer above it, it makes holes to the
    // layers below (Paint.NET's, Krita's).
    if (g_tool == TOOL_ERASER) return layers_active() > 0 ? 0x00000000u : g_colour[!slot];
    return g_colour[slot];
}

static int size_of(void) { return g_tool == TOOL_PENCIL ? 1 : g_brush; }
static enum upix_shape shape_of(void) {
    return g_tool == TOOL_BRUSH ? g_shape : UPIX_SQUARE;
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

void tools_pointer(enum uui_paint_phase phase, int x, int y, int slot, unsigned mods) {
    if (!g_layer->px) return;
    // A TEXT LAYER IS ITS WORDS until it is made pixels, and a brush on it
    // would be lost on its next edit: ask first, GIMP's "discard the text
    // information?".
    if (phase == UUI_PAINT_DOWN && layers_text(layers_active()) &&
        (g_tool == TOOL_PENCIL || g_tool == TOOL_BRUSH || g_tool == TOOL_ERASER || g_tool == TOOL_FILL)) {
        paint_ask_rasterize();
        return;
    }
    switch (g_tool) {
    case TOOL_PENCIL: case TOOL_BRUSH: case TOOL_ERASER:
        if (phase == UUI_PAINT_DOWN) {
            begin(slot);
            upix_stamp(g_layer, x, y, size_of(), shape_of(), ink(slot), &g_dirty);
            if (g_tool != TOOL_ERASER) paint_colour_used(slot);
        } else if (phase == UUI_PAINT_MOVE && g_stroke) {
            struct upix_rect seg = { 0, 0, 0, 0 };
            upix_line(g_layer, g_lx, g_ly, x, y, size_of(), shape_of(), ink(g_slot), 0, &seg);
            upix_rect_add(&g_dirty, g_layer, seg.x, seg.y, seg.w, seg.h);
            paint_changed(&seg);
        } else if (phase == UUI_PAINT_UP && g_stroke) {
            end();
        } else {
            return;
        }
        g_lx = x;
        g_ly = y;
        if (phase == UUI_PAINT_DOWN) paint_changed(&g_dirty);
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
