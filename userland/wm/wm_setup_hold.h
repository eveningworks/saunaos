#ifndef WM_SETUP_HOLD_H
#define WM_SETUP_HOLD_H

// THE DESKTOP IS HELD BACK WHILE FIRST-BOOT SETUP IS ON ITS WAY: from the
// compositor's first frame until setup's window has drawn itself full
// screen, the screen is setup's plain grey -- no icons, no taskbar, no
// notices -- so the desktop never shows for the second or two setup takes
// to start. Windows runs its setup before Explorer, macOS before Finder,
// GNOME Initial Setup in a session of its own; here the compositor is the
// desktop too, so it holds the desktop back itself.
//
// Decided by the same test setup uses (lib/ufbs.h's ufbs_needed()), and
// released by setup's window or, failing that, a timeout: a setup that
// crashed before drawing must not leave a grey screen.

void wm_setup_hold_begin(void);
// From the frame loop: let go once setup is on screen, or the time is up.
void wm_setup_hold_poll(void);
// Is the desktop held back this frame?
int wm_setup_hold_active(void);

#endif
