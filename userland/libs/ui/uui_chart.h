#ifndef UUI_CHART_H
#define UUI_CHART_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

// chart -- a value over TIME, which is the one thing uui_meter cannot
// show.
//
// A meter answers "how busy is it now"; this answers "and was it busy a
// minute ago", which is a different question and the reason Task
// Manager, GNOME System Monitor and KSysGuard all carry both.
//
// **RAW VALUES, NOT PERCENTAGES, AND A SCALE THAT SAYS WHICH.** A
// percentage-only chart cannot plot a RATE -- bytes per second has no
// natural 100 -- and rescaling a percentage series against a moving
// peak makes old samples lie, because they were scaled against an older
// one. So samples are stored as given and `scale_max` decides how they
// are read: 100 for a percentage, 0 to AUTOSCALE against the largest
// sample in view, which is what GNOME's network graph does.
//
// **THE RING IS INSIDE THE WIDGET** -- a caller-supplied array is one
// more thing for every caller to size wrongly. The main series' ring is
// in the struct; OVERLAID LINES (below) are allocated, one ring each,
// because how many there are is the caller's data (a core per line).
//
// NEWEST ON THE RIGHT, which is every system monitor's direction and
// the one that keeps the eye where new data arrives.
//
// **OVERLAID LINES** (uui_chart_set_lines()): several live series drawn
// at once over one scale, each its own line, colour and ring -- KDE
// System Monitor's per-core graph. In this mode the main series is not
// drawn (uui_chart_recent() still reads it: Task Manager keeps the
// machine's total there for its sparkline), the time axis is FIXED --
// a full ring spans the plot, so a window resize stretches the trace
// and never changes how far back it reaches -- and a line with fewer
// samples than another starts further right. A legend under the plot
// names each line with its reading. Not for fit mode, whose series[]
// is a sequence of runs in ONE trace (DiskMark's read, then write).
//
// **GLIDING** (uui_chart_set_smooth()): between two samples the trace
// moves left a WHOLE PIXEL at a time, by the share of the interval that
// has passed, never a fraction of one on a frame timer. The right edge
// is the time of the PREVIOUS sample, so the trace scrolls into data
// that exists and nothing is extrapolated; the value text still shows the newest
// reading. The chart keeps no clock and asks for no frames: its owner
// calls uui_chart_glide() when uui_chart_glide_next_ms() says the offset
// moves, and repaints when it did -- so a chart nobody can see costs
// nothing. ONLY A COLUMN OF TWO PIXELS OR MORE GLIDES (overlaid lines, a
// caller's own trace through uui_chart_glide_px()): a narrower one would
// move its one pixel just before the sample that moves it anyway, for a
// wake per sample and nothing seen. So the main series' one column per
// sample, a narrow chart, and fit mode step as before.

#define UUI_CHART_MAX 180
#define UUI_CHART_MARKS 8
#define UUI_CHART_SERIES 4
#define UUI_CHART_GLIDE_MIN_Q16 (2 << 16)   // the narrowest column that glides, 16.16

// One overlaid line. `name` and `value` are the caller's, uncopied, as
// the chart's own label and value are.
struct uui_chart_line {
    uint32_t *ring;          // UUI_CHART_MAX samples, allocated
    int count, head;
    // UUI_COLOR_UNSET -- the default -- takes the theme's action palette
    // by index (uui_chart_line_color()) at DRAW time.
    uint32_t color;
    const char *name;        // the legend's "CPU 0", or NULL
    const char *value;       // its reading, "22%", or NULL
};

// A labelled moment on the time axis -- "SEQ read" starting here. Its
// label sits in a strip ABOVE the plot, never under the trace.
struct uui_chart_mark { int at; const char *label; };

struct uui_chart {
    int x, y, w, h;
    const char *label;          // drawn top-left, or NULL for none
    // **THE READING, IN THE CALLER'S WORDS, drawn top-right.** Only the
    // caller knows what a sample MEANS -- "42%" for a processor,
    // "1.2 of 1.9 GiB" for memory, "3.4 MiB/s" for a disk. Formatting
    // it here would mean the widget choosing units for a number whose
    // meaning it does not have. Caller-owned and uncopied, like `label`.
    const char *value;

    uint32_t samples[UUI_CHART_MAX];
    // A SECOND SERIES THAT IS PART OF THE FIRST, not beside it: kernel
    // time inside total CPU time, which is the decomposition Windows
    // draws as a darker band. Drawn from the baseline UP TO its own
    // height, over the total, so the eye reads "this much of that".
    uint32_t parts[UUI_CHART_MAX];
    int has_parts;

    int count;                  // valid samples, capped at UUI_CHART_MAX
    int head;                   // where the next sample goes

