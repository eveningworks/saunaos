#ifndef UUI_COMPARE_H
#define UUI_COMPARE_H
// THE TWO FILES OF A NAME CONFLICT, SIDE BY SIDE -- a dialog's body:
// what is coming in and what is already there, each with its picture,
// size and date, a badge on the side that is NEWER and the one that is
// LARGER, and a "Do the same for the rest" box under them. Windows'
// Replace or Skip Files, Dolphin's and Nautilus's file conflict dialogs.
//
// IT DECIDES NOTHING: the app reads `rest.checked` with the button it was
// answered by. The facts come from the two paths (sys_stat); the pictures
// are the caller's -- a thumbnail it already has, or the type's icon.
#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_checkbox.h"

struct uimg;

struct uui_compare_side {
    char caption[64];          // "Coming in, from Pictures"
    char size[24], when[48];
    const struct uimg *picture;
    int missing;               // the path could not be read
    unsigned long long bytes;
    uint64_t modified;         // seconds since the epoch, for the badges
    int newer, larger;         // the badges, judged against the other side
};

struct uui_compare {
    int x, y, w, h;
    struct uui_compare_side side[2];   // 0 coming in, 1 already there
    struct uui_checkbox rest;          // "Do the same for the rest"
    int pressed;                       // a press on the box, awaiting its release
};

// `rest_label` NULL leaves the box out (a single file has no rest).
void uui_compare_init(struct uui_compare *c, const char *rest_label);
// Side `i` from `path`: its size and date read now, `caption` and
// `picture` as given. Judge the badges once both are set.
void uui_compare_set(struct uui_compare *c, int i, const char *caption, const char *path,
                     const struct uimg *picture);
// Side `i`'s picture again (a thumbnail that landed after opening).
void uui_compare_set_picture(struct uui_compare *c, int i, const struct uimg *picture);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_compare_ops;

#endif
