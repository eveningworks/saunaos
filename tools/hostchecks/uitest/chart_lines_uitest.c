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
    return uitest_end();
}
