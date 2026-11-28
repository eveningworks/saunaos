// _BIX/_BIF/_BST into one shape -- see battery_parse.h.
#include "battery_parse.h"
#include "kfmt.h"
#include "string.h"

#define ACPI_UNKNOWN32 0xFFFFFFFFull   // the spec's "unknown" for a DWORD field

static int num(const struct bat_val *v, uint64_t *out) {
    if (v->is_text) return -1;
    *out = v->n == ACPI_UNKNOWN32 ? BAT_UNKNOWN : v->n;
    return 0;
}

// A text field may also come as an Integer (some firmware gives the serial
// number that way): written out in decimal, as Linux does.
static void text(const struct bat_val *v, char *out) {
    if (v->is_text) {
        k_strlcpy(out, v->text, BAT_TEXT_MAX);
        return;
    }
    k_snprintf(out, BAT_TEXT_MAX, "%llu", (unsigned long long)v->n);
}

// x in the battery's own unit, to mWh/mW. Unknown stays unknown.
static uint64_t to_mw(uint64_t x, int mah, uint64_t mv) {
    if (x == BAT_UNKNOWN || !mah) return x;
    if (mv == BAT_UNKNOWN || mv == 0) return BAT_UNKNOWN;
    return x * mv / 1000;
}

// The common middle of the two info packages, from `power unit` on:
// unit, design, last full, technology, design voltage, warning, low.
static int info_core(const struct bat_val *v, struct bat_info *out) {
    uint64_t unit, design, full, tech, mv, warn, low;
    if (num(&v[0], &unit) || num(&v[1], &design) || num(&v[2], &full) || num(&v[3], &tech) ||
        num(&v[4], &mv) || num(&v[5], &warn) || num(&v[6], &low))
        return -1;
    if (unit > 1) return -1;
    out->mah = (int)unit;
    out->design_mv = mv;
    out->design_mwh = to_mw(design, out->mah, mv);
    out->full_mwh = to_mw(full, out->mah, mv);
    out->warn_mwh = to_mw(warn, out->mah, mv);
    out->low_mwh = to_mw(low, out->mah, mv);
    out->rechargeable = tech == 1;
    return 0;
}

int bat_parse_bif(const struct bat_val *v, int n, struct bat_info *out) {
    if (n < 13) return -1;
    struct bat_info b;
    k_memset(&b, 0, sizeof b);
    if (info_core(v, &b)) return -1;
    b.cycles = BAT_UNKNOWN;
    text(&v[9], b.model);
    text(&v[10], b.serial);
    text(&v[11], b.type);
    text(&v[12], b.maker);
    *out = b;
    return 0;
}

// _BIX: Revision first, then _BIF's fields with Cycle Count and six
// measurement fields inserted after the low capacity, the texts last.
int bat_parse_bix(const struct bat_val *v, int n, struct bat_info *out) {
    if (n < 20) return -1;
    struct bat_info b;
    k_memset(&b, 0, sizeof b);
    if (v[0].is_text || info_core(&v[1], &b)) return -1;
    if (num(&v[8], &b.cycles)) return -1;
    text(&v[16], b.model);
    text(&v[17], b.serial);
    text(&v[18], b.type);
    text(&v[19], b.maker);
    *out = b;
    return 0;
}

int bat_parse_bst(const struct bat_val *v, int n, const struct bat_info *info, struct bat_status *out) {
    if (n < 4) return -1;
    uint64_t state, rate, now, mv;
    if (num(&v[0], &state) || num(&v[1], &rate) || num(&v[2], &now) || num(&v[3], &mv)) return -1;
    if (state == BAT_UNKNOWN) return -1;
    uint64_t conv_mv = info->design_mv != BAT_UNKNOWN && info->design_mv ? info->design_mv : mv;
    // A NEGATIVE 16-bit rate is a firmware bug Linux corrects on every
    // machine (acpi_battery_get_state()): the magnitude is the rate.
    if (rate != BAT_UNKNOWN && rate <= 0xFFFF && (int16_t)rate < 0) rate = (uint64_t)-(int16_t)rate;
    out->state = (uint32_t)(state & (BAT_DISCHARGING | BAT_CHARGING | BAT_CRITICAL));
    out->rate_mw = to_mw(rate, info->mah, conv_mv);
    out->now_mwh = to_mw(now, info->mah, conv_mv);
    out->mv = mv;
    return 0;
}

int bat_percent(const struct bat_info *info, const struct bat_status *st) {
    uint64_t full = info->full_mwh != BAT_UNKNOWN && info->full_mwh ? info->full_mwh : info->design_mwh;
    if (full == BAT_UNKNOWN || !full || st->now_mwh == BAT_UNKNOWN) return -1;
    uint64_t p = (st->now_mwh * 100 + full / 2) / full;
    return p > 100 ? 100 : (int)p;
}
