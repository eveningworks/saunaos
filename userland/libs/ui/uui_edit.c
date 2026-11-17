// The shared editing semantics -- see ui/uui_edit.h for why they are
// here rather than in each widget.
#include "ui/uui_edit.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY
#include "ui/uui_widget.h" // uui_key_is_shortcut
#include "ui/uui_undo.h"
#include "base/uutf8.h"   // stepping by CHARACTER: docs/utf8-design.md
#include "base/uemoji.h"  // a variation selector goes with its character

// The text is UTF-8 and every index here is a byte offset on a character
// boundary. The ops reach the text a byte at a time, so these copy the
// at-most-four bytes a step needs and decode them with the Latin-1 rule
// (base/uutf8.h): a stray byte is one character, never split.
// A VARIATION SELECTOR belongs to the character before it, whatever it is
// (base/uemoji.h's rule) -- ❤️ and 1️⃣'s first two are one character to
// step, delete and select, as ugfx_text_next()/prev() draw it.
static int selector_at(const struct uui_edit_ops *ops, void *text, int i, int len) {
    if (i + 3 > len) return 0;
    char b[3] = { ops->at(text, i), ops->at(text, i + 1), ops->at(text, i + 2) };
    return uemoji_selector_utf8(b, 3) != 0;
}

static int char_step(const struct uui_edit_ops *ops, void *text, int i, int len) {
    char b[4];
    int n = 0;
    while (n < 4 && i + n < len) { b[n] = ops->at(text, i + n); n++; }
    int l;
    uutf8_decode(b, n, &l);
    return i + (l > 0 ? l : 1);
}

static int char_next(const struct uui_edit_ops *ops, void *text, int i, int len) {
    if (i >= len) return len;
    int j = char_step(ops, text, i, len);
    return ops->at(text, i) != '\n' && selector_at(ops, text, j, len) ? j + 3 : j;
}

static int char_back(const struct uui_edit_ops *ops, void *text, int i) {
    int from = i > 4 ? i - 4 : 0;
    char b[4];
    for (int k = from; k < i; k++) b[k - from] = ops->at(text, k);
    return from + uutf8_prev(b, i - from);
}

static int char_prev(const struct uui_edit_ops *ops, void *text, int i) {
    if (i <= 0) return 0;
    // As ugfx_text_prev(): a run of selectors pairs the way stepping
    // forward paired it (base/uemoji.h).
    int k = 0, q = i;
    while (q >= 3 && selector_at(ops, text, q - 3, q)) { k++; q -= 3; }
    if (!k) return char_back(ops, text, i);
    if (!uemoji_run_attached(k, q > 0 && ops->at(text, q - 1) != '\n')) return i - 3;
    return k == 1 ? char_back(ops, text, q) : i - 6;
}

// --- word and line selection ------------------------------------------

static uint32_t cp_at(const struct uui_edit_ops *ops, void *text, int i, int len) {
    char b[4];
    int n = 0;
    while (n < 4 && i + n < len) { b[n] = ops->at(text, i + n); n++; }
    int l;
    return uutf8_decode(b, n, &l);
}

// The kind of character a double click gathers: 0 a line break (never
// gathered), 1 blank, 2 word, 3 punctuation, 4 emoji. Past ASCII a
// character is a letter unless it is a space, general or CJK
// punctuation, or drawn as an emoji.
static int char_kind(uint32_t c, int emoji_sel) {
    if (c == '\n') return 0;
    if (c == ' ' || c == '\t' || c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A)) return 1;
    if (c < 0x80) {
        int alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        return alnum || c == '_' ? 2 : 3;
    }
    if (emoji_sel || uemoji_presentation(c)) return 4;
    if ((c >= 0x2010 && c <= 0x2BFF) || (c >= 0x3001 && c <= 0x303F) || (c >= 0xFE10 && c <= 0xFE6F)) return 3;
    return 2;
}

static int kind_at(const struct uui_edit_ops *ops, void *text, int i, int len) {
    int j = char_step(ops, text, i, len);
    return char_kind(cp_at(ops, text, i, len), j < len && selector_at(ops, text, j, len) &&
                                                   ops->at(text, j + 2) == (char)0x8F);
}

