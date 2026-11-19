# SMP: more than one core, staged

**Status: STAGE 1 IS BUILT (2026-08-30); STAGE 2 IS BUILT IN FULL (the
Local APIC 2026-08-30, its TIMER 2026-09-06, the I/O APIC and the MADT
interrupt source overrides 2026-09-10); STAGE 3 IS BUILT (2026-11-19:
the APs started and parked); STAGES 4-7 ARE DESIGNED, NOT BUILT;
RE-MEASURED AND RE-SIZED 2026-11-19.** The ACPI table walk and the MADT's processor list exist
(`kernel/acpi/`, read by `/bin/acpi` and `/bin/lscpu`), the Local APIC is
enabled and carries MSI-X for seven drivers, the tick runs on the LAPIC
timer through `kernel/clockevent.h`, and `kernel/arch/x86_64/ioapic.c`
routes the legacy lines with the MADT's overrides applied -- so nothing
goes through the 8259 any more. Stage 3's `kernel/arch/x86_64/smp.c`
starts every enabled core and parks it, and GS.base is the per-CPU
pointer on every entry; what remains is stage 4 onward -- nothing runs
on a second core yet.
The document is meant to be executed in order, each stage shippable and
testable on its own, with the honest case against at the end.

**Stage 1 landed for a different reason than SMP**, which is worth
knowing before reading it as progress: shutdown needed the FADT, so the
table walk had to exist, and parsing the MADT beside it cost one file.
That is the whole of it. The hard part of this document begins at
stage 2.

**Read this before touching `kernel/arch/x86_64/irq.c`, the scheduler's
`current_index`, or anything that adds a module-level buffer to a
subsystem a syscall can reach.**

**The one-sentence version:** SaunaOS should discover its other cores
through ACPI, start them, give each one its own GDT/TSS/stack and a
per-CPU pointer to what it is running, and let them all run user code in
parallel behind a SINGLE kernel lock -- then split that lock subsystem
by subsystem, in the order the measurements below give.

## What has just landed, and why it matters here

