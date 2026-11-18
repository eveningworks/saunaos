/* SDL_OpenURL on SaunaOS: xdg-open's job, done by base/uopen.h -- the
 * Web Browser for http: and https:, the file's own opener for file:.
 * Replaces SDL's src/misc/dummy/SDL_sysurl.c (tools/fetch/sdl_vendor.py
 * leaves that one out). */
#include "SDL_internal.h"

#include "misc/SDL_sysurl.h"
#include "SDL_saunaos_video.h"
#include "base/uopen.h"
#include "ui/uapp.h"

#include <string.h>   /* strerror() */

bool SDL_SYS_OpenURL(const char *url)
{
    /* With the video driver up, the app reaps what it started; without
     * it nothing does until this program ends, as for any uopen_url()
     * caller. */
    int pid = SAUNAOS_app ? uapp_open_url(SAUNAOS_app, url) : uopen_url(url);
    if (pid < 0 && uopen_is_web_url(url)) {
        /* The browser exists; its spawn failed, and the errno says why. */
        return SDL_SetError("The Web Browser did not start for %s: %s", url, strerror(-pid));
    }
    if (pid < 0) {
        return SDL_SetError("Nothing on SaunaOS opens %s", url);
    }
    return true;
}
