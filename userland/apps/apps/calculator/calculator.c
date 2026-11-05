// Calculator, as a RING-3 PROCESS.
//
// The same application as the kernel's apps/calculator.c (deleted
// once this one was the default), moved out of the kernel:
// same button grid, same keyboard handling, same commit-on-release
// behaviour, same look -- but running as an ordinary ring-3 program
// that talks to the window server over the windowing protocol
// (abi/win_proto.h) instead of being compiled into kernel.bin and
// called through gui_apps.h callbacks.
//
// WHAT IS ACTUALLY SHARED, AND WHAT IS A PORT
// -------------------------------------------
// The arithmetic is not reimplemented or copied: userland/apps/apps/calculator/
// calc_engine.c is the engine the kernel version ran, compiled into
// both rings while both apps existed and ring 3's alone since.
//
// What IS ported is the presentation layer: the window is declared in
// calculator.ui (ui/uui_builder.h), `gfx_*` became `ugfx_*`, and the
// THEME_* colours UTHEME_*. Behaviour is carried over unchanged.
//
// The three real differences from the kernel version, all forced by
// being a separate process rather than chosen:
//
//   1. No window_content_x/y(). A client draws into its own buffer at
//      (0,0) and the server places the window, so every coordinate here
//      is content-relative with no origin to add. This is simpler than
//      the kernel version, not harder.
//   2. No kzalloc/kfree per window. One process is one window, so the
//      instance is a plain static -- the multi-instance machinery
//      apps/calculator.c needs exists because many windows share one
//      address space, which stops being true here. Multiple calculators
//      are multiple processes.
//   3. Font size is read once at startup rather than live. The kernel
//      version recomputes BTN_W/BTN_H from gfx_char_w() on every draw
//      so `fontsize` affects an open window; a client would need a
//      "font changed" event to do the same, and there isn't one yet
//      (see docs/roadmap.md's Milestone 41).
#include <stdint.h>
#include "base/ulog.h"
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/uui_builder.h"
#include "ui/utheme.h"
#include "apps/apps/calculator/calc_engine.h"
#include "string.h"
#include "base/unum.h"   // the display, in the locale's decimal mark








// The window is calculator.ui's; a button's #name joins it to the
// engine's code (uui_builder.h: THE NAMES TABLE IS THE CONTRACT). Also
// the order the layout lines are logged in.
static const struct uui_builder_name NAMES[] = {
    { "clear", 'C' }, { "clear_entry", 'E' }, { "percent", '%' }, { "divide", '/' },
    { "seven", '7' }, { "eight", '8' }, { "nine", '9' }, { "multiply", '*' },
    { "four", '4' }, { "five", '5' }, { "six", '6' }, { "minus", '-' },
    { "one", '1' }, { "two", '2' }, { "three", '3' }, { "plus", '+' },
    { "sign", 's' }, { "zero", '0' }, { "point", '.' }, { "equals", '=' },
};
#define BUTTON_COUNT ((int)(sizeof NAMES / sizeof NAMES[0]))

// The display area's height is the only metric this app computes;
// everything else comes from the file's layout and the buttons' labels.

static int display_h(void) { return ugfx_char_h() + 16; }
static int expr_h(void)    { return ugfx_char_h() + 4; }

static struct calc_state g_calc;
// The engine works in '.', always; what is SHOWN is the locale's mark --
// the display, the pending expression and the point key's label.
static char g_point_label[2] = ".";
static struct uui_button *g_buttons[BUTTON_COUNT];   // the builder's

static void draw_display(struct ugfx_surface *s, const struct uui_custom *c) {
    uint32_t bg = UTHEME_PANEL_BG;
    uint32_t fg = UTHEME_TEXT;
    uint32_t display_bg = UTHEME_WHITE;

    // Expression-so-far ("12 +") while an operator is pending, right
    // aligned and muted -- secondary to the main display, exactly as in
    // the kernel version.
    if (g_calc.pending_op != 0) {
        uint32_t expr_fg = utheme_current()->text_muted;
        char num[CALC_DISPLAY_MAX];
        calc_format_scaled(g_calc.accumulator, num);
        unum_localize(num, sizeof num, UNUM_GROUP);
        char expr[CALC_DISPLAY_MAX + 2];
        int pos = 0;
        for (const char *p = num; *p; p++) expr[pos++] = *p;
        expr[pos++] = ' ';
        expr[pos++] = g_calc.pending_op;
        expr[pos] = '\0';

        int expr_x = c->x + c->w - 6 - ugfx_text_width(expr);
        if (expr_x < c->x + 4) expr_x = c->x + 4;
        ugfx_draw_string(s, expr_x, c->y, expr, expr_fg, bg);
    }

    // Display: right-aligned, like a real calculator. Positioned
    // against the rect the layout handed this item rather than against
    // the window -- which is what lets it sit anywhere the layout puts
    // it without this code knowing where that is.
    int display_y = c->y + expr_h() + 2;
    ugfx_fill_rect(s, c->x, display_y, c->w, display_h(), display_bg);
    char shown[CALC_DISPLAY_MAX + 8];
    strlcpy(shown, g_calc.display, sizeof shown);
    unum_localize(shown, sizeof shown, UNUM_GROUP);
    g_point_label[0] = localeconv()->decimal_point[0];
    int text_x = c->x + c->w - 6 - ugfx_text_width(shown);
    if (text_x < c->x + 4) text_x = c->x + 4;
    int text_y = display_y + (display_h() - ugfx_char_h()) / 2;
    ugfx_draw_string(s, text_x, text_y, shown, fg, display_bg);
}

