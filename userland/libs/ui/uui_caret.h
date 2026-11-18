#ifndef UUI_CARET_H
#define UUI_CARET_H

#ifdef __cplusplus
extern "C" {
#endif

// THE TEXT CARET'S BLINK -- one phase for the whole process, asked by
// every widget that draws a caret (uui_textbox, utext, so uui_textview).
// Windows (GetCaretBlinkTime, 530 ms), Qt (cursorFlashTime, 1000 ms a
// cycle) and GTK blink one caret per app the same way. GTK's
// gtk-cursor-blink-timeout is copied too: TEN SECONDS AFTER THE LAST
// INPUT THE CARET STOPS, SOLID, so an idle window sits still
// (docs/gui-guidelines.md, "An idle screen must SIT STILL").
//
// Input restarts the cycle ON, so the caret is never missing under a key
// just typed. `desktop.caret_blink` off draws it solid always. uapp wakes
// for the next flip only while a caret was drawn since the last one;
// nothing registers, so a widget destroyed mid-blink leaves nothing
// dangling (the uui_anim.h shape).

#define UUI_CARET_HALF_MS    500    // on for this long, then off as long
#define UUI_CARET_TIMEOUT_MS 10000  // then solid until the next input

// Draw a caret now? ALSO records that one was drawn, which is what makes
// uapp wake for the next flip -- so ask it only where a caret is drawn.
int uui_caret_visible(void);

// uapp: input arrived. Solid now, blink restarts; re-reads the setting
// at most every two seconds (this runs on every key, caret or not).
void uui_caret_reset(void);

// uapp: milliseconds until a drawn caret next changes, or -1 when none
// is blinking. 0 = repaint now, returned ONCE per phase -- so call it
// once per wait, and act on the 0.
int uui_caret_wait_ms(void);

// WHERE THE CARET IS, whatever the blink: a widget that draws one says so
// every frame, in the coordinates of the surface it draws on. uapp brackets
// a window's paint with _begin/_end and reports a change to the compositor
// (abi/win_proto.h's WIN_REQ_CARET_RECT), which opens the emoji picker there.
struct ugfx_surface;
void uui_caret_at(const struct ugfx_surface *s, int x, int y, int w, int h);
void uui_caret_frame_begin(void);
// The last caret placed on `s` since _begin: 1 and its rect, or 0 for none.
int  uui_caret_frame_end(const struct ugfx_surface *s, int *x, int *y, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif
