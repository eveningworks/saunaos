/* SDL_OpenURL on SaunaOS: xdg-open's job, done by base/uopen.h -- the
 * Web Browser for http: and https:, the file's own opener for file:.
 * Replaces SDL's src/misc/dummy/SDL_sysurl.c (tools/fetch/sdl_vendor.py
 * leaves that one out). */
#include "SDL_internal.h"

#include "misc/SDL_sysurl.h"
#include "SDL_saunaos_video.h"
#include "base/uopen.h"
#include "ui/uapp.h"

#include <strings.h>

bool SDL_SYS_OpenURL(const char *url)
{
    int pid;
    /* With the video driver up, the app reaps the browser when it exits;
     * without it nothing does until this program ends, as for any
     * uopen_url() caller. */
    if (SAUNAOS_app && (!strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8))) {
        pid = uapp_spawn(SAUNAOS_app, UOPEN_BROWSER, url);
    } else {
        pid = uopen_url(url);
    }
    if (pid < 0) {
        return SDL_SetError("Nothing on SaunaOS opens %s", url);
    }
    return true;
}
