// Console layer. Renders through one of two backends, chosen automatically
// at vga_init() time:
//   - legacy: direct writes to the 0xB8000 VGA text-mode buffer
//   - framebuffer: character cells drawn with the 8x8 font onto whatever
//     linear RGB framebuffer GRUB handed us (see gfx.c / multiboot.c)
// Every other file in the kernel just calls vga_write()/vga_putc()/etc and
// doesn't need to know which backend is actually active.
//
// **THERE ARE VGA_VC_COUNT SCREENS, AND ONE OF THEM IS SHOWN** -- the
// virtual consoles behind Ctrl+Alt+F1..F4 (kernel/vt.h). Each `struct vc`
// owns everything that describes ONE screen: the cursor, the colours,
// the ANSI parser, the scrolling region, its scrollback, and a GRID of
// cells -- Linux's vc_screenbuf -- because the pixels hold only the
// screen on display, and a hidden screen must be redrawable when it is
// switched to. Every write updates the grid; only the shown vc's also
// reaches the pixels (`vc_put()` and friends, "the screen" below). VT1
// is the kernel's console and tty0's: everything the kernel itself
// prints lands there, whichever VT is on screen.

#include "clockevent.h" // clockevent_idle_wake_at_tick() -- the blink is due at a time
#include "vga.h"
#include "vt.h"        // vga_vc_*() -- the screens behind Ctrl+Alt+Fn
#include "heap.h"      // a vc's grid and scrollback
#include "scheduler.h" // scheduler_preempt_disable() -- vga_putc_console()
#include "ansi.h"
#include "kutf8.h"     // the console decodes UTF-8 onto its Latin-1 font
#include "io.h"
#include "gfx.h"
#include "win_role.h" // win_server_any() -- is anything else painting the screen?
#include "splash.h"   // the boot splash holds presenting back
#include "display.h"   // display_power_set() -- a panic lights the screen
#include "timer.h"
#include "klog.h"
#include "kfmt.h"     // klog_printf()
#include "knum.h"
#include "string.h"
#include <stddef.h>

// driver-none: the text console above whatever display.c chose

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEM ((uint16_t *)0xB8000)

// Cell size used to come from compile-time FONT_CHAR_W/H macros; now
// that font size is runtime-selectable (see gfx_set_font_size()), every
// use below calls gfx_char_w()/gfx_char_h() fresh instead.
#define CELL_W gfx_char_w()
#define CELL_H gfx_char_h()

static int fb_mode = 0;

// ---- console double buffering ----
//
// The framebuffer console draws into gfx.c's back buffer and publishes
// with gfx_present(), rather than drawing straight at the display.
//
// The invariant this buys: **the console never READS the framebuffer.**
// It used to, in two places -- scrolling shifted the visible pixels up
// in place, and the cursor saved the cell underneath itself before
// painting over it. Both are reads of a surface that is write-combined
// (see paging.c), and WC is a write optimisation that makes reads
// strictly worse: stores coalesce into bursts, while each load is an
// uncached bus round trip with no cache fill and no prefetch. A
// full-screen shift is a whole screen of those, which on a real machine
// is slow enough to watch the scanline travel down the display. It is
// invisible under plain QEMU, whose TCG ignores guest memory types
// entirely -- `make run-kvm` and real hardware are where it shows.
//
// The trap, if you edit this: presenting is now a SEPARATE step from
// drawing, so anything that puts text on screen and then stops without
// reaching a flush point leaves that text in RAM only. The flush points
// are vga_present() from the kernel's idle work (vga_idle(), so the tail
// of any burst), the throttled present at the end of each vga_putc(),
// and the explicit call on the panic path. A new path that prints and
// then halts with interrupts off needs its own.
//
// There is deliberately NO "console has drawn something" flag here.
// gfx.c already tracks the dirty bounding box, and gfx_present() returns
// immediately when it is empty, so that IS the answer -- a second copy
// of it kept in this file could only ever disagree with it, and the
// direction it would disagree in is the silent one (text drawn, flag not
// set, nothing ever shown). Present unconditionally and let gfx.c decide.
static uint64_t fb_last_present_tick;

// The SCREEN's geometry, which every vc shares: the font and the mode
// are the machine's, not a terminal's. 80x25 on the legacy backend.
static size_t console_cols;
static size_t console_rows;

static uint16_t *buf = VGA_MEM;

// The console's OWN background, as opposed to whatever colour a caller
// has temporarily set. A cell nobody has written to belongs to the
// console, so blank space introduced by a scroll is painted with this
// rather than with the current background -- see line_feed(). Same
// reasoning cursor_hide() already spells out for the cursor cell.
#define CONSOLE_DEFAULT_BG VGA_BLACK

// ---- output sink redirection (see vga.h's struct vga_sink comment) ----
// The KERNEL's writer only (vga_putc() and friends): a terminal's output
// never goes through it, see vga_putc_console().
static const struct vga_sink *active_sink = 0;

// ---- the virtual consoles ------------------------------------------
//
// Scrollback ring: logical output LINES, see "Scrollback" below. VT1's
// is the static arrays further down, because the kernel prints before
// anything could ask for a VT; the others are allocated on first use.
struct vc_sb {
    char     *chars;     // lines * cols
    uint8_t  *attrs;     // lines * cols
    uint16_t *len;       // used columns per line
    uint32_t lines, cols;
    uint32_t total;      // lines ever completed (monotonic)
    uint32_t cur_len;    // columns used in the line being written
    uint32_t view;       // 0 = live; N = showing N lines further back
};

struct vc {
    int      num;                 // 1..VGA_VC_COUNT, what Ctrl+Alt+Fn names
    size_t   row, col;            // THE cursor -- there is no second one to keep in step
    enum vga_color fg, bg;
    // console_rows * console_cols cells, each the font byte | attr << 8 --
    // the 0xB8000 layout. NULL until the vc is first used; reallocated
    // (blank) when the geometry changes, see vga_reflow().
    uint16_t *grid;
    size_t   grid_rows, grid_cols;
    // A sequence arrives one byte at a time, so `ESC [ 3 1 m` is five
    // calls with the decision made on the last -- state per screen.
    struct ansi_parser ansi;
    int      ansi_armed;
    struct ansi_region region;
    size_t   region_rows;         // what it was set against; 0 = the whole screen
    size_t   saved_row, saved_col;
    struct kutf8_stream u8;       // a sequence split across two writes draws once
    int      cursor_suppressed;   // DECTCEM -- see cursor_draw()
    struct vc_sb sb;
};

static struct vc g_vc[VGA_VC_COUNT];
static struct vc *g_fg = &g_vc[0];   // the one on screen
#define VC_KERNEL (&g_vc[0])         // VT1: the kernel's console and tty0's

static int vc_shown(const struct vc *v) { return v == g_fg; }

static struct vc *vc_at(int n) {
    return n >= 1 && n <= VGA_VC_COUNT ? &g_vc[n - 1] : NULL;
}

static void vc_reset(struct vc *v) {
    v->row = v->col = 0;
    v->fg = VGA_LIGHT_GREY;
    v->bg = VGA_BLACK;
    v->ansi_armed = 0;
    v->region_rows = 0;
    v->saved_row = v->saved_col = 0;
    k_memset(&v->u8, 0, sizeof v->u8);
    v->cursor_suppressed = 0;
}

// A line feed -- inside DECSTBM's scrolling region for a terminal's
// output (`raw`), over the whole screen for the kernel's own; see "the
// scrolling region" below. putc calls it for a '\n' and for a wrap.
static void line_feed(struct vc *v, int raw);

static uint32_t palette_rgb(enum vga_color c) {
    switch (c) {
        case VGA_BLACK:         return gfx_rgb(0, 0, 0);
        case VGA_BLUE:          return gfx_rgb(0, 0, 170);
        case VGA_GREEN:         return gfx_rgb(0, 170, 0);
        case VGA_CYAN:          return gfx_rgb(0, 170, 170);
        case VGA_RED:           return gfx_rgb(170, 0, 0);
        case VGA_MAGENTA:       return gfx_rgb(170, 0, 170);
        case VGA_BROWN:         return gfx_rgb(170, 85, 0);
        case VGA_LIGHT_GREY:    return gfx_rgb(170, 170, 170);
        case VGA_DARK_GREY:     return gfx_rgb(85, 85, 85);
        case VGA_LIGHT_BLUE:    return gfx_rgb(85, 85, 255);
        case VGA_LIGHT_GREEN:   return gfx_rgb(85, 255, 85);
        case VGA_LIGHT_CYAN:    return gfx_rgb(85, 255, 255);
        case VGA_LIGHT_RED:     return gfx_rgb(255, 85, 85);
        case VGA_LIGHT_MAGENTA: return gfx_rgb(255, 85, 255);
        case VGA_LIGHT_BROWN:   return gfx_rgb(255, 255, 85);
        case VGA_WHITE:         return gfx_rgb(255, 255, 255);
    }
    return gfx_rgb(170, 170, 170);
}

static inline uint8_t make_color(enum vga_color fg, enum vga_color bg) {
    return fg | (bg << 4);
}

static inline uint8_t vc_attr(const struct vc *v) { return make_color(v->fg, v->bg); }

// What a cell nobody has written holds, and what a scroll brings in.
#define BLANK_ATTR make_color(VGA_LIGHT_GREY, CONSOLE_DEFAULT_BG)
#define CELL(ch, attr) ((uint16_t)(uint8_t)(ch) | (uint16_t)((uint16_t)(attr) << 8))

