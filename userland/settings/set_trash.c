// System Settings: Storage > Recycle Bin's "What the bins hold now" card,
// under the size limit -- each bin's items, bytes and what the STAGED
// limit would let it keep (lib/utrash.h), so a limit is read against the
// bin it would trim before it is applied.
// Split out as set_sound.c is: a page that needs more than its settings'
// generic rows.
#include "settings/settings_internal.h"
#include "lib/utrash.h"
#include "lib/human.h"
#include "ui/uui_primitives.h"
#include <stdlib.h>

#define BINS_MAX 8

static struct utrash_usage g_bins[BINS_MAX];
static int g_nbins = -1;   // -1: not read since the page opened
static struct uui_custom g_table;
static struct uui_setting_row g_row;

static struct slot *size_slot(void) {
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && !strcmp(g_name[g_slot[i].setting], UTRASH_SET_SIZE))
            return &g_slot[i];
    return 0;
}

static int line_h(void) { return ugfx_char_h() + 6; }

static void cell(struct ugfx_surface *s, int x, int y, int w, const char *text, uint32_t fg) {
    ugfx_draw_string_clipped(s, x, y, w, text, fg, UTHEME_WHITE);
}

static void draw_table(struct ugfx_surface *s, const struct uui_custom *c) {
    int col = ugfx_char_advance('n');
    int x[4] = { c->x, c->x + col * 24, c->x + col * 32, c->x + col * 42 };
    int w[4] = { col * 23, col * 7, col * 9, c->w - col * 42 };
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    static const char *const HEAD[] = { "Disk", "Items", "Holds", "Limit" };
    for (int k = 0; k < 4; k++) cell(s, x[k], c->y, w[k], HEAD[k], dim);
    if (g_nbins <= 0) {
        cell(s, x[0], c->y + line_h(), c->w, "The Recycle Bin is empty.", UTHEME_TEXT);
        return;
    }
    struct slot *sl = size_slot();
    int pct = sl ? atoi(staged_value(sl)) : 0;
    for (int b = 0; b < g_nbins; b++) {
        const struct utrash_usage *u = &g_bins[b];
        char disk[96], vol[16], items[16], held[16], limit[32];
        human_size_iec(vol, sizeof vol, u->volume_bytes);
        snprintf(disk, sizeof disk, "%s (%s)", u->point, vol);
        snprintf(items, sizeof items, "%d", u->items);
        human_size_iec(held, sizeof held, u->bytes);
        if (pct) human_size_iec(limit, sizeof limit, u->volume_bytes / 100 * (uint64_t)pct);
        else snprintf(limit, sizeof limit, "None");
        int y = c->y + line_h() * (b + 1);
        cell(s, x[0], y, w[0], disk, UTHEME_TEXT);
        cell(s, x[1], y, w[1], items, UTHEME_TEXT);
        cell(s, x[2], y, w[2], held, UTHEME_TEXT);
        cell(s, x[3], y, w[3], limit, UTHEME_TEXT);
    }
}

int trash_emit_after(struct uui_item *out, int n, int i) {
    if (g_slot[i].setting < 0 || strcmp(g_name[g_slot[i].setting], UTRASH_SET_SIZE)) return n;
    if (g_nbins < 0) {
        g_nbins = utrash_usage(g_bins, BINS_MAX);
        ulogf("settings: trash usage %d bin(s)\n", g_nbins);
    }
    int rows = g_nbins > 0 ? g_nbins : 1;
    g_table = (struct uui_custom){ .w = 0, .h = line_h() * (rows + 1), .draw = draw_table };
    g_row.title = "What the bins hold now";
    g_row.desc = "";
    g_row.stacked = 1;
    g_row.control = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_table,
                                       .flags = UUI_FILL_W, .name = "trash_usage" };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_row,
                                  .flags = UUI_FILL_W, .name = "trash_card" };
    return n;
}

void trash_page_opened(void) { g_nbins = -1; }
