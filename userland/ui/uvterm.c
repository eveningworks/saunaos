// One terminal -- see ui/uvterm.h. Moved out of the GUI Terminal
// (userland/gui/apps/terminal.c), which now hosts one per tab.
#include "ui/uvterm.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>      // chdir/getcwd, around the spawn

#include "rt/sys.h"
#include "ui/uapp.h"
#include "keyboard.h"
#include "termkey.h"     // a keysym becomes an ANSI sequence here
#include "lib/uutf8.h"   // cells hold characters; the stream is UTF-8
#include "lib/uwcwidth.h" // ...and a wide one takes two cells

// --- the buffers ---------------------------------------------------------------

// Flat, strided by the ALLOCATED width, which is what lets one realloc
// change the geometry without touching any of the code below.
static struct uvterm_cell *grid_row(const struct uvterm *t, int r) { return t->grid + (size_t)r * t->cap_cols; }
static struct uvterm_cell *sb_row(const struct uvterm *t, int r) { return t->sb + (size_t)r * t->cap_cols; }
static struct uvterm_cell *saved_row(const struct uvterm *t, int r) { return t->saved + (size_t)r * t->cap_cols; }

static struct uvterm_cell *cells_new(int rows, int cols, int fg, int bg) {
    size_t n = (size_t)rows * (size_t)cols;
    struct uvterm_cell *p = malloc(n * sizeof *p);
    if (!p) return 0;
    for (size_t i = 0; i < n; i++) {
        p[i].ch = ' '; p[i].fg = (uint8_t)fg; p[i].bg = (uint8_t)bg;
        p[i].wide = UVTERM_NARROW; p[i].pad_ = 0;
    }
    return p;
}

// Grows the allocation to hold rows x cols, keeping what is there (a
// crop-preserving copy -- xterm's behaviour; there is no reflow).
// Everything is allocated before anything is swapped, so a failed malloc
// leaves the terminal exactly as it was.
static int grow_caps(struct uvterm *t, int rows, int cols) {
    if (rows <= t->cap_rows && cols <= t->cap_cols && t->grid) return 1;
    int nr = rows > t->cap_rows ? rows : t->cap_rows;
    int nc = cols > t->cap_cols ? cols : t->cap_cols;
    struct uvterm_cell *ng = cells_new(nr, nc, t->fg, t->bg);
    struct uvterm_cell *nb = cells_new(t->sb_rows, nc, t->fg, t->bg);
    struct uvterm_cell *nv = cells_new(nr, nc, t->fg, t->bg);
    // UTF-8 scratch: up to four bytes a cell.
    char *rb = malloc((size_t)nc * 4 + 1), *ob = malloc((size_t)nc * 4 + 1);
    if (!ng || !nb || !nv || !rb || !ob) {
        free(ng); free(nb); free(nv); free(rb); free(ob);
        return 0;
    }
    if (t->grid) {
        for (int r = 0; r < t->cap_rows; r++)
            for (int c = 0; c < t->cap_cols; c++) {
                ng[(size_t)r * nc + c] = grid_row(t, r)[c];
                nv[(size_t)r * nc + c] = saved_row(t, r)[c];
            }
        for (int r = 0; r < t->sb_rows; r++)
            for (int c = 0; c < t->cap_cols; c++)
                nb[(size_t)r * nc + c] = sb_row(t, r)[c];
    }
    free(t->grid); free(t->sb); free(t->saved); free(t->runbuf); free(t->rowbuf);
    t->grid = ng; t->sb = nb; t->saved = nv; t->runbuf = rb; t->rowbuf = ob;
    t->cap_rows = nr;
    t->cap_cols = nc;
    return 1;
}

static void row_clear(const struct uvterm *t, struct uvterm_cell *row, int from) {
    for (int c = from; c < t->cap_cols; c++) {
        row[c].ch = ' '; row[c].fg = t->fg; row[c].bg = t->bg; row[c].wide = UVTERM_NARROW;
    }
}

// A blank in place of what was there, colours kept -- ERASE's mode 1.
static void blank_cell(struct uvterm_cell *c) { c->ch = ' '; c->wide = UVTERM_NARROW; }

static void reset_screen(struct uvterm *t) {
    uvterm_sel_clear(t);
    for (int r = 0; r < t->cap_rows; r++) row_clear(t, grid_row(t, r), 0);
    t->cr = t->cc = 0;
}