// ---- legacy 0xB8000 backend ----

static uint8_t box_cp437(char c);

// Text mode's font is the card's own CP437, so a box slot becomes that
// code (box_cp437()); everything else goes as it always has.
static inline uint16_t make_entry(char c, uint8_t clr) {
    return (uint16_t)box_cp437(c) | ((uint16_t)clr << 8);
}

static void legacy_update_cursor(void) {
    uint16_t pos = (uint16_t)(g_fg->row * VGA_WIDTH + g_fg->col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

// ---- blinking cursor (framebuffer mode only) ----
//
// The legacy 0xB8000 backend gets a blinking cursor for free from real
// VGA text-mode hardware (legacy_update_cursor() just tells it where to
// blink, not whether); the framebuffer backend draws its own console
// text pixel-by-pixel and has no such hardware to lean on, so it needs
// this. Kept deliberately simple: a solid block the size of one glyph
// cell at the shown vc's (row, col), toggled on a timer.
//
// cursor_on_screen tracks whether that block is currently painted, so
// cursor_hide() is a safe no-op when there's nothing to erase. It is the
// SCREEN's cursor, one for all the vcs: only the shown vc draws it, and
// it is put back where it was PAINTED (cursor_r/c), so a vc whose
// position moved meanwhile cannot make it restore the wrong cell.
static int cursor_on_screen = 0;
static size_t cursor_r, cursor_c;
static uint64_t cursor_last_toggle_tick = 0;

// The cursor SAVES THE PIXELS IT COVERS and puts them back when it
// hides, rather than erasing its cell to a colour and hoping that was
// what used to be there.
//
// That older approach erased to black, which was right at the append
// point (always a blank cell) and quietly wrong anywhere else -- a
// cursor sitting on a character ate the character when it moved or
// blinked off. Line editing made "anywhere else" the common case: the
// first mid-line edit shipped a visible hole where a '.' had been.
// Saving and restoring is exact regardless of what's underneath, which
// also means the cursor can blink mid-line again (it had to be pinned
// solid to stay safe) and that any cursor SHAPE works, since nothing
// has to reconstruct the cell.
//
// Sized for the largest glyph cell the biggest baked font produces,
// with a runtime guard below rather than a silent overflow if that ever
// stops being true.
#define CURSOR_SAVE_W 32
#define CURSOR_SAVE_H 64
static uint32_t cursor_save[CURSOR_SAVE_W * CURSOR_SAVE_H];
static int cursor_save_w = 0, cursor_save_h = 0; // what's actually stored

// How strongly the translucent style tints the cell it covers. High
// enough to read as a cursor at a glance, low enough that the character
// underneath stays legible -- the whole point of the style.
// Two strengths, because a cell is mostly glyph at these font sizes.
// The background has to move enough for the cell to read as a block at
// a glance; the glyph has to move FURTHER, or it sinks into its own
// cursor. Tinting both by the same amount was tried and left the
// character washed out (contrast against the block dropped from 170 to
// 89); tinting only the background was also tried and produced a block
// two pixels wide, because a glyph like 'r' fills most of its cell.
#define CURSOR_TINT_ALPHA_BG 130
#define CURSOR_TINT_ALPHA_FG 205

static enum vga_cursor_style cursor_style = VGA_CURSOR_TRANSLUCENT;

// Kept in lockstep with enum vga_cursor_style (vga.h), same convention
// as debugflags.c's DBGFLAG_NAMES -- `cursor` (no args) and
// `cursor <name>` both read this generically.
const char *const VGA_CURSOR_STYLE_NAMES[VGA_CURSOR_STYLE_COUNT] = {
    "translucent", "underline", "beam", "reverse",
};

// 100 Hz PIT (see idt.c's pit_init(100) call) -- 50 ticks is 500ms, so
// a full on/off blink cycle is about a second, a fairly ordinary
// terminal cursor rate.
#define CURSOR_BLINK_TICKS 50

// Paints the cursor at the shown vc's (row, col) in the active style,
// saving whatever it covers first. A no-op if the cursor is already on
// screen -- saving then would capture the cursor's own pixels and
// "restore" them later.
//
// The vc's `cursor_suppressed` is DECTCEM (`ESC[?25l`). A full-screen
// program hides the cursor for the whole time it is painting, and a
// blinking block left wandering across its output is the difference
// between a drawn screen and a flickering one. Checked here rather than
// at each call site, because the cursor is drawn from several places
// (after a putc, after a move, on the blink tick, on resume) and one of
// them would eventually be missed.
static void cursor_draw(void) {
    struct vc *v = g_fg;
    if (cursor_on_screen || v->cursor_suppressed) return;
    if (v->row >= console_rows || v->col >= console_cols) return;

    int w = (int)CELL_W, h = (int)CELL_H;
    if (w > CURSOR_SAVE_W || h > CURSOR_SAVE_H || w <= 0 || h <= 0) {
        return; // can't save it, so don't paint it -- see CURSOR_SAVE_W
    }
    int x0 = (int)(v->col * CELL_W), y0 = (int)(v->row * CELL_H);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            cursor_save[y * CURSOR_SAVE_W + x] = gfx_get_pixel(x0 + x, y0 + y);
        }
    }
    cursor_save_w = w;
    cursor_save_h = h;
    cursor_r = v->row;
    cursor_c = v->col;

    uint32_t color = palette_rgb(v->fg);
    switch (cursor_style) {
    case VGA_CURSOR_UNDERLINE: {
        int thick = h >= 20 ? 3 : 2;
        gfx_fill_rect(x0, y0 + h - thick, w, thick, color);
        break;
    }
    case VGA_CURSOR_BEAM: {
        int thick = w >= 12 ? 2 : 1;
        gfx_fill_rect(x0, y0, thick, h, color);
        break;
    }
    case VGA_CURSOR_REVERSE:
        // Inverting the saved pixels needs no knowledge of which of them
        // are glyph and which are background -- the character comes out
        // dark-on-light either way.
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t under = cursor_save[y * CURSOR_SAVE_W + x];
                gfx_put_pixel(x0 + x, y0 + y, ~under);
            }
        }
        break;
    case VGA_CURSOR_TRANSLUCENT:
    default: {
        // Tint every pixel in the cell instead of replacing it, so the
        // glyph shows through.
        //
        // Tint the whole cell, but the glyph harder than the
        // background (see the two alphas above) -- the background rises
        // enough to read as a block, the character rises further and
        // stays legible on top of it.
        //
        // "Background" is whatever matches the cell's own background
        // colour exactly. Antialiased glyph edges don't, so they get
        // the glyph treatment, which is the right side to err on: it
        // keeps the character's outline crisp.
        uint32_t bg_pixel = palette_rgb(v->bg);
        uint32_t tint = (v->bg <= VGA_DARK_GREY) ? palette_rgb(VGA_WHITE)
                                                  : palette_rgb(VGA_BLACK);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t under = cursor_save[y * CURSOR_SAVE_W + x];
                uint8_t alpha = (under == bg_pixel) ? CURSOR_TINT_ALPHA_BG
                                                     : CURSOR_TINT_ALPHA_FG;
                gfx_put_pixel(x0 + x, y0 + y, gfx_blend(under, tint, alpha));
            }
        }
        break;
    }
    }
    cursor_on_screen = 1;
}

static void cursor_hide(void) {
    if (!cursor_on_screen) return;
    int x0 = (int)(cursor_c * CELL_W), y0 = (int)(cursor_r * CELL_H);
    for (int y = 0; y < cursor_save_h; y++) {
        for (int x = 0; x < cursor_save_w; x++) {
            gfx_put_pixel(x0 + x, y0 + y, cursor_save[y * CURSOR_SAVE_W + x]);
        }
    }
    cursor_on_screen = 0;
}

static void cursor_show_and_reset_blink(void) {
    if (!fb_mode) return;
    cursor_draw();
    cursor_last_toggle_tick = coarse_ticks(); // full interval before the next auto-toggle,
                                            // so it doesn't flicker right after typing
}

// Around anything that moves a vc's cursor or draws under it: the
// screen's cursor comes off first and goes back on at the new place --
// for the SHOWN vc only, since a hidden one has nothing painted.
static void vc_cursor_off(const struct vc *v) {
    if (fb_mode && vc_shown(v)) cursor_hide();
}

static void vc_cursor_on(const struct vc *v) {
    if (!vc_shown(v)) return;
    if (fb_mode) cursor_show_and_reset_blink(); // reappear solid at the new position
    else legacy_update_cursor();
}

enum vga_cursor_style vga_cursor_style(void) { return cursor_style; }

void vga_set_cursor_style(enum vga_cursor_style style) {
    if (style >= VGA_CURSOR_STYLE_COUNT) return;
    if (fb_mode) cursor_hide(); // repaint in the new shape, not on top of the old one
    cursor_style = style;
    if (fb_mode) cursor_draw();
}

int vga_cursor_style_parse(const char *name, enum vga_cursor_style *out) {
    for (int i = 0; i < VGA_CURSOR_STYLE_COUNT; i++) {
        if (k_strcmp(name, VGA_CURSOR_STYLE_NAMES[i]) == 0) {
            *out = (enum vga_cursor_style)i;
            return 1;
        }
    }
    return 0;
}

// ---- framebuffer text-console backend ----