    // 100 for a percentage; 0 autoscales to the largest sample in view.
    uint32_t scale_max;
    // Milliseconds between samples, for the span caption. 0 draws none
    // -- a width that means nothing is better left unlabelled than
    // labelled wrongly.
    int sample_ms;
    // Which column the pointer is over, counted from the OLDEST drawn,
    // or -1. The widget marks it; the APP formats what it says, because
    // the app owns the units.
    int hover;
    // **A SPARKLINE IS THE TRACE AND NOTHING ELSE** -- no gridlines, no
    // frame. At three rows tall the furniture is most of the ink, and a
    // small boxed grid reads as an empty table rather than as a series.
    // Tufte's point, and what every sparkline in a dashboard does.
    int compact;

    // UUI_COLOR_UNSET lets the theme answer at DRAW time -- see utheme.h
    // on why a widget must not resolve its colours when it is built.
    uint32_t bg, grid, line, fill, part;

    // **A RUN, NOT A MONITOR** (uui_chart_set_fit()): the whole history
    // spread across the width as a line, oldest at the left -- GNOME
    // Disks' benchmark graph. It never scrolls anything off: when full it
    // halves its resolution, averaging pairs, and from then on averages
    // `stride` pushes into each sample, so the time axis stays uniform.
    int fit;
    int stride, pend_n;
    uint64_t pend_sum;
    // Which series each sample belongs to, drawn in series_col[] and
    // never joined across a change -- a read and a write in one run.
    uint8_t series[UUI_CHART_MAX];
    uint32_t series_col[UUI_CHART_SERIES];
    int cur_series;
    struct uui_chart_mark marks[UUI_CHART_MARKS];
    int mark_n;
    // **A LABELLED VALUE AXIS** (uui_chart_set_axis(), FIT MODE ONLY --
    // a scrolling chart's gutter would change how many samples show): a left gutter of
    // tick values, the top rounded up to a 1-2-5 step so every gridline
    // sits on a printed number. A sample divided by `axis_div` is the
    // printed value -- 1000 for a milli-unit series. NULL draws none.
    const char *axis_unit;
    uint32_t axis_div;

    // OVERLAID LINES: `nlines` of them, or none (NULL, 0). Owned by the
    // chart once set -- uui_chart_set_lines(c, 0) frees them.
    struct uui_chart_line *lines;
    int nlines;
    // The widest reading the legend must hold ("100%"): every entry
    // reserves its width, so the legend does not reflow as readings
    // change. NULL measures the readings as they are.
    const char *legend_widest;
    uint32_t legend_bg;      // UUI_COLOR_UNSET: the panel's

    // GLIDING (uui_chart_set_smooth(), above). `newest_ns` is when the
    // newest sample was pushed (uui_anim_now_ns()); `shift` the whole
    // pixels the trace has moved since, as uui_chart_glide() last set it.
    // `glides` counts the moves since that push and `glides_last` those
    // between the two before it -- what the describe op reports, so a
    // test sees the motion without a log line per frame.
    int smooth;
    unsigned long long newest_ns;
    int shift, glides, glides_last, glide_roll;

    // Lines mode: the pointer's x over the plot, or -1. A position, not
    // a sample -- under a gliding trace the sample there changes
    // (uui_chart_hover_back()).
    int hover_x;

    // THE LEGEND, MEASURED ONCE and kept until what it was measured from
    // changes: the size, the font, `legend_widest`, or a line's words
    // (uui_chart_set_line_label() -- a reading changed in place without
    // it keeps its old width). A CACHE, written through a const chart
    // like Qt's `mutable`. `legend_measures` counts measurings, for a test.
    struct {
        int valid, w, h, nlines, ch, adv;
        const void *font, *widest;
        unsigned gen;
        int n, cell_w, per_row, rows, line_h, sq, gap, name_w, lh;
    } legend_memo;
    unsigned legend_gen;
    int legend_measures;
};

void uui_chart_init(struct uui_chart *c, const char *label);

// 100 for a percentage, 0 to autoscale against the largest sample in
// view. Anything else fixes the top of the scale.
void uui_chart_set_scale(struct uui_chart *c, uint32_t max);

// How far apart samples are, so the span caption can say what the width
// covers. 0 (the default) draws no caption.
void uui_chart_set_interval(struct uui_chart *c, int ms);

void uui_chart_push(struct uui_chart *c, uint32_t value);

// The run mode above; clears the history. Then: which series the next
// pushes belong to (0..UUI_CHART_SERIES-1), and a labelled mark at the
// next sample. `label` is not copied.
void uui_chart_set_fit(struct uui_chart *c, int on);
void uui_chart_set_series(struct uui_chart *c, int series);
void uui_chart_add_mark(struct uui_chart *c, const char *label);
void uui_chart_clear(struct uui_chart *c);
// The value axis above: `unit` captions it ("MB/s"; not copied), a
// sample / `div` is what a tick prints. NULL turns it off.
void uui_chart_set_axis(struct uui_chart *c, const char *unit, uint32_t div);

// One sample where `part` is a component of `total` -- kernel time
// within CPU time. `part` is clamped to `total`: a component larger
// than its whole is a caller bug, and drawing it would paint outside
// the series it belongs to.
void uui_chart_push_split(struct uui_chart *c, uint32_t total, uint32_t part);

