// urace.h: the race amplifier's delay, read once.
#include <time.h>
#include "base/urace.h"
#include "base/usetting.h"

static int g_delay = -1;

int urace_delay_ms(void) {
    int d = __atomic_load_n(&g_delay, __ATOMIC_RELAXED);
    if (d < 0) {
        int v = 0;
        if (!usetting_get_int("desktop.test_race_delay", &v) || v < 0) v = 0;
        if (v > 2000) v = 2000;
        __atomic_store_n(&g_delay, v, __ATOMIC_RELAXED);
        d = v;
    }
    return d;
}

// AT MOST ONE DELAY A SECOND per process: a result held back is what
// exposes a race, and a stream of hundreds (a download's progress) held
// back every time is a throughput collapse that times tests out instead.
static long long g_last_ns;

void urace_delay(void) {
    int ms = urace_delay_ms();
    if (ms <= 0) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long t = (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
    long long last = __atomic_load_n(&g_last_ns, __ATOMIC_RELAXED);
    if (last && t - last < 1000000000LL) return;
    if (!__atomic_compare_exchange_n(&g_last_ns, &last, t, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;   // another thread took this second's delay
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}
