// A control method battery's packages, _BIX/_BIF/_BST, read into one
// shape (ACPI 6.5 10.2.2). The packages arrive as a flat list of values
// (uacpi_supply.c converts uACPI's objects), so this half names no uACPI
// type and its KTEST builds the lists by hand.
//
// UNITS ARE mWh AND mW HERE, whatever the firmware used: a battery
// reporting mAh/mA (power unit 1) is converted through its DESIGN
// voltage -- what UPower does, so a sagging present voltage does not make
// the energy jump. A battery that names no design voltage has its _BST
// converted through the present one, but its capacities stay unknown, so
// it has no percentage.
#ifndef BATTERY_PARSE_H
#define BATTERY_PARSE_H

#include <stdint.h>

#define BAT_UNKNOWN UINT64_MAX     // a value the firmware said it did not know
#define BAT_TEXT_MAX 32            // model, serial, type and maker, NUL-terminated

struct bat_val {
    int is_text;                   // a String or a Buffer; else an Integer
    uint64_t n;
    char text[BAT_TEXT_MAX];
};

// The static half: _BIX, or _BIF where there is no _BIX.
struct bat_info {
    int mah;                       // the firmware counts mAh (converted below)
    uint64_t design_mwh, full_mwh, warn_mwh, low_mwh;
    uint64_t design_mv;
    uint64_t cycles;               // _BIX only; BAT_UNKNOWN from _BIF
    int rechargeable;              // Battery Technology 1
    char model[BAT_TEXT_MAX], serial[BAT_TEXT_MAX], type[BAT_TEXT_MAX], maker[BAT_TEXT_MAX];
};

// _BST's state bits.
#define BAT_DISCHARGING (1u << 0)
#define BAT_CHARGING    (1u << 1)
#define BAT_CRITICAL    (1u << 2)

struct bat_status {
    uint32_t state;                // BAT_*
    uint64_t rate_mw, now_mwh, mv;
};

// 0 and *out filled, or -1 for a package that is not that object's shape
// (too short, a text where a number goes). A REJECTED package leaves the
// battery's last good facts in place; it is never guessed at.
int bat_parse_bix(const struct bat_val *v, int n, struct bat_info *out);
int bat_parse_bif(const struct bat_val *v, int n, struct bat_info *out);
// _BST needs the info's units and voltage to convert.
int bat_parse_bst(const struct bat_val *v, int n, const struct bat_info *info, struct bat_status *out);

// Remaining charge as a percentage of the last full charge (the design
// capacity when that is unknown), 0..100, or -1 when it cannot be said.
int bat_percent(const struct bat_info *info, const struct bat_status *st);

#endif