Threads (`docs/conventions/kernel.md`, "A THREAD IS A SLOT WHOSE `tgid`
NAMES SOMEBODY ELSE") are the prerequisite this project already paid
for. Not because SMP needs threads -- it does not -- but because the
scheduler entity is now the right shape: a slot is a schedulable thing
with its own kernel stack, FP state and trapframe, and a process is a
group of them sharing an address space. That is exactly the object a
per-CPU run queue holds.

It also creates the first real reason to want SMP in this system: two
threads of one program can now be runnable at the same instant, and on
one core that is a time slice each.

And it creates the first TLB shootdown hazard. Before threads, one
address space was live on one CPU by construction; now two cores can be
in the same PML4, so an unmap on one must be seen by the other. See
stage 6.

## What real systems do

**Linux 2.0 (1996) shipped SMP behind a single Big Kernel Lock.** One
CPU inside the kernel at a time; user code ran in parallel, everything
else serialised. It worked, it scaled badly, and removing it took
fifteen years -- the last `lock_kernel()` came out in 2.6.39 (2011).
The interesting part is not that they regretted it: it is that the BKL
was what made SMP *shippable* before the locking audit was done, and the
split then happened subsystem by subsystem against real measurements
rather than guesses.

**Windows NT was SMP from 3.1 (1993)**, designed rather than retrofitted:
per-CPU `KPRCB`, IRQL as a per-CPU interrupt-masking level, spinlocks
with a defined acquisition order, and a dispatcher lock. NT never had a
BKL because it never had a uniprocessor kernel to convert.

**Both discover CPUs the same way** -- the ACPI MADT's Local APIC
entries -- and start them the same way, with the INIT-SIPI-SIPI sequence
into a real-mode trampoline. Linux's is `arch/x86/realmode/rm/trampoline_64.S`.
There is no alternative shape worth considering here; this part is
architecture, not design.

**Where SaunaOS sits:** structurally it is pre-2.0 Linux. A preemptive
uniprocessor kernel with no spinlock primitive, whose only mutual
exclusion is "turn preemption off". So the BKL path is not an
imitation of a mistake -- it is the same position Linux was in, and the
same move is available.

## What is single-core here today, measured

Not a survey of everything that could be wrong. **Re-counted in the tree
on 2026-11-19** (first counted 2026-08-26); "sites" excludes `*_test.c`.
The audit has roughly sextupled since the first count -- threads, fork,
futexes, swap, sandboxes and the image cache all arrived single-core.

| | today | why SMP breaks it |
|---|---|---|
| GDT, TSS, `kernel_stack0` | one of each (`kernel/arch/x86_64/gdt.c`) | `TSS.RSP0` is per CPU: two cores sharing one would land two ring-3 traps on one stack |
| SYSCALL entry | DONE (stage 3) -- `syscall_entry.asm` takes RSP0 and the user-RSP scratch word from this core's `struct cpu` through `gs:` | each AP writes its own `STAR`/`LSTAR`/`FMASK`/`EFER.SCE` (`idt_load_ap()`) |
| GS.base | DONE (stage 3) -- this core's `struct cpu` (`percpu.h`), traded by `swapgs` on every entry; ring 3's is always 0 | the paranoid entry (NMI, #DB, #BP, #DF, #MC) tells whose GS is live from that 0; see stage 3's trap |
| IDT | one, loaded once | shareable as-is -- the IDT is read-only to the CPU. Each core must still `lidt` it |
| interrupt controller | DONE -- Local APIC + I/O APIC, MADT overrides applied (stage 2) | each AP enables its own LAPIC; IPIs are new code on the existing `lapic.c` |
| timer | DONE for the BSP -- LAPIC timer behind `kernel/clockevent.h`, whose device struct already has `per_cpu` | each AP needs its own instance, calibrated or copied from the BSP's |
| clocksource | the TSC, used only when CPUID says INVARIANT (`clocksource_tsc.c`) | invariant is necessary, not sufficient: TSCs across cores must also be checked for SYNC (Linux's `tsc_sync`), or time steps backwards on a migration |
| "what is running" | `current_index` (7 non-test files, all in `kernel/proc/` and `api/scheduler.h`), `rotation_pos` (14 refs), `g_next_kernel_rsp` (`kernel/proc/`, `arch/x86_64/idt.c`, `kernel/idt.h`, `drivers/ata.c`; `isr.asm` now takes its resume RSP back in `rax` from `isr_dispatch` and names it only in a comment), `kernel_saved_rsp` + `ROT_KERNEL` (the kernel's own rotation slot) | each has to become per CPU. Mostly in `kernel/proc/`; `g_next_kernel_rsp` reaches `idt.c` and `ata.c` too |
| run queue | the process table, sized from RAM, scanned by `find_next_runnable()` (`kernel/proc/scheduler.c`) | correct under a lock; a per-core queue is a later optimisation, not a correctness fix |
| preemption guard | `scheduler_preempt_disable()`: **73 sites in 24 files** (was 11 in 4) -- heap, klog, pipes, futex, sockets, tcp, shm, imgcache, sandbox, oom, vga, win_surface, usb, pci, tty, flock, module, kslots | turning preemption off on ONE core stops nothing on another. Every one is a critical section that needs a real lock |
| interrupts off | `irq_save()`: **93 sites in 25 files**, not counted in 2026-08 | the same hole: `cli` excludes this core's IRQs only. These are the `spin_lock_irqsave` candidates |
| sleeping locks | `kmutex` (`kernel/proc/kmutex.c`) and futexes exist; `kmutex`'s owner/depth/waiter state is guarded by `irq_save()` alone | its own state needs a spinlock inside, as Linux's `mutex` has `wait_lock` |
| filesystem | one sleeping lock per MOUNT (`docs/fslock-design.md`), a `kmutex`, so two volumes are independent; inside one volume tfs3 is serialised by that lock | the right SHAPE already, but only as safe as `kmutex` (the row above): it is fixed when `kmutex` gains its inner spinlock, with the 13 other non-test files that use one (block, ata, ahci, nvme, usb_storage, userfs, mmap, query...) |
| kernel heap | one free list + bins (`kernel/lib/heap_core.c`), guarded through ONE hook pair, `heap_os_lock()`/`heap_os_unlock()` in `kernel/mm/heap_os.c` | the hook is the whole split: stage 7's first item is a spinlock in those two lines |
| physical frames | `kernel/mm/pmm.c`'s bitmap, zone counters and frame refcounts, guarded by `irq_save()` (8 sites, inside the 93 above) | the hottest allocator once several cores fault pages in and break COW at once: a double-handed frame puts one process's page in another's space. Its own spinlock, early in stage 7 -- or in stage 5 if the soak shows it first |
| TLB invalidation | user mappings: `kernel/mm/vmm.c` (4 `invlpg`s and `flush_if_live()`, which tests only THIS core's CR3). Kernel mappings, which every core shares: `kernel/arch/x86_64/paging.c`, 9 CR3-reload `flush_tlb()` sites reached at runtime by module load/unload (`paging_set_kernel_exec()`), device mapping at driver bind (ahci, nvme, hda, xhci, virtio_pci, bochs, intel_display), write-combining retypes, and kernel-stack guard pages (`kstack.c`) | two places, not one: user shootdown needs a per-address-space record of which cores have it loaded (Linux's `mm_cpumask`); kernel changes go to EVERY core |
| FPU | eager `fxsave`/`fxrstor` per switch (`kernel/arch/x86_64/fpu.c`) | per-thread already; each AP must run the CR0/CR4 FPU setup |
| per-core CPU setup | done once, on the BSP: EFER.NXE (`boot.asm`), IA32_PAT (`pat_install_wc_slot()`), the variable MTRRs (`mtrr_apply()`), CR4.SMEP/SMAP (`paging_enable_smep_smap()`), all in `kernel/arch/x86_64/` | each is a per-core register. An AP without NXE takes a reserved-bit fault on its first NX kernel page (W^X marks all kernel data NX); a different PAT/MTRR gives the WC framebuffer conflicting memory types (undefined per the SDM); no SMEP/SMAP silently drops the protection. Each AP replays the BSP's values |
| kernel debugger | clears CR0.WP to plant a breakpoint (`kernel/debug/`) | must stop the other cores first (stage 5; parked cores run nothing) |
| console | `vga.c`'s cursor and back buffer (under the preemption guard) | interleaved output, and a torn present |
| ACPI | DONE -- tables and the MADT (`kernel/acpi/`) | -- |

**The RSDP is already reachable**, which is what made stage 1 cheap:
GRUB passes it in multiboot2 tags 14/15, and `kernel/core/multiboot.c`
already walked that list for the framebuffer, the command line, the
memory map and the modules. Finding the MADT needs no AML interpreter --
the tables that matter here are fixed-layout structures, and the
interpreter is only needed for the parts of ACPI this project has said
it does not want. That prediction held: `kernel/acpi/` is four files
and no interpreter.

## The staging

Each stage boots and is testable on its own. Stages 1-3 change no
behaviour at all -- they add facts and then add cores that do nothing --
which is what makes them safe to land separately.

**Sized 2026-11-19, against the table above** (a session is one
working day of this project's usual size):

| Stage | Size | Proven by | What a person sees |
|---|---|---|---|
| 3 -- APs up, parked | 1 session | TCG `-smp 4` + the ASUS: `lscpu` online count | `lscpu` / Task Manager list the cores; nothing else |
| 4 -- ticket lock + BKL | 1-2 sessions | a KTEST with the APs contending for a ticket lock; the BKL taken on every entry, with its recursion assertion, on the BSP | nothing -- the APs are still parked |
| 5 -- scheduler per core | 1-2 sessions | KVM soak with a positive control (drop the BKL on one path, soak goes red) + the ring-3 suite under `-smp`; the ASUS | per-core load in Task Manager; a two-thread program uses two cores |
| 6 -- TLB shootdown | 1-2 sessions (user and kernel mappings) | a KTEST: thread on core B reads a page core A unmapped, must fault | nothing (it must land WITH stage 5) |
| 7 -- split the lock | open-ended, per measurement | per split: soak + its own positive control | `make -j` and the compositor stop queueing on the BKL |

**Stages 5 and 6 ship together** -- stage 5 is the first stage that runs
anything on an AP, and alone it lets two threads of one program run on
two cores with no shootdown, which is a use-after-free of
a page frame, not a slowdown.

### Stage 1 -- ACPI tables, read-only -- DONE 2026-08-30

Find the RSDP from the multiboot2 tag, validate its checksum, walk the
XSDT (or RSDT on an older table), and expose the MADT. Nothing acts on
it.

**What shipped**, and where it differs from the sketch above. The RSDP
is found from tag 15, then tag 14, then by scanning the EBDA and the
BIOS ROM area -- the scan was not planned and exists because "no tag"
and "no ACPI" are otherwise the same answer and want opposite responses.
It is a fact and a command as intended, but three classes rather than
one: `QUERY_CPUS` for the processors, plus `QUERY_ACPI` and
`QUERY_ACPI_TABLE`, because poweroff needed the FADT read anyway and the
numbers it depends on had to be visible when a shutdown fails. The
command is `/bin/acpi`, not `cpuinfo`; `lscpu` grew the one line that
counts cores, since CPUID describes only the core executing it.

The FADT came in too, which the note below said to avoid. That was not
scope creep in this direction -- shutdown needed it (`docs/decisions.md`,
"ACPI stops at the tables"), and the table walk it needed is this stage.
**HPET is still untouched**: its table is listed now and nothing reads
it, which is exactly where that roadmap item wanted to start.

**What this must NOT become:** an ACPI subsystem. AML is not wanted at
all -- the DSDT's `_S5_` byte scan is the single, argued exception, and
anything requiring AML to be EVALUATED (battery, thermal, S3, GPEs) is
out. Parse fixed-layout tables and stop.

**Two things stage 2 inherits.** The MADT walk already reads the type-5
Local APIC address override, so `acpi_get_state()->lapic_phys` is the
right address rather than the 32-bit field's. It does NOT yet read the
type-2 interrupt source overrides, which stage 2's own trap note is
about -- that is a loop body in `acpi_madt_init()`, not new machinery.

### Stage 2 -- the Local APIC, still one core -- DONE 2026-09-10

Enable the BSP's LAPIC, move the timer off the PIT and onto the LAPIC
timer, and keep the PIC path working for a machine that reports no APIC.
Route the legacy IRQs through the I/O APIC using the MADT's interrupt
source overrides.

**The LAPIC itself landed 2026-08-30** and brought MSI/MSI-X with it,
which this section did not anticipate as part of the stage -- it framed
MSI-X as a later beneficiary. **The TIMER landed 2026-09-06**, behind a
`clockevent` registry rather than the bare switch sketched here: two
implementations exist (`pit`, `lapic-timer`), the higher-rated one takes
the tick at boot, and `nomsi` or a CPU with no APIC keeps the PIT. That
registry is the shape stage 3 wants, since a per-CPU timer is one
clockevent per core. The calibration is against the boot clock -- one
10 ms window on the PM timer's nanoseconds, or PIT ticks where there is
none -- which is why it happens in `kernel_main()` and not in
`lapic_init()`: the PIT path needs interrupts already on.

**The I/O APIC landed 2026-09-10 and this stage is DONE.**
`acpi_madt_init()` records the type-1 controllers and every type-2
override, `kernel/arch/x86_64/ioapic.c` programs the redirection table
and `irq.c` migrates every unmasked line off the 8259 (virtual wire
mode retired, LINT0 closed), and PCI INTx pins are routed by the
firmware's `_PRT` (`kernel/acpi/acpi_prt.c`) -- see
`docs/conventions/kernel.md` and `docs/decisions/drivers.md`.

This is worth landing alone even if SMP stops here: it is what MSI-X
needs (`docs/roadmap.md`'s Local APIC item), it gives more than 16
vectors, and a per-core timer is the prerequisite for anything tickless.

**The trap this stage carries:** an interrupt source override says the
ISA IRQ number is not the GSI. Ignoring them works on QEMU's default
machine and fails on real hardware and on some QEMU machine types --
which is exactly the class `tools/runners/qemu_matrix.py` exists to catch.

### Stage 3 -- application processors, parked -- DONE 2026-11-19

**Built as below, with four differences** (`kernel/arch/x86_64/smp.c`,
`smp_trampoline.asm`, `kernel/include/kernel/percpu.h`):

- **No per-core clockevent yet.** Each AP's LAPIC is enabled with every
  local vector masked, its timer included; registering one LAPIC timer
  per core belongs to stage 5, the first stage with a tick to deliver
  there.
- **The kdebug roundup moved to stage 5** (`docs/kdebug-design.md`): a
  parked core sits in `cli; hlt` and executes nothing while the
  debugger writes a breakpoint, so there is nothing to stop yet.
- **The paranoid entry covers #BP too** -- a kdebug breakpoint can sit
  in an entry window -- and the ordinary entry also swaps for a fault
  ON `isr_resume_frame`'s `iretq`, Linux's `error_entry` check.
- **The memory types are copied once.** A WC change after bring-up
  reaches the BSP alone; harmless while the APs are parked, and a
  roadmap item before stage 5 (`docs/roadmap.md`, SMP).

**The trap it found, now a convention** (`docs/conventions/kernel.md`,
"EVERY ROUTE TO RING 3 SWAPS GS"): an AP must touch no shared kernel
state before stage 4. The first version's AP logged a line; the racing
preemption count stayed raised and the boot locked up.


INIT-SIPI-SIPI each AP into a trampoline below 1 MiB that walks it from
real mode to long mode and into a C entry point. Each AP gets its own
GDT, its own TSS with its own `RSP0` stack, loads the shared IDT, enables
its LAPIC, and halts in a loop.

**Per-CPU state arrives here, and the mechanism is `swapgs` + GS.base.**
Each core's GS.base points at its own `struct cpu`, which holds at
minimum: the APIC id, the current thread, the per-CPU idle context, and
the depth of whatever lock it holds. This is what `current_index` becomes.
Linux's `this_cpu_ptr`, NT's `KPRCB`. `swapgs` on kernel entry and exit
is the part that must not be got wrong: it is why the kernel entry path,
not the scheduler, is the first thing to change in this stage.

`lscpu` now counts the APs as online, which is the whole test.

**The trap: `swapgs` only on an entry FROM RING 3.** `isr_common` is
entered from ring 0 too -- a fault or IRQ in kernel code, and an
NMI/#DB/#MC landing in the SYSCALL window before its own `swapgs`.
An unconditional `swapgs` there hands kernel code the USER GS, and the
next per-CPU access goes to 0 or user memory. Test the saved CS's RPL
on entry; NMI, #DB and #MC need Linux's "paranoid" entry (read
MSR_GS_BASE, do not trust the RPL), and the `swapgs` branch needs the
LFENCE CVE-2019-1125 added against speculating past it.

**The kernel debugger must stop the APs from here on.** `kernel/debug/`
clears CR0.WP to write a breakpoint and assumes nothing else runs while
it is stopped -- both true only on one CPU. An NMI IPI that parks every
other core before a stop (KGDB's `kgdb_roundup_cpus()`) lands with this
stage (`docs/kdebug-design.md`).

**What it touches (re-sized 2026-11-19):** new -- the real-mode
trampoline (NASM, copied below 1 MiB), the AP C entry, a `struct cpu`
array sized from the MADT, IPI send in `lapic.c`; changed --
`gdt.c` (a GDT+TSS+RSP0 stack per core), `syscall_entry.asm` and
`isr.asm` (`swapgs`, `syscall_user_rsp` into `struct cpu`), the
clockevent registration (one LAPIC timer per core, held stopped), the
kdebug roundup, and the AP's replay of the BSP's per-core CPU setup
(NXE, PAT, MTRRs, SMEP/SMAP, the FPU's CR0/CR4 -- the table's row).
GS.base is unused, so the per-CPU pointer displaces nothing, but every
entry path gains the ring check above. **No scheduler change**: APs halt, so `current_index` stays a
global this stage. One session; testable in TCG (`-smp 4`, `lscpu`
says four online) and on the ASUS (2 cores, 4 threads).

### Stage 4 -- a real spinlock, and the kernel lock

Two things, in this order.

**A spinlock primitive.** A ticket lock, not a test-and-set: a
test-and-set lock is unfair and can starve a waiter indefinitely, which
under a hypervisor turns into a several-millisecond stall. It must use
`cpu_relax()` in its spin (`kernel/include/kernel/barrier.h` already
says why, and the reason is the same: KVM's Pause-Loop Exiting). It must
also record the holder for a diagnostic, because the first SMP deadlock
is otherwise a machine that simply stops.

**One lock around the kernel.** Taken on every entry from ring 3 --
syscall, fault, interrupt -- and released on the way out. Once stage 5
lets the APs schedule, user code runs on every core in parallel and
kernel code does not. **In this stage the APs stay parked**: running
user code on them needs a per-CPU `current` (stage 5) and a shootdown
(stage 6). The lock is proven by a KTEST that wakes the APs to contend
for a ticket lock, and by the BKL's recursion assertion on the BSP.

The rule that makes this safe is the one it is easy to get wrong: **an
interrupt handler must take the same lock**, or an IRQ on core B walks
into a data structure core A is halfway through. That means interrupts
must be disabled while the lock is held on the same core (or the handler
deadlocks against itself), which is exactly the `spin_lock_irqsave`
discipline Linux and NT's IRQL both encode.

**Under the BKL the 73 preemption-guard and 93 `irq_save()` sites stay
correct** -- the lock makes kernel code single-threaded again -- so this
stage converts none of them. It must still change two things that are
not kernel-entry-shaped: `kmutex`'s internal state (a spinlock inside
-- which is what makes the per-mount fs lock and the other `kmutex`
users safe, not the BKL), and every sleeping path, which must DROP the BKL across the sleep and
retake it, as Linux 2.0's `schedule()` did. The lost-wakeup arguments in
`scheduler_block_current()` and `sys_waitpid()` are where that bites.

### Stage 5 -- the scheduler picks per core

`find_next_runnable()` becomes "find a runnable slot no other core is
running", and `switch_to()` writes the per-CPU `current` rather than a
global. One global run queue under the kernel lock is the right first
version: a per-core queue is a scalability optimisation and it brings
load balancing, work stealing and affinity with it -- three problems
this system does not have yet.

**The kernel's own rotation participant (`ROT_KERNEL`) is the awkward
part.** It exists because the kernel context is a schedulable thing here.
With N cores it becomes the per-core idle loop, which is what
`scheduler_idle()` already is -- but "the kernel context" as a single
saved trapframe (`kernel_saved_rsp`) is a uniprocessor idea and has to
become per CPU.

### Stage 6 -- TLB shootdown

Once two cores can be in one address space -- which threads make
routine -- unmapping a page needs every core that has it cached to
invalidate it. An IPI to the cores running that address space, and a
wait for their acknowledgement, before the frame is freed.

**User mappings** -- the paths that change a live one, all in
`kernel/mm/vmm.c` (counted 2026-11-19): `vmm_unmap_user_page()` (window revocation, `munmap`
in `mm/mmap.c`, the sound driver's shared buffer),
`vmm_release_user_page()`, `vmm_set_swap_entry()` (swap-out), and
`vmm_protect_user_page()` behind `mprotect()` and fork's COW downgrade
-- plus the poisoning of a dead client's mapping. Teardown is safe by
construction (the address space has no runners left); the rest are not.
Every one ends in `vmm.c`'s own `invlpg` or its `flush_if_live()`, so
the IPI goes in those few lines rather than at every caller. But
**`flush_if_live()` cannot say WHICH cores**: it compares the space
against this core's CR3 only (the "needs an IPI here" comment is at
`vmm_protect_user_page()`'s call). The address space needs a set of the
cores that have it loaded, set and cleared by `switch_to()` -- Linux's
`mm_cpumask` -- and the shootdown targets that set.

**Kernel mappings** -- shared by every core, so a change goes to ALL of
them. `kernel/arch/x86_64/paging.c` reloads CR3 (`flush_tlb()`) after
nine kinds of change; the ones reached after boot are module load and
unload (`paging_set_kernel_exec()` from `core/module.c` -- an unloaded
module's text left executable on another core is a W^X hole), a
driver's `paging_map_device()` at bind, a write-combining retype, and
`kstack.c`'s guard page (`paging_unmap_kernel_page()`). Linux's
`flush_tlb_kernel_range()` is the shape. This is an audit of
`paging.c`, not of `vmm.c`'s callers.

### Stage 7 -- split the lock, in measured order

The order comes from the table above, and each split is its own change
with its own test:

1. **The kernel heap** -- one lock inside `heap_core.c`. Every subsystem
   allocates, so this is the most contended thing in the kernel.
2. **The filesystem** -- and this one is not a lock, it is the scratch
   buffers. `tfs3.c`'s module-level state is what
   `scheduler_preempt_disable()` was papering over; the honest fix is
   per-call state, and a lock over the backend is the interim.
3. **The console** -- a lock around `vga.c`'s present and cursor.
4. **The scheduler's own table**, which is what lets a core reschedule
   without holding the kernel lock.
5. Everything else, when something measures it.

**Do not skip the measurement.** The BKL is not a bug to be ashamed of;
splitting a lock nothing contends for adds a race surface and buys
nothing.

## Testing, and the part that is genuinely hard

**TCG no longer serialises an x86-64 guest on an x86-64 host.**
Corrected 2026-11-19: on the dev host's QEMU 11.1, `-accel tcg -smp 4`
runs four host threads (`CPU 0/TCG` .. `CPU 3/TCG`, seen with
`-name x,debug-threads=on`), and only `-accel tcg,thread=single` puts
them on one (`ALL CPUs/TCG`). The earlier text said every `-smp` TCG
run was round-robin in one thread -- that was QEMU's old default and
is not this host's. So a missing lock CAN go red under TCG; it is still
far slower than hardware and its timing is nothing like a real core's,
so a race that needs a tight window is rarer there. `thread=single` is
a useful CONTROL: a failure that disappears with it is a race.
(`kernel/include/kernel/barrier.h` and `docs/fslock-design.md` say
"TCG serialises" about today's ONE-vCPU guests, where it is true in
effect.) Older QEMUs in `tools/runners/qemu_matrix.py` may differ --
check a version before trusting it with `-smp`.

The SMP suite is still a KVM suite FIRST, because KVM runs the cores at
speed. `tools/tests/manual/kvm_soak.py` is the existing shape to extend,
and the stages have very different testability:

- Stages 1-3 are testable under TCG, because "how many cores are online"
  is a fact, not a race. `lscpu` counting four online cores under
  `-smp 4` is a real check.
- Stages 4-7 are meaningfully testable under KVM with `-smp` (and,
  more weakly, multi-threaded TCG), under load, for minutes -- and the honest statement is that a soak test
  finding nothing is weak evidence. The strong evidence is a lock
  discipline that is stated and checked by inspection, plus an assertion
  in the lock itself that catches a recursive acquire.
- **A positive control is mandatory here and unusually easy**: remove
  one lock and the soak must fail. If it does not, the soak is not
  exercising the path.

## The honest case against

**One core is enough for what this system does.** The desktop, the
compositor and every app in the tree fit inside a 100 Hz round-robin
with time to spare; the measured problems in `docs/bugs.md` are not
throughput problems. SMP would make nothing here faster that anybody
notices.

**It adds a bug class this project cannot test well.** Every existing
correctness argument in the kernel assumes one thread of control, and
some of them are load-bearing and subtle -- the lost-wakeup arguments in
`scheduler_block_current()` and `sys_waitpid()` are correct *because*
interrupts are off and there is one core. Each of those becomes a
lock-ordering argument instead, and TCG cannot check any of them.

**The parts worth having are separable.** Stage 2 -- the Local APIC and
a per-core timer -- delivers MSI-X, more vectors and tickless idle
without a single line of locking. If the goal is "the machine gets
better", stage 2 is most of the value and none of the risk.

**What SMP is actually for here** is that it is a real operating system
milestone with a real design, and the locking audit it forces would find
things. That is a legitimate reason to build it. It is not a performance
argument, and it should not be sold as one.

## Out of scope, deliberately

NUMA. CPU hotplug. Scheduling domains, load balancing and work stealing.
CPU affinity as an API. Per-core memory allocators. `sched_ext`-style
pluggable policy -- and note the common misreading: Linux does not "let
you switch schedulers", it has compile-time classes.