int uvterm_init(struct uvterm *t, int rows, int cols, int sb_rows, int fg, int bg) {
    // ZEROED, keeping the buffers: a recycled terminal must not inherit
    // the last program's screen, and its allocation is reused rather than
    // freed and made again.
    struct uvterm_cell *g = t->grid, *b = t->sb, *v = t->saved;
    char *rb = t->runbuf, *ob = t->rowbuf;
    int cr = t->cap_rows, cc = t->cap_cols, old_sb = t->sb_rows;
    memset(t, 0, sizeof *t);
    t->grid = g; t->sb = b; t->saved = v; t->runbuf = rb; t->rowbuf = ob;
    t->cap_rows = cr; t->cap_cols = cc; t->sb_rows = old_sb;
    t->fg = (uint8_t)fg;
    t->bg = (uint8_t)bg;
    t->master = -1;
    t->scroll_on_output = 1;
    if (t->grid && t->sb_rows != sb_rows) uvterm_set_scrollback(t, sb_rows);
    t->sb_rows = sb_rows;
    if (!grow_caps(t, rows, cols)) return 0;
    t->rows = rows;
    t->cols = cols;
    t->cursor_shown = 1;
    ansi_init(&t->vt, t->fg, t->bg);
    reset_screen(t);
    return 1;
}

void uvterm_free(struct uvterm *t) {
    free(t->grid); free(t->sb); free(t->saved); free(t->runbuf); free(t->rowbuf);
    t->grid = t->sb = t->saved = 0;
    t->runbuf = t->rowbuf = 0;
    t->cap_rows = t->cap_cols = 0;
}

// --- the screen ----------------------------------------------------------------

// The top line leaves the screen and becomes history. THE ONLY PLACE A
// STREAM STILL EXISTS -- scrollback is a record of what went past, while
// the screen is a thing being drawn on.
static void scroll_up(struct uvterm *t) {
    uvterm_sel_clear(t);
    size_t rowbytes = (size_t)t->cap_cols * sizeof(struct uvterm_cell);
    if (t->sb_count == t->sb_rows) {
        memmove(sb_row(t, 0), sb_row(t, 1), (size_t)(t->sb_rows - 1) * rowbytes);
        t->sb_count--;
    }
    memcpy(sb_row(t, t->sb_count), grid_row(t, 0), rowbytes);
    t->sb_count++;
    memmove(grid_row(t, 0), grid_row(t, 1), (size_t)(t->rows - 1) * rowbytes);
    row_clear(t, grid_row(t, t->rows - 1), 0);
}

static void newline(struct uvterm *t) {
    t->cr++;
    if (t->cr >= t->rows) { t->cr = t->rows - 1; scroll_up(t); }
}

// Writing over HALF of a wide character blanks its other half, as xterm
// does -- a lone half would draw as nothing and copy as nothing.
static void unpair(const struct uvterm *t, struct uvterm_cell *row, int c) {
    if (c < 0 || c >= t->cap_cols) return;
    if (row[c].wide == UVTERM_CONT && c > 0) blank_cell(&row[c - 1]);
    if (row[c].wide == UVTERM_WIDE && c + 1 < t->cap_cols) blank_cell(&row[c + 1]);
}

// One decoded CHARACTER into the grid.
static void putc_raw(struct uvterm *t, uint32_t c) {
    if (c == '\n') { t->cc = 0; newline(t); return; } // no OPOST: LF is CRLF here
    if (c == '\r') { t->cc = 0; return; }
    if (c == '\b') { if (t->cc > 0) t->cc--; return; }
    if (c == '\t') {
        do { t->cc++; } while (t->cc % 8 && t->cc < t->cols);
        if (t->cc >= t->cols) { t->cc = 0; newline(t); }
        return;
    }
    // Anything else unprintable is dropped: the C0 controls, and the C1
    // ones a UTF-8 stream can name (U+0080-U+009F).
    if (c < 32 || (c >= 0x80 && c < 0xA0)) return;
    int w = uwcwidth(c);
    if (w > t->cols) w = 1;
    // A WIDE CHARACTER NEVER STRADDLES THE EDGE: one that does not fit
    // starts the next line, as xterm and VTE wrap it.
    if (t->cc + w > t->cols) { t->cc = 0; newline(t); }
    struct uvterm_cell *row = grid_row(t, t->cr);
    unpair(t, row, t->cc);
    if (w == 2) unpair(t, row, t->cc + 1);
    struct uvterm_cell *cell = &row[t->cc];
    cell->ch = c;
    cell->fg = (uint8_t)t->vt.fg;
    cell->bg = (uint8_t)t->vt.bg;
    cell->wide = w == 2 ? UVTERM_WIDE : UVTERM_NARROW;
    if (w == 2) {
        cell[1].ch = 0;
        cell[1].fg = cell->fg;
        cell[1].bg = cell->bg;
        cell[1].wide = UVTERM_CONT;
    }
    t->cc += w;
}

static void clamp_cursor(struct uvterm *t) {
    if (t->cr < 0) t->cr = 0;
    if (t->cr >= t->rows) t->cr = t->rows - 1;
    if (t->cc < 0) t->cc = 0;
    if (t->cc >= t->cols) t->cc = t->cols - 1;
}

