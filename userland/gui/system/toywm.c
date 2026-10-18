// toywm -- the window manager and compositor, as a ring-3 process.
//
// This file is only the entry point; the desktop is `userland/wm/`,
// linked in through the Makefile's EXTRA_OBJS_toywm.
#include "rt/sys.h"
#include "wm/wm.h"
#include "lib/ubootcfg.h"   // ubootcfg_cmdline(), ubootcfg_onoff_in()
#include "ui/ugfx.h"        // ugfx_simd_set()
#include <stdlib.h>

int main(void) {
    // `simd=off` runs ugfx's C pixel loops here and, through UGFX_SIMD in
    // the environment, in every program this desktop starts -- read here
    // because a sandboxed client may not query the command line itself.
    char line[1024];
    if (ubootcfg_cmdline(line, sizeof line) >= 0 && ubootcfg_onoff_in(line, "simd=") == 2) {
        setenv("UGFX_SIMD", "off", 1);
        ugfx_simd_set("off");
    }
    // wm_run() owns the whole desktop lifetime and returns only when the
    // session ends; the kernel restores the console when it sees the
    // compositor deregister (R7).
    wm_run();
    return 0;
}
