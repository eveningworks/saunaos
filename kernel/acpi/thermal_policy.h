// A thermal zone's arithmetic, apart from uACPI so its KTEST can drive
// it: the firmware's deci-Kelvin as millidegrees Celsius, and passive
// cooling's step (ACPI 6.5 11.1.5.1, Linux's old acpi_thermal_passive()).
//
// PASSIVE COOLING is an integral controller the firmware tunes: every
// _TSP while the zone is at or over _PSV (or still slowed from a spell
// that was), the performance limit moves by
//
//     dP[%] = _TC1 * (Tn - Tn-1) + _TC2 * (Tn - _PSV)      (T in kelvin)
//
// -- down while it heats or sits above _PSV, up once it cools below. The
// spell ends when the zone is under _PSV with the limit back at 100.
#ifndef THERMAL_POLICY_H
#define THERMAL_POLICY_H

#include <stdint.h>

#define THERMAL_NONE INT64_MIN          // QUERY_THERMAL_NONE's value
#define THERMAL_PERF_MIN 1              // passive cooling never asks for 0%

// _TMP/_CRT/_PSV/_HOT answer tenths of a kelvin. 0 and anything under
// absolute zero's tenth-kelvin count are firmware placeholders: NONE.
int64_t thermal_dk_to_mc(uint64_t dk);

struct thermal_passive {
    int active;                         // a spell is running
    int perf_pct;                       // THERMAL_PERF_MIN..100
    int64_t last_mc;                    // the previous sample, for the _TC1 term
};

// One _TSP sample at `t_mc`. `tc1`/`tc2` are the zone's constants;
// returns the new limit (also in s->perf_pct).
int thermal_passive_step(struct thermal_passive *s, int64_t t_mc, int64_t psv_mc, int tc1, int tc2);

#endif