// One completed cursor/erase sequence. The parser has already applied
// every default ("a missing count means 1", 1-based rows), so this only
// acts -- the point of it being a shared parser.
static void ctrl(struct uvterm *t) {
    switch (t->vt.op) {
    case ANSI_OP_MOVE_TO: t->cr = t->vt.a - 1; t->cc = t->vt.b - 1; break;
    case ANSI_OP_UP:      t->cr -= t->vt.a; break;
    case ANSI_OP_DOWN:    t->cr += t->vt.a; break;
    case ANSI_OP_RIGHT:   t->cc += t->vt.a; break;
    case ANSI_OP_LEFT:    t->cc -= t->vt.a; break;
    case ANSI_OP_COLUMN:  t->cc = t->vt.a - 1; break;
    case ANSI_OP_ROW:     t->cr = t->vt.a - 1; break;
    case ANSI_OP_ERASE_LINE:
        if (t->vt.a == 0) row_clear(t, grid_row(t, t->cr), t->cc);
        else if (t->vt.a == 1)
            for (int c = 0; c <= t->cc && c < t->cap_cols; c++) blank_cell(&grid_row(t, t->cr)[c]);
        else row_clear(t, grid_row(t, t->cr), 0);
        break;
    case ANSI_OP_ERASE_DISPLAY:
        if (t->vt.a == 0) {
            row_clear(t, grid_row(t, t->cr), t->cc);
            for (int r = t->cr + 1; r < t->rows; r++) row_clear(t, grid_row(t, r), 0);
        } else if (t->vt.a == 1) {
            for (int r = 0; r < t->cr; r++) row_clear(t, grid_row(t, r), 0);
            for (int c = 0; c <= t->cc && c < t->cap_cols; c++) blank_cell(&grid_row(t, t->cr)[c]);
        } else {
            for (int r = 0; r < t->rows; r++) row_clear(t, grid_row(t, r), 0);
        }
        break;
    case ANSI_OP_ALT_ON:
        // Idempotent: switching twice must not overwrite the saved screen
        // with the alternate one.
        if (!t->alt) {
            memcpy(t->saved, t->grid, (size_t)t->cap_rows * t->cap_cols * sizeof(struct uvterm_cell));
            t->saved_cr = t->cr;
            t->saved_cc = t->cc;
            t->alt = 1;
        }
        for (int r = 0; r < t->rows; r++) row_clear(t, grid_row(t, r), 0);
        t->cr = t->cc = 0;
        break;
    case ANSI_OP_ALT_OFF:
        if (t->alt) {
            memcpy(t->grid, t->saved, (size_t)t->cap_rows * t->cap_cols * sizeof(struct uvterm_cell));
            t->cr = t->saved_cr;
            t->cc = t->saved_cc;
            t->alt = 0;
        }
        break;
    case ANSI_OP_SHOW: t->cursor_shown = 1; break;
    case ANSI_OP_HIDE: t->cursor_shown = 0; break;
    // SAVE/RESTORE have no users yet, and a half-remembered position is
    // worse than none.
    default: break;
    }
    clamp_cursor(t);
}

// A title the program sent, COPIED: the parser rewrites its own buffer on
// the next OSC. A path is also where the program stands (tosh's title is
// its cwd), taken even when the title is locked.
// CUT BETWEEN CHARACTERS: a title is UTF-8, and a copy that stops inside
// a sequence would end in a stray byte drawn as a Latin-1 letter.
static void copy_utf8(char *dst, int cap, const char *src) {
    int n = uutf8_cut(src, (int)strlen(src), cap - 1);
    memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
}

static void osc_title(struct uvterm *t) {
    if (t->vt.osc[0] == '/') copy_utf8(t->cwd, (int)sizeof t->cwd, t->vt.osc);
    if (t->title_locked) return;
    char want[UVTERM_TITLE_MAX];
    copy_utf8(want, (int)sizeof want, t->vt.osc);
    // An EMPTY title asks for the host's default, never for a blank label.
    if (!want[0] && t->default_title) copy_utf8(want, (int)sizeof want, t->default_title);
    if (strcmp(want, t->title) == 0) return;
    strlcpy(t->title, want, sizeof t->title);
    t->title_moved = 1;
}

void uvterm_write(struct uvterm *t, const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        switch (ansi_feed(&t->vt, buf[i])) {
        case ANSI_PASS: {
            // UTF-8, decoded across calls (`t->u8`): a byte completes 0,
            // 1 or 2 characters -- a stray byte is its Latin-1 letter.
            uint32_t cps[2];
            int n = kutf8_stream_feed(&t->u8, (unsigned char)buf[i], cps);
            for (int k = 0; k < n; k++) putc_raw(t, cps[k]);
            break;
        }
        case ANSI_CTRL:  ctrl(t); break;
        case ANSI_OSC:   osc_title(t); break;
        case ANSI_SGR:   break; // the colours are read off the parser per cell
        case ANSI_EATEN: break;
        }
    }
    // NEW OUTPUT PINS THE VIEW TO THE BOTTOM unless the host says not to:
    // a program printing while you read history brings you back, or what
    // you ran appears to have done nothing (Konsole offers the other).
    if (t->scroll_on_output) t->sb_view = 0;
}

