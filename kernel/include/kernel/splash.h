#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>

// THE BOOT SPLASH: the toy-os logo on a dark screen from the moment the
// framebuffer is up until the compositor claims it -- in place of the
// scrolling kernel log and the ring-0 shell's prompt, which a graphical
// boot used to show for a second or two. Windows' and Plymouth's
// flicker-free boot; the WM carries the same logo on with a spinner
// (api/splash_layout.h).
//
// The logo alone in the middle, a line of text near the bottom; the
// compositor adds the spinner above that line (Windows' arrangement).
//
// The console keeps drawing into its back buffer underneath, and
// presenting is what the splash holds back (vga.c), so the log is all
// there the moment the splash ends with it. The splash is drawn ONCE,
// statically: animating it would mean flushing the display from a timer
// interrupt, which a virtio-gpu flush may not do.
//
// Not shown with `splash=off`, `target=text` or `target=rescue` on the
// command line, nor on a mode the console cannot double-buffer.

void splash_begin(void);
int  splash_active(void);

// init has started: with `boot_messages=detailed` the line under the
// spinner says so ("Starting services..."); the default says "Starting
// toy-os..." throughout. The compositor carries the line on
// (wm_setup_hold.c).
void splash_stage_services(void);
void splash_setting_register(void);   // system.boot_messages

// Ends it. `show_log` puts the console's whole page on screen (Esc, a
// panic, a text boot); 0 leaves the logo there for whoever draws next
// -- the compositor, whose first frame replaces it.
void splash_end(int show_log);

// From the input core, in interrupt context: Esc asks for the log, which
// the next console present then shows (splash_poll()).
void splash_key(uint16_t keycode, int down);

// From vga_present(): acts on a pending Esc, and ends the splash with
// the log if the compositor has not claimed the screen in time -- a
// desktop that never starts must not leave a logo and no way in.
void splash_poll(void);

#endif
