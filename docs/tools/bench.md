# `tools/bench/` -- Benchmarks and measurements

One entry per tool in `tools/bench/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

- **`vblank_pacing.py`** -- whether toywm's frames land on the vblank,
  on a machine whose display says where the beam is (Intel; it SKIPS
  (77) elsewhere): drags a Notepad window by its title bar with `guictl
  drag ... STEPS HZ`, alternating `gui pacing refresh` and `vblank`, and
  reads `gui compositor`'s frames shown, frames replaced before a vblank
  showed them, and change-to-shown lag. The refresh runs are the
  control: they skip refreshes the vblank runs must not. Against a
  machine you name (`--host`); opens or reuses Notepad there. A
  measurement on hardware, so in no runner (`ondemand_sweep.py` says
  why). The ASUS, 2026-10-31: 116 -> 120 of 120 refreshes shown, p99
  lag 32 -> 19 ms.
- **`vnc_latency.py`** -- how long a VNC viewer waits: each round moves
  the pointer, asks for an incremental update and times it; the full
  frame's size and time first. Pixels are not decoded (only lengths are
  read), so the client costs nothing beside what it measures. With the
  Cursor pseudo-encoding offered -- the default -- a pointer move sends
  NOTHING, and the rounds report as "no update within 1 s": that is the
  result, not a fault; `--no-cursor` measures the pointer drawn into the
  picture; `--tls` connects through VeNCrypt, to price the encryption;
  `--encoding tight` / `tight-jpeg` (quality level 5) to price Tight.
  Against a machine you name; changes nothing there but the
  pointer. A measurement, so in no runner (`ondemand_sweep.py` says why).
  The ASUS, 1920x1080, 2026-10-08: pointer move 62 ms -> 12 ms median
  once remoted followed the compositor's damage, and a full
  ZRLE frame 168 -> 68 ms.
- **`host_latency.py`** -- `with shallow_idle("tool"):` holds the HOST's CPUs out of
  their deep idle states (`/dev/cpu_dma_latency`, cyclictest's PM QoS
  request) while a guest's deadlines are timed; `ktest_run.py` and
  `usertest_run.py` run under it. A guest's timer fires through QEMU,
  which wakes from whatever C-state the host chose: on the HP Z1 G8 that
  made `sleep_test`'s 16ms sleeps ~550 us late against ~250 us held.
  Without permission (root, or the `audio` group) or the device (a
  container) it holds nothing and prints `host idle states NOT held` --
  a timing check that goes red there is measuring the host.
- **`boot_rate.py`** -- reboots a BARE-METAL machine N times and counts
  how often a substring appears in its `dmesg`. An intermittent fault is
  a RATE, and `docs/bugs.md` asks for exactly this by name for the USB
  enumeration bug ("reboot that machine 5-10 times counting `dmesg |
  grep -c polls`"). Three things it gets right that doing it by hand
  does not: it waits for the machine to go DOWN and come back rather
  than sleeping, so it cannot read the previous boot's log; it stops
  dead if the machine does not return, because every boot counted after
  a machine you have lost is a lie in the denominator; and it prints a
  line per boot as well as the rate, since an intermittent that CLUSTERS
  is a different animal from one evenly spread and a total hides that.
  It does not flash -- measure one kernel, and change kernels
  deliberately between runs. On demand only: it needs hardware.

### `tools/bench/idle_cpu.py` -- how much CPU does SaunaOS burn doing NOTHING?

Boots a VM, lets the desktop settle, and measures the **host's** CPU
time for the QEMU process over a window in which nothing is sent to the
guest. Also reports what `ps` says `toywm`'s state is, once, before the
window opens.

    python3 tools/bench/idle_cpu.py                # 30 s window
    python3 tools/bench/idle_cpu.py --window 60    # longer, less noise

**WHY THE HOST'S CLOCK AND NOT THE GUEST'S.** The guest cannot answer
this. `ps` bills whichever process was current at each timer tick, and
on a machine where something is always runnable that measures who got
scheduled rather than work done -- parking the compositor changed its
reported CPU by ZERO while moving its state from `ready` to
`block(event)`. The host's view of the emulator sits outside that
accounting entirely.

**QUOTE DIFFERENCES, NEVER THE ABSOLUTE NUMBER.** A large part of the
reading is TCG itself: translating the guest's 100 Hz tick costs host
cycles no guest change can remove. Worse, it tracks the host's own load
at the time -- the compositor's wait measured 42% of a core against
37-38% on 2026-08-27 and 0.76 s against 0.57 s per 30 s on 2026-08-29,
and only the ratios are comparable. Run it twice against the same host
with one thing changed, and take three samples an arm: the pairs above
are tight enough to separate only because they were.

**Do not run it beside anything else.** A second guest or a build
competing for cores lands directly in the number.

### `tools/bench/timer_bench.py` -- what a timer configuration costs and buys

Boots one build on a copy of its disk and reports three things: IDLE
(timer interrupts and periodic ticks a second over a quiet desktop, the
share of time the tick was stopped, and the QEMU process's host CPU),
LATENCY (`/tests/timer_bench`'s sleep overshoot at 1/3/7/16 ms, alone
and against two busy processes) and WORK (units one busy process
completes in 2 s, and two). It is what `option hz`'s default was chosen
on.

    python3 tools/bench/timer_bench.py                         # this checkout's build
    python3 tools/bench/timer_bench.py --kvm --runs 3 --label hz250 --json out.jsonl

To compare rates, build each one and point the tool at its image:
`make iso HZ=250`, copy `disk.img` somewhere, repeat, then run the tool
once per copy with `--disk`. `KCMDLINE="highres=off"` measures the
periodic path of the same build.

**THE IDLE COUNTERS ARE THE KERNEL'S OWN** (`config get clock.tick_*`,
QUERY_CLOCK), so a build from before them reports host CPU only. **SAY
WHICH ACCELERATOR** -- TCG and KVM disagree on everything here, and a
KVM guest without APICv pays VM exits per timer interrupt that TCG does
not model. WORK moves a few percent between runs of ONE build; believe
only a difference bigger than that, with `--runs`. Named by no runner:
it produces a comparison, not a verdict.

`tools/build/gen_kconfig.sh` writes the other half -- `build/gen/kconfig.h`
from build.conf's `option hz/tick/highres`, at Makefile parse time and
only when a value changed, so the `.d` files rebuild exactly what
includes it.

### `tools/bench/sys_bench.py` -- what a syscall, a switch and a read or write cost

Boots one build on a copy of its disk (or uses a running machine with
`--host`, over `remote.py`) and runs `/tests/syscall_bench`: a `getpid`,
a `yield`, a one-byte pipe write+read, a two-process round trip over two
pipes and the switch derived from it (lmbench's `lat_ctx` subtraction),
the median spawn-to-waitpid of `/bin/hello` (needs only the C library)
and of `/bin/ls /` (the toolkit libraries too) -- the cost of a program's
`DT_NEEDED` list, its output drained through a pipe -- and read/write of
64 B .. 1 MiB against a `/tmp` file beside two ring-3 `memcpy`s of the
same size.

    python3 tools/bench/sys_bench.py --kvm
    python3 tools/bench/sys_bench.py --kvm --runs 3 --label before --json out.jsonl
    python3 tools/bench/sys_bench.py --kvm --sizes 49152,61440,65536   # find a step
    python3 tools/bench/sys_bench.py --host <asus-ip>

**THE `copy` COLUMN IS A REFERENCE, NOT A FLOOR**: below ~256 KiB the
kernel's `rep movsb` path beats two ring-3 copies, so `read-copy` goes
negative there; it is what exposed the bounce buffer's 64 KiB step
(`fd_bounce_alloc()`), where the read was 4x the copy. **The program
SPAWNS a child for the switch row**, so it is spawned itself (it was written when
`run` had no scheduler slot to fork from), and its rows go
to stderr, which reaches `dmesg`. The `machine:` line names the CPU and
the hypervisor -- a TCG cycle count means little. Named by no runner: it
produces a comparison, not a verdict.