// Same passthrough the kernel version uses: the keyboard driver already
// delivers shifted symbols as the right ASCII character, so only the
// few keys that spell differently from their button code need mapping.
static char code_for_key(int key) {
    if (key >= '0' && key <= '9') return (char)key;
    // ',' is the point too: the Finnish keypad's decimal key types it.
    if (key == ',') return '.';
    if (key == '.' || key == '+' || key == '-' || key == '*' ||
        key == '/' || key == '%' || key == '=') return (char)key;
    if (key == '\r' || key == '\n') return '=';
    if (key == '\b') return 'B';
    if (key == 'c' || key == 'C') return 'C';
    return 0;
}


static int append_int(char *buf, int n, int v) {
    if (v < 0) { buf[n++] = '-'; v = -v; }
    char d[12];
    int c = 0;
    if (v == 0) d[c++] = '0';
    while (v > 0) { d[c++] = (char)('0' + v % 10); v /= 10; }
    while (c > 0) buf[n++] = d[--c];
    return n;
}

static void log_rect(const char *what, const char *name, int x, int y, int w, int h) {
    char b[80];
    int n = 0;
    const char *pre = "calculator: layout ";
    while (pre[n]) { b[n] = pre[n]; n++; }
    for (const char *p = what; *p; p++) b[n++] = *p;
    b[n++] = ' ';
    if (name) { for (const char *p = name; *p; p++) b[n++] = *p; b[n++] = ' '; }
    n = append_int(b, n, x); b[n++] = ' ';
    n = append_int(b, n, y); b[n++] = ' ';
    n = append_int(b, n, w); b[n++] = ' ';
    n = append_int(b, n, h);
    b[n++] = '\n';
    b[n] = '\0';
    ulog(b);
}

static struct uui_custom *g_display;

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    // Esc deliberately does NOT close. Closing is Alt+F4 (a WM
    // shortcut) or the title bar's X -- see docs/decisions.md. Esc is
    // the app's own key, for cancelling whatever it has open.
    char code = code_for_key(key);
    if (code) { calc_input(&g_calc, code); uapp_redraw(a); }
}

static void on_action(struct uapp *a, int code) {
    calc_input(&g_calc, (char)code);
    uapp_redraw(a);
}

static void on_open(struct uapp *a) {
    (void)a;
    calc_reset(&g_calc);

    // Geometry is settled by now: uapp runs the layout before calling
    // this, so every rect below is the one that will actually be drawn.
    log_rect("display", 0, g_display->x, g_display->y, g_display->w, g_display->h);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        const struct uui_button *bt = g_buttons[i];
        log_rect("btn", bt->label, bt->x, bt->y, bt->w, bt->h);
    }
    ulog("calculator: ready\n");
}

int main(void) {
    if (!ugfx_font_init()) return 2; // the display's height is font-derived

    struct uui_builder *ui = uui_builder_load("calculator", NAMES, BUTTON_COUNT);
    if (!ui) return 1;
    g_display = uui_builder_widget(ui, "main", "display", "custom");
    for (int i = 0; i < BUTTON_COUNT; i++)
        g_buttons[i] = uui_builder_widget(ui, "main", NAMES[i].name, "button");
    if (uui_builder_error(ui)) return uui_builder_run_error(ui);

    // Roles, resolved when a button draws, so a theme switch reaches them.
    for (int i = 0; i < BUTTON_COUNT; i++) {
        char c = (char)NAMES[i].value;
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=')
            g_buttons[i]->bg = UUI_ROLE(selection_bg);   // operators stand out slightly
        if (c == '.') g_buttons[i]->label = g_point_label;
    }
    g_display->w = 0; // no width preference -- fill the column
    g_display->h = expr_h() + 2 + display_h();
    g_display->draw = draw_display;

    struct uapp_desc desc = {
        .x         = 340,
        .y         = 150,
        .on_open   = on_open,
        .on_key    = on_key,
        .on_action = on_action,
    };
    if (!uui_builder_window(ui, "main", &desc)) return uui_builder_run_error(ui);
    return uapp_run(&desc);
}
