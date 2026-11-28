# AML: a namespace this kernel can walk, staged

**2026-11-28: STAGES 4a AND 4b ARE BUILT -- uACPI 7.0.0 VENDORED AND
LOADED, AND THE EMBEDDED CONTROLLER SERVED, BEHIND `acpi=uacpi`.** The
power button needs ACPI mode, and on the ASUS a session in ACPI mode
needs two presses to start again even with S5 written the legacy way --
its embedded controller's events (`_Qxx`) and `_PTS` are Methods. The
maintainer picked vendoring uACPI over writing stage 4 or carrying
ACPICA (2026-11-10); "Stage 4" below is the plan, 4a-4c built, 4e's
batteries, AC adapter and lid built (2026-11-28), and 4d (retiring the old
paths) next. Everything up to it is history that
still describes the code until stage 4d retires it.

**Status: STAGES 0 AND 1 ARE BUILT (2026-08-31); STAGE 2 IS BUILT AS
FAR AS `_PRT` NEEDS IT (2026-09-10: integers, strings, buffers,
packages and NameStrings decoded and resolved, `kernel/acpi/aml_data.c`);
STAGE 3 IS DESIGNED, NOT BUILT.** The first consumer of stage 2 is PCI
interrupt routing (`kernel/acpi/acpi_prt.c`), which reads `_PRT` by its
measured shapes and evaluates nothing but one flag -- the line this
document draws holds; see `docs/decisions/drivers.md`. Two things the
walk got wrong were found by it: a `Scope` re-opens an existing object
rather than declaring a second one, and the namespace is built at boot
now, in `acpi_init()`. A table can be captured and checksum-verified
(`acpi --dump`, `tools/dev/acpi_dump.py`), and `kernel/acpi/aml.c` walks the
DSDT and SSDTs into a namespace of declarations -- 336 nodes and 53
devices on QEMU's DSDT, with nothing refused. Nothing is executed and no
value is decoded yet.

**This reverses a decision.** `docs/decisions.md` says ACPI here "stops
at the tables" and "must not grow into" a subsystem, with the `_S5_`
byte scan as the single deliberate exception. That was the right call
when the only question was how to power the machine off. It stopped
being sufficient on 2026-08-31, when a laptop needed its GPE wake set
and the only place that is written down is a `_PRW` object per device.
The reversal is the maintainer's, made knowingly; this document is what
makes it a plan rather than a drift.

**Read this before adding anything that parses AML**, and note the line
this draws: **a namespace walk is not an interpreter.** Stages 0-3 read
DECLARATIONS and never execute a Method. Stage 4 is where the real
interpreter begins -- a vendored one, uACPI, not this walk grown up.

**The one-sentence version:** parse the DSDT and the SSDTs into a tree
of named objects, skipping every Method body by its length, so that
constant Names like `_PRW` can be found where they actually live --
and stop there, because everything past that point needs execution.

## What real systems do

**Linux** carries ACPICA, Intel's reference implementation: a full AML
interpreter with a namespace, method execution, an operation-region
subsystem that talks to the embedded controller, and a garbage
collector. It is roughly 60,000 lines and it is a vendored upstream
project, not something Linux maintains itself. **Windows** does the same
thing with its own interpreter in `acpi.sys`. **FreeBSD** vendors ACPICA
too. Nobody hand-writes this twice.

The lesson is not "vendor ACPICA" -- it is far past what this OS wants,
and its portability layer alone is larger than `kernel/acpi/`. The
lesson is **where the natural seam is**: ACPICA itself separates the
namespace (`nsxxx.c` -- building and searching the tree) from the
interpreter (`exxx.c` -- executing opcodes). The namespace half is the
part with a small, closed grammar. That is the half worth having.

**What SaunaOS should copy: the split.** What it should not copy: the
size, the operation regions, or the ambition. See the honest case at
the end.

## What exists today

`kernel/acpi/acpi_power.c` has ~60 lines of AML reading, all of it in
`acpi_scan_s5()`:

