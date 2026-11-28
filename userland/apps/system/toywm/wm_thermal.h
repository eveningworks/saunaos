#ifndef WM_THERMAL_H
#define WM_THERMAL_H

// THE HOT NOTICES, from the firmware's thermal zones (QUERY_THERMAL,
// `acpi=uacpi`): "Running hot" once each time passive cooling starts
// slowing the CPUs, with Task Manager on it; "The computer is too hot"
// within QUERY_THERMAL_WARN_MC of a zone's critical trip, which gets
// through Do Not Disturb and stays until the zone cools. The kernel does
// the cooling and the shutdown; this only tells. A CPU sensor's TjMax is
// the hardware's own throttle point, not a shutdown, so it raises neither.

// Once a second, from the compositor's loop.
void wm_thermal_poll(void);

// For `gui thermal --json`: the hottest zone's reading and its trips (in
// millidegrees; INT64_MIN for none), and which notices are up.
struct wm_thermal_debug {
    int zones;
    long long temp_mc, passive_mc, critical_mc;
    int slowing, near_critical, hot_notice, too_hot_notice;
};
void wm_thermal_debug(struct wm_thermal_debug *d);

#endif // WM_THERMAL_H
