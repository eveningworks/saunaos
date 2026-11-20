// uui_chart's OVERLAID LINES (uui_chart_set_lines()) -- Task Manager's
// one line per logical CPU -- read back as PIXELS from a real frame, with
// the toolkit's own font so the legend's geometry is the desktop's.
//
// What each check would miss if the widget were wrong:
//   - a push landing in the wrong ring (or every ring) moves another
//     line, which the "only line k" checks see as samples and as ink;
//   - a colour resolved when the line is CREATED rather than drawn keeps
//     the old palette after a theme change -- checked against a theme
//     with a colour no default has;
//   - a short line drawn from the LEFT edge, not ending at the newest
//     column, puts ink where there is no sample yet;
//   - a cap at UUI_CHART_SERIES' four fails the sixteen-line allocation.
//
// Prints one line per check and exits with the number of failures.
#include <stdio.h>
#include <string.h>
#include "uitest.h"
#include "ui/uui_chart.h"
#include "ui/utheme.h"
#include "ui/uui_anim.h"   // uui_anim_take(): the chart asks for no frames

#define CX 10
#define CY 10
#define CW 600
#define CH 300
#define NLINES 16

static struct uui_chart g_c;
static char g_name[NLINES][16], g_val[NLINES][16];

// Does column `x` of the chart's framed part hold `color` anywhere?
static int column_has(struct uitest_win *w, int x, uint32_t color) {
    int bx, by, bw, bh;
    // The framed part ends where the first legend row begins.
    int body_bot = uui_chart_legend_rect(&g_c, 0, &bx, &by, &bw, &bh) ? by : CY + CH;
    for (int y = CY + 1; y < body_bot - 1; y++)
        if (uitest_pixel_at(w, x, y) == (color & 0xFFFFFFu)) return 1;
    return 0;
}

// --- GLIDING (uui_chart_set_smooth()) ---------------------------------------
//
// 180 samples over a 600 px chart's 596 px: a column of 3.33 px, so at
// 500 ms a sample the trace moves three whole pixels between two samples
// and is redrawn three times -- once per pixel, never per frame.
static struct uui_chart g_g;

// Does column x hold `color` in the upper half of the chart?
static int high_ink(struct uitest_win *w, int x, uint32_t color) {
    for (int y = CY + 1; y < CY + CH / 2; y++)
        if (uitest_pixel_at(w, x, y) == (color & 0xFFFFFFu)) return 1;
    return 0;
}

static int same_frame(struct uitest_win *w, const uint32_t *ref) {
    for (int i = 0; i < w->surface.w * w->surface.h; i++)
        if (w->pixels[i] != ref[i]) return 0;
    return 1;
}