// The reading to print top-right, or NULL. NOT copied -- point it at
// storage that outlives the frame.
void uui_chart_set_value(struct uui_chart *c, const char *value);

uint32_t uui_chart_last(const struct uui_chart *c);

// Which sample the pointer is over, as an index into the drawn MAIN
// series (0 = oldest drawn), or -1. With uui_chart_sample() this is what
// lets an app print the hovered reading in its own units. -1 in lines
// mode, which draws no main series: there uui_chart_hover_back() names
// the sample, for uui_chart_line_recent().
int uui_chart_hover_index(const struct uui_chart *c);
uint32_t uui_chart_sample(const struct uui_chart *c, int i);
// How many main-series samples are actually DRAWN -- min(count, width);
// 0 in lines mode.
int uui_chart_drawn(const struct uui_chart *c);
// Lines mode: how many pushes ago the sample under the pointer is (0 =
// the newest), counting the glide, or -1 -- off the plot, or no lines.
// Whether a given line HAS a sample that old is uui_chart_line_count()'s.
int uui_chart_hover_back(const struct uui_chart *c);
// The sample `back` pushes ago (0 = the newest), or 0 past the history.
// Independent of the chart's WIDTH, unlike uui_chart_sample(): a chart
// that is never laid out -- a trace drawn somewhere else, like Task
// Manager's device list -- has drawn nothing and still has its history.
uint32_t uui_chart_recent(const struct uui_chart *c, int back);

// --- overlaid lines (see the top of this file) ---------------------------
//
// Have `n` lines: kept lines keep their samples, new ones start empty,
// lines past `n` are freed; 0 frees them all and goes back to the one
// series. Returns 0, and changes nothing, when it cannot allocate.
int  uui_chart_set_lines(struct uui_chart *c, int n);
void uui_chart_push_line(struct uui_chart *c, int k, uint32_t value);
// The legend's words for line k -- both uncopied, either NULL.
void uui_chart_set_line_label(struct uui_chart *c, int k, const char *name, const char *value);
// A colour (or a UUI_ROLE) for line k; UUI_COLOR_UNSET for the default.
void uui_chart_set_line_color(struct uui_chart *c, int k, uint32_t color);
// The colour line k is drawn in NOW: its own, else the theme's action
// palette in order -- NAV, ARRANGE, VIEW, EDIT, CREATE, DANGER, MEDIA --
// and past the seventh the same again, each round darker.
uint32_t uui_chart_line_color(const struct uui_chart *c, int k);
// Line k's sample `back` pushes ago, as uui_chart_recent().
uint32_t uui_chart_line_recent(const struct uui_chart *c, int k, int back);
int  uui_chart_line_count(const struct uui_chart *c, int k);
// The legend's box for entry k, content-relative, at the chart's
// current geometry; 0 when there is none (no lines, or no room: the
// legend never takes more than half the chart's height).
int  uui_chart_legend_rect(const struct uui_chart *c, int k, int *x, int *y, int *w, int *h);

// --- gliding (see the top of this file) ----------------------------------
//
// On: the trace glides and runs one sample behind, where its column is
// wide enough to glide. Off (the default) is stepped, exactly as before.
// Ignored in fit mode.
void uui_chart_set_smooth(struct uui_chart *c, int on);
// The offset, in whole pixels, of a trace whose columns are `col_q16`
// wide (16.16 fixed point) at `now_ns`: the share of the interval since
// the newest push times a column, 0 just after a push, at most one whole
// column -- where it waits if the next sample is late. 0 when the chart
// does not glide (off, fit mode, no interval, nothing pushed yet, or a
// column under UUI_CHART_GLIDE_MIN_Q16).
int uui_chart_glide_px(const struct uui_chart *c, int32_t col_q16, unsigned long long now_ns);
// Milliseconds from `now_ns` until that offset next changes, at least 1;
// -1 when it will not before the next sample.
int uui_chart_glide_wait_ms(const struct uui_chart *c, int32_t col_q16, unsigned long long now_ns);
// The chart's own column, 16.16: a plot's width over the ring in lines
// mode, 0 for one column per sample (the main series) and fit mode.
int32_t uui_chart_col_q16(const struct uui_chart *c);
// THE OWNER'S CLOCK HAND: `shift` brought to `now_ns`. 1 when it moved
// -- repaint; 0 when the frame would be the same. Call it again after
// uui_chart_glide_next_ms(), never on a fixed timer.
int uui_chart_glide(struct uui_chart *c, unsigned long long now_ns);
int uui_chart_glide_next_ms(const struct uui_chart *c, unsigned long long now_ns);

void uui_chart_natural_size(const struct uui_chart *c, int *out_w, int *out_h);
void uui_chart_set_geometry(struct uui_chart *c, int x, int y, int w, int h);
void uui_chart_draw(struct ugfx_surface *s, const struct uui_chart *c);

extern const struct uui_widget_ops uui_chart_ops;

#ifdef __cplusplus
}
#endif

#endif