// Publishes the console's back buffer. TWO ENTRY POINTS, and the
// difference is who owns the screen.
//
// vga_present() is the ROUTINE one and does NOTHING while a compositor
// holds the screen: the console keeps drawing into its own back buffer,
// it just stops blitting that over the desktop. Linux's KD_GRAPHICS, for
// the same reason -- a program's output must not paint over a graphical
// session. Anything a process printed is still there and vga_resume()
// puts it on screen when the desktop goes away.
//
// vga_present_force() ignores that, for the one caller whose whole
// purpose is to paint over whatever is there: a PANIC. Guarding it would
// have made every panic under a running desktop invisible.
//
// Default-safe on purpose: a new routine caller gets the check without
// knowing it exists, and the two that mean to override say so.
static void present_now(void) {
    gfx_present();
    gfx_flush();
    fb_last_present_tick = coarse_ticks();
}

static void vc_redraw(struct vc *v);

// Lights the screen and presents whatever VT is shown -- vga_resume()'s,
// which must not move the screen to VT1 behind kernel/tty/vt.c's back:
// the desktop's hidden state would be left set, with VT1 showing.
static void present_forced(void) {
    display_power_set(1);   // ...and an idle-dark screen comes back to show it
    splash_end(1);   // a panic is the log's, splash or not
    present_now();
}

void vga_present_force(void) {
    if (!fb_mode) return;
    // A PANIC IS PRINTED ON VT1, so a text VT on screen gives way to it.
    // Allocates nothing, so it is safe here; vt.c's state is left as it
    // was, since the machine is stopping.
    if (!vc_shown(VC_KERNEL)) {
        g_fg = VC_KERNEL;
        vc_redraw(g_fg);
    }
    present_forced();
}

static int g_held;              // the dead desktop's last frame is on screen
static uint64_t g_held_until;   // coarse tick when the console takes it back

void vga_present(void) {
    if (!fb_mode) return;
    if (g_held) return;   // the held frame (vga_hold_begin) is the screen's
    // THE BOOT SPLASH OWNS IT (splash.h) until Esc, a text boot or the
    // compositor -- the log keeps collecting in the back buffer.
    splash_poll();
    if (splash_active()) return;
    // THE DESKTOP OWNS THE SCREEN -- while ITS VT, VT1, is shown. Without
    // this, any ring-3 process writing to fd 1 blits the text console over
    // the whole desktop -- DOOM's startup banner was the visible case,
    // `dmesg` covers 100% of the screen, and it is not the writer's fault
    // in either. A text VT on screen has hidden the desktop (kernel/vt.h).
    if (win_server_any() && vc_shown(VC_KERNEL)) return;
    // Both modes, in the one place, so no caller has to know which is
    // live. Double-buffered, gfx_present() blits the dirty box and
    // publishes it; drawing straight at the display it is a no-op and
    // gfx_flush() is what tells a driver-owned mode to show the region.
    // Exactly one of the two does the work on any given boot.
    present_now();
}

// The mid-burst present. Bounded to one per PIT tick (100Hz) so a
// command dumping hundreds of scrolling lines pays for one full-screen
// blit per tick rather than one per line, while still animating instead
// of appearing to freeze until it finishes. The tail of the burst is
// caught by vga_present() from the idle work (vga_idle()), so nothing
// relies on this firing on the last line.
static void fb_present_throttled(void) {
    if (coarse_ticks() == fb_last_present_tick) return;
    vga_present();
}

void vga_idle(void) {
    // THE TAIL OF THE LAST BURST: a program that printed and then went to
    // sleep or to compute left its last characters within a tick of the
    // throttled present above, and nothing else would show them.
    vga_present();
}

// ---- line drawing ---------------------------------------------------
//
// THE C1 SLOTS CARRY LINE DRAWING. The baked font is Latin-1, whose
// 0x80-0x9F are controls that never draw, so vga_console_glyph() puts
// U+2500's lines and U+2580's blocks there and a cell draws them as
// rectangles -- what the Linux VT gets from the CP437 half of its font.
// Arms run from the cell's centre to its edges, so neighbours join.
enum { ARM_L = 1, ARM_R = 2, ARM_U = 4, ARM_D = 8,
       BOX_FULL = 16, BOX_TOP, BOX_BOTTOM, BOX_LEFT, BOX_RIGHT, BOX_SHADE1, BOX_SHADE2, BOX_SHADE3 };
#define BOX_SLOT0 0x80
static const struct { uint16_t cp; uint8_t shape, cp437; } g_box[] = {
    { 0x2500, ARM_L | ARM_R, 0xC4 },         { 0x2502, ARM_U | ARM_D, 0xB3 },
    { 0x250C, ARM_R | ARM_D, 0xDA },         { 0x2510, ARM_L | ARM_D, 0xBF },
    { 0x2514, ARM_R | ARM_U, 0xC0 },         { 0x2518, ARM_L | ARM_U, 0xD9 },
    { 0x251C, ARM_U | ARM_D | ARM_R, 0xC3 }, { 0x2524, ARM_U | ARM_D | ARM_L, 0xB4 },
    { 0x252C, ARM_L | ARM_R | ARM_D, 0xC2 }, { 0x2534, ARM_L | ARM_R | ARM_U, 0xC1 },
    { 0x253C, ARM_L | ARM_R | ARM_U | ARM_D, 0xC5 },
    { 0x256D, ARM_R | ARM_D, 0xDA },         { 0x256E, ARM_L | ARM_D, 0xBF },   // rounded corners,
    { 0x256F, ARM_L | ARM_U, 0xD9 },         { 0x2570, ARM_R | ARM_U, 0xC0 },   // drawn square
    { 0x2580, BOX_TOP, 0xDF },    { 0x2584, BOX_BOTTOM, 0xDC }, { 0x2588, BOX_FULL, 0xDB },
    { 0x258C, BOX_LEFT, 0xDD },   { 0x2590, BOX_RIGHT, 0xDE },  { 0x2591, BOX_SHADE1, 0xB0 },
    { 0x2592, BOX_SHADE2, 0xB1 }, { 0x2593, BOX_SHADE3, 0xB2 },
};
#define BOX_COUNT ((int)(sizeof g_box / sizeof g_box[0]))
_Static_assert(BOX_COUNT <= 0x20, "the box glyphs fit the C1 slots");

static int box_index(char c) {
    int i = (unsigned char)c - BOX_SLOT0;
    return i >= 0 && i < BOX_COUNT ? i : -1;
}

static uint8_t box_cp437(char c) {
    int i = box_index(c);
    return i < 0 ? (uint8_t)c : g_box[i].cp437;
}

// One cell: a font glyph, or a box slot drawn as rectangles.
static void cell_draw(int x, int y, char c, uint32_t fg, uint32_t bg) {
    int i = box_index(c);
    if (i < 0) { gfx_draw_char(x, y, c, fg, bg); return; }
    int w = CELL_W, h = CELL_H, shape = g_box[i].shape;
    gfx_fill_rect(x, y, w, h, bg);
    if (shape < BOX_FULL) {
        int t = w / 8 > 1 ? w / 8 : 1;   // the stroke: a pixel at 8 wide, more on a big font
        int cx = x + (w - t) / 2, cy = y + (h - t) / 2;
        if (shape & ARM_L) gfx_fill_rect(x, cy, cx - x + t, t, fg);
        if (shape & ARM_R) gfx_fill_rect(cx, cy, x + w - cx, t, fg);
        if (shape & ARM_U) gfx_fill_rect(cx, y, t, cy - y + t, fg);
        if (shape & ARM_D) gfx_fill_rect(cx, cy, t, y + h - cy, fg);
        return;
    }
    switch (shape) {
    case BOX_FULL:   gfx_fill_rect(x, y, w, h, fg); return;
    case BOX_TOP:    gfx_fill_rect(x, y, w, h / 2, fg); return;
    case BOX_BOTTOM: gfx_fill_rect(x, y + h / 2, w, h - h / 2, fg); return;
    case BOX_LEFT:   gfx_fill_rect(x, y, w / 2, h, fg); return;
    case BOX_RIGHT:  gfx_fill_rect(x + w / 2, y, w - w / 2, h, fg); return;
    default: break;
    }
    // The shades: a quarter, a half (a checkerboard) or three quarters of
    // the pixels in the foreground, as CP437's dot patterns are.
    for (int r = 0; r < h; r++)
        for (int k = 0; k < w; k++) {
            int on = shape == BOX_SHADE2 ? ((r + k) & 1)
                   : shape == BOX_SHADE1 ? (!(r & 1) && !(k & 1))
                   : !((r & 1) && (k & 1));
            if (on) gfx_fill_rect(x + k, y + r, 1, 1, fg);
        }
}

// ---- the screen: the SHOWN vc's cells as pixels (or 0xB8000) ---------
//
// Called only for the shown vc -- vc_put() and friends below decide.
// The fb backend draws into the back buffer; presenting is separate.

static void scr_cell(size_t r, size_t c, uint16_t cell) {
    char ch = (char)(cell & 0xFF);
    uint8_t a = (uint8_t)(cell >> 8);
    if (fb_mode)
        cell_draw((int)(c * CELL_W), (int)(r * CELL_H), ch,
                  palette_rgb((enum vga_color)(a & 0x0F)),
                  palette_rgb((enum vga_color)(a >> 4)));
    else
        buf[r * VGA_WIDTH + c] = make_entry(ch, a);
}