static void check_glide(void) {
    uui_chart_init(&g_g, "gliding");
    uui_chart_set_scale(&g_g, 100);
    uui_chart_set_interval(&g_g, 500);
    uui_chart_set_geometry(&g_g, CX, CY, CW, CH);
    uui_chart_set_lines(&g_g, 1);
    struct uui_item items[1] = { { .ops = &uui_chart_ops, .widget = &g_g, .name = "glide" } };
    struct uitest_win win;
    uitest_window(&win, items, 1, NULL, CX * 2 + CW, CY * 2 + CH);
    uint32_t col = uui_chart_line_color(&g_g, 0);
    int right = CX + CW - 2;   // where the newest sample sits, stepped
    static uint32_t ref[(CX * 2 + CW) * (CY * 2 + CH)];

    // --- OFF: stepped exactly as before ----------------------------------
    for (int i = 0; i < UUI_CHART_MAX - 1; i++) uui_chart_push_line(&g_g, 0, 0);
    uui_chart_push_line(&g_g, 0, 100);   // only the newest is high
    uitest_frame(&win);
    utest_check(high_ink(&win, right, col), "stepped: the newest sample is at the right edge");
    memcpy(ref, win.pixels, sizeof ref);
    uitest_advance_ms(300);
    utest_check(!uui_chart_glide(&g_g, uitest_now_ns()) && uui_chart_glide_next_ms(&g_g, uitest_now_ns()) == -1,
                "...asks for no redraw between samples, and names no next one");
    uitest_frame(&win);
    utest_check(same_frame(&win, ref), "...and the frame 300 ms on is the same frame");

    // --- ON: one sample behind, then a whole pixel at a time -------------
    uui_chart_set_smooth(&g_g, 1);
    uui_chart_push_line(&g_g, 0, 0);
    uui_chart_push_line(&g_g, 0, 100);   // low at the edge, high just past it
    unsigned long long t0 = uitest_now_ns();
    uitest_check_int("the column is the plot over the ring, 596/179 px (16.16)",
                     uui_chart_col_q16(&g_g), (596 << 16) / 179);
    utest_check(!uui_chart_glide(&g_g, t0), "just after a push the offset is 0: no redraw");
    uitest_frame(&win);
    utest_check(!high_ink(&win, right, col),
                "one sample behind: the newest is past the right edge, not drawn at it");
    utest_check(uui_anim_take() == 0, "...and the chart asked the toolkit for no frame of its own");

    int first = uui_chart_glide_next_ms(&g_g, t0);
    uitest_check_int("the first whole pixel is due at 151 ms (500 ms / 3.33 px)", first, 151);
    uitest_advance_ms(first - 1);
    utest_check(!uui_chart_glide(&g_g, uitest_now_ns()) && g_g.shift == 0, "...not a millisecond earlier");
    uitest_advance_ms(1);
    utest_check(uui_chart_glide(&g_g, uitest_now_ns()) && g_g.shift == 1, "...and then it moves one pixel");

    int redraws = 0, wrong = 0, at400 = -1;
    for (int ms = first + 1; ms <= 700; ms++) {
        uitest_advance_ms(1);
        int was = g_g.shift;
        int r = uui_chart_glide(&g_g, uitest_now_ns());
        redraws += r;
        if (r != (g_g.shift != was)) wrong++;
        if (ms == 400) at400 = g_g.shift;
    }
    uitest_check_int("a redraw is asked for exactly when the offset changes", wrong, 0);
    uitest_check_int("...twice more by the next sample: three pixels in all, never more", redraws, 2);
    uitest_check_int("at 400 ms the offset is floor(0.8 x 3.33) = 2", at400, 2);
    uitest_check_int("a LATE sample: at 700 ms it waits a column on, at floor(3.33)", g_g.shift, 3);
    uitest_check_int("...and names no next move until a sample comes",
                     uui_chart_glide_next_ms(&g_g, uitest_now_ns()), -1);
    uitest_frame(&win);
    utest_check(high_ink(&win, right - 1, col), "a column on, the newest has slid in to the edge");
    utest_check(uui_anim_take() == 0, "...still without a frame asked of the toolkit");

    uui_chart_push_line(&g_g, 0, 0);
    uui_chart_glide(&g_g, uitest_now_ns());
    uitest_check_int("the next push files the three moves away (the describe op's `glides`)",
                     g_g.glides_last, 3);
    uitest_check_int("...and starts the offset again from 0", g_g.shift, 0);

    // --- the pointer names a sample, counting the glide -------------------
    int at10 = right - (596 * 10) / 179;
    uitest_motion_at(&win, at10, CY + CH / 4, 0);
    uitest_check_int("gliding, the pointer at sample 10's stepped column is over sample 11",
                     uui_chart_hover_back(&g_g), 11);
    uui_chart_set_smooth(&g_g, 0);
    uitest_check_int("stepped, it is over sample 10", uui_chart_hover_back(&g_g), 10);
    uitest_check_int("...and the main-series index says none", uui_chart_hover_index(&g_g), -1);

    // --- OFF again is stepped again ------------------------------------------
    uitest_frame(&win);
    memcpy(ref, win.pixels, sizeof ref);
    uitest_advance_ms(300);
    utest_check(!uui_chart_glide(&g_g, uitest_now_ns()), "switched off, nothing glides");
    uitest_frame(&win);
    utest_check(same_frame(&win, ref), "...and the frame stands still between samples");

    // --- one column per sample has nowhere to glide ------------------------
    static struct uui_chart main_only;
    uui_chart_init(&main_only, "main");
    uui_chart_set_interval(&main_only, 500);
    uui_chart_set_geometry(&main_only, CX, CY, CW, CH);
    uui_chart_set_smooth(&main_only, 1);
    uui_chart_push(&main_only, 50);
    uitest_advance_ms(250);
    utest_check(uui_chart_col_q16(&main_only) == 0 && !uui_chart_glide(&main_only, uitest_now_ns()) &&
                uui_chart_glide_next_ms(&main_only, uitest_now_ns()) == -1,
                "the main series (a pixel per sample) never glides or wakes anyone");

    // --- nor does a column under two pixels: a 300 px chart's is 1.65 ---
    uui_chart_set_geometry(&g_g, CX, CY, 300, CH);
    uui_chart_set_smooth(&g_g, 1);
    uui_chart_push_line(&g_g, 0, 100);
    uitest_advance_ms(400);
    utest_check(!uui_chart_glide(&g_g, uitest_now_ns()) &&
                uui_chart_glide_next_ms(&g_g, uitest_now_ns()) == -1,
                "a 1.65 px column does not glide (its pixel would come just before the step)");
    uitest_frame(&win);
    utest_check(high_ink(&win, CX + 300 - 2, col), "...nor runs behind: the newest is at its edge");
    uitest_window_free(&win);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uitest_begin("chart_lines", "uui_chart's overlaid series");

    uui_chart_init(&g_c, "Utilization, each logical CPU");
    uui_chart_set_scale(&g_c, 100);
    uui_chart_set_interval(&g_c, 500);
    g_c.legend_widest = "100%";
    uui_chart_set_geometry(&g_c, CX, CY, CW, CH);

    // --- sized to the data: sixteen lines, not UUI_CHART_SERIES' four ---
    utest_check(uui_chart_set_lines(&g_c, NLINES) && g_c.nlines == NLINES,
                "sixteen overlaid lines allocate (no cap at four)");
    for (int k = 0; k < NLINES; k++) {
        snprintf(g_name[k], sizeof g_name[k], "CPU %d", k);
        snprintf(g_val[k], sizeof g_val[k], "%d%%", k);
        uui_chart_set_line_label(&g_c, k, g_name[k], g_val[k]);
    }

    struct uui_item items[1] = { { .ops = &uui_chart_ops, .widget = &g_c, .name = "chart" } };
    struct uitest_win win;
    uitest_window(&win, items, 1, NULL, CX * 2 + CW, CY * 2 + CH);

    // --- a push to line k moves line k and nothing else ------------------
    for (int i = 0; i < 20; i++) uui_chart_push_line(&g_c, 2, 50);
    int others = 0;
    for (int k = 0; k < NLINES; k++) if (k != 2) others += uui_chart_line_count(&g_c, k);
    uitest_check_int("line 2 holds the twenty samples pushed to it", uui_chart_line_count(&g_c, 2), 20);
    uitest_check_int("...and no other line holds any", others, 0);
    uitest_check_int("...nor does the main series", g_c.count, 0);
    uitest_check_int("line 2's newest sample is the one pushed", (long)uui_chart_line_recent(&g_c, 2, 0), 50);

    uitest_frame(&win);
    int right = CX + CW - 8;   // a few columns left of the newest point
    uint32_t c0 = uui_chart_line_color(&g_c, 0), c2 = uui_chart_line_color(&g_c, 2);
    utest_check(c0 != c2, "lines 0 and 2 have different colours");
    utest_check(column_has(&win, right, c2), "line 2 is drawn, at the right edge, in its colour");
    utest_check(!column_has(&win, right, c0), "...and line 0, which has no samples, is not drawn");

    // --- fewer samples: drawn right-aligned, newest at the right ---------
    utest_check(!column_has(&win, CX + CW / 4, c2),
                "line 2's twenty samples leave the left of the plot empty");
    for (int i = 0; i < UUI_CHART_MAX; i++) uui_chart_push_line(&g_c, 0, 20);
    uitest_frame(&win);
    utest_check(column_has(&win, CX + 8, c0), "a full ring reaches the left edge");
    utest_check(column_has(&win, right, c2) && !column_has(&win, CX + CW / 4, c2),
                "...while the shorter line still ends at the right and starts there too");

    // --- the palette is the theme's, read at DRAW time --------------------
    utest_check(c0 == utheme_action(UTHEME_ACT_NAV) && uui_chart_line_color(&g_c, 1) ==
                utheme_action(UTHEME_ACT_ARRANGE) && c2 == utheme_action(UTHEME_ACT_VIEW) &&
                uui_chart_line_color(&g_c, 3) == utheme_action(UTHEME_ACT_EDIT),
                "lines 0-3 take NAV, ARRANGE, VIEW, EDIT from the action palette");
    int distinct = 1;
    for (int a = 0; a < NLINES; a++)
        for (int b = a + 1; b < NLINES; b++)
            if (uui_chart_line_color(&g_c, a) == uui_chart_line_color(&g_c, b)) distinct = 0;
    utest_check(distinct, "all sixteen lines have different colours");

    struct utheme t = *utheme_current();
    uint32_t magenta = ugfx_rgb(250, 0, 250);
    t.action[UTHEME_ACT_NAV] = magenta;
    utheme_set(&t);
    uitest_frame(&win);
    utest_check(column_has(&win, CX + 8, magenta) && !column_has(&win, CX + 8, c0),
                "a theme change recolours line 0 at the next frame");
    int lx, ly, lw, lh;
    int have = uui_chart_legend_rect(&g_c, 0, &lx, &ly, &lw, &lh);
    utest_check(have && uitest_pixel_at(&win, lx + 2, ly + lh / 2) == (magenta & 0xFFFFFFu),
                "...and its legend swatch");
    utheme_settings_reload();

    // --- the legend: one entry per line, in reading order, inside the box -
    int ok_rects = 1, prev_x = -1, prev_y = -1;
    for (int k = 0; k < NLINES; k++) {
        int x, y, w, h;
        if (!uui_chart_legend_rect(&g_c, k, &x, &y, &w, &h)) { ok_rects = 0; break; }
        if (x < CX || x + w > CX + CW || y + h > CY + CH) ok_rects = 0;
        if (y == prev_y && x <= prev_x) ok_rects = 0;
        if (y < prev_y) ok_rects = 0;
        prev_x = x; prev_y = y;
    }
    utest_check(ok_rects, "sixteen legend entries, left to right then down, inside the chart");

    // --- the hover contract in lines mode --------------------------------
    // The main series is filled but not drawn here, so nothing of it may
    // be named as hovered: hover_index() + sample() would read a column
    // the pointer is not over.
    for (int i = 0; i < 40; i++) uui_chart_push(&g_c, 77);
    uitest_motion_at(&win, CX + CW / 2, CY + CH / 4, 0);
    uitest_check_int("lines mode draws no main series: uui_chart_drawn() is 0", uui_chart_drawn(&g_c), 0);
    uitest_check_int("...so uui_chart_hover_index() names none of it", uui_chart_hover_index(&g_c), -1);

    // --- the legend is measured once, not once per entry asked ----------
    // A frame draws sixteen entries and the plot asks for the legend's
    // height; each ask used to measure every name and reading again.
    uitest_frame(&win);
    int before = g_c.legend_measures;
    uitest_frame(&win);
    for (int k = 0; k < NLINES; k++) uui_chart_legend_rect(&g_c, k, &lx, &ly, &lw, &lh);
    uitest_check_int("a frame and sixteen legend rects measure the legend no more times",
                     g_c.legend_measures - before, 0);
    uui_chart_set_line_label(&g_c, 3, g_name[3], "100%");
    uitest_frame(&win);
    uitest_check_int("...a reading set through set_line_label() measures it again, once",
                     g_c.legend_measures - before, 1);
    uui_chart_set_geometry(&g_c, CX, CY, CW - 200, CH);
    uitest_frame(&win);
    uitest_check_int("...and so does a new width", g_c.legend_measures - before, 2);
    uui_chart_set_geometry(&g_c, CX, CY, CW, CH);

    // --- fewer lines keeps what is kept; none is the plain chart again ----
    utest_check(uui_chart_set_lines(&g_c, 3) && uui_chart_line_count(&g_c, 2) == 20,
                "shrinking to three lines keeps line 2's samples");
    utest_check(uui_chart_set_lines(&g_c, 0) && !g_c.lines && !g_c.nlines,
                "no lines frees them");
    utest_check(!uui_chart_legend_rect(&g_c, 0, &lx, &ly, &lw, &lh), "...and the legend goes");

    // --- fit mode's series[] is untouched: DiskMark's read, then write ----
    static struct uui_chart fit;
    uui_chart_init(&fit, "fit");
    uui_chart_set_fit(&fit, 1);
    uui_chart_push(&fit, 10);
    uui_chart_set_series(&fit, 1);
    uui_chart_push(&fit, 20);
    utest_check(fit.count == 2 && fit.series[0] == 0 && fit.series[1] == 1,
                "fit mode still tags sequential runs by series");

    uitest_window_free(&win);
    check_glide();
    return uitest_end();
}
