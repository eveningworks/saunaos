#ifndef UUI_PATHBAR_H
#define UUI_PATHBAR_H

#include "ui/ugfx.h"
#include "ui/uui_textbox.h"

#ifdef __cplusplus
extern "C" {
#endif

// A BREADCRUMB PATH: the folder you are in as a row of buttons, one per
// level, each of which takes you there -- Explorer's address bar,
// Dolphin's URL navigator, GTK's path bar. A click past the last
// segment (or uui_pathbar_begin_edit(), Ctrl+L in an app) turns it into
// a text field holding the whole path, which Enter commits and Esc
// abandons: both desktops do exactly this, because typing a path and
// clicking one are two different jobs.
//
// **THE WIDGET NAVIGATES NOTHING.** A click or an Enter parks the path
// for uui_pathbar_take(); the app decides whether it exists and goes
// there, then calls uui_pathbar_set_path() with where it actually is.
// A path the app refuses leaves the field up, holding what was typed.
//
// Too narrow for every segment, the first ones fold into "..." after
// the root, and that button goes to the parent of the first one shown.
//
// The path is the widget's heap copy, sized to fit: uui_pathbar_free()
// gives it back.

#define UUI_PATHBAR_SEGS  24   // shown at once; a deeper path folds into "..."

struct uui_pathbar {
    int x, y, w, h;
    char *path;               // OWNED; read it with uui_pathbar_path()
    const char *root_label;   // what "/" is called ("System"); NULL = "/"
    const char *root_icon;    // icon_get() name beside it; NULL = none
    // A VIRTUAL FOLDER'S ROOT: a path beginning "trash:/" shows its
    // registered label as the root chip instead of "System" -- KIO's
    // URL navigator names a scheme the same way. See set_scheme.
    struct { const char *prefix, *label, *icon; } scheme[4];
    int nscheme;

    int hot;      // hovered segment, UUI_PATHBAR_ELIDED, or -1; OWNED
    int armed;    // pressed segment awaiting its release, or -1; OWNED
    char *taken;              // OWNED: a navigation, until taken

    int editing;               // 1 while it is a text field
    struct uui_textbox edit;
    int focused;               // OWNED -- the focus ring's set_focused
};

#define UUI_PATHBAR_ELIDED (-2)   // the "..." button

void uui_pathbar_init(struct uui_pathbar *p, const char *root_label, const char *root_icon);
// Gives back the path; init again to reuse it.
void uui_pathbar_free(struct uui_pathbar *p);
// The path shown ("/" when none could be had).
const char *uui_pathbar_path(const struct uui_pathbar *p);
// Name a virtual folder's root: `prefix` ("trash:/") shows as `label`
// with `icon`. The strings are NOT copied.
void uui_pathbar_set_scheme(struct uui_pathbar *p, const char *prefix,
                            const char *label, const char *icon);
// Shows `path`. Ends an edit in progress: the app calls this after it
// has navigated, and a field left open over the new place would lie.
void uui_pathbar_set_path(struct uui_pathbar *p, const char *path);
// Opens the text field over the current path, all of it selected so
// typing replaces it.
void uui_pathbar_begin_edit(struct uui_pathbar *p);
void uui_pathbar_end_edit(struct uui_pathbar *p);
int  uui_pathbar_is_editing(const struct uui_pathbar *p);
// The path a click or an Enter asked for, allocated (free() it), or NULL
// when nothing was asked since the last call. An Enter's is the typed
// text, unresolved -- relative paths are the app's to resolve.
char *uui_pathbar_take(struct uui_pathbar *p);

// Segment `i`'s rect (0 is the root) as currently laid out, and its
// path. For tests and layout logs; 0 when that segment is not shown.
int  uui_pathbar_segment_rect(const struct uui_pathbar *p, int i,
                               int *x, int *y, int *w, int *h);
int  uui_pathbar_segment_count(const struct uui_pathbar *p);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_pathbar_ops;

#ifdef __cplusplus
}
#endif

#endif
