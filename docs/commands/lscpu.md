# lscpu

**a `/bin` program.**

**Category:** System information

## Synopsis

    lscpu

## Options

- `-h`, `--help` -- print the usage and exit.

## Description

CPU identity and features, over `SYS_CPU_INFO`: vendor, brand, family/
model/stepping, measured or CPUID-reported speed, and which protection
features are actually ENABLED (SSE, NX, SMEP, SMAP). Then the cores the
firmware lists and the load on each (`QUERY_CPUS`, `QUERY_CORELOAD`).

Enabled rather than merely supported, which is the distinction worth
having: a CPU advertising SMAP that the kernel never turned on offers
no protection, and only one of those two numbers says so.

## The clock, on bare-metal Intel

Below `CPU MHz` (the TSC's rate), an Intel family-6 machine gets the
clock it really runs at, from its frequency MSRs:

    Ratio now:            20 (2000 MHz)  (IA32_PERF_STATUS)
    Ratio requested:      20 (2000 MHz)  (IA32_PERF_CTL, as the firmware left it)
    Busy clock:           1996 MHz over 101 ms of load  (APERF/MPERF)
    Ratio range:          lowest 5 (500 MHz), base 10 (1000 MHz)  (MSR_PLATFORM_INFO)
    Turbo ratios:         20 with 1, 20 with 2 core(s) busy  (MSR_TURBO_RATIO_LIMIT)
    Turbo:                available  (CPUID leaf 06H)
    SpeedStep:            enabled -- the CPU obeys a ratio written to IA32_PERF_CTL
    HWP:                  not supported  (Skylake and later have it)

(the ASUS's Core M-5Y10c). SaunaOS writes no ratio, so "requested" is
whatever the firmware left. **Busy clock** is turbostat's `Bzy_MHz`:
`lscpu` spins for ~100 ms and scales the TSC rate by APERF/MPERF, which
count only while the core is not halted -- the clock work actually got,
turbo included, not an average with the idle time. A ratio is in units
of the 100 MHz bus clock.

A line is left out when its MSR is not there to read: an MSR the CPU
lacks is a fault in the kernel, so each is read only behind its CPUID
bit, and the two non-architectural ones (`PLATFORM_INFO`,
`TURBO_RATIO_LIMIT`) only on the models in `kernel/arch/x86_64/cpufreq.c`'s
table. Under a hypervisor -- every QEMU run -- and on AMD there is one
line instead:

    Frequency:            not read -- a guest's frequency MSRs describe no real clock

## Counting cores is a different question, from a different source

CPUID describes **the core executing the instruction** and cannot see
the others, so the `Logical CPUs` line does not come from it — it comes
from the ACPI MADT, over `QUERY_CPUS`. A machine whose firmware supplied
no MADT reports `1 (no ACPI MADT -- nothing counted the others)`, which
is honest rather than a guess.

The `online` count is the cores the kernel STARTED: every core the
firmware enabled is brought up, and each runs processes, so a four-core
guest (`make run CPUS=4`) reads `4 (4 enabled by firmware, 4 online)`.
Booted with `smp=park` the others are started but run nothing, and the
line adds `scheduling on 1`. `nosmp` or `maxcpus=N` on the kernel
command line starts fewer (`docs/smp-design.md`). `acpi` prints the per-core detail.

## Load on each core

After the topology, one line per online core: the share of a 100 ms
interval a process ran there, read from `QUERY_CORELOAD` twice with
`lscpu` asleep in between (spinning would be the load it measured):

    Load over 100 ms (a process running counts as busy):
      CPU 0     3%  (APIC id 0)
      CPU 1   100%  (APIC id 1)
      CPU 2     0%  (APIC id 2)
      CPU 3     0%  (APIC id 3)

A core that is online but runs no processes (`smp=park`) adds
`, parked -- runs no processes` and reads 0%. **Busy means a process**:
the boot core's own kernel work -- the text-mode shell, the disk's
write-back, the network poll -- is not counted, which is the same
caveat Task Manager's Utilization carries. Another core's slice still in
progress is counted when it ends, so a reading over 100 ms can trail by
up to one time slice. A kernel without `QUERY_CORELOAD` prints no block.

## See also

`acpi` for the processor list in full, `lspci` and `lsusb` for the same
shape of question about other hardware.