// Cells c0..c1 of row r, blank in attribute `a`.
static void scr_blank(size_t r, size_t c0, size_t c1, uint8_t a) {
    if (fb_mode) {
        // One rect rather than one per cell: the whole point of erasing
        // is that it is cheaper than printing spaces.
        gfx_fill_rect((int)(c0 * CELL_W), (int)(r * CELL_H),
                      (int)((c1 - c0 + 1) * CELL_W), (int)CELL_H,
                      palette_rgb((enum vga_color)(a >> 4)));
        return;
    }
    for (size_t x = c0; x <= c1; x++) buf[r * VGA_WIDTH + x] = make_entry(' ', a);
}

// Rows s->top..s->bot move by s->n (up when positive); the uncovered
// ones are blank in `a`. The WHOLE screen moving up is gfx_scroll_up(),
// which also carries the strip below the last whole row.
static void scr_shift(const struct ansi_shift *s, uint8_t a) {
    int n = s->n > 0 ? s->n : -s->n, keep = s->bot - s->top + 1 - n;
    if (fb_mode) {
        uint32_t bg = palette_rgb((enum vga_color)(a >> 4));
        if (s->top == 0 && s->bot == (int)console_rows - 1 && s->n > 0)
            gfx_scroll_up(n * (int)CELL_H, bg);
        else
            gfx_scroll_band(s->top * (int)CELL_H, (s->bot - s->top + 1) * (int)CELL_H,
                            s->n * (int)CELL_H, bg);
        return;
    }
    uint16_t blank = make_entry(' ', a);
    if (s->n > 0) {
        for (int r = s->top; r < s->top + keep; r++)
            for (size_t x = 0; x < VGA_WIDTH; x++) buf[r * VGA_WIDTH + x] = buf[(r + n) * VGA_WIDTH + x];
        for (int r = s->bot - n + 1; r <= s->bot; r++)
            for (size_t x = 0; x < VGA_WIDTH; x++) buf[r * VGA_WIDTH + x] = blank;
    } else {
        for (int r = s->bot; r >= s->top + n; r--)
            for (size_t x = 0; x < VGA_WIDTH; x++) buf[r * VGA_WIDTH + x] = buf[(r - n) * VGA_WIDTH + x];
        for (int r = s->top; r < s->top + n; r++)
            for (size_t x = 0; x < VGA_WIDTH; x++) buf[r * VGA_WIDTH + x] = blank;
    }
}

static void scr_clear(uint8_t a) {
    if (fb_mode) {
        gfx_clear(palette_rgb((enum vga_color)(a >> 4)));
        cursor_on_screen = 0; // whatever was drawn is gone along with everything else
        return;
    }
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) buf[i] = make_entry(' ', a);
}

// ---- a vc's cells: the grid always, the screen when shown -----------

// The grid matches the screen, or it is not used: a vc whose grid is
// from before a geometry change (or that has none) draws without one.
static int vc_has_grid(const struct vc *v) {
    return v->grid && v->grid_rows == console_rows && v->grid_cols == console_cols;
}

// A blank grid of the screen's size, replacing whatever was there.
static int vc_grid_alloc(struct vc *v) {
    size_t n = console_rows * console_cols;
    uint16_t *g = n ? kmalloc(n * sizeof *g) : NULL;
    if (!g) return 0;
    for (size_t i = 0; i < n; i++) g[i] = CELL(' ', BLANK_ATTR);
    if (v->grid) kfree(v->grid);
    v->grid = g;
    v->grid_rows = console_rows;
    v->grid_cols = console_cols;
    return 1;
}

static void vc_put(struct vc *v, size_t r, size_t c, char ch, uint8_t a) {
    if (r >= console_rows || c >= console_cols) return;
    uint16_t cell = CELL(ch, a);
    if (vc_has_grid(v)) v->grid[r * console_cols + c] = cell;
    if (vc_shown(v)) scr_cell(r, c, cell);
}

// Cells c0..c1 (inclusive) of row r.
static void vc_blank(struct vc *v, size_t r, size_t c0, size_t c1, uint8_t a) {
    if (r >= console_rows || c0 > c1 || c0 >= console_cols) return;
    if (c1 >= console_cols) c1 = console_cols - 1;
    if (vc_has_grid(v))
        for (size_t c = c0; c <= c1; c++) v->grid[r * console_cols + c] = CELL(' ', a);
    if (vc_shown(v)) scr_blank(r, c0, c1, a);
}

static void vc_shift(struct vc *v, const struct ansi_shift *s, uint8_t a) {
    if (vc_has_grid(v)) {
        size_t w = console_cols;
        int n = s->n > 0 ? s->n : -s->n;
        if (n > s->bot - s->top + 1) n = s->bot - s->top + 1;
        uint16_t *g = v->grid;
        if (s->n > 0) {
            for (int r = s->top; r + n <= s->bot; r++)
                k_memcpy(&g[(size_t)r * w], &g[(size_t)(r + n) * w], w * sizeof *g);
            for (int r = s->bot - n + 1; r <= s->bot; r++)
                for (size_t c = 0; c < w; c++) g[(size_t)r * w + c] = CELL(' ', a);
        } else {
            for (int r = s->bot; r - n >= s->top; r--)
                k_memcpy(&g[(size_t)r * w], &g[(size_t)(r - n) * w], w * sizeof *g);
            for (int r = s->top; r < s->top + n; r++)
                for (size_t c = 0; c < w; c++) g[(size_t)r * w + c] = CELL(' ', a);
        }
    }
    if (vc_shown(v)) scr_shift(s, a);
}

// The whole screen again, from the grid -- a switch, a return from the
// scrollback, the desktop going away. A blank default cell is the clear
// already, so only what was written is drawn.
static void vc_redraw(struct vc *v) {
    scr_clear(BLANK_ATTR);
    if (vc_has_grid(v)) {
        for (size_t r = 0; r < console_rows; r++)
            for (size_t c = 0; c < console_cols; c++) {
                uint16_t cell = v->grid[r * console_cols + c];
                if (cell != CELL(' ', BLANK_ATTR)) scr_cell(r, c, cell);
            }
    }
    vc_cursor_on(v);
}

static void vc_clear(struct vc *v) {
    vc_cursor_off(v);
    if (vc_has_grid(v))
        for (size_t i = 0; i < console_rows * console_cols; i++)
            v->grid[i] = CELL(' ', vc_attr(v));
    if (vc_shown(v)) scr_clear(vc_attr(v));
    v->row = v->col = 0;
    v->region_rows = 0; // a cleared console scrolls as a whole again
    vc_cursor_on(v);
}

// `raw`: a '\n' is a bare LINE FEED (a terminal's bytes, ONLCR already
// applied by the discipline) rather than the kernel's own newline, which
// also returns the carriage -- see vga_putc_console().
static void vc_putc(struct vc *v, char c, int raw) {
    vc_cursor_off(v); // erase before row/col move -- see cursor_hide()
    if (c == '\n') {
        if (!raw) v->col = 0;
        line_feed(v, raw);
    } else if (c == '\r') {
        v->col = 0;
    } else {
        vc_put(v, v->row, v->col, c, vc_attr(v));
        v->col++;
        if (v->col >= console_cols) {
            v->col = 0;
            line_feed(v, raw);
        }
    }
    vc_cursor_on(v);
}

static void vc_backspace(struct vc *v) {
    vc_cursor_off(v);
    if (v->col == 0) {
        if (v->row == 0) { vc_cursor_on(v); return; }
        v->row--;
        v->col = console_cols - 1;
    } else {
        v->col--;
    }
    vc_put(v, v->row, v->col, ' ', vc_attr(v));
    vc_cursor_on(v);
}

// ---------------------------------------------------------------------
// Scrollback
//
// The console draws glyphs into the back buffer and scrolls by moving
// pixels. Nothing keeps what scrolled off, which is why the boot
// messages -- and the output of anything longer than a screen -- used
// to be simply gone. The GUI Terminal has had scrollback since its
// text_scrollback widget existed; the physical console never did.
//
// This is a ring of logical output LINES per vc, recorded at the
// putc level so it works the same for both backends. It stores the
// character and its colour per cell (one byte each), because output
// here is routinely multi-coloured within a line -- a green prompt then
// grey input, `ls`'s per-type colouring -- and replaying it in a single
// colour would be a visibly worse copy of what you saw.
//
// VT1's is a fixed .bss array rather than a heap allocation: the
// kernel's console must work whatever the heap does, and its size is a
// compile-time constant either way. 256 lines x 512 cols x 2 bytes =
// 256KB. 512 columns covers the widest this console gets at the largest
// mode display.c will now ask for (3840px / the 8pt font's cell width is
// 480); a narrower screen or a wider font just leaves the tail of each
// row unused. VT2-4's are allocated when the VT is first used, as wide
// as the screen is then, and widened by vga_reflow() when a smaller
// font makes the screen wider (sb_fit()).
//
// It was 256, sized against a 1280px screen, and the failure it produced
// was quiet by design: sb_record_attr() simply stops recording past the
// ring's width, so scrollback of a wide console showed correct lines
// with their right-hand ends missing. Raise it alongside DISPLAY_MAX_W.
#define SB_LINES 256
#define SB_COLS  512

static char sb1_char[SB_LINES * SB_COLS];
static uint8_t sb1_attr[SB_LINES * SB_COLS];
static uint16_t sb1_len[SB_LINES];

static char *sb_line_chars(struct vc_sb *b, uint32_t line) {
    return b->chars + (size_t)(line % b->lines) * b->cols;
}
static uint8_t *sb_line_attrs(struct vc_sb *b, uint32_t line) {
    return b->attrs + (size_t)(line % b->lines) * b->cols;
}

