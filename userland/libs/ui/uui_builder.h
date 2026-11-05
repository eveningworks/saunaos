#ifndef UUI_BUILDER_H
#define UUI_BUILDER_H

// uui_builder -- a window's widget tree read from a .ui FILE when the app
// starts, instead of built in C. GtkBuilder's shape (Android's inflate):
// the file holds the structure and the properties, the C keeps the
// behaviour, and NAMES join them. The language is base/udecl.h's; the
// widget types and every attribute are in docs/ui-files-design.md.
//
//     static const struct uui_builder_name NAMES[] = {
//         { "reports", ID_TABLE }, { "open", CMD_OPEN },
//     };
//     struct uui_builder *b = uui_builder_load("crashreports", NAMES, 2);
//     struct uui_table *t = uui_builder_widget(b, "list", "reports", "table");
//     if (uui_builder_error(b)) return uui_builder_run_error(b);
//     struct uapp_desc desc = { .on_widget = on_widget };
//     if (!uui_builder_window(b, "list", &desc)) return uui_builder_run_error(b);
//     return uapp_run(&desc);
//
// THE NAMES TABLE IS THE APP'S SIDE OF THE CONTRACT: a `#name` found in
// it is that widget's id (what on_widget hears), and every `command=`
// word MUST be in it -- a button whose command the app does not know is
// a load error, never a dead button. A name not in the table is still a
// name: the app can look it up, and the layout log reports it.
//
// THE WHOLE FILE IS BUILT AT LOAD, every window in it, so a mistake in
// one face is an error whichever face opened. Any error -- the file's, or
// a lookup naming something the file lacks -- is kept, the first one
// only, and uui_builder_error() returns it; check once, after the
// lookups and before using what they returned (they return NULL then).
//
// What the builder makes is what the C made: the same widget structs,
// laid out and drawn by the same code, so a built window costs nothing
// more once it is open. The cost is reading the file, once.

#include "ui/uapp.h"
#include "ui/uui_widget.h"

#define UUI_BUILDER_DIR "/usr/share/ui"

struct uui_builder_name {
    const char *name;
    int value;
};

struct uui_builder;

// UUI_BUILDER_DIR/<app>.ui. NULL only when memory is out; a file that
// is missing or wrong is a builder carrying its error.
struct uui_builder *uui_builder_load(const char *app, const struct uui_builder_name *names, int count);
// The same from text, for a test; `what` names it in messages.
struct uui_builder *uui_builder_parse(const char *what, const char *src, unsigned long len,
                                      const struct uui_builder_name *names, int count);
void uui_builder_free(struct uui_builder *b);

// "<file>:<line>: <what>", or NULL when everything so far is right.
const char *uui_builder_error(const struct uui_builder *b);

// A named widget of window `window`, checked to be of `type` -- the word
// the file uses ("table"; NULL: any). It is the OBJECT the type made,
// which for a composite (colourpick) is not its item's widget. NULL, and
// the builder's error set, when absent or of another type.
void *uui_builder_widget(struct uui_builder *b, const char *window, const char *name, const char *type);
// Its place in the tree: hide it, read its rect.
struct uui_item *uui_builder_item(struct uui_builder *b, const char *window, const char *name);
// A page of stack #stack, checked to be one (uui_stack_show() takes it). A
// page moved out of its stack in the file is an error here, at load,
// rather than a switch that silently does nothing.
struct uui_item *uui_builder_page(struct uui_builder *b, const char *window, const char *stack, const char *page);

// A wizard, checked to have the program's `steps`: the program walks its
// steps by index, so a file may edit a step's page but not add or remove
// one -- an error at the wizard's line, like any other.
struct uui_wizard;
struct uui_wizard *uui_builder_wizard(struct uui_builder *b, const char *window, const char *name, int steps);

// A dropdown's or listbox's items are known by their `id=`, so the file
// may reorder and relabel them: the index holding `id` (-1, and the error
// set, when none does -- ask for each id the app needs at start), and the
// id at `index` (NULL when it has none).
int uui_builder_choice(struct uui_builder *b, const char *window, const char *name, const char *id);
const char *uui_builder_choice_id(struct uui_builder *b, const char *window, const char *name, int index);

// Fills `desc` from window `window`: layout, widgets, the focus ring,
// and whichever of title, app_id, cells_w/h, min_w/h and UAPP_RESIZABLE
// the file says -- leaving the app's callbacks, and anything the file
// leaves out, as the app set them. Call it AFTER hiding what should not
// show: the focus ring is built then, and skips a hidden widget; it
// starts on its first only when the window says `autofocus`. 0, and
// the error set, when there is no such window.
int uui_builder_window(struct uui_builder *b, const char *window, struct uapp_desc *desc);

// A window saying what is wrong with the file, the rest of the app not
// started: what a person who just edited it needs to see. Returns 1, an
// exit status.
int uui_builder_run_error(struct uui_builder *b);

// How long the file took to read and build, in microseconds (a test's
// number, and the log line's).
unsigned uui_builder_build_us(const struct uui_builder *b);

#endif
