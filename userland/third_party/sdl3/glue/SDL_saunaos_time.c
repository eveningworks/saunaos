// SDL3's time module for SaunaOS (SDL_TIME_PRIVATE).
//
// SDL's own Unix module reads `struct tm`'s tm_gmtoff, which tolibc's
// struct tm has not got: adding it grows the struct, and the cross
// toolchain's prebuilt libstdc++.a would then hand localtime_r() a
// buffer of the old size. tolibc's tz_offset_seconds() is the same
// number without the struct change. Everything else follows
// src/time/unix/SDL_systime.c.
#include "SDL_internal.h"

#ifdef SDL_TIME_PRIVATE

#include "time/SDL_time_c.h"
#include <errno.h>
#include <langinfo.h>
#include <time.h>

// The date and time order from the region's strftime formats, by their
// first telling conversion -- what SDL's Unix module does.
void SDL_GetSystemTimeLocalePreferences(SDL_DateFormat *df, SDL_TimeFormat *tf)
{
    if (df) {
        for (const char *s = nl_langinfo(D_FMT); s && *s; s++) {
            if (*s == 'Y' || *s == 'y' || *s == 'F' || *s == 'C') {
                *df = SDL_DATE_FORMAT_YYYYMMDD;
                break;
            }
            if (*s == 'd' || *s == 'e') {
                *df = SDL_DATE_FORMAT_DDMMYYYY;
                break;
            }
            if (*s == 'b' || *s == 'D' || *s == 'h' || *s == 'm') {
                *df = SDL_DATE_FORMAT_MMDDYYYY;
                break;
            }
        }
    }
    if (tf) {
        for (const char *s = nl_langinfo(T_FMT); s && *s; s++) {
            if (*s == 'H' || *s == 'k' || *s == 'T') {
                *tf = SDL_TIME_FORMAT_24HR;
                break;
            }
            if (*s == 'I' || *s == 'l' || *s == 'r') {
                *tf = SDL_TIME_FORMAT_12HR;
                break;
            }
        }
    }
}

bool SDL_GetCurrentTime(SDL_Time *ticks)
{
    CHECK_PARAM(!ticks) {
        return SDL_InvalidParamError("ticks");
    }
    struct timespec tp;
    if (clock_gettime(CLOCK_REALTIME, &tp) != 0) {
        return SDL_SetError("Failed to retrieve system time (%i)", errno);
    }
    *ticks = SDL_SECONDS_TO_NS(tp.tv_sec) + tp.tv_nsec;
    return true;
}

bool SDL_TimeToDateTime(SDL_Time ticks, SDL_DateTime *dt, bool localTime)
{
    CHECK_PARAM(!dt) {
        return SDL_InvalidParamError("dt");
    }

    int rem_ns = (int)(ticks % SDL_NS_PER_SECOND);
    if (rem_ns < 0) {
        ticks -= SDL_NS_PER_SECOND;
        rem_ns += SDL_NS_PER_SECOND;
    }
    const time_t tval = (time_t)SDL_NS_TO_SECONDS(ticks);

    struct tm storage;
    struct tm *tm = localTime ? localtime_r(&tval, &storage) : gmtime_r(&tval, &storage);
    if (!tm) {
        return SDL_SetError("SDL_DateTime conversion failed (%i)", errno);
    }
    dt->year = tm->tm_year + 1900;
    dt->month = tm->tm_mon + 1;
    dt->day = tm->tm_mday;
    dt->hour = tm->tm_hour;
    dt->minute = tm->tm_min;
    dt->second = tm->tm_sec;
    dt->nanosecond = rem_ns;
    dt->day_of_week = tm->tm_wday;
    dt->utc_offset = localTime ? (int)tz_offset_seconds(&tval) : 0;
    return true;
}

#endif // SDL_TIME_PRIVATE
