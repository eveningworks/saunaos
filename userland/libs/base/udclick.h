#ifndef ULIB_UDCLICK_H
#define ULIB_UDCLICK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// udclick -- the double-click time: how soon a second click must follow
// the first to count as a double click. ONE value for the whole desktop,
// `system.double_click` (Input > Mouse), as Windows' GetDoubleClickTime()
// and GNOME's double-click key are -- every widget that tells a double
// click from two clicks asks here, so two of them never disagree.
//
// READ ONCE AND CACHED: a click is no time to read /etc. udclick_reload()
// forgets the value; uapp and the compositor call it on every settings
// change (WIN_EV_SETTING), so a program never has to.

#define UDCLICK_SETTING "system.double_click"

int udclick_ms(void);
uint64_t udclick_ticks(void);   // the same, in sys_ticks() (USER_HZ)
uint64_t udclick_ns(void);      // ... and in sys_monotonic_ns()

// The time one of the setting's words names ("normal" is 400 ms), or the
// default's for a word it does not know -- what System Settings' test
// folder uses for a speed staged but not yet applied.
int udclick_word_ms(const char *word);

void udclick_reload(void);

// A RUN OF CLICKS: each press within the double-click time of the last
// and within UDCLICK_SLOP px of it counts on -- 1, 2 (a double click),
// 3 (a triple), then 1 again -- as Windows (SM_CXDOUBLECLK) and Qt count
// them. What a text editor selects by: a word on 2, the line on 3.
#define UDCLICK_SLOP 4
struct udclick_run {
    uint64_t ns;
    int x, y, n;
};
int udclick_count(struct udclick_run *r, int x, int y);

#ifdef __cplusplus
}
#endif

#endif
