#ifndef WM_LOGIN_H
#define WM_LOGIN_H
// The login screen and the lock screen -- ONE full-screen overlay
// (users D2b/D2c, docs/users-design.md; mockup L1): the clock, the
// account's picture, name and password, the computer's name, Restart and
// Shut down. In the GREETER (toywm --greeter, the `greeter` account) it
// signs in through privd, which asks init for the session; in the
// person's own desktop it LOCKS, and unlocks through privd's check of
// their own password. The compositor never reads /etc/shadow either way.
#include <stdint.h>

#define WM_LOGIN_GREETER 0
#define WM_LOGIN_LOCK    1

extern int wm_login_open;

void wm_login_show(int mode);
// Each frame: the sign-in's answer from its worker, and the clock.
void wm_login_poll(void);

// The overlay registry's ops (wm_overlay.h).
void wm_login_draw(int mx, int my);
int  wm_login_click(int mx, int my);
void wm_login_update_press(int mx, int my, uint8_t buttons);
int  wm_login_hover_at(int mx, int my);
void wm_login_damage(void);
int  wm_login_key(int key, uint8_t mods);
// It hides the whole scene: windows, desktop and taskbar are not drawn.
int  wm_login_covers(void);

// A PASSWORD IS BEING TYPED to the compositor itself -- this screen, or
// the Authentication Required dialog's field: NO key edge of any kind
// reaches a client then, not even the releases (which otherwise go
// straight to the focused one) or a key by position. A window under the
// lock could read the password key by key from its key-ups.
int  wm_keys_private(void);

// For `gui login --json`: the mode, the account shown, the error line,
// BUSY, the characters typed, and each control's rect by name.
int  wm_login_state(int *mode, const char **account, const char **err, int *busy, int *typed);
int  wm_login_control(int i, const char **name, int *x, int *y, int *w, int *h);

#endif