// Oldest line still retained. Everything before it has been overwritten.
static uint32_t sb_oldest(const struct vc_sb *b) {
    return b->total >= b->lines ? b->total - b->lines + 1 : 0;
}

static void sb_start_line(struct vc_sb *b) {
    b->total++;
    b->cur_len = 0;
    b->len[b->total % b->lines] = 0;
}

// Records one character into the current scrollback line. Wrapping is
// recorded as a line break, matching what the screen actually showed --
// the alternative (unbounded logical lines re-wrapped at display time)
// would be more faithful to the text but would not reproduce the screen
// after a `fontsize` change, and reproducing the screen is the point.
static void sb_record_attr(struct vc *v, char c, uint8_t attr) {
    struct vc_sb *b = &v->sb;
    if (!b->chars) return;
    if (c == '\n') { sb_start_line(b); return; }
    if (c == '\r') { b->cur_len = 0; return; }

    if (b->cur_len >= b->cols) return; // beyond what's retained; the screen wraps and starts a new line
    uint32_t idx = b->total % b->lines;
    sb_line_chars(b, b->total)[b->cur_len] = c;
    sb_line_attrs(b, b->total)[b->cur_len] = attr;
    b->cur_len++;
    if (b->cur_len > b->len[idx]) b->len[idx] = (uint16_t)b->cur_len;
}

static void sb_record(struct vc *v, char c) { sb_record_attr(v, c, vc_attr(v)); }

static void sb_record_backspace(struct vc *v) {
    struct vc_sb *b = &v->sb;
    if (!b->chars || b->cur_len == 0) return;
    b->cur_len--;
    uint32_t idx = b->total % b->lines;
    if (b->len[idx] > b->cur_len) b->len[idx] = (uint16_t)b->cur_len;
}

// A ring of `lines` x `cols`, keeping what `old` held (its lines, cut to
// the new width). 0 when there is no memory -- the old one stays.
static int sb_alloc(struct vc_sb *b, uint32_t lines, uint32_t cols) {
    char *ch = kmalloc((size_t)lines * cols);
    uint8_t *at = kmalloc((size_t)lines * cols);
    uint16_t *len = kzalloc((size_t)lines * sizeof *len);
    if (!ch || !at || !len) {
        if (ch) kfree(ch);
        if (at) kfree(at);
        if (len) kfree(len);
        return 0;
    }
    struct vc_sb n = { .chars = ch, .attrs = at, .len = len, .lines = lines, .cols = cols,
                       .total = b->total, .cur_len = b->cur_len, .view = 0 };
    if (b->chars) {
        for (uint32_t line = sb_oldest(b); line <= b->total; line++) {
            uint32_t w = b->len[line % b->lines];
            if (w > cols) w = cols;
            k_memcpy(sb_line_chars(&n, line), sb_line_chars(b, line), w);
            k_memcpy(sb_line_attrs(&n, line), sb_line_attrs(b, line), w);
            n.len[line % lines] = (uint16_t)w;
        }
        if (n.cur_len > cols) n.cur_len = cols;
        kfree(b->chars);
        kfree(b->attrs);
        kfree(b->len);
    }
    *b = n;
    return 1;
}

// A VT2-4 ring at least as wide as the screen. VT1's is static and
// always SB_COLS wide.
static void sb_fit(struct vc *v) {
    if (v == VC_KERNEL || !v->sb.chars || v->sb.cols >= console_cols) return;
    if (!sb_alloc(&v->sb, v->sb.lines, (uint32_t)console_cols))
        klog_printf("console: no memory to widen tty%d's scrollback\n", v->num);
}

// Paints history: `rows` lines ENDING `back` lines above the newest. The
// live screen (back 0) is the grid's, not this -- see vga_scroll_forward().
static void sb_repaint(struct vc *v, uint32_t back) {
    struct vc_sb *b = &v->sb;
    uint32_t rows = (uint32_t)console_rows;
    if (rows == 0 || !b->chars) return;

    cursor_hide();

    // The window is `rows` lines ENDING at (total - back), clamped so it
    // never starts before the oldest retained line -- and then filled
    // DOWNWARD from that start, so scrolling to the very top shows a
    // full screen of history rather than one line stranded at the top
    // with blankness under it.
    uint32_t oldest = sb_oldest(b);
    uint32_t newest = b->total - back;
    uint32_t first = (newest + 1 >= rows) ? newest + 1 - rows : 0;
    if (first < oldest) first = oldest;
    uint32_t last_possible = first + rows - 1;
    if (newest < last_possible) newest = last_possible > b->total ? b->total : last_possible;

    scr_clear(make_color(VGA_LIGHT_GREY, v->bg));
    size_t r = 0;
    for (uint32_t line = first; line <= newest && r < rows; line++, r++) {
        uint32_t len = b->len[line % b->lines];
        for (uint32_t i = 0; i < len && i < (uint32_t)console_cols; i++)
            scr_cell(r, i, CELL(sb_line_chars(b, line)[i], sb_line_attrs(b, line)[i]));
    }

    // A repaint is a whole new screen and it is the direct answer to a
    // PageUp/PageDown keypress, so it publishes immediately rather than
    // waiting for the idle work -- this is not part of an output burst.
    vga_present();
}

// Scrolling acts on the SHOWN vc: it is the one the keys are pressed at.
void vga_scroll_back(int lines) {
    struct vc *v = g_fg;
    struct vc_sb *b = &v->sb;
    if (lines <= 0 || console_rows == 0 || !b->chars) return;
    uint32_t available = b->total - sb_oldest(b);
    if (available == 0) return;   // no line completed yet: nothing behind the screen
    // How far back the view may go. When there's more history than fits,
    // that's "until the oldest line reaches the top of the screen".
    //
    // The `: 1` case matters and is easy to miss: vga_clear() wipes the
    // screen WITHOUT discarding history, and boot does exactly that via
    // vga_reflow() when the persisted font size loads. Afterwards the
    // screen shows three lines while the ring holds thirty -- all of
    // which "fit", so a formula based only on counting would conclude
    // there was nothing to scroll to. One step back is what reveals
    // them.
    uint32_t limit = available > (uint32_t)console_rows
                         ? available - (uint32_t)console_rows + 1
                         : 1;
    uint32_t want = b->view + (uint32_t)lines;
    if (want > limit) want = limit;
    if (want == b->view) return;
    b->view = want;
    sb_repaint(v, b->view);
}

void vga_scroll_forward(int lines) {
    struct vc *v = g_fg;
    struct vc_sb *b = &v->sb;
    if (lines <= 0 || b->view == 0) return;
    b->view = (uint32_t)lines >= b->view ? 0 : b->view - (uint32_t)lines;
    if (b->view) { sb_repaint(v, b->view); return; }
    // BACK TO LIVE IS THE GRID, not the ring's last lines: the ring is a
    // record of the byte stream, and a cursor-addressed screen is not.
    vc_redraw(v);
    vga_present();
}

int vga_scrolled_back(void) {
    return g_fg->sb.view != 0;
}

// Any new output snaps the view back to the bottom first -- otherwise
// writes would land on a screen showing history, and the two would
// interleave into nonsense. A hidden vc only forgets where it was.
static void sb_snap_to_live(struct vc *v) {
    if (v->sb.view == 0) return;
    v->sb.view = 0;
    if (vc_shown(v)) vc_redraw(v);
}

// ---- public API ----

void vga_init(void) {
    for (int i = 0; i < VGA_VC_COUNT; i++) {
        g_vc[i].num = i + 1;
        vc_reset(&g_vc[i]);
    }
    VC_KERNEL->sb = (struct vc_sb){ .chars = sb1_char, .attrs = sb1_attr, .len = sb1_len,
                                    .lines = SB_LINES, .cols = SB_COLS };

    if (gfx_init()) {
        fb_mode = 1;
        // Draw into the back buffer, not at the display -- see the
        // double-buffering comment at the top of this file for why the
        // console must never read the framebuffer. Falls back to drawing
        // straight at the screen if the surface is too large for
        // back_buffer, which still works, just slowly on a real machine.
        gfx_set_double_buffered(1);
        console_cols = (size_t)gfx_width() / CELL_W;
        console_rows = (size_t)gfx_height() / CELL_H;
    } else {
        fb_mode = 0;
        console_cols = VGA_WIDTH;
        console_rows = VGA_HEIGHT;
    }
    // The heap is up (kernel.c): VT1's grid exists from its first line.
    if (!vc_grid_alloc(VC_KERNEL)) klog_write("console: no memory for the screen grid\n");
    vc_clear(VC_KERNEL);
    if (fb_mode) {
        vga_present(); // a clear is a visible event in its own right, not part of a burst
        // klog_write(), not vga_write() -- serial_init() has already run
        // by the time kernel.c calls vga_init() (see kernel.c's ordering),
        // so this reaches dmesg fine even though the physical console
        // isn't ready to receive its own "SaunaOS booting..." line yet.
        klog_write("vga: linear framebuffer console active (");
        klog_write_dec((uint32_t)gfx_width());
        klog_write("x");
        klog_write_dec((uint32_t)gfx_height());
        klog_write(")\n");
    } else {
        klog_write("vga: no linear framebuffer from GRUB -- legacy text-mode console\n");
    }
}

const struct vga_sink *vga_set_sink(const struct vga_sink *sink) {
    const struct vga_sink *prev = active_sink;
    active_sink = sink;
    return prev;
}

int vga_sink_active(void) {
    return active_sink != 0;
}

