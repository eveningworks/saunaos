#ifndef UUI_WIZARD_H
#define UUI_WIZARD_H

// uui_wizard -- a whole window of steps: a RAIL listing them on the left,
// the current step's page on the right under "STEP 2 OF 7" and its title,
// Back and Next in a bar along the bottom, and an optional Skip under the
// rail. Calamares' and KDE Initial System Setup's shape; First-boot setup
// (/bin/wm/system/firstboot) is the first user.
//
// **THE FRAME IS THE WIDGET'S, THE PAGES ARE THE APP'S** -- uui_prefs's
// rule. A step's page is the app's own item (usually a uui_layout of its
// controls), and the app's widgets report their ids to the app as
// anywhere. This owns the rail, the headings, the buttons and which page
// shows. Back, Next and Skip arrive at the app's on_action as UUI_WIZARD_*
// codes: hand them to uui_wizard_action() first.
//
// THE RAIL JUMPS BACK, NEVER AHEAD: a step already reached can be clicked,
// as in Calamares, so a later step's can_leave() is never bypassed.

#include "ui/uui_widget.h"
#include "ui/uui_layout.h"
#include "ui/uui_label.h"
#include "ui/uui_button.h"

#define UUI_WIZARD_STEPS_MAX 10
#define UUI_WIZARD_ID_BASE   0x7E00   // the wizard's codes; an app's stay below

enum { UUI_WIZARD_BACK = UUI_WIZARD_ID_BASE, UUI_WIZARD_NEXT, UUI_WIZARD_SKIP };

struct uui_wizard_step {
    const char *title;       // the rail row and the page heading
    const char *note;        // a line under the heading, or NULL
    struct uui_item page;    // the app's page; flags are the wizard's
};

struct uui_wizard_desc {
    const char *heading;     // above the rail: "Set up toy-os"
    const struct uui_wizard_step *steps;
    int count;
    const char *skip;        // the Skip button's text, NULL for none
    const char *finish;      // Next's text on the last step: "Start using toy-os"
    // Asked before Next leaves `step`; 0 keeps it (a name that is not
    // valid yet). NULL lets every step go.
    int (*can_leave)(int step);
    void (*on_enter)(int step);   // `step` is showing now -- refresh what it shows
    void (*on_finish)(void);      // Next on the last step
    void (*on_skip)(void);        // Skip; the app confirms and finishes
};

// The rail, a widget of its own: steps drawn, pressed and described.
struct uui_wizard_rail {
    int x, y, w, h;
    struct uui_wizard *wz;
    int armed;                // the row a press landed on, -1 none
};

struct uui_wizard {
    struct uui_wizard_desc d;
    int step;                 // showing
    int reached;              // the furthest step shown so far
    int x, y, w, h;

    // OWNED, all of it.
    struct uui_wizard_rail rail;
    struct uui_label count_l, title_l, note_l, spacer;
    char count_text[24];
    struct uui_button back, next, skip;
    struct uui_item left_items[2], body_items[3 + UUI_WIZARD_STEPS_MAX], bar_items[3],
                    main_items[2], root_items[2], inner, self_item;
    struct uui_layout left, body, bar, main, root;
};

void uui_wizard_init(struct uui_wizard *w, const struct uui_wizard_desc *d);

// The wizard as ONE item, filling whatever it is given -- what an app puts
// in its layout and its `widgets`.
struct uui_item *uui_wizard_item(struct uui_wizard *w);

// The app's on_action, first: 1 when `code` was the wizard's (and handled).
int uui_wizard_action(struct uui_wizard *w, int code);

// Show `step`. Moves backward freely; forward only through can_leave().
void uui_wizard_go(struct uui_wizard *w, int step);
int uui_wizard_step(const struct uui_wizard *w);

// The buttons, for the app's focus ring: Back, Next, then Skip if shown.
// Returns how many were written to `ops`/`widgets`.
int uui_wizard_focusables(struct uui_wizard *w, const struct uui_widget_ops **ops,
                          void **widgets, int max);

extern const struct uui_widget_ops uui_wizard_ops;

#endif