- `aml_small_int()` -- ZeroOp/OneOp/BytePrefix/WordPrefix into a byte.
- A PkgLength decode: `p += 1 + ((aml[p] >> 6) & 3)`.
- A byte scan for `NameOp '_S5_' PackageOp`, with the `\_S5_` variant.

That is a scan, not a walk: it looks for one signature anywhere in the
table and never learns what scope it was in. It works because `_S5_` is
unique and at the root. It is the right shape for exactly one object.

## The staging

Each stage ships and is testable on its own.

### Stage 0 -- `acpi --dump <TABLE>`, and a fixture  *(BUILT)*

Hex-dump a named ACPI table from inside SaunaOS, the way `lsusb -D`
dumps a USB configuration. Same argument, and it is the same argument
that was made for USB descriptors earlier the same day: **a parser for
firmware data must be tested against real firmware data, and a
hand-written fixture only ever agrees with the parser written beside
it.**

The kernel already has every table mapped and `QUERY_ACPI_TABLE` already
lists them; this is a dump provider over the bytes, and a `--dump`
flag in `/bin/acpi`. A real DSDT is ~28 KB, so it needs a range
(`--dump DSDT --at 0x1200 --len 256`) rather than emitting all of it.

**Payoff on its own:** every future ACPI question stops costing a round
trip through a second operating system.

**And a measurement that changes what stage 3 needs: the laptop's DSDT
is 103,006 bytes.** At 512 bytes a command that is 201 commands typed by
hand, so TRANSFERRING it is not the plan. The plan is that the parser
runs ON that machine and reports what it found -- `acpi --prw` listing
the GPE numbers it derived. QEMU's 8,605-byte DSDT is the development
fixture for the WALK; the laptop is the oracle for the WAKE SET, and it
answers in one line instead of 100 KB of hex.

### Stage 1 -- the namespace walk, declarations only  *(BUILT)*

Parse a table's AML term list into a tree. The grammar that must be
understood:

- **NameString**: RootChar `\`, ParentPrefixChar `^`, DualNamePath `2E`,
  MultiNamePath `2F`, NullName `00`, and the bare 4-byte NameSeg.
- **Containers**, which open a scope and whose bodies are walked:
  `DefScope` (10), `DefDevice` (5B 82), `DefPowerRes` (5B 84),
  `DefThermalZone` (5B 85), `DefProcessor` (5B 83, deprecated but
  present on older firmware).
- **`DefName` (08)**, which binds a name to a DataObject.
- **`DefMethod` (14)**, whose PkgLength is what lets its body be skipped
  entirely. That is the trick that keeps this a walk: a Method's body is
  opaque, and a parser that does not execute need not understand it.
- **The rest of what legally appears in a scope**, each of which needs
  its own length rule. **This is where the first draft of this document
  was WRONG**: it said "skipped by its PkgLength", and several of these
  do not have one. `DefOpRegion` (5B 80) is `NameString`, a space byte,
  then two TermArgs; `DefMutex` (5B 01), `DefEvent` (5B 02),
  `DefExternal` (15) and `DefAlias` (06) are a NameString plus a fixed
  trailer. Only the Field forms (5B 81/86/87) carry a PkgLength.
  Measured on QEMU's own DSDT, which has seven OperationRegions in its
  root scope, so this is not a corner case -- it is the second thing the
  parser meets.
- **Anything else: REFUSE.** Stop the walk and count it, rather than
  guess a length. The result is a partial namespace, which is a
  different answer from a wrong one.

The output is a tree of `(parent, 4-char segment, kind, offset)`. No
values are decoded at this stage beyond recording where the object's
data begins.

**The trap to write down now:** AML is UNTRUSTED INPUT, from firmware
this kernel did not write, parsed in ring 0. Every PkgLength, every
NameString and every offset is bounds-checked against the table's own
length, and a malformed table makes the walk STOP rather than guess --
the posture `ttf.c` takes for fonts and `usb_enum.c` for descriptors.
The failure mode must be "no namespace", never "a namespace built from
whatever followed".

**Testable on its own:** `acpi --namespace` prints the tree; a KTEST
walks a real DSDT captured in stage 0 and asserts the device count and
a few known paths.

### Stage 2 -- constant DataObjects

Decode the DataObject forms a `Name` can hold without execution:
integers (Zero/One/Byte/Word/DWord/QWord prefixes), Strings, Buffers
with constant lengths, and Packages of those. `aml_small_int()` grows
into this rather than being replaced.

**Not included:** anything whose value is an expression. `Name(X,
Add(1,2))` is legal AML and needs the interpreter; such an object is
recorded as PRESENT WITH AN UNREAD VALUE, which is a different answer
from absent and must not be conflated with it.

### Stage 3 -- `_PRW`, and the wake set

**MEASURED 2026-10-09, AND THE PREMISE BELOW DID NOT SURVIVE IT.** On the
machine it was for, all 17 `_PRW`s are Methods -- 12 `Return (GPRW (gpe,
state))`, 3 `Return (Package)`, 2 conditional -- so a reader of constant
Names finds none of them. And none wakes from S5, so the wake set this
stage would compute there is empty, which is what SaunaOS already does.
The two-press power button had another cause: entering ACPI mode
just before the S5 write, fixed by letting the firmware's legacy path
power the machine off (`docs/decisions.md`).
`tools/dev/aml_walk.py --prw` is the measurement. What follows is the
original plan, kept for the record.

**This stage is the actual fix, and there is now evidence that nothing
cheaper substitutes.** Three versions were measured on the machine that
needs it (`docs/decisions.md`): disable everything and it powers off but
takes two presses of the power button; restore the enables and it
reboots; rearm only the GPEs whose status stays clear and it ALSO
reboots, with nothing having re-latched. That last result is why this
stage is load-bearing -- **the waking source is not asserting when the
sleep is prepared**, so no measurement taken at one instant can find it.
Only a declared wake set can.

With stages 1 and 2, the wake set is a search: every Device with a
`_PRW` child, whose package's first element is an integer, contributes
that GPE bit. `acpi_poweroff()` then does what Linux does -- disable
all, clear all, enable only the wake set -- instead of restoring the
firmware's enables wholesale.

**This is the stage that pays for the project**, and it is worth being
honest that the payment is small. (This used to add that restoring the
firmware's enables had fixed the machine that prompted it. It had not:
restoring them made that machine reboot, as `docs/decisions.md`
records.) What stage 3 buys is the
removal of a residual risk (a non-wake GPE re-latching) and a wake set
that is *correct* rather than a superset.

The `_PRW` first element may also be a Package naming a GPE block
device and an index, on GPIO-signalled systems. Those are skipped and
COUNTED, so "we ignored three of them" is visible rather than silent.

### Stage 4 -- method execution, by vendoring uACPI (4a BUILT 2026-11-28)

Everything above stops at declarations. Battery state, thermal zones,
lid state, `_STA`, `_CRS`, `_PTS` and the embedded controller all need
Methods run: an operand stack, integer/string/buffer semantics, control
flow, mutexes, and OperationRegion handlers for SystemMemory, SystemIO,
PCI config and the EC.

**Nobody writes this twice, and SaunaOS will not either.** Linux, FreeBSD
and Haiku vendor ACPICA; Windows has its own engine in `acpi.sys`; hobby
kernels (Managarm, Ironclad and some twenty others) vendor **uACPI**.
Measured 2026-11-10 at upstream c0edece, vendored at release 7.0.0
(2026-11-28): MIT licence, ~25,800 lines of
C in `source/` and ~9,700 of headers, no floating point (the kernel is
`-mno-sse`), freestanding with its own overridable string functions,
thread-safe. Firmware is written against Windows' interpreter, not the
spec, so a hand-written one would spend years on quirks uACPI has already
met; ACPICA does the same job at several times the size with a larger
OS layer. **uACPI is chosen.**

**Where it lives: ring 0, like Linux's.** AML reads and writes I/O ports,
physical memory and PCI config space on the firmware's behalf, at
moments (an SCI, S5) a ring-3 process cannot own. `kernel/third_party/
uacpi/upstream/` is the first vendored code in the kernel and takes
userland's rule: NEVER EDITED IN PLACE, a change is a patch beside it,
re-fetched by a tool pinned to a commit and a hash
(`docs/conventions/build.md`). The glue is SaunaOS's own,
`kernel/acpi/uacpi_*.c`.

**What SaunaOS supplies (`uacpi/kernel_api.h`), mapped:**

| uACPI asks for | SaunaOS has (`kernel/acpi/uacpi_glue.c`) |
|---|---|
| `get_rsdp`, `map`/`unmap` | `acpi_state.rsdp_phys` (GRUB's copy, in the multiboot info `pmm_init()` reserves); under 4 GiB an address is its pointer, above it usable RAM's identity map (SPARSE there: whole 2 MiB slots of type-1 memory only) or else `paging_map_device()`, never unmapped |
| `io_map`/`io_read*`/`io_write*` | `inb`/`inw`/`inl` -- a handle is the base port, bit 32 set so port 0 is not NULL |
| `pci_device_open`, `pci_read*`/`write*` | a `struct pci_device` naming only the address, through `pci_config_read*`/`write*`; segment 0, the first 256 bytes (no ECAM) |
| `alloc`/`alloc_zeroed`/`free` | `kmalloc`/`kzalloc`/`kfree` (`UACPI_NATIVE_ALLOC_ZEROED`; `UACPI_SIZED_FREES` off) |
| `get_nanoseconds_since_boot`, `stall`, `sleep` | `clocksource_now_ns()`; a busy wait; a timed `scheduler_block_kernel_until()`, a busy wait where nothing can park |
| semaphores, `get_thread_id` | a count under a spinlock, waited on by address (park, or poll in the kernel context); the pid + 1 |
| spinlocks, `disable`/`restore_interrupts` | `spinlock.h`'s ticket lock (`spin_lock_irqsave`) and `irqflags.h` |
| `install_interrupt_handler` | **HELD, not installed**: `acpi_sci.c` keeps the SCI line and calls the held handler -- the global lock's release, from 4b the embedded controller's GPE, and from 4c the power button as uACPI's fixed event (whose press is sent on after the dispatch returns) |
| `schedule_work`, `wait_for_work_completion` | `kacpid`'s queue -- an item queued at most once; a waiter with no kacpid drains it itself |
| `log` | `klog`, the first 128 lines straight through and then one a second |
| `memcpy`/`memmove`/`memset`/`memcmp` | `k_mem*`, through `glue/uacpi_libc.h` (`UACPI_OVERRIDE_LIBC`): GCC's builtins would call a `memcpy()` the kernel deliberately does not have |

**The embedded controller is the host's, as in Linux's `drivers/acpi/
ec.c`**: an address-space handler for EmbeddedControl (read 0x80 /
write 0x81 through the ECDT's or the `_CRS`'s two ports) and a GPE
handler that issues QR_EC (0x84) and runs the `_Qxx` it names --
`kernel/acpi/uacpi_ec.c` on uACPI, `ec_proto.c` the byte protocol alone
(host-checked against a simulated EC, `tools/hostchecks/ec_hostcheck.py`).

**The stages, each shippable and each with a boot word to step back:**

- **4a -- vendored and loaded, nothing acting on it. BUILT 2026-11-28.**
  `tools/fetch/uacpi_vendor.py` copies release 7.0.0 (SHA-256 pinned)
  into `kernel/third_party/uacpi/upstream/`; behind `acpi=uacpi` (off by
  default) `kacpid` runs `uacpi_initialize()` + namespace load +
  `_STA`/`_INI`; `acpi` prints the interpreter's line; suite `uacpi`
  checks it against the byte scan and the walk. Measured: i440fx 354
  nodes / 54 devices in 20-34 ms, q35 and OVMF 267 / 37 in 27-28 ms, no
  AML errors; the power button still powers the guest off. **The ASUS:
  4339 nodes / 202 devices in 43 ms** (2026-11-28), no AML errors, and
  `kacpid` did not stall on the global lock. The decisions it made:
  - **The interpreter starts in a kernel thread, `kacpid`, after the
    scheduler, and boot does not wait** (Linux: ACPICA's tables early,
    its interpreter at an initcall, deferred work on `kacpid`).
    `acpi_init()` runs before the heap, and the kernel context can park
    on nothing, while uACPI allocates on every step and AML may
    `Sleep()`. A consumer (4d's `_PRT`) waits on `ACPI_UACPI_READY`.
  - **SaunaOS keeps the mode**: `UACPI_FLAG_NO_ACPI_MODE`, since
    `acpi_sci.c` decides ACPI vs legacy (the ASUS stays legacy).
  - **`_OSI` is uACPI's default table**: yes to "Windows 2000" through
    "Windows 2022" (Windows 11 22H2), no to "Linux", the host-feature
    strings off -- Linux's answer.
  - **No `uacpi_finalize_gpe_initialization()`**: no GPE is enabled
    (from 4b, the embedded controller's alone).
  - **The suite's own instance runs only under a hypervisor**: on real
    firmware with `acpi=uacpi` off, `ktest` skips rather than run that
    machine's `_INI`/`_REG` the boot never asked for.
- **What building 4a found:**
  - **uACPI's event set-up DISABLES EVERY FIXED EVENT**, the power
    button's with it, while `acpi_sci.c` still owns the SCI. Its start
    and its teardown call `acpi_sci_rearm()`. Measured: without it the
    smoke test's power button does nothing under `acpi=uacpi`.
  - **...and ENABLES ONE, the global lock's** (`GBL_EN`), whose release
    notification only uACPI's SCI handler would clear or answer: a
    `GBL_STS` left set holds the level-triggered SCI asserted, and a
    contended lock waits for ever. So `acpi_sci.c`'s handler passes the
    interrupt on to the held one. **Where no SCI is routed** -- legacy
    mode, the ASUS -- a lock contended with the firmware still waits for
    a release nothing delivers: `kacpid` would stay `loading`. 4b's to
    measure there.
  - **`aml.c` keeps only a name's LAST segment**, so QEMU's `Device
    (PCI0.PRES)` inside `Scope (\_SB)` is recorded as `\_SB.PRES` --
    where uACPI rightly has `\_SB.PCI0.PRES`. The walk is being retired
    (4d), so the KTEST looks such a device up beneath its scope.
  - **PCI config access had no lock** (`pci.c`'s address/data pair), so
    an AML access from `kacpid` could interleave with a driver's on
    another core -- harmless until 4b's `_Qxx` ran AML after boot. It has
    one now, Linux's `pci_lock`, a read-modify-write held for its span.
  - **A kernel thread runs on core 0 with interrupts off**, never
    preempted (which suits uACPI's wish for GPE work on CPU 0), so each
    step holds that core: `kacpid` yields between steps, but an AML busy
    loop inside one holds it up to uACPI's 30-second loop timeout, and
    with one core the rest of boot waits for each step. 4b's, where work
    first runs after boot.
- **4b -- the embedded controller. BUILT 2026-11-28.** The address-space
  and query handlers above, in `uacpi_ec.c`. Measured on the ASUS: the
  ECDT's EC (`\_SB.PCI0.LPCB.EC0`, ports 0x66/0x62, GPE 0x0A); its
  `_REG` sets `ECAV`, which 46 of its methods test; the lid, the
  brightness keys (`_Q0E`/`_Q0F`) and others arrive -- 67 events of 6
  kinds in one run, 32 of 5 in another, no timeout, no storm, the
  machine responsive. The decisions it made:
  - **`acpi=uacpi` enters ACPI mode on an EC machine.** The exception in
    `acpi_sci.c` existed because nothing could run the EC's methods; with
    the interpreter serving them it is lifted (Linux's shape). In legacy
    mode the EC's GPE is the firmware's SMI handler's, and no SCI arrives
    anyway, so events are off there. **And it is handed back** when the
    promise fails -- the interpreter fails or cannot start, or the EC's
    events cannot be turned on: `acpi_sci_ec_unserved()` leaves ACPI mode
    (the line left unmasked, as it may be shared) and the machine is as
    if the word were absent.
  - **Each wait spins 1 ms, then parks 1 ms between polls, failing at 500
    ms** -- Linux's polling mode (a 550 us spin, then sleeps). The first
    build's 1 ms bound measured a 934 us byte on the ASUS, then a 2027 us
    one, which the nap path took. The EC's lock is a `kmutex`: held
    across a nap, and `kacpid` is the only thing running AML.
  - **The handler goes on the ROOT when the ECDT names the EC**, before
    `_INI` runs, so AML outside the EC device reaches it early (Linux's
    `acpi_ec_ecdt_probe()`); found by PNP0C09 instead, on the device.
  - **`_Qxx` runs on `kacpid`**; the GPE handler only reads the status
    port and queues the work, and a pass that ends with SCI_EVT still set
    queues the next itself (the edge was taken at dispatch). Over 100
    GPEs in a second -- counted per GPE, so one firing with no event
    behind it counts -- leaves it off for the boot (`acpi` says so).
- **What building 4b found:**
  - **The brightness keys' `_Q0E`/`_Q0F` run, and the screen does not
    change**: they end in a `Notify()` nothing listens to yet -- the
    roadmap's item, beside 4e.
  - **uACPI's `<uacpi/acpi.h>` names its structs as `kernel/acpi.h`
    does** (`struct acpi_gas`), so glue that needs a table reads it
    through the kernel's `acpi_find_table()`.
  - **The ASUS raises `_QB2` at boot, which its DSDT has no method
    for** -- logged and skipped, as Linux does.
- **4c -- power through uACPI.** `uacpi_prepare_for_sleep_state(S5)`
  (`_PTS`) then `uacpi_enter_sleep_state(S5)` replace `acpi_poweroff()`'s
  ladder; the fixed power button through uACPI's fixed-event handler
  replaces `acpi_sci.c`'s; ACPI mode stays on, with no hand-back.
  **ACCEPTANCE: the ASUS, ACPI mode on, power button armed, starting
  again on ONE press** -- after a Start-menu shutdown and after a button
  press. Then `acpi=uacpi` becomes the default and the EC-legacy
  exception goes.
- **What building 4c found (BUILT 2026-11-28, 36792c5a):**
  - **The acceptance passed and the control did not discriminate**: the
    ASUS started on one press after both shutdowns, and ALSO with `_PTS`
    skipped, with no interpreter at all, and with no interpreter and no
TCO watchdog driver (`docs/decisions/kernel.md`,
    the embedded-controller entry) -- the two-press start did not
    reproduce on this build. The ASUS boots `acpi=uacpi` by default for
    daily use; the project default flips after a few clean days.
  - **The ladder stays the fallback** for a namespace not READY, no
    `\_S5`, an `acpimode=` experiment, a sleep write that returned, a
    failed `_PTS` when the byte scan has `_S5_` -- and for the WRITE on
    hardware-reduced firmware, whose uACPI path polls `WAK_STS` with no
    bound and interrupts off.
  - **uACPI's fixed-event handler runs under the SCI's lock**, so the
    press is noted there and SIGPWR sent after the dispatch returns.
- **4d -- retire what it replaces.** `_PRT` routing (`acpi_prt.c`) and
  the `_S5_` scan move onto uACPI -- but PCI drivers probe at `INIT_BUS`,
  before `kacpid` can exist, so boot-time routing needs `acpi_prt.c`'s
  reader or a re-route once the namespace is ready (`docs/decisions/kernel.md`,
  "uACPI comes up in a kernel thread"); `aml.c`'s walk stays only if
  something still needs it without the interpreter (`acpi=off`).
- **4e -- what a laptop wants.** Lid (PNP0C0D), AC adapter (ACPI0003),
  battery (PNP0C0A, `_BIF`/`_BST`) as kernel facts the panel can read,
  thermal zones, and the GPE wake set from `_PRW` (stage 3, finally).
  Each its own roadmap item. **The first three BUILT 2026-11-28**
  (`uacpi_supply.c`, Linux's `battery.c`/`ac.c`/`button.c` in one file):
  found by HID once the namespace is up, read on kacpid -- `_BIX`, else
  `_BIF` (`battery_parse.c`, its KTEST suite `battery`), then `_BST`,
  `_PSR`, `_LID` -- re-read on each device's Notify(0x80/0x81) and every
  30 s while a battery is present (firmware rarely notifies a falling
  percentage, which is why UPower polls), `_BTP` armed at 3%. Energy is
  mWh whatever the firmware counts in, converted through the DESIGN
  voltage. Readers get copies under one spinlock (`QUERY_POWER`, its
  `generation`, and `QUERY_POWER_SUPPLY`). **An info package with every
  number unknown is NOT information**: the ASUS answers its default
  `NBIX` while its EC's battery-type byte (`EB0T`, an index-window field
  at ports 0x25A/0x25B, not the EC protocol) reads 0xFF, so it is asked
  again every 10 s for the first minute, then at the 30 s reading. The
  critical charge (3% on battery for 20 s, or the firmware's own critical
  bit) sends init SIGPWR from the kernel; `battery=noshutdown` turns that
  off. The desktop's half -- the tray item, the Battery page, the low
  notice, the lid's action (`desktop.lid`) -- is toywm's
  (`battery_popup.c`, `wm_lid.c`). **Thermal zones and `_PRW` BUILT the
  same day.** `uacpi_thermal.c` (Linux's `thermal.c`): every
  `ThermalZone`'s `_TMP`, `_CRT`, `_HOT`, `_PSV` read on its Notify and
  every `_TZP` or 5 s; the critical trip, read twice a second apart, sends
  init SIGPWR; `_PSV` with `_PSL`/`_TC1`/`_TC2` runs passive cooling
  (`thermal_policy.c`, KTEST suite `thermal`), a frequency ceiling on every
  core (`kernel/arch/x86_64/cputherm.c`, which also reads the CPU's own
  sensors, Linux's coretemp, KTEST `cputherm`). `uacpi_wake.c`: every
  `_PRW`'s GPE marked for wake (`acpi --wake`), the buttons and lid that
  wake from S5 armed, then `uacpi_finalize_gpe_initialization()` -- the
  firmware's `_Lxx`/`_Exx` run. Neither runs in the KTEST's inline
  instance. The desktop's half: Task Manager's Temperature device and the
  hot notices (`wm_thermal.c`, `desktop.hot_notices`, on by default). Why each is shaped so:
  `docs/decisions/kernel.md`, "A thermal zone is read on a timer...".

**Testing.** uACPI's own suite (`tests/run_tests.py`, AML test cases
against a host runner) as a host check, `tools/hostchecks/uacpi_hostcheck.py`
(built with gcc, no CMake; a SKIP without `iasl`); the kernel suite
`uacpi`, which on a default boot under a hypervisor brings up its own
instance and takes it down (`acpi_uacpi_bring_up_inline()`), so every
`make test` runs the interpreter -- its `_STA` check goes red when port
reads fail; QEMU's three DSDTs for boot;
the ASUS for 4b and 4c, by hand, because the property is about a power
button and a firmware's memory. The positive control for 4c: the same
build with `_PTS` skipped must bring the second press back.

**Testing 4e.** QEMU has no battery, AC adapter or lid, so
`tools/tests/fixtures/acpi/power.asl` -- compiled by `iasl`, appended with
`vm.py --acpi-table` -- gives it all three, and its `T*` methods move them
and Notify() as an EC query would, run through `acpi --eval` on a boot with
`acpieval` (`SYS_ACPI_EVAL`, Linux's `acpi_call`, closed by default).
`tools/tests/sweep/power_test.py` drives it end to end, critical shutdown
included; booted without the table every check after the boot goes red.
The parser has its own KTESTs (`battery`), red with the mAh conversion
removed.

**Testing the thermal zone and `_PRW`.** The same fixture carries
`\_TZ.TZ0` (passive 80 C, critical 100 C, `_TSP` one second; `_SB.TSTZ`
moves it and Notify()s), a lid whose `_PRW` wakes from S5 and an S3-only
device; `tools/tests/sweep/thermal_test.py` drives the kernel's reading,
`acpi --wake`, Task Manager, passive cooling, both notices and the
critical shutdown, and goes red without the table. In a guest the
frequency ceiling is never written (`cputherm.c` refuses under a
hypervisor), so the throttle was proven on the ASUS with `thermal.psv=50`
on a trial entry: ratio 8 and a 798 MHz busy clock at 63%, the firmware's
ratio back once the zone cooled.

## Testing

- **A real DSDT is the fixture**, captured with stage 0 from the machine
  that needs the feature. **Measured, once stage 0 existed: QEMU's q35
  DSDT is 8605 bytes with 53 `Device` declarations and ZERO `_PRW`
  objects.** So it tests stage 1 well and cannot test stage 3 at all --
  the wake set needs the laptop's tables, and that is a fact about the
  emulator rather than a gap in the plan.
- `tools/dev/acpi_dump.py` is the capture, and it verifies the ACPI checksum
  rather than trusting that the bytes arrived: asking for the whole
  table in one console command lost 48 of 8605 bytes, scattered.
- **The walk must be fuzzed against truncation**: for a real table,
  every prefix of it must either parse or stop cleanly, and none may
  read past the end. That is a cheap KTEST loop and it is the one that
  matters, because the input is firmware.
- **A positive control for stage 3**: with the wake set deliberately
  emptied, the machine that reported the bug must fail to stay off.

## The honest case against

*(Written before stage 4 was scheduled. Its last paragraph's prediction
held: `_PRW` landed nothing, and the next wall -- the embedded controller
-- needed execution. Kept because the attack-surface point still stands:
uACPI parses and RUNS firmware in ring 0, as Linux's ACPICA does.)*

**The payoff is thin and the surface is wide.** Stage 3 replaces a
working superset with a correct set, on a symptom already fixed. Every
line of stages 1-2 parses vendor firmware in ring 0, which is the same
class of attack surface `ttf.c` is, without `ttf.c`'s justification
(fonts are user-supplied and unavoidable; the DSDT is read once at
boot).

**And the next question after `_PRW` needs stage 4.** Battery, lid,
thermal -- the things a person actually wants from ACPI on a laptop --
are all Methods. So stages 1-3 do not open the door they look like they
open: they land one wake set and then stop at the same wall, one level
further in.

**The counter-argument, which is why this is scheduled anyway:** the
namespace is the half with a closed grammar and a bounded size, the
`_S5_` scan is already a worse version of it (a signature hunt with no
notion of scope), and a second such scan for `_PRW` would be the third
copy of a pattern that wants to be a parser. Building the small correct
thing once is cheaper than three scans that each work by luck.

## Out of scope, deliberately

- Vendoring ACPICA: uACPI does the same job at a hobby kernel's size
  (stage 4).
- ~~OperationRegion access of any kind, especially the embedded
  controller~~ -- reversed 2026-11-10 by stage 4: the EC handler is
  SaunaOS's own, as Linux's `ec.c` is.
- Writing anything back to the namespace -- from `aml.c`'s walk. Under
  stage 4 the namespace is uACPI's, with its own locks and lifetimes.
- `_OSI`, `_REV` and the OS-identification games firmware plays -- for
  the walk. Under stage 4 they matter (firmware branches on them), are
  uACPI's (`osi.c`), and which Windows versions SaunaOS claims was
  decided in 4a: uACPI's default table.
