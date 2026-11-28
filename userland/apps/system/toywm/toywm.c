// toywm -- the window manager and compositor, as a ring-3 process.
//
// This file is only the entry point; the desktop is `userland/apps/system/toywm/`,
// linked in through the Makefile's EXTRA_OBJS_toywm.
#include "media/uclip.h"   // uclip_set_wait_ms()
#include "rt/sys.h"
#include "apps/system/toywm/wm.h"
#include "base/ubootcfg.h"   // ubootcfg_cmdline(), ubootcfg_onoff_in()
#include "base/usetting.h"   // usetting_session_apply()
#include "ui/ugfx.h"        // ugfx_simd_set()
#include <stdlib.h>
#include <string.h>
#include "apps/system/toywm/wm_internal.h"

int main(int argc, char **argv) {
    // `--greeter`: the login screen (users D2b, data/etc/services.d/greeter).
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--greeter")) wm_greeter = 1;
    // `simd=off` runs ugfx's C pixel loops here and, through UGFX_SIMD in
    // the environment, in every program this desktop starts -- read here
    // because a sandboxed client may not query the command line itself.
    char line[1024];
    if (ubootcfg_cmdline(line, sizeof line) >= 0 && ubootcfg_onoff_in(line, "simd=") == 2) {
        setenv("UGFX_SIMD", "off", 1);
        ugfx_simd_set("off");
    }
    // THE PERSON'S POINTER, KEYBOARD AND FONTS (users D3c): the kernel holds
    // the machine's until the session's compositor applies theirs, as
    // Windows applies HKCU's at logon; init puts the machine's back after.
    if (!wm_greeter) usetting_session_apply();
    // A LARGE COPY OR DRAG FROM THE DESKTOP waits on clipboardd for its
    // object; the compositor waits a quarter second, not seconds, and a
    // copy that outlasts it is refused with a card -- a compositor never
    // blocks on another process (Mutter and KWin's rule for clients).
    uclip_set_wait_ms(250);
    // wm_run() owns the whole desktop lifetime and returns only when the
    // session ends; the kernel restores the console when it sees the
    // compositor deregister (R7).
    wm_run();
    return wm_exit_code;   // INITCTL_EXIT_SIGN_OUT ends the session (init)
}