void uui_edit_select_word(struct uui_edit *e, const struct uui_edit_ops *ops, void *text, int index) {
    int len = ops->len(text);
    if (index < 0) index = 0;
    if (index > len) index = len;
    // Past the last character of a line: the one before it.
    if ((index >= len || ops->at(text, index) == '\n') && index > 0 && ops->at(text, index - 1) != '\n')
        index = char_prev(ops, text, index);
    int kind = index < len ? kind_at(ops, text, index, len) : 0;
    int start = index, end = index;
    if (kind) {
        while (start > 0) {
            int p = char_prev(ops, text, start);
            if (kind_at(ops, text, p, len) != kind) break;
            start = p;
        }
        while (end < len && kind_at(ops, text, end, len) == kind) end = char_next(ops, text, end, len);
    }
    e->sel_anchor = start;
    e->cursor = end;
    e->sel_active = 1;
}

void uui_edit_select_line(struct uui_edit *e, const struct uui_edit_ops *ops, void *text, int index) {
    if (!ops->line_start || !ops->line_end) { uui_edit_select_all(e, ops, text); return; }
    int len = ops->len(text);
    if (index < 0) index = 0;
    if (index > len) index = len;
    int end = ops->line_end(text, index);
    if (end < len && ops->at(text, end) == '\n') end++;
    e->sel_anchor = ops->line_start(text, index);
    e->cursor = end;
    e->sel_active = 1;
}

void uui_edit_init(struct uui_edit *e) {
    e->cursor = 0;
    e->sel_anchor = 0;
    e->sel_active = 0;
    e->undo = 0;
    e->flags = 0;
}

int uui_edit_insert(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                    int index, char c) {
    if (!ops->insert(text, index, c)) return 0;
    if (e->undo) uui_undo_note_insert(e->undo, index, &c, 1);
    return 1;
}

int uui_edit_insert_text(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                         int index, const char *s, int n) {
    int put = 0;
    if (ops->insert_text) put = ops->insert_text(text, index, s, n);
    else
        while (put < n && ops->insert(text, index + put, s[put])) put++;
    if (put > 0 && e->undo) uui_undo_note_insert(e->undo, index, s, put);
    return put;
}

void uui_edit_erase(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                    int start, int end) {
    int len = ops->len(text);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return;
    if (e->undo && e->undo->buf) {
        // The bytes must be read BEFORE they go; a big range is copied
        // straight into the history rather than through the stack.
        char small[256];
        int n = end - start;
        if (n <= (int)sizeof small) {
            for (int i = 0; i < n; i++) small[i] = ops->at(text, start + i);
            uui_undo_note_erase(e->undo, start, small, n);
        } else {
            uui_undo_note_erase_from(e->undo, start, n, ops, text);
        }
    }
    ops->erase(text, start, end);
}

static int step(struct uui_edit *e, const struct uui_edit_ops *ops, void *text, int redo) {
    if (!e->undo) return 0;
    int cur = e->cursor;
    if (!uui_undo_apply(e->undo, redo, ops, text, &cur)) return 0;
    e->cursor = cur;
    uui_edit_clear_selection(e);
    return 1;
}

int uui_edit_undo(struct uui_edit *e, const struct uui_edit_ops *ops, void *text) {
    return step(e, ops, text, 0);
}

int uui_edit_redo(struct uui_edit *e, const struct uui_edit_ops *ops, void *text) {
    return step(e, ops, text, 1);
}

int uui_edit_has_selection(const struct uui_edit *e) {
    return e->sel_active && e->sel_anchor != e->cursor;
}

void uui_edit_range(const struct uui_edit *e, int *out_start, int *out_end) {
    int a = e->sel_anchor, b = e->cursor;
    if (a > b) { int t = a; a = b; b = t; }
    if (!e->sel_active) { a = b = e->cursor; }
    if (out_start) *out_start = a;
    if (out_end) *out_end = b;
}

void uui_edit_clear_selection(struct uui_edit *e) {
    e->sel_active = 0;
    e->sel_anchor = e->cursor;
}

