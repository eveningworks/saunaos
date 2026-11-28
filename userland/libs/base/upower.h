#ifndef ULIB_UPOWER_H
#define ULIB_UPOWER_H

// The machine's batteries and charger as one answer: the charge, whether
// it is charging, and how long is left -- what UPower's DisplayDevice is
// on Linux and the battery flyout reads on Windows. From QUERY_POWER and
// QUERY_POWER_SUPPLY (`acpi=uacpi`); several batteries add up.
//
// The kernel moves QUERY_POWER's `generation` with every change, so a
// poller reads upower_generation() -- one small record -- and calls
// upower_read() only when it moved.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The desktop's levels. The kernel shuts down at its own (QUERY_POWER's
// critical_pct, 3%), below these.
#define UPOWER_LOW_PCT      15   // the icon turns amber
#define UPOWER_WARN_PCT     10   // the "Battery low" notice
#define UPOWER_CRITICAL_PCT  5   // the icon turns red

struct upower {
    uint64_t generation;
    uint64_t flags;           // QUERY_POWER_* (abi/query_abi.h)
    int batteries;            // in their bays
    int percent;              // of the full charge, 0..100; -1 unknown or no battery
    int charging, discharging;
    int plugged;              // the charger is in (or nothing says it is out and nothing discharges)
    int64_t seconds;          // to empty when discharging, to full when charging; -1 unknown
    uint64_t now_mwh, full_mwh, design_mwh, rate_mw;   // summed; 0 where unknown
    int critical_pct;         // the kernel's shutdown level
};

// 0 and *out filled -- `batteries` 0 on a machine with none -- or -1 when
// the kernel has no such record.
int upower_read(struct upower *out);
uint64_t upower_generation(void);

// "4 h 53 min", "52 min", "under a minute"; "" for unknown.
void upower_duration(int64_t seconds, char *out, unsigned cap);
// "78% · 4 h 53 min left", "45% · charging, 1 h 10 min to full",
// "100% · plugged in", "78%" -- one line for a footer or a tooltip.
void upower_summary(const struct upower *p, char *out, unsigned cap);
// The state alone: "On battery", "Charging", "Plugged in", "Fully charged".
const char *upower_state(const struct upower *p);

#ifdef __cplusplus
}
#endif

#endif