// --- the program ---------------------------------------------------------------

static void *reader_main(void *arg) {
    struct uvterm *t = arg;
    char buf[512];
    for (;;) {
        // BLOCKING, which is the whole point of a reader thread.
        int64_t n = sys_read(t->master, buf, sizeof buf);
        if (n <= 0) break;   // the program is gone, or the master went away
        for (int64_t i = 0; i < n; i++) {
            unsigned head = __atomic_load_n(&t->head, __ATOMIC_RELAXED);
            unsigned next = (head + 1) % UVTERM_RING_BYTES;
            // BACK-PRESSURE RATHER THAN DROPPING: a dropped byte corrupts
            // the screen in a way nobody could diagnose from the result.
            while (next == __atomic_load_n(&t->tail, __ATOMIC_ACQUIRE)) sys_yield();
            t->ring[head] = buf[i];
            __atomic_store_n(&t->head, next, __ATOMIC_RELEASE);
        }
        uapp_post(t->app, t->post_id, 0);
    }
    t->eof = 1;
    uapp_post(t->app, t->post_id, 1);
    // LAST: nothing after this touches the terminal, so the host may
    // hand the struct to a new program.
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int uvterm_spawn(struct uvterm *t, const char *prog, const char *dir) {
    t->started_ns = sys_monotonic_ns();
    strlcpy(t->prog, prog, sizeof t->prog);
    int slave = -1;
    if (sys_openpty(&t->master, &slave) < 0) return 0;
    // The child's 0/1/2 are the slave: dup2 around the spawn, as tosh's
    // own redirection does -- SYS_SPAWN inherits the descriptor table.
    int in0 = sys_dup(0), out1 = sys_dup(1), err2 = sys_dup(2);
    sys_dup2(slave, 0);
    sys_dup2(slave, 1);
    sys_dup2(slave, 2);
    // THE CHILD INHERITS OUR CWD, so its directory is set by standing in
    // it for the spawn -- the same dance as the fds above.
    char back[160];
    int moved = dir && getcwd(back, sizeof back) && chdir(dir) == 0;
    if (moved) strlcpy(t->cwd, dir, sizeof t->cwd);
    // ITS OWN GROUP AND SESSION: this pty is its own terminal, so Ctrl-C
    // reaches the job on it and no other (abi/syscall_abi.h's SPAWN_SETSID).
    t->child = sys_spawn_flags(t->prog, 0, -1, 0, PGID_NEW, SPAWN_SETSID);
    if (moved) chdir(back);
    if (in0  >= 0) { sys_dup2(in0, 0);  sys_close(in0); }
    if (out1 >= 0) { sys_dup2(out1, 1); sys_close(out1); }
    if (err2 >= 0) { sys_dup2(err2, 2); sys_close(err2); }
    // OUR copy of the slave goes now, or the master never sees
    // end-of-file when the program dies.
    sys_close(slave);
    if (t->child < 0) { sys_close(t->master); t->master = -1; return 0; }

    struct tty_winsize ws = { (uint16_t)t->rows, (uint16_t)t->cols };
    sys_tcsetwinsz(t->master, &ws);
    t->live = 1;
    t->done = 0;
    // DETACHED: nothing joins it. The struct is recycled on `done`, which
    // carries what a join would have.
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->reader, &at, reader_main, t) != 0) {
        t->live = 0;
        sys_close(t->master);
        t->master = -1;
        return 0;
    }
    t->reader_started = 1;
    return 1;
}

void uvterm_stop(struct uvterm *t) {
    if (!t || !t->live) return;
    if (t->child > 0) {
        // SIGTERM, the polite one: the person closed the tab, nobody
        // force-quit the program. Closing the master alone would end it
        // only when it next READS, and a shell waiting on a job does not.
        sys_kill(t->child, SIGTERM);
        int status = 0;
        sys_waitpid(t->child, &status);
        t->child = 0;
    }
    t->live = 0;
}

int uvterm_drain(struct uvterm *t) {
    int got = 0;
    for (;;) {
        unsigned tail = __atomic_load_n(&t->tail, __ATOMIC_RELAXED);
        if (tail == __atomic_load_n(&t->head, __ATOMIC_ACQUIRE)) break;
        char c = t->ring[tail];
        __atomic_store_n(&t->tail, (tail + 1) % UVTERM_RING_BYTES, __ATOMIC_RELEASE);
        uvterm_write(t, &c, 1);
        got = 1;
    }
    if (got && !t->spoke) uvterm_release_held(t);
    return got;
}

