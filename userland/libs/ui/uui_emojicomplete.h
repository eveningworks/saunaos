#ifndef UUI_EMOJICOMPLETE_H
#define UUI_EMOJICOMPLETE_H

#ifdef __cplusplus
extern "C" {
#endif

// :NAME: COMPLETION in a text field -- Slack's, GitHub's and Discord's: ':'
// then two or more letters lists up to UUI_EMOJICOMPLETE_MAX emoji whose
// names hold them, under the field and inside the window. Tab or Enter puts
// the chosen one in place of ":text", Up and Down choose, Esc dismisses, and
// typing a space or anything not a letter ends it. The ':' must start the
// text or follow a space, so "12:30" and "http://" never complete.
//
// ONE LIST PER PROCESS, belonging to the focused uui_textbox, which calls
// this from its key, press and overlay slots. It is OFF in a masked field
// and in one that sets `no_complete`, and it only ever opens for a field the
// router's overlay pass has drawn -- a field an app paints by hand could not
// show the list, and a list nobody can see must not take Enter.
struct uui_textbox;
struct ugfx_surface;

#define UUI_EMOJICOMPLETE_MAX 6

// After a key the field took: re-reads the query at the caret.
void uui_emojicomplete_update(struct uui_textbox *f);
// A key for `f`'s open list (Tab, Enter, Esc, Up, Down): 1 when taken.
int  uui_emojicomplete_key(struct uui_textbox *f, int key);
// Is `f`'s list showing?
int  uui_emojicomplete_open(const struct uui_textbox *f);
// The overlay pass: draws `f`'s list, and notes that `f` can show one.
void uui_emojicomplete_draw(struct ugfx_surface *s, const struct uui_textbox *f);
// A press while the list is open: 1 when it chose a row (inserted), 0 when
// it fell outside -- the list is then dismissed and the press is not its.
int  uui_emojicomplete_press(struct uui_textbox *f, int cx, int cy);
// Dismisses whatever is open.
void uui_emojicomplete_cancel(void);

#ifdef __cplusplus
}
#endif

#endif
