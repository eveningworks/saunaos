// mmsystem16.c -- MMSYSTEM, the multimedia library, as a program finds it
// with NO WAVE DEVICE: waveOutGetNumDevs answers 0, so a program that
// checks first (Hearts) keeps its sounds off, and sndPlaySound plays
// nothing. Both are stand-ins (task16.c's PARTLY) until a program's sound
// reaches toy-os's (the roadmap's w16 sound item).
#include "w16_internal.h"

// sndPlaySound(lpszSound, fuSound) -> FALSE: nothing played
static int api_sndPlaySound(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a);
    w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    return 6;
}

// waveOutGetNumDevs() -> 0
static int api_waveOutGetNumDevs(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    w->cpu.r[R_AX] = 0;
    return 0;
}

const struct w16_api g_mmsystem_api[] = {
    { 2, api_sndPlaySound },
    { 401, api_waveOutGetNumDevs },
};
const unsigned g_mmsystem_api_n = sizeof g_mmsystem_api / sizeof g_mmsystem_api[0];