// Every keystroke-shaped write goes through here, so type-ahead held
// before the program first spoke keeps its order. Past the hold buffer a
// key is written at once: losing it would be worse than an early echo.
void uvterm_send(struct uvterm *t, const char *buf, int n) {
    if (!t || t->master < 0 || n <= 0) return;
    if (!t->spoke && t->nheld + n <= (int)sizeof t->held) {
        memcpy(t->held + t->nheld, buf, (size_t)n);
        t->nheld += n;
        return;
    }
    sys_write(t->master, buf, (size_t)n);
}

void uvterm_release_held(struct uvterm *t) {
    t->spoke = 1;
    if (t->nheld > 0 && t->master >= 0) sys_write(t->master, t->held, (size_t)t->nheld);
    t->nheld = 0;
}

// **A TERMINAL EMULATOR TRANSLATES A KEYSYM INTO A SEQUENCE, and that is
// the whole job.** The toolkit delivers KEY_* (what an X11 or Wayland
// client receives) and the pty gets ANSI: Up is `ESC [ A`. Ctrl and Alt
// arrive as bits beside the key and are encoded by tty_input()'s rule:
// Ctrl with a character that is not a letter has no code and is not sent,
// and Alt is readline's meta prefix, ESC then the key -- never before a
// KEY_* special.
int uvterm_key(struct uvterm *t, int key, unsigned mods) {
    if (IS_PRINTABLE_KEY(key) && (mods & KEY_MOD_CTRL)) return 0;
    char seq[TERMKEY_MAX];
    int n = termkey_encode(key, seq, sizeof seq);
    if (n <= 0) return 0;
    if ((mods & KEY_MOD_ALT) && !IS_SPECIAL_KEY(key) && t->master >= 0) uvterm_send(t, "\x1b", 1);
    uvterm_send(t, seq, n);
    return 1;
}

// --- geometry ------------------------------------------------------------------

// **SHRINKING SCROLLS; GROWING PULLS BACK** -- xterm's semantics, which
// Konsole and VTE share: rows the cursor would fall past scroll off the
// top into history exactly as if printed, and growing takes them back,
// so the two are a ROUND TRIP. The whole grid moves by one amount, which
// keeps a shell's own idea of where its prompt started valid.
static void rows_shrunk(struct uvterm *t, int old_rows, int new_rows) {
    int over = t->cr - (new_rows - 1);
    if (over <= 0) return;
    if (over > old_rows) over = old_rows;
    size_t rowbytes = (size_t)t->cap_cols * sizeof(struct uvterm_cell);
    for (int i = 0; i < over; i++) {
        if (t->sb_count == t->sb_rows) {
            memmove(sb_row(t, 0), sb_row(t, 1), (size_t)(t->sb_rows - 1) * rowbytes);
            t->sb_count--;
        }
        memcpy(sb_row(t, t->sb_count), grid_row(t, 0), rowbytes);
        t->sb_count++;
        memmove(grid_row(t, 0), grid_row(t, 1), (size_t)(old_rows - 1) * rowbytes);
        row_clear(t, grid_row(t, old_rows - 1), 0);
    }
    t->cr -= over;
}

static void rows_grown(struct uvterm *t, int old_rows, int new_rows) {
    int want = new_rows - old_rows;
    int have = t->sb_count < want ? t->sb_count : want;
    if (have <= 0) return;
    size_t rowbytes = (size_t)t->cap_cols * sizeof(struct uvterm_cell);
    // Down first, from the bottom: the two ranges overlap.
    for (int r = old_rows - 1; r >= 0; r--) memcpy(grid_row(t, r + have), grid_row(t, r), rowbytes);
    for (int i = 0; i < have; i++) memcpy(grid_row(t, have - 1 - i), sb_row(t, --t->sb_count), rowbytes);
    for (int r = old_rows + have; r < new_rows; r++) row_clear(t, grid_row(t, r), 0);
    t->cr += have;
}

int uvterm_resize(struct uvterm *t, int rows, int cols) {
    int ok = grow_caps(t, rows, cols);
    if (!ok) {
        if (rows > t->cap_rows) rows = t->cap_rows;
        if (cols > t->cap_cols) cols = t->cap_cols;
    }
    if (rows == t->rows && cols == t->cols) return ok;
    int old_rows = t->rows;
    t->rows = rows;
    t->cols = cols;
    // THE ALTERNATE SCREEN IS NOT SCROLLED: it has no scrollback, and a
    // full-screen program redraws every row when told the size changed.
    if (!t->alt) {
        if (rows < old_rows)      rows_shrunk(t, old_rows, rows);
        else if (rows > old_rows) rows_grown(t, old_rows, rows);
    }
    clamp_cursor(t);
    // LAST, because it raises SIGWINCH: the program repaints from where
    // the cursor is, so the grid has to be in its final shape first.
    if (t->live && t->master >= 0) {
        struct tty_winsize ws = { (uint16_t)rows, (uint16_t)cols };
        sys_tcsetwinsz(t->master, &ws);
    }
    return ok;
}

