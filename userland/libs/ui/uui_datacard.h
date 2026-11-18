#ifndef UUI_DATACARD_H
#define UUI_DATACARD_H

// THE GAME-DATA CARD -- what a program shows over a black window when the
// data it plays is not on the machine: a card of what it can download
// (base/udatafetch.h), its licence to agree to or read, and the download
// with a Cancel. DOOM's IWADs (one, radio rows) and ScummVM's free games
// (several, checkboxes) both run on it.
//
// Not a uui_widget: it fills the window and owns its input while it is
// up, so the app routes press, motion, release, key and its posted
// events here first and draws nothing else. The download runs on a
// worker thread (System Update's shape) that only counts and posts.
//
// A TEST PRESSES IT BY ASKING: every frame logs "<log>: layout view
// data|licence|fetch", then each control -- rowN (its item's id last),
// usefile, cancel, primary, back, agree, stop, and the licence's text --
// as x y w h, and "selected N" (and "checked I J ..." with several).

#include "base/udatafetch.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/uui_para.h"

#ifdef __cplusplus
extern "C" {
#endif

// The posted events it handles, from its worker (uapp_post's a0).
#define UUI_DATACARD_EV_PROGRESS 0x5DC1
#define UUI_DATACARD_EV_DONE     0x5DC2

struct uui_datacard_desc {
    const char *log;            // the log lines' prefix: "doom"
    const char *conf;           // licences agreed to, and `mirror=`
    const char *dir;            // where the items live
    const struct udata_item *items;
    int count;
    int multi;                  // checkboxes, several at once; else one, radio rows
    const char *title_empty;    // "DOOM needs game data", while none is here
    const char *title_some;     // "Game data"
    const char *subtitle;       // "Choose one. Downloads go to /usr/share/doom."
    const char *aux_label;      // "Use a file..." on the left, or NULL
    const char *noun, *nouns;   // several: the footer's "2 games, 71.6 MiB"
    int (*download)(struct uhttp_download *d);   // uhttp_download
    // What comes next. on_done: the downloads finished (`last` the last
    // one); on_use: one item mode, Use on one already here; on_aux: the
    // left button; on_cancel: Cancel or Esc, offered only when given and
    // the card was opened with `back`.
    void (*on_done)(struct uapp *a, const struct udata_item *last, void *ctx);
    void (*on_use)(struct uapp *a, const struct udata_item *it, void *ctx);
    void (*on_aux)(struct uapp *a, void *ctx);
    void (*on_cancel)(struct uapp *a, void *ctx);
    void *ctx;
};

enum uui_datacard_view { UUI_DATACARD_OFF, UUI_DATACARD_ROWS, UUI_DATACARD_LICENCE,
                         UUI_DATACARD_FETCH };

struct uui_datacard_rect { int x, y, w, h; };

// The app's, at file scope; zero it once.
struct uui_datacard {
    const struct uui_datacard_desc *d;
    struct uapp *app;
    enum uui_datacard_view view;
    int back;
    int sel;                     // the row with the focus (and, with one, the choice)
    unsigned char *checked;      // several: per item
    unsigned char *here;         // per item: on the disk, read at open and after a fetch
    char note[160];              // the last error, under the rows
    // The licences still to show and the items to fetch, in order.
    int *queue, queue_n, queue_at, lic_at;
    int enter_up;                // Enter agrees only once let go (the Enter that pressed Download)
    struct uui_para lic;         // the licence shown: selectable and copyable
    int lic_drag, lic_menu;      // a primary drag in it; a secondary press on it
    int hot, pressed;
    int W, H;                    // the surface last drawn
    struct uui_datacard_rect *r; // the controls, laid out each frame
    struct uui_datacard_rect card;
    // --- shared with the worker ---
    volatile unsigned long got, total;
    volatile int phase, part, cancel, rc;
    unsigned long long last_post;
    char host[64];
    struct udata_fetch fetch;
    char mirror[256];
};

// Shows the card, the focus on `select` when it is one of the items.
void uui_datacard_open(struct uui_datacard *c, struct uapp *a, const struct uui_datacard_desc *d,
                       const struct udata_item *select, int back);
void uui_datacard_close(struct uui_datacard *c);
int uui_datacard_up(const struct uui_datacard *c);
// A sentence under the rows (a refused file): shown until the next action.
void uui_datacard_note(struct uui_datacard *c, const char *text);

void uui_datacard_draw(struct uui_datacard *c, struct ugfx_surface *s);
// `buttons` as uapp's on_press/on_release give them: the licence's text
// takes a primary drag and opens Copy / Select All on a secondary click.
void uui_datacard_press(struct uui_datacard *c, int x, int y, unsigned buttons);
void uui_datacard_motion(struct uui_datacard *c, int x, int y);
void uui_datacard_release(struct uui_datacard *c, int x, int y, unsigned buttons);
// 1 when it took the key (Ctrl+A and Ctrl+C on the licence among them).
// `phys` is the key's edge, for the licence's Enter.
int uui_datacard_key(struct uui_datacard *c, int key, unsigned mods);
void uui_datacard_phys(struct uui_datacard *c, int keycode, int down);
// The worker's events: 1 when `a0` was one.
int uui_datacard_user(struct uui_datacard *c, int a0, int a1);

#ifdef __cplusplus
}
#endif

#endif