void vga_clear(void) {
    if (active_sink) {
        if (active_sink->clear) active_sink->clear(active_sink->ctx);
        return;
    }
    vc_clear(VC_KERNEL);
    if (vc_shown(VC_KERNEL)) vga_present();
}

void vga_resume(void) {
    if (!fb_mode) return;
    // The window manager turned double buffering on for itself and the
    // back buffer still holds its last frame, which has nothing to do
    // with the console's idea of where text is. Re-enable (a no-op if it
    // is already on -- but it also resets the dirty box, which is right:
    // nothing of the console's is pending) and repaint the shown vc from
    // its grid, which kept every write while the desktop had the screen.
    gfx_set_double_buffered(1);
    g_fg->sb.view = 0;
    vc_redraw(g_fg);
    // FORCED, so this does not depend on the caller having already
    // cleared the compositor registration. It has (win_server.c's
    // compositor_gone() runs after), but a repaint whose whole point is
    // "the desktop is gone, show the console" must not be silently
    // skippable by a future reordering. NOT vga_present_force(), whose
    // switch to VT1 is a panic's: a desktop dying behind a text VT leaves
    // that VT shown.
    if (fb_mode) present_forced();
}

// A card with rounded corners: gfx has no rounded fill, and this is its
// one kernel caller.
static void fill_round(int x, int y, int w, int h, int r, uint32_t c) {
    gfx_fill_rect(x + r, y, w - 2 * r, h, c);
    gfx_fill_rect(x, y + r, r, h - 2 * r, c);
    gfx_fill_rect(x + w - r, y + r, r, h - 2 * r, c);
    gfx_fill_circle(x + r, y + r, r, c);
    gfx_fill_circle(x + w - r - 1, y + r, r, c);
    gfx_fill_circle(x + r, y + h - r - 1, r, c);
    gfx_fill_circle(x + w - r - 1, y + h - r - 1, r, c);
}

int vga_hold_begin(void) {
    if (!fb_mode) return 0;
    static const char title[] = "Restarting the desktop";
    static const char sub[] = "It stopped unexpectedly. Your windows will close.";
    int ch = gfx_char_h(), pad = ch;
    int spin = ch + ch / 2;
    gfx_set_bold(1);
    int tw = gfx_text_width(title);
    gfx_set_bold(0);
    int sw = gfx_text_width(sub);
    if (sw > tw) tw = sw;
    int w = pad + spin + pad + tw + pad + pad / 2, h = 2 * pad + 2 * ch + ch / 2;
    int x = (gfx_width() - w) / 2, y = (gfx_height() - h) / 2;
    fill_round(x, y, w, h, ch / 2, gfx_rgb(150, 160, 176));
    fill_round(x + 1, y + 1, w - 2, h - 2, ch / 2 - 1, gfx_rgb(244, 244, 246));
    // A still spinner: nothing runs to turn it, so it says "busy" by
    // shape -- a light ring with a quarter of it in the accent, from the
    // top round to the right. The arc is dots along the ring's middle,
    // close enough to read as one stroke; sines in thousandths, every
    // 7.5 degrees, since the kernel has no floating point.
    static const int SIN[13] = { 0, 131, 259, 383, 500, 609, 707, 793, 866, 924, 966, 991, 1000 };
    int cx = x + pad + spin / 2, cy = y + h / 2, r = spin / 2, t = r / 3;
    gfx_fill_circle(cx, cy, r, gfx_rgb(205, 220, 240));
    gfx_fill_circle(cx, cy, r - t, gfx_rgb(244, 244, 246));
    int mid = r - t / 2;
    for (int i = 0; i <= 12; i++)
        gfx_fill_circle(cx + mid * SIN[i] / 1000, cy - mid * SIN[12 - i] / 1000, t / 2,
                        gfx_rgb(70, 110, 160));
    int tx = x + pad + spin + pad, ty = y + pad;
    gfx_set_bold(1);
    gfx_draw_string(tx, ty, title, gfx_rgb(20, 20, 20), gfx_rgb(244, 244, 246));
    gfx_set_bold(0);
    gfx_draw_string(tx, ty + ch + ch / 2, sub, gfx_rgb(74, 74, 82), gfx_rgb(244, 244, 246));
    present_now();
    g_held = 1;
    g_held_until = coarse_ticks() + (uint64_t)VGA_HOLD_S * COARSE_HZ;
    return 1;
}

void vga_hold_end(int resume) {
    if (!g_held) return;
    g_held = 0;
    if (resume) vga_resume();
}

int vga_held(void) { return g_held; }

int vga_hold_expired(void) {
    if (!g_held || coarse_ticks() < g_held_until) return 0;
    klog_write("console: no desktop came back -- the console has the screen\n");
    vga_hold_end(1);
    return 1;
}

void vga_reflow(void) {
    if (!fb_mode) return; // legacy 80x25 text mode has no variable cell size
    scheduler_preempt_disable();
    cursor_hide();
    console_cols = (size_t)gfx_width() / CELL_W;
    console_rows = (size_t)gfx_height() / CELL_H;
    // EVERY vc STARTS AGAIN BLANK at the new size, as the console always
    // has on a font change; the history is kept (and widened).
    for (int i = 0; i < VGA_VC_COUNT; i++) {
        struct vc *v = &g_vc[i];
        if (!v->grid && v != VC_KERNEL) continue;   // never used
        if (!vc_grid_alloc(v))
            klog_printf("console: no memory for tty%d's grid at the new size\n", v->num);
        v->row = v->col = 0;
        v->region_rows = 0;
        v->sb.view = 0;
        sb_fit(v);
    }
    // THE REDRAW IS SKIPPED WHILE A COMPOSITOR OWNS THE SCREEN. Recomputing
    // the geometry is always right; painting is not -- it ends in a
    // present, which blits the console's whole buffer over whatever the
    // desktop has on screen. `fontsize` at a running desktop wiped the
    // top two thirds of it, and the console the user cannot see is the one
    // that does not need clearing. Same reasoning as the suspended keyboard
    // reader (win_server_set_compositor()); see docs/conventions/gui.md.
    if (!win_server_any() || !vc_shown(VC_KERNEL)) {
        vc_redraw(g_fg);
        vga_present();
    }
    scheduler_preempt_enable();
}

uint32_t vga_rows(void) {
    if (active_sink) {
        return active_sink->rows ? active_sink->rows(active_sink->ctx) : 24;
    }
    return (uint32_t)console_rows;
}

// Current console width in text columns -- 80 in legacy 80x25 text
// mode, or gfx_width()/gfx_char_w() in framebuffer mode (varies with
// the active font size, see vga_reflow()). Peer of vga_rows() above,
// added for the same reason: a caller doing its own line-wrapping math
// against the console (see apps/editor.c's windowed redraw) needs both
// dimensions, not just the row count console_page() already needed.
// Sinks don't track a column count any more than they track rows (see
// vga_rows()'s comment) -- 80 is a sane default for the same reason.
uint32_t vga_cols(void) {
    if (active_sink) return 80;
    return (uint32_t)console_cols;
}

// Called from the idle loops on every wake-up, but only actually does
// anything (a fill_rect) once every CURSOR_BLINK_TICKS, toggling the
// cursor block on/off -- and asks a tickless idle to wake for that
// moment. A plain no-op in legacy text mode, where the hardware cursor
// already blinks on its own.
void vga_cursor_tick(void) {
    // A sink owns its own cursor presentation (or has none) -- nothing
    // for the physical console's blink logic to do while one's active.
    if (active_sink) return;
    if (!fb_mode) return;
    if (coarse_ticks() - cursor_last_toggle_tick < CURSOR_BLINK_TICKS) {
        clockevent_idle_wake_at_tick(cursor_last_toggle_tick + CURSOR_BLINK_TICKS);
        return;
    }
    cursor_last_toggle_tick = coarse_ticks();
    clockevent_idle_wake_at_tick(cursor_last_toggle_tick + CURSOR_BLINK_TICKS);
    // Safe mid-line now that hiding restores the pixels it saved -- this
    // used to be suppressed off the append point, because the old
    // erase-to-black hide would have eaten the character underneath.
    if (cursor_on_screen) cursor_hide();
    else if (!g_fg->sb.view) cursor_draw();
    vga_present(); // the blink is its own event, not part of an output burst
}

// Repositions the insertion point without erasing anything -- see
// vga.h for the contract and why a line editor needs it. The kernel's
// console, VT1, like every other vga_* the kernel writes through.
//
// Works in text-cell space (one flat index over rows*cols) rather than
// per-row bookkeeping, so wrapping across a row boundary is just
// division and there's no separate "am I at column 0" case to get
// wrong.
void vga_cursor_move(int delta) {
    if (active_sink) return; // a sink owns its own cursor -- see vga_cursor_tick()
    if (delta == 0) return;
    struct vc *v = VC_KERNEL;
    sb_snap_to_live(v); // moving the cursor is an edit; don't do it while scrolled back

    uint32_t cols = (uint32_t)console_cols, rows = (uint32_t)console_rows;
    if (cols == 0 || rows == 0) return;

    vc_cursor_off(v); // restores whatever it covered, so moving off a character no longer eats it

    long target = (long)v->row * (long)cols + (long)v->col + delta;
    if (target < 0) target = 0;
    long last = (long)rows * (long)cols - 1;
    if (target > last) target = last;

    v->row = (size_t)(target / (long)cols);
    v->col = (size_t)(target % (long)cols);

    if (vc_shown(v) && fb_mode) cursor_draw();
    else if (vc_shown(v)) legacy_update_cursor(); // real hardware cursor; nothing to erase
}