void uvterm_set_scrollback(struct uvterm *t, int lines) {
    if (lines == t->sb_rows || lines < 1) return;
    if (!t->sb) { t->sb_rows = lines; return; }
    struct uvterm_cell *nb = cells_new(lines, t->cap_cols, t->fg, t->bg);
    if (!nb) return;   // the old depth: a smaller scrollback, never a broken one
    int keep = t->sb_count < lines ? t->sb_count : lines;
    int from = t->sb_count - keep;
    for (int r = 0; r < keep; r++)
        memcpy(nb + (size_t)r * t->cap_cols, sb_row(t, from + r),
               (size_t)t->cap_cols * sizeof(struct uvterm_cell));
    free(t->sb);
    t->sb = nb;
    t->sb_rows = lines;
    t->sb_count = keep;
    if (t->sb_view > keep) t->sb_view = keep;
}

// A cell stores an INDEX and has no "this is the default" bit, so a new
// scheme's pair would otherwise leave the scrollback in two colours the
// scheme reserves for something else. The halves move independently
// (`ls --color` sets a foreground over a default background).
void uvterm_set_default_pair(struct uvterm *t, int fg, int bg) {
    int old_fg = t->fg, old_bg = t->bg;
    t->fg = (uint8_t)fg;
    t->bg = (uint8_t)bg;
    t->vt.fg = (enum vga_color)fg;
    t->vt.bg = (enum vga_color)bg;
    if (old_fg == fg && old_bg == bg) return;
    struct uvterm_cell *banks[3] = { t->grid, t->saved, t->sb };
    int rows[3] = { t->cap_rows, t->cap_rows, t->sb_rows };
    for (int b = 0; b < 3; b++) {
        if (!banks[b]) continue;
        size_t n = (size_t)rows[b] * (size_t)t->cap_cols;
        for (size_t k = 0; k < n; k++) {
            if (banks[b][k].fg == old_fg) banks[b][k].fg = (uint8_t)fg;
            if (banks[b][k].bg == old_bg) banks[b][k].bg = (uint8_t)bg;
        }
    }
}

int uvterm_scroll_to(struct uvterm *t, int want) {
    if (want > t->sb_count) want = t->sb_count;
    if (want < 0) want = 0;
    if (want == t->sb_view) return 0;
    t->sb_view = want;
    return 1;
}

// --- the menu's verbs ----------------------------------------------------------

void uvterm_clear_screen(struct uvterm *t) {
    if (t->alt) return;
    size_t rowbytes = (size_t)t->cap_cols * sizeof(struct uvterm_cell);
    struct uvterm_cell *keep = malloc(rowbytes);
    if (keep) memcpy(keep, grid_row(t, t->cr), rowbytes);
    int cc = t->cc;
    reset_screen(t);
    t->cc = cc;
    if (keep) { memcpy(grid_row(t, 0), keep, rowbytes); free(keep); }
    t->cr = 0;
    t->sb_view = 0;
}

void uvterm_clear_scrollback(struct uvterm *t) { t->sb_count = 0; t->sb_view = 0; }

void uvterm_reset(struct uvterm *t) {
    ansi_init(&t->vt, t->fg, t->bg);
    memset(&t->u8, 0, sizeof t->u8);
    reset_screen(t);
    t->alt = 0;
    t->cursor_shown = 1;
    t->sb_view = 0;
}

// --- reading -------------------------------------------------------------------

int uvterm_view_line(const struct uvterm *t, int r) { return t->sb_count - t->sb_view + r; }

const struct uvterm_cell *uvterm_line(const struct uvterm *t, int line) {
    if (line < 0) return 0;
    if (line < t->sb_count) return sb_row(t, line);
    int r = line - t->sb_count;
    if (r >= t->rows) return 0;
    return grid_row(t, r);
}

struct uvterm_point uvterm_point_at(const struct uvterm *t, int x, int y, int cw, int ch) {
    int r = y / ch;
    if (r < 0) r = 0;
    if (r >= t->rows) r = t->rows - 1;
    // Snapped to the nearest GAP, as a text field's caret is: a glyph's
    // right half puts the point after it.
    int c = (x + cw / 2) / cw;
    if (c < 0) c = 0;
    if (c > t->cols) c = t->cols;
    struct uvterm_point p = { uvterm_view_line(t, r), c };
    return p;
}

// --- the selection -------------------------------------------------------------
//
// **IT IS DROPPED WHEN THE TEXT MOVES UNDER IT** -- a scroll into history
// or a cleared screen -- rather than tracked through both. Konsole keeps
// it, at the cost of a line identity a ring of evicted rows does not carry.

void uvterm_sel_clear(struct uvterm *t) { t->sel_on = 0; t->selecting = 0; }

