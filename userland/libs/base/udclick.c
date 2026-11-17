// The double-click time -- see udclick.h.
#include "base/udclick.h"
#include "base/usetting.h"
#include "rt/sys.h"   // USER_HZ
#include <string.h>

// The setting's words, slowest first -- the order its slider shows them.
// "normal" is GNOME's 400 ms; Windows ships 500.
static const struct { const char *word; int ms; } SPEEDS[] = {
    { "veryslow", 700 }, { "slow", 550 }, { "normal", 400 }, { "fast", 300 }, { "veryfast", 250 },
};
#define DEFAULT_MS 400

static int g_ms;   // 0: not read since the last reload

int udclick_word_ms(const char *word) {
    for (unsigned i = 0; i < sizeof SPEEDS / sizeof SPEEDS[0]; i++)
        if (word && !strcmp(word, SPEEDS[i].word)) return SPEEDS[i].ms;
    return DEFAULT_MS;
}

int udclick_ms(void) {
    if (!g_ms) {
        char v[16];
        g_ms = udclick_word_ms(usetting_get(UDCLICK_SETTING, v, sizeof v) ? v : 0);
    }
    return g_ms;
}

uint64_t udclick_ticks(void) { return (uint64_t)udclick_ms() * USER_HZ / 1000; }
uint64_t udclick_ns(void)    { return (uint64_t)udclick_ms() * 1000000ull; }

void udclick_reload(void) { g_ms = 0; }

int udclick_count(struct udclick_run *r, int x, int y) {
    uint64_t now = sys_monotonic_ns();
    int dx = x - r->x, dy = y - r->y;
    int near = dx <= UDCLICK_SLOP && -dx <= UDCLICK_SLOP && dy <= UDCLICK_SLOP && -dy <= UDCLICK_SLOP;
    r->n = r->n > 0 && r->n < 3 && near && now - r->ns <= udclick_ns() ? r->n + 1 : 1;
    r->ns = now;
    r->x = x;
    r->y = y;
    return r->n;
}