// Public wrapper around cursor_hide() -- every vga_putc() call
// unconditionally repaints the cursor solid at the new (row, col)
// after drawing (see vc_putc()), which is exactly the
// interactive-typing behavior wanted at a shell prompt but looks like
// a stray block hovering past a program's own output when nothing is
// actually waiting for a keystroke -- e.g. `stress`'s progress bar,
// which redraws in place (see stress_print_progress() in
// kernel/shell/shell_sys.c) and never calls vga_cursor_tick() during its tight
// write/read loop, so the cursor painted by its own trailing pad
// spaces just sits there solid instead of blinking. A caller doing
// exactly that can call this once after its own output to suppress it
// -- the next real vga_write()/vga_putc() (e.g. the next prompt) shows
// a fresh one again automatically, no explicit "show" call needed.
// No-op outside framebuffer mode, same as vga_cursor_tick() above --
// legacy text mode's hardware cursor isn't under this kind of
// per-character software control.
void vga_cursor_hide(void) {
    if (!fb_mode || !vc_shown(VC_KERNEL)) return;
    cursor_hide();
}

static void vc_set_color(struct vc *v, enum vga_color fg, enum vga_color bg) {
    v->fg = fg;
    v->bg = bg;
}

void vga_set_color(enum vga_color fg, enum vga_color bg) {
    if (active_sink) {
        if (active_sink->set_color) active_sink->set_color(active_sink->ctx, fg, bg);
        return;
    }
    vc_set_color(VC_KERNEL, fg, bg);
}

enum vga_color vga_current_fg(void) {
    return VC_KERNEL->fg;
}

uint32_t vga_color_rgb(enum vga_color c) {
    return palette_rgb(c);
}

// --- ANSI cursor and erase ------------------------------------------
//
// ANSI ESCAPES ARE PARSED IN FRONT OF EVERYTHING ELSE (vc_emit()) --
// before the sink check, so a sink gets the COLOUR rather than the
// escape bytes. One parser per vc: a sequence is state, and two
// terminals writing at once must not finish each other's.
//
// The vc's own row/col ARE the terminal cursor -- there is no second
// position to keep in step, which is what makes this small.
//
// WHAT THE GRID FIXES AND THE SCROLLBACK DOES NOT: the grid is the
// screen, so a program that paints by moving the cursor is redrawn
// exactly on a switch or a return from history. The SCROLLBACK still
// records the byte stream (sb_record()), so such a program leaves a
// history that is not a transcript of anything. Real terminals have the
// same problem and answer it with an alternate screen buffer, which this
// console does not have. Erasing does not touch the history either --
// `ESC[2J` clears the SCREEN, and a terminal that threw away scrollback
// on it would be losing the user's data on a program's say-so. That is
// also why ED mode 3 ("and the scrollback") is treated as mode 2.
//
// Every op snaps to live first: moving or erasing is an edit, and doing
// one while the user is scrolled back would paint into a view of the
// past. vga_cursor_move() already had that rule.

// Clamps a 0-based coordinate to the screen and stores it.
static void ansi_goto(struct vc *v, size_t r, size_t c) {
    if (console_rows == 0 || console_cols == 0) return;
    if (r >= console_rows) r = console_rows - 1;
    if (c >= console_cols) c = console_cols - 1;
    v->row = r;
    v->col = c;
}

// Blanks an inclusive span of cells, in reading order from (r0,c0) to
// (r1,c1). Uses the CURRENT background, so erasing after a colour
// change paints what the program asked for rather than the default --
// which is how a full-screen program gets a coloured backdrop.
static void erase_span(struct vc *v, size_t r0, size_t c0, size_t r1, size_t c1) {
    size_t rows = console_rows, cols = console_cols;
    if (rows == 0 || cols == 0) return;
    if (r0 >= rows) return;
    if (r1 >= rows) r1 = rows - 1;
    if (c1 >= cols) c1 = cols - 1;
    for (size_t r = r0; r <= r1; r++) {
        size_t from = (r == r0) ? c0 : 0;
        size_t to = (r == r1) ? c1 : cols - 1;
        if (from > to || from >= cols) continue;
        vc_blank(v, r, from, to, vc_attr(v));
    }
}

// --- the scrolling region ------------------------------------------
//
// DECSTBM's region; ansi.c holds the rules (which rows a line feed, IL,
// DL, SU, SD or RI moves) and this only moves them. Kept for the row
// count it was set against: a font change that alters the grid puts it
// back to the whole screen, as a resize does in xterm.
static struct ansi_region *vc_region(struct vc *v) {
    if (v->region_rows != console_rows) {
        v->region_rows = console_rows;
        ansi_region_reset(&v->region, (int)v->region_rows);
    }
    return &v->region;
}

// Down a row; on the region's bottom row the REGION scrolls instead,
// with the rows coming in in the console's own colour.
//
// **THE KERNEL'S OWN TEXT IGNORES THE REGION** (`raw` 0), where Linux's
// printk honours it: a program that set one and died would otherwise
// trap every later klog line -- and a panic banner -- in its rows, or
// below them on a last row that never scrolls.
static void line_feed(struct vc *v, int raw) {
    int r = (int)v->row, c = (int)v->col;
    struct ansi_shift s;
    struct ansi_region whole;
    ansi_region_reset(&whole, (int)console_rows);
    if (ansi_region_op(raw ? vc_region(v) : &whole, (int)console_rows, ANSI_OP_LINEFEED, 1,
                       &r, &c, &s))
        vc_shift(v, &s, BLANK_ATTR);
    v->row = (size_t)r;
}

static void vc_ansi_ctrl(struct vc *v, const struct ansi_parser *p) {
    sb_snap_to_live(v);
    vc_cursor_off(v); // erase it before row/col move, as vga_cursor_move() does

    size_t rows = console_rows, cols = console_cols;
    size_t n = p->a;
    size_t row = v->row, col = v->col;

    switch (p->op) {
    case ANSI_OP_MOVE_TO: ansi_goto(v, p->a - 1, p->b - 1); break;
    case ANSI_OP_UP:      ansi_goto(v, row > n ? row - n : 0, col); break;
    case ANSI_OP_DOWN:    ansi_goto(v, row + n, col); break;
    case ANSI_OP_RIGHT:   ansi_goto(v, row, col + n); break;
    case ANSI_OP_LEFT:    ansi_goto(v, row, col > n ? col - n : 0); break;
    case ANSI_OP_COLUMN:  ansi_goto(v, row, p->a - 1); break;
    case ANSI_OP_ROW:     ansi_goto(v, p->a - 1, col); break;

    case ANSI_OP_ERASE_DISPLAY:
        if (p->a == 0)      erase_span(v, row, col, rows - 1, cols - 1);
        else if (p->a == 1) erase_span(v, 0, 0, row, col);
        else {
            erase_span(v, 0, 0, rows - 1, cols - 1);
            // ED 2 does NOT home the cursor -- that is `ESC[H` and
            // programs send both. Clearing and homing together is a
            // common shortcut and it breaks anything that erases, then
            // positions, then draws.
        }
        break;

    case ANSI_OP_ERASE_LINE:
        if (p->a == 0)      erase_span(v, row, col, row, cols - 1);
        else if (p->a == 1) erase_span(v, row, 0, row, col);
        else                erase_span(v, row, 0, row, cols - 1);
        break;

    case ANSI_OP_SAVE:    v->saved_row = row; v->saved_col = col; break;
    case ANSI_OP_RESTORE: ansi_goto(v, v->saved_row, v->saved_col); break;

    case ANSI_OP_HIDE:
        // vc_cursor_off() above already took it off the screen; the flag
        // is what stops the next draw putting it back.
        v->cursor_suppressed = 1;
        break;

    case ANSI_OP_MARGINS:
        // A region that took homes the cursor, as a VT100 does.
        if (ansi_region_set(vc_region(v), (int)rows, p->a, p->b)) { v->row = 0; v->col = 0; }
        break;
    case ANSI_OP_INSERT_LINES: case ANSI_OP_DELETE_LINES:
    case ANSI_OP_SCROLL_UP: case ANSI_OP_SCROLL_DOWN:
    case ANSI_OP_REVERSE_INDEX: {
        // In the CURRENT background, as an erase is (erase_span()).
        int r = (int)row, c = (int)col;
        struct ansi_shift s;
        if (ansi_region_op(vc_region(v), (int)rows, p->op, p->a, &r, &c, &s))
            vc_shift(v, &s, vc_attr(v));
        v->row = (size_t)r;
        v->col = (size_t)c;
        break;
    }
    case ANSI_OP_SHOW:
        v->cursor_suppressed = 0;
        break;
    default: break;
    }

    vc_cursor_on(v);
    // A cursor-addressed program paints in bursts and may never emit a
    // newline, so nothing else would push this to the screen.
    if (fb_mode && vc_shown(v)) vga_present();
}