void uvterm_sel_start(struct uvterm *t, struct uvterm_point p, int extend) {
    if (extend && t->sel_on) t->sel_b = p;
    else { t->sel_a = t->sel_b = p; t->sel_on = 1; }   // armed, empty until a drag
    t->selecting = 1;
}

void uvterm_sel_extend(struct uvterm *t, struct uvterm_point p) { t->sel_b = p; }

static int before(struct uvterm_point a, struct uvterm_point b) {
    return a.line < b.line || (a.line == b.line && a.col < b.col);
}

// Sorted, and 0 for nothing selected -- including an anchor and a cursor
// at one point, which is what a plain click leaves.
static int sel_range(const struct uvterm *t, struct uvterm_point *from, struct uvterm_point *to) {
    if (!t->sel_on) return 0;
    if (before(t->sel_b, t->sel_a)) { *from = t->sel_b; *to = t->sel_a; }
    else                            { *from = t->sel_a; *to = t->sel_b; }
    return before(*from, *to);
}

static void sel_cols(const struct uvterm *t, int line, int *c0, int *c1) {
    struct uvterm_point from, to;
    *c0 = *c1 = 0;
    if (!sel_range(t, &from, &to)) return;
    if (line < from.line || line > to.line) return;
    *c0 = line == from.line ? from.col : 0;
    *c1 = line == to.line ? to.col : t->cols;
    if (*c1 > t->cols) *c1 = t->cols;
    if (*c0 > *c1) *c0 = *c1;
}

// Konsole's word characters, the conservative half: double-clicking a path
// stops at a `/` rather than swallowing the line. A LETTER PAST ASCII IS
// ONE (`Kesä`, `слово`), and a wide character's second cell belongs to it.
static int word_char(const struct uvterm_cell *row, int c) {
    if (row[c].wide == UVTERM_CONT) return c > 0 && word_char(row, c - 1);
    return kutf8_isalnum(row[c].ch) || row[c].ch == '_';
}

int uvterm_sel_word(struct uvterm *t, struct uvterm_point p) {
    const struct uvterm_cell *row = uvterm_line(t, p.line);
    if (!row) return 0;
    int c = p.col >= t->cols ? t->cols - 1 : p.col;
    if (c < 0 || !word_char(row, c)) return 0;
    int a = c, b = c;
    while (a > 0 && word_char(row, a - 1)) a--;
    while (b + 1 < t->cols && word_char(row, b + 1)) b++;
    t->sel_a.line = t->sel_b.line = p.line;
    t->sel_a.col = a;
    t->sel_b.col = b + 1;
    t->sel_on = 1;
    return 1;
}

// The whole line, break included: a triple-click copy of two lines cannot
// run them together.
void uvterm_sel_line(struct uvterm *t, struct uvterm_point p) {
    t->sel_a.line = p.line; t->sel_a.col = 0;
    t->sel_b.line = p.line; t->sel_b.col = t->cols;
    t->sel_on = 1;
}

void uvterm_sel_all(struct uvterm *t) {
    t->sel_a.line = 0; t->sel_a.col = 0;
    t->sel_b.line = t->sb_count + t->rows - 1; t->sel_b.col = t->cols;
    t->sel_on = 1;
}

int uvterm_sel_text(const struct uvterm *t, char *out, int cap) {
    struct uvterm_point from, to;
    if (!sel_range(t, &from, &to)) return 0;
    int n = 0;
    for (int line = from.line; line <= to.line; line++) {
        const struct uvterm_cell *row = uvterm_line(t, line);
        int c0, c1;
        sel_cols(t, line, &c0, &c1);
        int end = c1;
        if (row && line < to.line)                       // trailing blanks are
            while (end > c0 && row[end - 1].ch == ' ') end--;   // padding, not text
        // UTF-8 OUT, A CHARACTER AT A TIME: a wide character's second
        // cell holds nothing of its own, and a cut through the copy
        // (`cap`) never splits a sequence.
        for (int c = c0; row && c < end; c++) {
            if (row[c].wide == UVTERM_CONT) continue;
            char u[4];
            int k = uutf8_encode(row[c].ch, u);
            if (out && n + k <= cap) memcpy(out + n, u, (size_t)k);
            n += k;
        }
        if (line < to.line) { if (out && n < cap) out[n] = '\n'; n++; }
    }
    if (out && n < cap) out[n] = '\0';
    return n;
}

// --- drawing -------------------------------------------------------------------
//
// A ROW AT A TIME, IN RUNS OF ONE COLOUR PAIR: per-cell drawing would be
// 12,000 calls a frame; a run is one string call and usually one per row.

