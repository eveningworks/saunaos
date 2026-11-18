// THE EMOJI GRID'S SKIN-TONE STRIP (ui/uui_emojigrid.h): what a right-click,
// the Menu key and Shift+F10 open over a people emoji, and what it hands
// back. The picker (toywm) and Character Map's Emoji page share the widget,
// so this checks it once, on the host, with the real emoji face.
//
// THE FORMS ARE TOLD APART BY PIXELS: a template whose tone slot came out
// empty would draw six identical hands and still "open". The handed-back
// sequence is compared as BYTES.
#include <stdio.h>
#include <string.h>

#include "uitest.h"
#include "keyboard.h"
#include "base/uemojidata.h"
#include "ui/uui_emojigrid.h"

#define W 420
#define H 320

static int find(const char *name) {
    const struct uemojidata *d = uemojidata();
    for (int i = 0; i < d->count; i++)
        if (!strcmp(d->entries[i].name, name)) return i;
    return -1;
}

// The strip's form `t` centre, as uui_emojigrid.c places the strip.
static void form_at(const struct uui_emojigrid *g, int t, int *x, int *y) {
    int cx, cy, cw, ch, pad = ugfx_px(4);
    uui_grid_cell_rect(&g->grid, g->var_index, &cx, &cy, &cw, &ch);
    int sw = 6 * cw + 2 * pad, left = cx - pad;
    if (left + sw > g->grid.x + g->grid.w) left = g->grid.x + g->grid.w - sw;
    if (left < g->grid.x) left = g->grid.x;
    int top = cy - (ch + 2 * pad) >= g->grid.y ? cy - (ch + 2 * pad) : cy + ch;
    *x = left + pad + t * cw + cw / 2;
    *y = top + pad + ch / 2;
}

// A hash of the pixels of form `t`'s cell: equal for identical drawings.
static unsigned form_hash(struct uitest_win *win, const struct uui_emojigrid *g, int t) {
    int x, y, cw, ch;
    uui_grid_cell_rect(&g->grid, g->var_index, &x, &y, &cw, &ch);
    int fx, fy;
    form_at(g, t, &fx, &fy);
    unsigned h = 2166136261u;
    for (int j = -ch / 2 + 2; j < ch / 2 - 2; j++)
        for (int i = -cw / 2 + 2; i < cw / 2 - 2; i++) h = (h ^ uitest_pixel_at(win, fx + i, fy + j)) * 16777619u;
    return h;
}