void uui_edit_select_all(struct uui_edit *e, const struct uui_edit_ops *ops, void *text) {
    e->sel_anchor = 0;
    e->cursor = ops->len(text);
    e->sel_active = 1;
}

int uui_edit_delete_selection(struct uui_edit *e, const struct uui_edit_ops *ops, void *text) {
    if (!uui_edit_has_selection(e)) return 0;
    int start, end;
    uui_edit_range(e, &start, &end);
    uui_edit_erase(e, ops, text, start, end);
    e->cursor = start;
    uui_edit_clear_selection(e);
    return 1;
}

void uui_edit_place(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                     int index, int extend) {
    int len = ops->len(text);
    if (index < 0) index = 0;
    if (index > len) index = len;

    if (extend) {
        // Keep the anchor. A shift-click with no selection yet anchors
        // where the caret already was, which is what makes
        // click-then-shift-click select the span between them.
        if (!e->sel_active) { e->sel_anchor = e->cursor; e->sel_active = 1; }
        e->cursor = index;
    } else {
        e->cursor = index;
        uui_edit_clear_selection(e);
    }
    if (e->undo) uui_undo_break(e->undo);   // a moved caret ends a typing run
}

// Moves the caret and maintains the selection the way every desktop
// does: with Shift the anchor stays and the range grows, without it the
// selection collapses -- and collapsing a NON-EMPTY selection with a
// plain arrow puts the caret at the near end rather than moving one
// character further, which is a detail users feel and never articulate.
static void move(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                 int to, int extend) {
    if (!extend && uui_edit_has_selection(e)) {
        int start, end;
        uui_edit_range(e, &start, &end);
        uui_edit_clear_selection(e);
        // Collapsing: which end depends on which way this move went.
        e->cursor = (to <= e->cursor) ? start : end;
        if (e->undo) uui_undo_break(e->undo);
        return;
    }
    uui_edit_place(e, ops, text, to, extend);
}

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int uui_edit_key(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                  int key, unsigned mods) {
    // Shift arrives as its own key code, not as a bit -- see below. Ctrl
    // or Alt with a character is a shortcut, declined (uui_widget.h).
    if (uui_key_is_shortcut(key, mods)) return 0;
    int len = ops->len(text);
    e->cursor = clamp(e->cursor, 0, len);
    int ro = (e->flags & UUI_EDIT_READONLY) != 0;

    // ---- the clipboard. Consumed even when there is nothing to do (no
    //      selection, a masked field): a Ctrl+C that fell through to the
    //      app would reach whatever else it binds the key to.
    //      Shift+Delete and Shift+Insert are matched before the plain
    //      keys below; Ctrl+Insert is unambiguous.
    if (key == 0x03 || (key == KEY_INSERT && (mods & KEY_MOD_CTRL))) {   // Ctrl-C
        uui_edit_copy(e, ops, text);
        return 1;
    }
    if (key == 0x18 || (key == KEY_DELETE && (mods & KEY_MOD_SHIFT))) {  // Ctrl-X
        if (ro) uui_edit_copy(e, ops, text);   // a read-only view still copies
        else uui_edit_cut(e, ops, text);
        return 1;
    }
    if (key == 0x16 || (key == KEY_INSERT && (mods & KEY_MOD_SHIFT))) {  // Ctrl-V
        if (!ro) uui_edit_paste(e, ops, text);
        return 1;
    }

    // Nothing below changes a read-only text except by selecting: decline
    // the rest of the editing keys so they stay the app's.
    if (ro && (key == 0x1A || key == 0x19 || key == '\b' || key == KEY_DELETE ||
               uui_key_is_text(key)))
        return 0;

    switch (key) {
    // ---- selection ------------------------------------------------
    case 0x01: // Ctrl-A. Ctrl+letter reaches an app as a CONTROL CODE --
               // see api/keyboard.h's "Ctrl and Alt".
        uui_edit_select_all(e, ops, text);
        return 1;

    // ---- history: declined when there is none, so an app may use them
    case 0x1A: // Ctrl-Z
        if (!e->undo) return 0;
        uui_edit_undo(e, ops, text);
        return 1;
    case 0x19: // Ctrl-Y
        if (!e->undo) return 0;
        uui_edit_redo(e, ops, text);
        return 1;

    // ---- deletion -------------------------------------------------
    case '\b':
        if (uui_edit_delete_selection(e, ops, text)) return 1;
        if (e->cursor > 0) {
            // The whole CHARACTER: both bytes of a ż, not the half that
            // would leave a stray lead byte drawn as a Latin-1 letter.
            int at = char_prev(ops, text, e->cursor);
            uui_edit_erase(e, ops, text, at, e->cursor);
            e->cursor = at;
        }
        uui_edit_clear_selection(e);
        return 1;

    case KEY_DELETE:
        if (uui_edit_delete_selection(e, ops, text)) return 1;
        if (e->cursor < len)
            uui_edit_erase(e, ops, text, e->cursor, char_next(ops, text, e->cursor, len));
        uui_edit_clear_selection(e);
        return 1;

    // ---- motion. The Shift variants are distinct KEY CODES here
    //      (api/keyboard.h), which is why this switch reads as pairs.
    case KEY_ARROW_LEFT:        move(e, ops, text, char_prev(ops, text, e->cursor), 0); return 1;
    case KEY_SHIFT_ARROW_LEFT:  move(e, ops, text, char_prev(ops, text, e->cursor), 1); return 1;
    case KEY_ARROW_RIGHT:       move(e, ops, text, char_next(ops, text, e->cursor, len), 0); return 1;
    case KEY_SHIFT_ARROW_RIGHT: move(e, ops, text, char_next(ops, text, e->cursor, len), 1); return 1;

    case KEY_HOME:
    case KEY_SHIFT_HOME: {
        int extend = (key == KEY_SHIFT_HOME);
        int to = ops->line_start ? ops->line_start(text, e->cursor) : 0;
        move(e, ops, text, to, extend);
        return 1;
    }
    case KEY_END:
    case KEY_SHIFT_END: {
        int extend = (key == KEY_SHIFT_END);
        int to = ops->line_end ? ops->line_end(text, e->cursor) : len;
        move(e, ops, text, to, extend);
        return 1;
    }

    // Up/Down exist only where there ARE lines. A single-line field
    // leaves these ops NULL and declines the key, so the app can use it
    // for something else rather than having it silently swallowed.
    case KEY_ARROW_UP:
    case KEY_SHIFT_ARROW_UP: {
        if (!ops->line_up) return 0;
        move(e, ops, text, ops->line_up(text, e->cursor), key == KEY_SHIFT_ARROW_UP);
        return 1;
    }
    case KEY_ARROW_DOWN:
    case KEY_SHIFT_ARROW_DOWN: {
        if (!ops->line_down) return 0;
        move(e, ops, text, ops->line_down(text, e->cursor), key == KEY_SHIFT_ARROW_DOWN);
        return 1;
    }

    default:
        break;
    }

    // ---- typing ---------------------------------------------------
    //
    // A printable character REPLACES the selection. Deleting first and
    // then inserting is the whole implementation, and doing it in this
    // order is what stops a replacement from being an insert next to
    // text the user thought they had just overwritten.
    if (uui_key_is_text(key)) {
        // Replacing a selection is ONE step: the erase and the insert.
        int replacing = uui_edit_has_selection(e);
        if (replacing && e->undo) uui_undo_begin(e->undo);
        uui_edit_delete_selection(e, ops, text);
        // THE KEY'S UTF-8, all of it or none: a Latin-1 letter is two
        // bytes now, and half a character is worse than a refused key.
        char u[4];
        int n = uutf8_encode((uint32_t)key, u);
        if (n == 1) {
            if (uui_edit_insert(e, ops, text, e->cursor, u[0])) e->cursor++;
        } else if (n > 1) {
            int put = uui_edit_insert_text(e, ops, text, e->cursor, u, n);
            if (put == n) e->cursor += n;
            else if (put > 0) uui_edit_erase(e, ops, text, e->cursor, e->cursor + put);
        }
        if (replacing && e->undo) uui_undo_end(e->undo);
        uui_edit_clear_selection(e);
        return 1;
    }

    // Notably Enter: committing a field, inserting a newline or ignoring
    // it is the widget's decision, not this core's.
    return 0;
}
