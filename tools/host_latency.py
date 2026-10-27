"""Holds this HOST's CPUs out of their deep idle states while a guest's
timing is measured.

    from host_latency import shallow_idle

    with shallow_idle("usertest_run"):
        ...boot the guest, run the checks...

A guest's one-shot timer fires through QEMU, a host process, and when
the guest idles that process sleeps on a host CPU. The longer it sleeps
the deeper the C-state the host's cpuidle governor picks, and the slower
it wakes: on the HP Z1 G8 (C3 declares 1048 us) a 16ms guest sleep came
back ~600 us late against ~250 us with C2/C3 held off -- the guest's
deadline was right both times. cyclictest does the same thing for the
same reason: /dev/cpu_dma_latency, the PM QoS request, held open for as
long as the run lasts and released when the file closes.

Nothing is held when the file cannot be opened (no permission -- root or,
on some distributions, the `audio` group -- or a container without it);
the run goes ahead and says so, because a timing check that goes red
there is measuring the host.
"""
import contextlib
import struct

PATH = "/dev/cpu_dma_latency"

# Microseconds. Low enough to rule out every state with a real exit
# latency (C2 and deeper declare hundreds), high enough to keep C1, so
# the host does not spin at 100% on every idle core the way 0 (POLL
# only) would.
MAX_WAKE_US = 10


@contextlib.contextmanager
def shallow_idle(tool):
    try:
        f = open(PATH, "wb", buffering=0)
        f.write(struct.pack("i", MAX_WAKE_US))
    except OSError as e:
        print(f"{tool}: host idle states NOT held ({PATH}: {e.strerror}) -- "
              "a late-wake timing check measures this host's C-states")
        yield False
        return
    try:
        yield True
    finally:
        f.close()