int main(void) {
    uitest_begin("emojitone", "the emoji grid's skin-tone strip: opens, draws six forms, hands back the toned emoji");
    const struct uemojidata *d = uemojidata();
    int wave = find("waving hand"), grin = find("grinning face"), fam = find("family: man, woman, girl");
    utest_check(wave >= 0 && grin >= 0 && fam >= 0, "the list has a toned hand, a face and a family");
    if (wave < 0 || grin < 0 || fam < 0) return uitest_end();
    utest_check(d->entries[wave].toned && !d->entries[grin].toned, "the hand takes tones, the face does not");

    static int items[24];
    int n = 0;
    items[n++] = grin;
    items[n++] = fam;
    for (int i = 0; i < 22; i++) items[n++] = wave;   // rows below, so the strip opens above a later one
    static struct uui_emojigrid eg;
    uui_emojigrid_init(&eg, ugfx_char_h() * 5 / 2, ugfx_char_h() * 3 / 2);
    uui_emojigrid_set(&eg, items, n);
    struct uui_item it = { .ops = &uui_emojigrid_ops, .widget = &eg, .id = 1, .name = "egrid" };
    it.ops->set_geometry(it.widget, 10, 10, W - 20, H - 20);
    struct uitest_win win;
    uitest_window(&win, &it, 1, NULL, W, H);
    struct uui_focusable ff = { &eg, &uui_emojigrid_ops };
    struct uui_focus focus;
    uui_focus_init(&focus, &ff, 1);
    uitest_set_focus(&win, &focus);
    uui_focus_set(&focus, 0);            // the keys go to the grid from the start
    uitest_frame(&win);

    // A face has no tones: nothing opens.
    utest_check(!uui_emojigrid_variants(&eg, 0) && !uui_emojigrid_variants_open(&eg),
                "a right-click on a face opens nothing");

    // The hand: six forms, every one drawn differently.
    utest_check(uui_emojigrid_variants(&eg, 10) && uui_emojigrid_variants_open(&eg), "a right-click on the hand opens its tones");
    uitest_frame(&win);
    unsigned hs[6];
    for (int t = 0; t < 6; t++) hs[t] = form_hash(&win, &eg, t);
    int distinct = 1;
    for (int a = 0; a < 6; a++)
        for (int b = a + 1; b < 6; b++) distinct &= hs[a] != hs[b];
    utest_check(distinct, "the strip draws six DIFFERENT forms: the hand and its five tones");
    uitest_keep(&win, "open");

    // Keys: Right three times, Enter -- the medium tone, U+1F44B U+1F3FD.
    for (int i = 0; i < 3; i++) uitest_key(&win, KEY_ARROW_RIGHT, 0);
    uitest_key(&win, '\n', 0);
    char seq[64];
    int e = uui_emojigrid_take_seq(&eg, seq, sizeof seq);
    utest_check(e == wave && !strcmp(seq, "\xF0\x9F\x91\x8B\xF0\x9F\x8F\xBD"),
                "Right x3 then Enter hands back the hand in the medium tone");
    utest_check(!uui_emojigrid_variants_open(&eg), "taking a form shuts the strip");

    // Esc shuts it and takes nothing.
    uui_emojigrid_variants(&eg, 10);
    uitest_key(&win, 0x1B, 0);
    utest_check(!uui_emojigrid_variants_open(&eg) && uui_emojigrid_take_seq(&eg, seq, sizeof seq) < 0,
                "Esc shuts the strip and inserts nothing");

    // A click on the darkest form takes it; a click elsewhere only shuts.
    uui_emojigrid_variants(&eg, 10);
    uitest_frame(&win);
    int fx, fy;
    form_at(&eg, 5, &fx, &fy);
    uitest_click_at(&win, fx, fy);
    e = uui_emojigrid_take_seq(&eg, seq, sizeof seq);
    utest_check(e == wave && !strcmp(seq, "\xF0\x9F\x91\x8B\xF0\x9F\x8F\xBF"), "a click on the last form takes the dark tone");
    uui_emojigrid_variants(&eg, 10);
    uitest_click_at(&win, W - 15, H - 15);
    utest_check(!uui_emojigrid_variants_open(&eg) && uui_emojigrid_take_seq(&eg, seq, sizeof seq) < 0,
                "a click outside the strip shuts it and takes nothing");

    // The Menu key and Shift+F10 open it on the focused cell, as a right-click.
    uui_grid_select(&eg.grid, 12);
    uitest_key(&win, KEY_MENU, 0);
    utest_check(uui_emojigrid_variants_open(&eg) && eg.var_index == 12, "the Menu key opens the focused emoji's tones");
    uitest_key(&win, 0x1B, 0);
    uitest_key(&win, KEY_F10, KEY_MOD_SHIFT);
    utest_check(uui_emojigrid_variants_open(&eg), "Shift+F10 opens them too");
    uitest_key(&win, 0x1B, 0);

    // A family is a sequence: taken whole.
    uui_grid_select(&eg.grid, 1);
    uitest_key(&win, '\n', 0);
    e = uui_emojigrid_take_seq(&eg, seq, sizeof seq);
    utest_check(e == fam && !strcmp(seq, d->entries[fam].seq), "Enter on a family hands back its whole sequence");

    uitest_window_free(&win);
    return uitest_end();
}
