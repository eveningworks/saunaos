#ifndef UUI_HELPVIEW_H
#define UUI_HELPVIEW_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_sbar.h"
#include "lib/uhlp.h"

// uui_helpview -- ONE WINDOWS HELP TOPIC, drawn as a document: the text
// in the fonts the file names, links and pop-ups as clickable runs,
// pictures with their hotspots, tables, and the non-scrolling heading
// that stays put while the rest scrolls.
//
// **THE PARSING IS NOT HERE.** `lib/uhlp.h` owns what a .HLP file means
// and hands this a `struct uhlp_doc`; this widget owns only how that
// looks, in the same split uui_markdown has with lib/umd.
//
// **THE CALLER OWNS THE DOCUMENT AND THE FILE.** The widget keeps
// pointers and lays them out when the width, the zoom, the font or the
// document changes; set_doc() again to show another. It copies nothing
// of the file and frees nothing of it.
//
// Sizes: the file's half-points are 96 dpi at the interface face's default size (a
// 10-point face is 13 px, as WINHELP.EXE draws it) and in proportion to it when the face
// is changed; Zoom then steps by 4 half-points -- WinHelp's Options > Font Small / Normal /
// Large, as many steps as UHV_ZOOM_* allow.
//
// A POP-UP is a second document shown in a card over this one
// (uui_helpview_popup); a click anywhere closes it, and a link inside it
// is reported like any other. A link click is a press and a release on
// the same link, reported through uui_helpview_take_link().

struct uhv_fonts;
struct uhv_item;

// Zoom steps of 4 half-points (2 pt) each: smaller than the file asks for, and a good deal larger.
#define UHV_ZOOM_MIN   (-2)
#define UHV_ZOOM_MAX   4

#define UHV_FILE_CAP   192
#define UHV_MACRO_CAP  320

struct uui_helpview_link {
    int kind;                       // UHLP_LINK_*
    uint32_t hash;                  // JUMP / POPUP: a context hash, or a topic offset when `by_offset`
    int by_offset;
    int window;                     // index into the file's windows, -1 current
    char file[UHV_FILE_CAP];        // an external file's name, "" for this one
    char macro[UHV_MACRO_CAP];      // MACRO: the text
    int ax, ay, aw, ah;             // where the link is drawn, widget coordinates: a pop-up's anchor
};

struct uui_helpview {
    // Content-relative geometry of the WHOLE control, text plus bar.
    int x, y, w, h;

    const struct uhlp *help;
    const struct uhlp_doc *doc;
    int doc_serial;                 // changed by set_doc(): a new layout

    struct uui_sbar bar;
    int scroll;                     // pixels down the scrolling region
    int zoom;                       // UHV_ZOOM_MIN .. UHV_ZOOM_MAX

    // --- the layout, rebuilt when its key changes ---------------------------
    struct uhv_item *item;
    int nitem, item_cap;
    char *arena;
    size_t arena_len, arena_cap;
    int ns_h, sc_h;                 // the two regions' heights
    int bar_on;                     // the bar is showing: the text is narrower
    const struct uhlp_doc *m_doc;   // what the layout was made for
    int m_serial, m_w, m_zoom, m_em, m_font_gen, m_bar;

    // --- fonts -----------------------------------------------------------------
    struct uhv_fonts *fonts;        // shared with a pop-up
    int owns_fonts;

    // --- input -----------------------------------------------------------------
    int armed, hover;               // doc link indices, -1 none
    int armed_hot;                  // the hotspot armed, or -1
    struct uui_helpview_link taken;
    int have_taken;

    // --- the selection: dragged over the text, in reading order ---------------------------
    int sel_on;                     // a selection exists
    int sel_a_ord, sel_a_ch;        // where the drag began: a text item in reading order, a byte in it
    int sel_f_ord, sel_f_ch;        // where it is now
    int selecting;                  // a press that may become a drag
    int press_x, press_y;
    int *order;                     // the layout's text items in reading order
    int norder;

    // --- the right-click menu ----------------------------------------------------------------
    int can_back, can_fwd;          // the APP keeps these true as history allows: the menu greys by them
    int cmd;                        // a menu row the app is to run (UHV_CMD_*), until taken
    int ctx_link;                   // the link the menu opened over, or -1
    int ctx_ax, ctx_ay, ctx_aw, ctx_ah;

    // --- highlight (Find) ----------------------------------------------------------
    char hl[64];

    // --- the pop-up -----------------------------------------------------------------
    struct uui_helpview *pop;
    int pop_ax, pop_ay, pop_aw, pop_ah;
    int is_popup;
};

void uui_helpview_init(struct uui_helpview *v);
void uui_helpview_free(struct uui_helpview *v);

// Shows `d` (a topic of `h`), scrolled to `scroll` pixels. Both must
// outlive the next set_doc() or free().
void uui_helpview_set_doc(struct uui_helpview *v, const struct uhlp *h, const struct uhlp_doc *d, int scroll);
void uui_helpview_set_zoom(struct uui_helpview *v, int zoom);   // UHV_ZOOM_MIN .. UHV_ZOOM_MAX
// The words to mark in the text, or "" for none (a Find result).
void uui_helpview_set_highlight(struct uui_helpview *v, const char *words);

// The selection: dragged with the pointer, or all of the topic. The text
// is allocated (free() it), paragraphs on separate lines and a wrapped line
// joined by a space; NULL when nothing is selected.
int   uui_helpview_has_selection(const struct uui_helpview *v);
char *uui_helpview_selection_text(const struct uui_helpview *v);
void  uui_helpview_select_all(struct uui_helpview *v);
void  uui_helpview_clear_selection(struct uui_helpview *v);

// What a right-click menu row asks of the APP, taken after the widget
// reports a change: the rows that are about history or the clipboard, not
// the topic. 0 for none.
#define UHV_CMD_COPY 1
#define UHV_CMD_BACK 2
#define UHV_CMD_FORWARD 3
#define UHV_CMD_CONTENTS 4
int uui_helpview_take_command(struct uui_helpview *v);

// The link clicked since the last call: 1, or 0 when none.
int uui_helpview_take_link(struct uui_helpview *v, struct uui_helpview_link *out);

// Shows `d` in a card by the link rect (ax, ay, aw, ah) in widget coordinates.
void uui_helpview_popup(struct uui_helpview *v, const struct uhlp *h, const struct uhlp_doc *d,
                        int ax, int ay, int aw, int ah);
void uui_helpview_popup_close(struct uui_helpview *v);
int  uui_helpview_popup_open(const struct uui_helpview *v);

// Pixels of text a Page Up / Page Down moves, and key handling for the
// scrolling keys. Returns 1 when it used the key.
int uui_helpview_key(struct uui_helpview *v, int key, unsigned mods);

// How tall the whole topic is, and where each hotspot is, for a test.
int uui_helpview_content_height(const struct uui_helpview *v);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_helpview_ops;

#endif