// Cells [i, j) of one colour pair, at the CELL PITCH. A character whose
// glyph is not exactly a cell wide -- a wide one, or one from a fallback
// face -- is drawn on its own at its cell; the rest go out as one string,
// which is the call that keeps a frame cheap.
static void draw_run(const struct uvterm *t, struct ugfx_surface *s, int x, int y,
                     const struct uvterm_cell *row, int i, int j,
                     uint8_t fg, uint8_t bg, const struct uvterm_look *k) {
    if (j <= i || !t->runbuf) return;
    uint32_t f = k->pal[fg & 15], b = k->pal[bg & 15];
    // **THE BACKGROUND IS A RECTANGLE, NOT THE STRING CALL'S `bg`**: that
    // only blends the glyph's pixels, it does not fill the CELL -- reverse
    // video came out as dark letters on black instead of a bar.
    if ((bg & 15) != t->bg) ugfx_fill_rect(s, x + i * k->cw, y, (j - i) * k->cw, k->ch, b);
    int n = 0, from = i;
    for (int q = i; q <= j; q++) {
        int plain = q < j && row[q].wide == UVTERM_NARROW &&
                    (row[q].ch < 0x80 || ugfx_cp_advance(row[q].ch) == k->cw);
        if (plain) {
            if (!n) from = q;
            n += uutf8_encode(row[q].ch, t->runbuf + n);
            continue;
        }
        if (n) {
            t->runbuf[n] = '\0';
            ugfx_draw_string(s, x + from * k->cw, y, t->runbuf, f, b);
            n = 0;
        }
        if (q < j && row[q].wide != UVTERM_CONT && row[q].ch != ' ')
            ugfx_draw_cp(s, x + q * k->cw, y, row[q].ch, f, b);
    }
}

// A SELECTED CELL SWAPS ITS OWN COLOURS rather than taking a highlight,
// which is what every terminal does and keeps a coloured `ls` legible.
static void draw_row(const struct uvterm *t, struct ugfx_surface *s, const struct uvterm_cell *row,
                     int x, int y, int sc0, int sc1, const struct uvterm_look *k) {
    int i = 0;
    while (i < t->cols) {
        int sel = i >= sc0 && i < sc1;
        int j = i;
        while (j < t->cols && row[j].fg == row[i].fg && row[j].bg == row[i].bg &&
               (j >= sc0 && j < sc1) == sel) j++;
        // TRAILING BLANKS IN THE DEFAULT COLOURS ARE NOT DRAWN -- the
        // ground is that colour already. A SELECTED blank is: it shows the
        // selection reaching the line's end.
        int blank = 1;
        for (int q = i; q < j; q++) if (row[q].ch != ' ') { blank = 0; break; }
        if (sel || !(blank && row[i].bg == t->bg))
            draw_run(t, s, x, y, row, i, j,
                     sel ? row[i].bg : row[i].fg, sel ? row[i].fg : row[i].bg, k);
        i = j;
    }
}

void uvterm_draw(const struct uvterm *t, struct ugfx_surface *s, int x, int y,
                 const struct uvterm_look *k) {
    // Scrolled back, the top rows come from history and the rest from the
    // screen, without a seam: scrollback is evicted ROWS, not a stream.
    for (int r = 0; r < t->rows; r++) {
        int line = uvterm_view_line(t, r);
        const struct uvterm_cell *row = uvterm_line(t, line);
        if (!row) continue;   // before the oldest line kept
        int sc0, sc1;
        sel_cols(t, line, &sc0, &sc1);
        draw_row(t, s, row, x, y + r * k->ch, sc0, sc1, k);
    }
}

void uvterm_draw_caret(const struct uvterm *t, struct ugfx_surface *s, int x, int y,
                       const struct uvterm_look *k) {
    // The caret only where the program has not hidden it (`ESC[?25l`) and
    // on the live screen; the host says whether this frame shows it at all.
    if (!k->caret || !t->cursor_shown || t->sb_view != 0) return;
    int cx = x + t->cc * k->cw, cy = y + t->cr * k->ch;
    if (k->cursor_shape == UVTERM_CURSOR_UNDER) {
        int th = k->ch / 8 + 1;
        ugfx_fill_rect(s, cx, cy + k->ch - th, k->cw, th, k->cursor_rgb);
    } else if (k->cursor_shape == UVTERM_CURSOR_BAR) {
        ugfx_fill_rect(s, cx, cy, k->cw / 8 + 1, k->ch, k->cursor_rgb);
    } else {
        // A BLOCK REDRAWS ITS CHARACTER in the background colour, or it
        // hides the one thing the caret points at.
        // Over a wide character the block covers both its cells.
        const struct uvterm_cell *row = uvterm_line(t, uvterm_view_line(t, t->cr));
        int wide = row && row[t->cc].wide == UVTERM_WIDE;
        ugfx_fill_rect(s, cx, cy, wide ? 2 * k->cw : k->cw, k->ch, k->cursor_rgb);
        if (row && row[t->cc].ch != ' ' && row[t->cc].wide != UVTERM_CONT)
            ugfx_draw_cp(s, cx, cy, row[t->cc].ch, k->pal[t->bg], k->cursor_rgb);
    }
}
