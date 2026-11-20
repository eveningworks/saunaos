# SMP: more than one core, staged

**Status: STAGE 1 IS BUILT (2026-08-30); STAGE 2 IS BUILT IN FULL (the
Local APIC 2026-08-30, its TIMER 2026-09-06, the I/O APIC and the MADT
interrupt source overrides 2026-09-10); STAGE 3 IS BUILT (2026-11-19:
the APs started and parked); STAGE 4 IS BUILT (2026-11-19: a ticket
spinlock, the call IPI and the kernel lock); STAGE 6 IS BUILT AHEAD OF
STAGE 5 (2026-11-19: the TLB shootdown, `kernel/arch/x86_64/tlb.c`);
STAGE 5 IS BUILT AND ON BY DEFAULT (2026-11-19 behind `smp=run`; the
default since 2026-11-20, `smp=park` the way back); STAGE 7 IS
DESIGNED, NOT BUILT;
RE-MEASURED AND RE-SIZED 2026-11-19.** The ACPI table walk and the MADT's processor list exist
(`kernel/acpi/`, read by `/bin/acpi` and `/bin/lscpu`), the Local APIC is
enabled and carries MSI-X for seven drivers, the tick runs on the LAPIC
timer through `kernel/clockevent.h`, and `kernel/arch/x86_64/ioapic.c`
routes the legacy lines with the MADT's overrides applied -- so nothing
goes through the 8259 any more. Stage 3's `kernel/arch/x86_64/smp.c`
starts every enabled core and parks it, and GS.base is the per-CPU
pointer on every entry; stage 4's kernel lock is taken on every entry
and a parked core runs what `smp_call()` hands it; stage 5 puts
processes on every core (`smp=run`, the default). What remains is
stage 7, the lock split, and the "Not yet" list under stage 5.
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
| interrupt controller | DONE -- Local APIC + I/O APIC, MADT overrides applied (stage 2); a fixed IPI (`lapic_send_ipi()`) and the call vector 0xFB (stage 4) | -- |
| timer | DONE for the BSP -- LAPIC timer behind `kernel/clockevent.h`, whose device struct already has `per_cpu` | each AP needs its own instance, calibrated or copied from the BSP's |
| clocksource | the TSC, used only when CPUID says INVARIANT (`clocksource_tsc.c`) | invariant is necessary, not sufficient: TSCs across cores must also be checked for SYNC (Linux's `tsc_sync`), or time steps backwards on a migration |
| "what is running" | `current_index` (7 non-test files, all in `kernel/proc/` and `api/scheduler.h`), `rotation_pos` (14 refs), `g_next_kernel_rsp` (`kernel/proc/`, `arch/x86_64/idt.c`, `kernel/idt.h`, `drivers/ata.c`; `isr.asm` now takes its resume RSP back in `rax` from `isr_dispatch` and names it only in a comment), `kernel_saved_rsp` + `ROT_KERNEL` (the kernel's own rotation slot) | each has to become per CPU. Mostly in `kernel/proc/`; `g_next_kernel_rsp` reaches `idt.c` and `ata.c` too |
| run queue | the process table, sized from RAM, scanned by `find_next_runnable()` (`kernel/proc/scheduler.c`) | correct under a lock; a per-core queue is a later optimisation, not a correctness fix |
| preemption guard | `scheduler_preempt_disable()`: **73 sites in 24 files** (was 11 in 4) -- heap, klog, pipes, futex, sockets, tcp, shm, imgcache, sandbox, oom, vga, win_surface, usb, pci, tty, flock, module, kslots | turning preemption off on ONE core stops nothing on another. Every one is a critical section that needs a real lock |
| interrupts off | `irq_save()`: **93 sites in 25 files**, not counted in 2026-08 | the same hole: `cli` excludes this core's IRQs only. These are the `spin_lock_irqsave` candidates |
| sleeping locks | DONE (stage 4) -- `kmutex`'s owner/depth/handoff state is under its own `wait_lock` spinlock, taken irqsave; futexes are still `irq_save()` | -- |
| filesystem | one sleeping lock per MOUNT (`docs/fslock-design.md`), a `kmutex`, so two volumes are independent; inside one volume tfs3 is serialised by that lock | the right SHAPE already, but only as safe as `kmutex` (the row above): it is fixed when `kmutex` gains its inner spinlock, with the 13 other non-test files that use one (block, ata, ahci, nvme, usb_storage, userfs, mmap, query...) |
| kernel heap | one free list + bins (`kernel/lib/heap_core.c`), guarded through ONE hook pair, `heap_os_lock()`/`heap_os_unlock()` in `kernel/mm/heap_os.c` | the hook is the whole split: stage 7's first item is a spinlock in those two lines |
| physical frames | `kernel/mm/pmm.c`'s bitmap, zone counters and frame refcounts, guarded by `irq_save()` (8 sites, inside the 93 above) | the hottest allocator once several cores fault pages in and break COW at once: a double-handed frame puts one process's page in another's space. Its own spinlock, early in stage 7 -- or in stage 5 if the soak shows it first |
| TLB invalidation | user mappings: `kernel/mm/vmm.c` (4 `invlpg`s and `flush_if_live()`, which tests only THIS core's CR3). Kernel mappings, which every core shares: `kernel/arch/x86_64/paging.c`, 9 CR3-reload `flush_tlb()` sites reached at runtime by module load/unload (`paging_set_kernel_exec()`), device mapping at driver bind (ahci, nvme, hda, xhci, virtio_pci, bochs, intel_display), write-combining retypes, and kernel-stack guard pages (`kstack.c`) | two places, not one: user shootdown needs a per-address-space record of which cores have it loaded (Linux's `mm_cpumask`); kernel changes go to EVERY core |
| FPU | eager `fxsave`/`fxrstor` per switch (`kernel/arch/x86_64/fpu.c`) | per-thread already; each AP must run the CR0/CR4 FPU setup |
| per-core CPU setup | done once, on the BSP: EFER.NXE (`boot.asm`), IA32_PAT (`pat_install_wc_slot()`), the variable MTRRs (`mtrr_apply()`), CR4.SMEP/SMAP (`paging_enable_smep_smap()`), all in `kernel/arch/x86_64/` | each is a per-core register. An AP without NXE takes a reserved-bit fault on its first NX kernel page (W^X marks all kernel data NX); a different PAT/MTRR gives the WC framebuffer conflicting memory types (undefined per the SDM); no SMEP/SMAP silently drops the protection. Each AP replays the BSP's values |
| kernel debugger | clears CR0.WP to plant a breakpoint (`kernel/debug/`) | must stop the other cores first -- NOT DONE: a stub stop halts core 0 only ("Not yet" under stage 5) |
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
| 6 -- TLB shootdown (DONE, before 5) | 1 session | a KTEST: a parked core that cached a page reads the new frame after a remap | nothing -- no user target until stage 5 |
| 7 -- split the lock | open-ended, per measurement | per split: soak + its own positive control | `make -j` and the compositor stop queueing on the BKL |

**Stage 6 came first, so stage 5 never runs without it** -- stage 5 is
the first stage that runs anything on an AP, and alone it would let two
threads of one program run on two cores with no shootdown, which is a
use-after-free of a page frame, not a slowdown.

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

### Stage 4 -- a real spinlock, and the kernel lock -- DONE 2026-11-19

**Built as below** (`kernel/proc/spinlock.c`, `kernel/proc/bkl.c`,
`smp.c`'s `smp_call()`; KTESTs in `kernel/proc/spinlock_test.c`, in
the `smp` suite), **with these differences and additions:**

- **The kernel lock is OWNED BY A CORE AND RECURSIVE, Linux 2.0's
  `kernel_flag`, and its depth TRAVELS WITH THE CONTEXT.** The depth is
  in `struct cpu`; `switch_to()` saves the outgoing context's and loads
  the incoming one's, as it does `isr_depth` and the preemption depth.
  The core holds the lock across the switch, so "drop the BKL across
  every sleep" needed no code at the sleeping paths: a sleeping context
  holds nothing, and whoever resumes it is in the kernel. A fresh
  context starts at depth 1 through `isr_start_frame`, which drops it
  for a ring-3 frame (Linux's `ret_from_fork` shape).
- **So the lock passes to another core only where, on one core, another
  context could already run** -- a context preempted in the kernel holds
  nothing once the one switched to returns to ring 3. Kernel code that
  is safe against preemption on one core is safe under the lock on
  many, and the 73 preemption-guard and 93 `irq_save()` sections keep
  meaning what they mean. The first KTEST found this: an AP got the lock
  during a 50 ms wait because a tick switched the test away.
- **From ring 0, the paranoid vectors (NMI, #DB, #BP, #DF, #MC) do not
  take it**; from ring 3 every entry does (an `int3` can end in a signal
  and a switch). An NMI inside `take()` on the holding core would queue
  behind its own ticket, so those handlers may touch nothing shared --
  Linux's NMI rule; stage 5's kdebug roundup stops the other cores
  instead.
- **The parked cores take work through a call IPI** (vector 0xFB,
  Linux's `CALL_FUNCTION_VECTOR`): a mailbox in `struct cpu`, `sti; hlt`
  in the park loop, and `isr_dispatch()` sends an AP's interrupt to
  `smp_ap_interrupt()` before its first global. The call vector does
  not take the lock (its sender waits holding it). Stage 6's shootdown
  is the same shape on its own vector (0xFD), since it must reach a
  core that is not parked.
- **A memory-type change after bring-up reaches every core**:
  `paging_set_write_combining()` re-snapshots the PAT and MTRRs and each
  AP replays them through the call IPI. Serially, not Linux's
  `stop_machine()` rendezvous -- enough while the APs touch no memory;
  stage 5 must revisit it.
- **The idle halt drops the lock** (`clockevent_idle_halt()`), so the
  other cores are not kept out by a halted BSP; the interrupt that wakes
  it takes the lock for its own handler.
- **The kmutex fallback spin still holds the kernel lock** (a caller that
  cannot park spins with `cpu_relax()`): one core, nothing changes; from
  stage 5 the holder on another core needs the lock to finish, so that
  spin must drop it.
- **The recursion assertion is four checks, counted by `bkl_faults()`**
  and logged (`bkl: ...`, the first eight): an entry from ring 3 at a
  non-zero depth, a return to ring 3 still holding it, an exit at depth
  0, and a switch that does not hold it or loads a depth below 1. A
  KTEST asserts the count is 0 after boot; a fresh context started at
  depth 0 made it 28.
- **A spin past a second logs once and keeps spinning** (`spinlock:
  lockup suspected`, Linux's `CONFIG_DEBUG_SPINLOCK`), naming the
  holder's core and call site; a recursive acquire panics, on any core
  since 2026-11-20. An AP only counts (`spin_lockups()`) -- it may log
  nothing until stage 5.
- **`spin_lock()` raises the preemption guard**, as Linux's implies
  `preempt_disable()`: a preempted holder would keep the other cores
  spinning for a slice, and the next context on its core to take the
  lock would read as a recursive acquire. On the BSP only until stage 5
  (the guard's depth is one global an AP may not touch). The kernel lock
  takes its spinlock RAW -- it is held whenever the kernel runs.
- **The idle halt drops the kernel lock only where another context
  could run anyway**: interrupts on, preemptible, depth 1. A caller
  halting inside a guarded section (`diag.c`'s wait) keeps it.
- **A core that faults in a call, or a parked one that does not take
  one within a second, is `CPU_WEDGED`** and given no more calls; a
  fault drops the kernel lock it held first. A call not taken in that
  second is TAKEN BACK (a swap of `call_fn`, which the target's claim
  also swaps), so it never runs later with a dead `arg`; one already
  running is waited for however long (Linux's `csd_lock_wait()`). A
  waiting sender answers calls and shootdowns sent to its own core, and
  a mailbox still full is refused at once.
- **A memory-type replay runs only when a register moved**, compared in
  place by `paging_memory_types_snapshot()`, and one core at a time --
  stage 5 must make it a rendezvous.
- **Proven by positive controls**: counting without the ticket lock
  lost 5992 of 8000 increments on four TCG vCPUs; an AP that skips the
  kernel lock turns the exclusion KTEST red.

The text below is the plan as written.

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

### Stage 5 -- the scheduler picks per core -- BUILT 2026-11-19, ON BY DEFAULT 2026-11-20

**What was built** (the picks below held):

- **What runs is per core**: `current_index`, `rotation_pos` and
  `g_need_resched` are this core's `struct cpu` fields behind macros
  (`sched_internal.h`'s `sched_here()`), so their 140 uses read the
  right core's -- Linux's `current`. So are the slice and the billing
  start. Everything else in `procs[]` is shared under the kernel lock,
  and the per-CONTEXT globals (`g_isr_depth`, the preemption depth) stay
  global: they are 0 whenever the lock is free, so the holder's are the
  only ones that exist.
- **An AP's idle context** is its park loop's successor (`smp.c`'s
  `ap_schedule()`): `sti; hlt` holding nothing, its own saved context
  in `struct cpu`. Its tick and the reschedule IPI share vector 0xFC and
  reach `scheduler_ap_event()` -- the rotation only; sleepers, the
  watchdog and the idle work stay with the BSP's clockevent. **The tick
  is ONE-SHOT, re-armed by each event**: periodic, three APs' timers
  sent QEMU's TCG main loop into a spin that starved every vCPU (a
  4-core boot hung; 2 cores and KVM did not).
- **A wake kicks an idle AP**; a signal or a stop aimed at a thread
  running on another core kicks that core (Linux's `kick_process()`).
- **A thread running on another core is never freed in place**: group
  exit and kill count it as parked mid-call, so the death is deferred
  (the thread takes its SIGKILL there), and exec waits for it to leave
  (`group_wait_off_cores()`, Linux's `de_thread()`). `scheduler_kill()`
  of such a process returns before it has died -- as for one parked
  mid-call, now just more often.
- **The BSP's kernel context holds the kernel lock whenever it runs**, so
  a busy-waiting one (a KTEST polling for an exit) would keep every AP
  out for good. At a tick where it could be preempted it lets any core
  queued for the lock through first (`kernel_let_waiters_in()`) -- the
  preemption it would have had on one core.
- **Proven by** `sched_smp_test.c` (busy processes accumulate time on two
  or more APs; with the APs picking nothing it fails), the whole KTEST
  suite under TCG `-smp 4 smp=run` (four clean runs of six; the misses
  are in `docs/bugs.md`), and the ring-3 suite on 4 cores under TCG and
  KVM (134/134 each).

**The soak, and the default flipped 2026-11-20.** Under KVM on 4 cores
it found three bugs, each fixed with a test that went red without it: a
per-core read split by a migration (the machine wedged on a mount lock
its owner did not recognise -- the reads are one `%gs` instruction now),
a dying group's thread faulting with no address space found, and a
not-present fault answered as a protection fault after another core had
mapped the page (`samepage_test`; telnetd on the ASUS crashed at nearly
every session end). After them: KTEST clean under TCG and KVM bar the
pre-existing KVM idle-deadline miss, the ring-3 suite under KVM,
`kvm_soak.py --cpus 4` (red with the kernel lock off on the APs, and
without user TLB shootdowns), and the ASUS on its `smp=run` entry -- 25
rounds of the race tests under `diskbench` and 60 remote sessions.
A second round the same day, from the gate going red under load after
the flip: a call resumed after a park ran on with interrupts off, so
fsck's walk held the kernel lock past a second; with the flag restored,
a stale "which core am I" in `shoot()` made a migrated sender wait for
itself, and the 1 s timeout wrote a live core off. Fixed with the flag
restore, decisions on the core taken with interrupts off, no write-off of
a scheduling core, and a contended lock under a call yielded by a
preemption to the AP's idle context (`docs/decisions/kernel.md`). Proven
by nine loaded TCG suites (three guests at once) that had failed 12 in
12, and 60 runs of the `thread` KTESTs under load that had broken by run
16-21.

**A kernel fault on an AP panics as the boot core's does** (2026-11-20):
the same report with `on core N`, no kernel lock taken for it, and every
other core stopped by an NMI first (`panic_begin()`, `smp_stop_others()`;
`docs/decisions/kernel.md`). It used to halt that core with `cli; hlt`
and no word, a running process with it. `panic_store_test.py --kind
ap-gp-fault`, on a scheduling and a parked core.

**`cpus` reads each core's state** (2026-11-20): `/bin/cpus` and the
debug console's `cpus` over `QUERY_CPUSTATE` (`smp_query.c`) -- what had
to be read through QEMU's gdbstub before (`docs/commands/cpus.md`).

**`make DEBUG_PREEMPT=1` checks every per-core read** (2026-11-20),
Linux's `CONFIG_DEBUG_PREEMPT` (`docs/conventions/kernel.md`).

**Not yet**: the kdebug roundup (a stub stop halts core 0 only) and lockup
reports from an AP. Per-core load is read since 2026-11-20:
`QUERY_CORELOAD` (`kernel/proc/cpuload_query.c`) reports each online
core's `busy_ns`, and Task Manager's CPU page and `lscpu` divide it.

**Decided 2026-11-19, before building**:

**Decided 2026-11-19, before building** (the maintainer's picks, with
stage 6's):

- **Behind an opt-in boot flag, off by default**, until a KVM soak and
  the ASUS are clean -- the `syscall_irq` precedent. So stages 5 and 6
  may land as separate commits: the unsafe half never runs unless asked.
  Flipping the default is its own later change.
- **The APs run processes only.** Each gets its own idle loop that picks
  runnable processes; everything the kernel context does stays on the
  BSP -- the ring-0 shell, the debug console, and the idle work (disk
  write-back, `net_poll()`, input polling). Linux's per-CPU idle task,
  with the work left where it is.
- **Device interrupts stay on the BSP.** The I/O APIC and every MSI keep
  targeting it; an AP gets its own LAPIC timer for preemption, and IPIs.
  Linux before irqbalance.
- **Stage 6's first shootdown flushes the whole space** (BUILT, below,
  with the set kept per core): each address
  space keeps the set of cores that have it loaded (Linux's
  `mm_cpumask`; a core idling in the kernel context keeps the last CR3,
  so it stays in the set -- lazy TLB), an unmap sends those cores one
  IPI to reload CR3 and waits for every acknowledgement before the frame
  is freed, and a kernel-map change goes to every core. Per-page ranges
  wait for a measurement.

**Found 2026-11-19, reading for the build: three paths free a thread
that is in ring 3, which on one core means "not running" and from
stage 5 does not.** Each is safe today and a use-after-free of a kernel
stack and an address space the moment a sibling runs on another core:

- `scheduler_exit_group()` (`sched_exit.c`): after `group_defer_death()`
  finds no sibling mid-call, `group_release_threads_but()` frees the
  rest in place -- "every other thread is in ring 3".
- `scheduler_exec()` (`sched_fork.c`): `group_release_threads()` frees
  every sibling, then the old space is destroyed.
- `scheduler_kill()`: a victim neither READY nor BLOCKED is refused as
  "running (handled above)" -- on another core it is simply not killed.

The fix is Linux's shape, and this kernel already has its first half:
**a thread RUNNING on another core is counted as one parked mid-call**
(`group_parked()`), so the death is deferred -- a pending SIGKILL, which
that thread takes at its next kernel entry and leaves on its own -- and
the sender kicks its core with a reschedule IPI rather than waiting out
its slice (Linux's `kick_process()`). exec, which has no deferred form,
must wait for those siblings to be gone first (Linux's `de_thread()`).
Under the kernel lock a thread RUNNING elsewhere is in ring 3 or
queued for the lock, never mid-call, so this is the whole set.

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

### Stage 6 -- TLB shootdown -- DONE 2026-11-19

**Built before stage 5, unflagged** (the maintainer's pick): with every
AP parked on the kernel's CR3 a user shootdown has no target, so it
changes nothing on an ordinary boot, and stage 5 lands on a proven
shootdown rather than with one. What was built (`kernel/include/kernel/tlb.h`):

- **The set of cores is kept per CORE, not per space** --
  `struct cpu->cr3`, written by every CR3 load (`tlb_load_cr3()`, which
  `vmm_switch_address_space()` now is) and kept while the core idles
  (lazy TLB). Linux's `mm_cpumask` transposed: no per-space bitmap to
  size or grow, and choosing targets is one pass over the cores, which
  is what a whole-space flush wants anyway.
- **One mailbox per core and a sender that holds the kernel lock** --
  vector 0xFD, answered in `isr_dispatch()` before anything shared on
  any core and from either ring. The sender waits with interrupts off,
  so **a core spinning for a lock answers from the spin**
  (`spinlock.c`'s `take()` calls `tlb_service()`), Linux 2.0's
  `smp_invalidate_needed` check in `lock_kernel()` -- and so does the
  SENDER's own wait, since a target may be waiting on it. A parked
  target that does not answer in a second is `CPU_WEDGED`; a scheduling
  one is waited for however long, counted as a spinlock lockup is.
- **The target reloads CR3** -- no global pages are mapped, so that is
  the whole TLB. Every `vmm.c` site that changes a live user leaf
  (unmap, release, swap-out, mprotect, hide, COW break) calls
  `tlb_flush_user_page()`; fork's COW downgrade does one
  `tlb_flush_user_space()` at the end of the walk (Linux's
  `flush_tlb_mm()` after `dup_mmap()`); `paging.c`'s kernel-map changes
  call `tlb_flush_kernel()`, every core; the MTRR/PAT protocol keeps a
  local flush, since each core runs it for itself.
- **Teardown is NOT safe by construction, as this section once said:**
  a core idling on a dead process's CR3 still walks its tables, so
  `vmm_destroy_address_space()` first sends `tlb_leave_space()` -- a
  target on that space loads the kernel's (Linux's `leave_mm()`).
- **Proven by** `tlb_test.c`: a parked core loads a test space and reads
  a page, the BSP remaps the page to another frame, and the core must
  read the new frame's bytes. With the shootdown removed it reads the
  old one under TCG `-smp 4` (`FAIL: p.seen`), so the test sees the
  break.

The design as it was written before the build:

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
(A one-vCPU guest -- what `vm.py` and `make run` boot unless told
otherwise -- is still serial in effect; `ktest_run.py` boots four.) Older QEMUs in `tools/runners/qemu_matrix.py` may differ --
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
