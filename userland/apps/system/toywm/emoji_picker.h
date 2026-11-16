#ifndef WM_EMOJI_PICKER_H
#define WM_EMOJI_PICKER_H

#include <stdint.h>

// The EMOJI PICKER: Super+period (and Super+semicolon) opens a compact
// panel at the focused window's text caret -- Windows 11's emoji panel in
// shape: a search field, category tabs, a Recently used row, the group's
// grid and a footer naming the focused emoji. Enter or a click types it into
// the focused window and closes the panel; Esc closes it.
//
// AN OVERLAY, LIKE THE ON-SCREEN KEYBOARD (osk.h), for the same two reasons:
// it is not a window, so it never takes focus from the field it types into;
// and the characters go out through wm_client_send_key(), as 32-bit code
// points, so no ring-3 program gains a way to type into another
// (docs/decisions/gui.md). Where it opens comes from the client's
// WIN_REQ_CARET_RECT (abi/win_proto.h); with no caret known it centres in
// the window's lower half.
extern int emoji_picker_open;

// Opens it for the focused window, or closes it when it is up.
void emoji_picker_toggle(void);

// The overlay registry's ops -- see wm_overlay.h.
void emoji_picker_draw(int mx, int my);
int  emoji_picker_handle_click(int mx, int my);
int  emoji_picker_hover_at(int mx, int my);
void emoji_picker_damage(void);
int  emoji_picker_rect(int *x, int *y, int *w, int *h);
void emoji_picker_update_press(int mx, int my, uint8_t buttons);
int  emoji_picker_wheel(int mx, int my, int notches);
int  emoji_picker_key(int key, uint8_t mods);
void emoji_picker_close(void);
// The caret's next blink, for the loop's wait; -1 when closed.
int  emoji_picker_wait_ms(void);

// `gui emoji`: the panel, its parts and what is focused, for a test.
struct dbg_out;
void emoji_picker_report(struct dbg_out *o, int json);

#endif
