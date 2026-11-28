#ifndef WM_LID_H
#define WM_LID_H

// THE LID: closing a laptop's lid does `desktop.lid` -- lock and turn off
// the screen (the default), turn off the screen, shut down, or nothing --
// and opening it turns the screen back on. logind's HandleLidSwitch and
// Windows' "When I close the lid" are the shape; with no S3 yet there is
// no sleep among the choices. The fact is the kernel's (QUERY_POWER's
// lid bits, `acpi=uacpi`); on the login screen closing only turns the
// screen off, since there is no session to lock or apps to close.

// Once a second, from the compositor's loop.
void wm_lid_poll(void);

// For `gui battery --json`: whether there is a lid, whether it is open,
// how many closes this desktop acted on, and the last action's name.
void wm_lid_state(int *present, int *open, int *acted, const char **last);

#endif // WM_LID_H
