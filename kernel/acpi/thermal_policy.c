// The thermal arithmetic thermal_policy.h describes.
#include "thermal_policy.h"

// 2732 tenths of a kelvin is 0.2 C: a reading at or below it is no
// laptop's and every placeholder firmware returns (0, or a KELV() of 0).
#define DK_FLOOR 2732

int64_t thermal_dk_to_mc(uint64_t dk) {
    if (dk <= DK_FLOOR || dk > 10000) return THERMAL_NONE;   // over 726 C: a garbage read
    return (int64_t)dk * 100 - 273150;
}

int thermal_passive_step(struct thermal_passive *s, int64_t t_mc, int64_t psv_mc, int tc1, int tc2) {
    if (!s->active) {
        if (psv_mc == THERMAL_NONE || t_mc < psv_mc) return s->perf_pct = 100;
        s->active = 1;
        s->perf_pct = 100;
        s->last_mc = t_mc;   // no trend on the first sample: the _TC2 term alone
    }
    // Millikelvin differences times the constants, rounded to whole
    // percent away from zero so a small steady excess still moves it.
    int64_t d = (int64_t)tc1 * (t_mc - s->last_mc) + (int64_t)tc2 * (t_mc - psv_mc);
    int64_t step = d >= 0 ? (d + 500) / 1000 : -((-d + 500) / 1000);
    int64_t p = (int64_t)s->perf_pct - step;
    if (p > 100) p = 100;
    if (p < THERMAL_PERF_MIN) p = THERMAL_PERF_MIN;
    s->perf_pct = (int)p;
    s->last_mc = t_mc;
    if (t_mc < psv_mc && s->perf_pct == 100) s->active = 0;
    return s->perf_pct;
}