// THE PHYSICAL CONSOLE, whatever sink is installed. A sink redirects the
// KERNEL SHELL's own output for the length of a command; ring-3 output
// to the console is somebody else's, and while the serial debug console
// ran a command it landed in that command's reply -- a leftover job's
// output inside the next answer. So a terminal's bytes never consult
// the sink (vc_emit()'s `sink` is NULL for them). PREEMPTION off, so
// the kernel shell cannot print in the middle -- not interrupts, since
// a scroll is megabytes of copying; an IRQ that echoes comes through
// here too, and nests.
//
// **AND ITS '\n' IS A BARE LINE FEED** -- xterm's and the VT100's: down a
// row, same column. What reaches here is tty0's output, which the line
// discipline has already put through ONLCR (`\n` -> `\r\n`), so a program
// that clears OPOST gets the line feed it asked for. The kernel's own
// writers -- vga_write(), the ring-0 shell, a panic -- reach vga_putc()
// instead and keep the newline that returns the carriage too, which is
// how Linux's vt_console_print() treats a printk next to the tty's path.
static void vc_emit(struct vc *v, char c, int raw, const struct vga_sink *sink);

void vga_putc_console(char c) {
    scheduler_preempt_disable();
    vc_emit(VC_KERNEL, c, 1, NULL);
    scheduler_preempt_enable();
}

void vga_vc_write(int n, const char *s, unsigned len) {
    struct vc *v = vc_at(n);
    if (!v) return;
    scheduler_preempt_disable();
    for (unsigned i = 0; i < len; i++) vc_emit(v, s[i], 1, NULL);
    scheduler_preempt_enable();
}

static void console_draw(struct vc *v, char c, int raw);

void vga_putc(char c) { vc_emit(VC_KERNEL, c, 0, active_sink); }

static void vc_emit(struct vc *v, char c, int raw, const struct vga_sink *sink) {
    if (!v->ansi_armed) { ansi_init(&v->ansi, v->fg, v->bg); v->ansi_armed = 1; }
    switch (ansi_feed(&v->ansi, c)) {
    case ANSI_PASS:
        break;
    case ANSI_SGR:
        if (sink) { if (sink->set_color) sink->set_color(sink->ctx, v->ansi.fg, v->ansi.bg); }
        else vc_set_color(v, v->ansi.fg, v->ansi.bg);
        return;
    case ANSI_CTRL:
        if (!sink) vc_ansi_ctrl(v, &v->ansi);   // a logger has no cursor -- see vga_sink
        return;
    case ANSI_EATEN:
    default:
        return;
    }

    if (sink) {
        if (c == '\b') {
            if (sink->backspace) sink->backspace(sink->ctx);
        } else {
            if (sink->putc) sink->putc(sink->ctx, c);
        }
        return;
    }

    // **THE CONSOLE DECODES UTF-8** (docs/utf8-design.md, "Decided here",
    // 10): what reaches it from a program, and what the keyboard now
    // types, is UTF-8, while its font stays the baked Latin-1 one -- the
    // boot font has to exist before any process does, as Linux's VT font
    // does. A character the font has is drawn; anything past it is ONE
    // `?`; a byte that is not UTF-8 is its Latin-1 letter (the transition
    // rule). A sink above gets the BYTES: what it carries is a transcript,
    // not a screen. One decoder per vc, so a sequence split across two
    // writes still draws once; allocation-free, as this runs from the
    // keyboard IRQ's echo too.
    uint32_t cps[2];
    int n = kutf8_stream_feed(&v->u8, (unsigned char)c, cps);
    for (int i = 0; i < n; i++) console_draw(v, (char)vga_console_glyph(cps[i]), raw);
}

int vga_console_glyph(uint32_t cp) {
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) return (int)cp;
    for (int i = 0; i < BOX_COUNT; i++)
        if (g_box[i].cp == cp) return BOX_SLOT0 + i;
    return '?';
}

static void vc_backspace_rec(struct vc *v) {
    sb_snap_to_live(v);
    sb_record_backspace(v);
    vc_backspace(v);
}

// One cell's worth of output, already decoded to the font's own byte.
// `raw`: see vga_putc_console().
static void console_draw(struct vc *v, char c, int raw) {
    // '\b' is a control code, not a printable glyph -- keyboard_read_line()
    // (the kernel's own line reader) never sends it through here, it calls
    // vga_backspace() directly. But a terminal's output carries whatever
    // bytes a ring-3 process hands it, one byte at a time, with no such
    // special-casing of its own -- so a process that writes a literal
    // '\b' (see userland/echo.c) needs this to actually erase-and-move-back
    // instead of drawing font_ttf's glyph for character 8, which is
    // neither of those things (see gfx_draw_char).
    if (c == '\b') {
        vc_backspace_rec(v);
        return;
    }
    sb_snap_to_live(v);

    // A BARE LINE FEED keeps the column, so the scrollback's new line
    // starts with as many blanks as the screen's does -- or a transcript
    // of a program that turned ONLCR off would not look like its screen.
    if (c == '\n' && raw) {
        size_t at = v->col;
        sb_record(v, '\n');
        // The console's own colours: nothing was painted in those cells.
        for (size_t i = 0; i < at; i++) sb_record_attr(v, ' ', BLANK_ATTR);
        vc_putc(v, c, 1);
        if (vc_shown(v)) fb_present_throttled();
        return;
    }

    // THE KERNEL'S NEWLINE (not the tty's, above): while a non-default
    // background is set it PADS TO THE END OF
    // THE LINE with spaces instead of just moving down, so a coloured
    // run reads as one full-width band (a panic banner) rather than as a
    // ragged highlight whose right edge depends on the text length.
    //
    // Done here, in the one funnel, so the padding goes through
    // sb_record() as well -- a fix applied in the putc alone would look
    // right until the user pressed PageUp and the line redrew from
    // scrollback without it.
    //
    // The padding REPLACES the newline: writing to the last column wraps
    // (col back to 0, row++), which is the same move, so emitting '\n'
    // as well would leave a blank line behind.
    if (c == '\n' && v->bg != CONSOLE_DEFAULT_BG) {
        // The count is computed ONCE, before any space is written.
        // Looping on `col < width` instead does not terminate: writing
        // the last column wraps col back to 0, which satisfies the
        // condition again, and the console fills solid.
        size_t pad = console_cols - v->col;
        for (size_t i = 0; i < pad; i++) {
            sb_record(v, ' ');
            vc_putc(v, ' ', 0);
        }
        // The screen's line break came from the wrap above, but
        // scrollback's has to be recorded explicitly -- sb_record('\n')
        // is what calls sb_start_line(). Without it every padded line
        // accumulates into ONE scrollback line, and a PageUp/PageDown
        // round trip redraws the banner as a single stripe with four of
        // its five lines missing.
        sb_record(v, '\n');
        if (vc_shown(v)) fb_present_throttled();
        return;
    }

    sb_record(v, c);
    vc_putc(v, c, raw);
    if (vc_shown(v)) fb_present_throttled();
}

void vga_backspace(void) {
    if (active_sink) {
        if (active_sink->backspace) active_sink->backspace(active_sink->ctx);
        return;
    }
    vc_backspace_rec(VC_KERNEL);
}

void vga_write(const char *s) {
    while (*s) vga_putc(*s++);
}

// These two are now three lines each over knum.h's converters. They
// used to be a full digit loop apiece -- and klog.c had its own
// identical pair, with a comment explaining that duplicating them was
// cheaper than the dependency. knum.c is that dependency, deliberately
// depending on nothing itself, so both sinks can share one
// implementation (and one set of tests) without either pulling in the
// other.
void vga_write_dec(uint32_t n) {
    char buf[21];
    k_utoa(n, buf, sizeof buf);
    vga_write(buf);
}

void vga_write_exit_code(int code) {
    // Negative here only ever means PROCESS_CRASHED (process.h) in
    // practice -- no real exit code produces one (see its comment) --
    // but this stays generic ("any negative value") rather than
    // importing process.h and comparing against that exact constant, to
    // avoid a driver (this file) depending on kernel/core.
    if (code < 0) {
        vga_write("CRASHED");
    } else {
        vga_write_dec((uint32_t)code);
    }
}

void vga_write_hex(uint64_t n) {
    char buf[17];
    k_htoa(n, buf, sizeof buf, 0); // 0 = shortest form, no leading zeros
    vga_write("0x");
    vga_write(buf);
}

// ---- switching ------------------------------------------------------
//
// kernel/tty/vt.c decides WHEN (a keypress, deferred to idle work); this
// only moves the screen.

int vga_vc_shown(void) { return g_fg->num; }

int vga_vc_prepare(int n) {
    struct vc *v = vc_at(n);
    if (!v) return 0;
    if (!vc_has_grid(v) && !vc_grid_alloc(v)) return 0;
    if (!v->sb.chars && !sb_alloc(&v->sb, SB_LINES, (uint32_t)console_cols))
        klog_printf("console: no memory for tty%d's scrollback -- it has none\n", n);
    return 1;
}

int vga_vc_show(int n) {
    struct vc *v = vc_at(n);
    if (!v || !vc_has_grid(v)) return 0;
    if (v == g_fg) return 1;
    scheduler_preempt_disable();
    if (fb_mode) cursor_hide();   // the old screen's, wherever it was painted
    g_fg = v;
    v->sb.view = 0;               // a switch shows the screen, not where its history was left
    vc_redraw(v);
    scheduler_preempt_enable();
    vga_present();
    return 1;
}

void vga_vc_clear(int n) {
    struct vc *v = vc_at(n);
    if (!v) return;
    scheduler_preempt_disable();
    vc_reset(v);
    vc_clear(v);
    scheduler_preempt_enable();
    if (vc_shown(v)) vga_present();
}

void vga_vc_size(uint16_t *rows, uint16_t *cols) {
    *rows = (uint16_t)console_rows;
    *cols = (uint16_t)console_cols;
}
