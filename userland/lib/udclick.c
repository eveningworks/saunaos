// The double-click time -- see udclick.h.
#include "lib/udclick.h"
#include "lib/usetting.h"
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
