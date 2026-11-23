# SMP: more than one core, staged

**Status: STAGE 1 IS BUILT (2026-08-30); STAGE 2 IS BUILT IN FULL (the
Local APIC 2026-08-30, its TIMER 2026-09-06, the I/O APIC and the MADT
interrupt source overrides 2026-09-10); STAGE 3 IS BUILT (2026-11-19:
the APs started and parked); STAGE 4 IS BUILT (2026-11-19: a ticket
spinlock, the call IPI and the kernel lock); STAGE 6 IS BUILT AHEAD OF
STAGE 5 (2026-11-19: the TLB shootdown, `kernel/arch/x86_64/tlb.c`);
STAGE 5 IS BUILT AND ON BY DEFAULT (2026-11-19 behind `smp=run`; the
default since 2026-11-20, `smp=park` the way back); STAGE 7 IS UNDER
WAY (2026-11-21: `lockstat`, a per-core guard and the heap and frame
locks, then the first syscalls off the kernel lock and the clock page,
then the run-queue lock; 2026-11-22: the descriptor tables' locks and a
lock-free `lseek`);
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
| kernel heap | DONE (stage 7) -- one free list + bins (`kernel/lib/heap_core.c`) behind its own irqsave spinlock in `heap_os_lock()`/`heap_os_unlock()` (`kernel/mm/heap_os.c`) | -- |
| physical frames | DONE (stage 7) -- `kernel/mm/pmm.c`'s bitmap, zone counters and frame refcounts behind its own irqsave spinlock, taken after the heap's | -- |
| descriptor tables | DONE (stage 7) -- `kernel/proc/syscall_fd.c`: the fd lock over the description pool, then each table's own spinlock (Linux's `file_lock`) | -- |
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
  (the guard's depth was one global an AP may not touch; each core's own
  since stage 7). The kernel lock
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
  only ones that exist. (Per core since stage 7, which needs a guard
  outside the kernel lock.)
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

**The kdebug roundup is built** (2026-11-22, `docs/kdebug-design.md`,
"Every core stops"): a stop NMIs every other core into
`smp_hold_here()`, and GDB reads each core's held frame. **Not yet**:
lockup reports from an AP. Per-core load is read since 2026-11-20:
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

**Built 2026-11-21: the measurement and the prerequisites.** No path
left the kernel lock in this part (the next one is below); what changed
is what a path that does would stand on:

- **`lockstat`** (`kernel/proc/lockstat.c`, `docs/commands/lockstat.md`)
  -- Linux's `lock_stat` for the counted spinlocks: takes, waits, wait
  and hold totals and maxima, per lock (`QUERY_LOCKSTAT`), and for the
  kernel lock per core and per ENTRY, a syscall or a vector
  (`QUERY_BKLSTAT`). `SPINLOCK_COUNTED()` registers a lock by declaring
  it. Every field is written by the holder under the lock, so it needs
  no atomics. **Hold times are armed** (`lockstat track on`, the
  `kernel.lockstat` tunable): timed always, they measured +25% on a
  `getpid` and +44% on a 1-byte pipe round trip on one KVM core. Off,
  takes and waits are still counted and an uncontended take reads no
  clock: `getpid` +2%, the pipe +14% -- the heap's lock itself, an
  uncontended atomic and an irqsave twice per call (the bounce buffer's
  kmalloc/kfree, before the per-core bounce buffer below took it off the
  heap). Per-core allocation caches (Linux's per-CPU slabs and pagesets)
  are what would win back the rest.
- **The preemption guard and the interrupt depth are per core** (`struct
  cpu`'s `preempt_depth`/`isr_depth`, moved by one `%gs` instruction).
  As one global each they were the kernel lock holder's alone, so a
  spinlock taken outside the kernel lock could not raise the guard.
  GS.base now points at the boot core's `struct cpu` from
  `kernel_main()`'s first line, since the heap and the frame allocator
  take spinlocks before `gdt_init()`.
- **The heap and the frame allocator each have an irqsave spinlock**,
  heap before frames. Proven by KTESTs (`kernel/proc/lockstat_test.c`)
  that have every core allocate at once through `smp_call()`, outside
  the kernel lock: red with either lock removed (a frame handed to two
  cores; a corrupt heap header and a panic), red with the guard sent to
  the boot core's field.

**Measured on KVM, four vCPUs** (`lockstat -i N -e`, holds timed):

| load | kernel lock held | takes that waited | longest holders |
|---|---|---|---|
| idle desktop, 3 s | 4% | 23% | -- |
| `diskbench`, 8 s | 19% | 49% | `core tick` (an AP's tick and reschedule) 0.83 s, `kernel` 0.23 s, `read`/`write` 0.14 s each; the boot core's own timer waited 0.49 s to get in |
| 3 x `syscall_bench`, 5 s | 86% | 81% | `yield` 2.9 s, `getpid` 0.94 s over 9.8 M takes |
| heap, frames | under 0.5% held | 0 waits in every load | -- |

**On the ASUS** (UX305FA, two cores and four threads; 8-10 s each):

| load | kernel lock held | takes that waited | longest holders |
|---|---|---|---|
| idle desktop | 0.3% | 8% | `lapic_timer_isr`, `query`; no hold over 0.15 ms |
| `diskbench` | 1.8% | 21% | the disk interrupt (`irq_handler`) 79 ms, holds up to 1 ms |

Since boot, an AP's `core tick` and a `read` each once waited 192 ms
for the lock -- at boot, before hold times could be armed, so its holder
is not established.

So the heap and the frame allocator are not where the contention is,
and splitting them bought correctness for later, not time now. **The
numbers point at two things**: calls that need nothing the lock protects
(`getpid`, `monotonic_ns`, `ticks` -- FreeBSD's `MPSAFE` flag in its
syscall table is the shape, a flag in `syscall_table.c`), and the
scheduler's own tick and rotation, which is item 4 below.

**Built 2026-11-21 (later): the first syscalls off the lock, and the
clock page.** FreeBSD's `SYF_MPSAFE` in its `sysent` was the shape:

- **A row says its handler needs no kernel lock** --
  `SYSCALL_ROW_MPSAFE` in `abi/syscall_rows.h`, `SYSCALL_F_MPSAFE` in the
  table -- and `syscall_try_lockless()` (`kernel/proc/syscall.c`) runs it
  from `isr_dispatch()` BEFORE the lock is taken, interrupts off, for
  `getpid`, `gettid`, `ticks`, `monotonic_ns` and `clock_page`. It
  declines, touching nothing, whenever the locked path has work around
  the call: strace on anywhere, a sandboxed process, a re-issue, a
  deliverable signal or a reschedule (`scheduler_lockless_ok()`).
  Nothing can become due during the call without an interrupt: a remote
  wake or signal kicks the core, and the kick lands when SYSRET reopens
  IF. `kernel.lockless_syscalls` turns it off for an A/B in one boot;
  `lockstat -e` counts the calls in a `LOCKLESS` column.
- **The clock is read without writing anything.** It accumulated on
  every read -- one global total behind the kernel lock. It is now a
  snapshot (base nanoseconds, the remainder below one, and the raw value)
  that the boot core's tick retakes, and a read adds the delta since.
  A full-width counter (the TSC) behind the base reads as the base; a
  narrow one (the PM timer's 24 bits) is one global register and never
  is, since a tickless idle runs it past half its span.
  The snapshot is a LATCH (Linux's `raw_write_seqcount_latch()`, as its
  `ktime_get_mono_fast_ns()` uses): two copies and a sequence number, so a
  reader never waits for the writer, and an NMI inside the writer still
  reads a whole copy. The remainder is kept, so retaking it loses no time.
- **The latch IS the clock page** (`abi/clock_abi.h`): one frame, mapped
  read-only and NX in every process at `UADDR_CLOCK_PAGE` through a PML4
  slot every address space shares, as it shares entry 0 -- no page table
  per process, and the free, fork and audit walks skip it. Linux's vDSO
  data page and NT's `KUSER_SHARED_DATA` are the shape. `rt`'s
  `sys_monotonic_ns()` and `sys_ticks()` read it with `lfence; rdtsc`
  when the source is the TSC, and make the syscall otherwise. A process
  asks `SYS_CLOCK_PAGE` where it is once; a kernel without one answers
  `-ENOSYS`, so a newer `/bin` under an older kernel (a `previous` boot)
  keeps making the syscall.

Proven by positive controls: a fold that drops the remainder turns
`clocksource`'s retake KTEST red; a retake written over the copy readers
use turned the cross-core reader KTEST red in 2 of 2 runs under TCG; a
page reader 5 us ahead failed `lockless_test`'s bracket check on 99999 of
100000 reads; `kernel.lockless_syscalls off` is the in-test control for
the counting.

**Measured on KVM, four vCPUs, the TSC the clocksource** (`syscall_bench
calls`, ns per call; "through the lock" is `kernel.lockless_syscalls off`
on the same boot):

| | 1 copy, through the lock | 1 copy, lock-free | 4 copies, through the lock | 4 copies, lock-free |
|---|---|---|---|---|
| `getpid` | 137 | 55 | 539-675 | 53-73 |
| `monotonic_ns` by syscall | 102 | 66 | 543-693 | 63-79 |
| the clock page | -- | 15 | -- | 15-22 |

**On the ASUS** (UX305FA, two cores and four threads, the TSC the
clocksource), the same rows:

| | 1 copy, through the lock | 1 copy, lock-free | 4 copies, through the lock | 4 copies, lock-free |
|---|---|---|---|---|
| `getpid` | 222 | 112 | 1187-1304 | 145-150 |
| `monotonic_ns` by syscall | 228 | 121 | 1261-1378 | 171-173 |
| the clock page | -- | 29 | -- | 37-45 |

What the desktop gains is smaller than these tables: idle, the lock was
held 0.3% of the time on the ASUS (above). The calls that matter there are
the clock reads every frame and every timeout make, which on a TSC
machine no longer enter the kernel at all.

**A cross-core call function must not take a counted lock that its
target might be spinning for**: `take()` answers calls while it spins
(`smp_poll_ipis()`), so the call would queue behind its own core's
ticket. True today because nothing takes the heap or frame lock outside
the kernel lock, which every call's sender holds; the first path that
does makes it a rule to enforce. The clock's lock is the first taken
outside it -- by a reader on any core that finds the snapshot stale --
and only by TRYLOCK, which never queues.

**Built 2026-11-21 (later still): the scheduler lock, and an AP's
scheduling without the kernel lock.** FreeBSD 5's `sched_lock` beside
Giant, and NT's dispatcher lock, are the shape
(`docs/decisions/kernel.md`):

- **One run-queue lock** (`sched_lock()`, `sched_internal.h`; `sched` in
  `lockstat`), after the kernel lock and never before it, recursive per
  core, interrupts off while held. Every write of `procs[].state` and
  every check-then-act on another thread's run state holds it: the pick
  and the switch, wakes, spawn and fork publishing READY, stop and
  continue, a signal's pending bit and its kick, kill, group exit and
  exec's wait. A switch is entered holding it once and lets it go before
  the restore.
- **An AP's event or a yield that needs no kernel lock takes none**
  (`scheduler_ap_event_lockless()`, `scheduler_yield_lockless()`): idle
  with nothing ready, a slice not over -- decided with no lock at all --
  or a switch between a ring-3 process (or the idle loop) and a context
  this path saved, under the scheduler lock alone. It declines, having
  changed nothing, for anything else, and the ordinary path takes the
  event. A context it saves is at kernel-lock depth 0; a core holding the
  lock that switches into one lets it go (`bkl_load_depth(0)`).
- **A READY context stays "live" until its core is off its stack**: the
  mark clears after RSP moves (`process_context_restore_release()`), and
  the pick and `slot_claim()` skip a live one.
- **The shootdown pairs with a lock-free switch by fences**: a core
  records the CR3 it is about to load, and keeps the old one in
  `cr3_old` until the load is done; `shoot()` fences, then reads both.
- **An idle AP bills its time at each event**, not as one lump when it
  next switches: with the boot core no longer holding back every AP's
  billing, a lump of seconds landed inside the CPU-split KTEST's window.
- **Not built: a lock-free switch INTO a context that holds the kernel
  lock**, taking it at the end of the switch. Built and measured: every
  AP event lock-free, and the KTEST suite panicked 2 runs in 2; the
  roadmap keeps it.

Proven by: a KTEST of oversubscribed spinners (each runs, each dies when
killed, APs took events lock-free, no double resume, no lock fault --
red with the lock-free path refused), a unit KTEST that a READY context
still live is neither picked nor its slot claimed (red without the mark),
lockdep on a `PROFILE=debug` image under load (1.8 M takes, nothing
found), KTEST 3 in 3 under TCG, the ring-3 suite under KVM, and
`kvm_soak.py --cpus 4` 5 in 5.

**Measured on KVM, four vCPUs, the TSC the clocksource**, each pair one
boot with `kernel.lockless_sched` on and off:

| load | kernel lock takes | takes that waited | `core tick` takes |
|---|---|---|---|
| idle desktop, 5 s | 2417 / 6282 | 2.2% / 26.1% | 0 / 3655 |
| 6 spinners on 4 cores, 5 s | 12814 / 14830 | 0.7% / 8.4% | 2804 / 4623 |

| `syscall_bench` yield, ns | lock-free | through the lock |
|---|---|---|
| 1 copy | 220 | 906 |
| 3 copies | 650-700 | 3145-3237 |

With the spinners, about 38% of an AP's events switched lock-free: the
rest picked a context the boot core had switched out under the kernel
lock, and declined. Their CPU shares stayed equal either way.

**On the ASUS** (UX305FA, two cores and four threads, the TSC):

| | lock-free | through the lock |
|---|---|---|
| idle desktop, 8 s: kernel lock takes / waited | 11342 / 1.0% | 17459 / 10.9% |
| ...`core tick` takes | 0 | 6008 |
| yield, 1 copy | 714 ns | 1861 ns |
| yield, 3 copies | 766-1950 ns | 7698-8764 ns |

**Re-measured 2026-11-22, after the scheduler lock** (`lockstat -i N -e`,
holds timed; KVM four vCPUs, and the ASUS):

| load | kernel lock held | takes that waited | longest holders |
|---|---|---|---|
| KVM, idle desktop, 5 s | 0.9% | 4.7% | `lapic_timer_isr` 18 ms, `kernel` 10 ms |
| KVM, `diskbench`, 8 s | 6.0% | 83% | `read` 180 ms, `kernel` 108 ms, `core tick` 103 ms |
| KVM, 3 x `syscall_bench`, 6 s | 61% | 82% | `read` 2.0 s, `write` 1.2 s, `lseek` 0.17 s: 93% of every hold |
| ASUS, idle desktop, 8 s | 1.1% | 0.2% | `lapic_timer_isr` 42 ms, `query` 32 ms |
| ASUS, `diskbench` 2 GiB, 10 s | 16.7% | 0.7% | the disk interrupt (`irq_handler`) 1.3 s, `write` 131 ms |

So under load the file calls are the lock, and the next split is the
descriptor path, in FreeBSD 5's order and Linux 2.6's: the descriptor
tables, then `lseek`, then `read`/`write`. On the ASUS the disk interrupt
holds most but the lock rarely waits; it matters once several cores make
file calls at once. Also seen: one AP's `core tick` waited 277 ms for the
lock since boot on the ASUS, holder not established.

**Built 2026-11-22: the descriptor tables on their own locks, and
`lseek` without the kernel lock.** Linux's `files_struct` `file_lock` is
the shape:

- **Two locks, in this order** (`kernel/proc/syscall_fd.c`). The FD LOCK
  (`fd` in `lockstat`) covers the description pool -- a claim, every
  `refs`/`ops` change -- and `g_spaces`' slots; each descriptor table's
  own spinlock covers that table (its arrays, `next_fd`, `pml4`). Both
  were the preemption guard, which since the per-core guard excludes only
  its own core: the kernel lock had been doing the rest. A table and
  `g_spaces` are retired, never freed, so a reader holding the kernel lock
  still reads without either lock.
- **A descriptor's reference keeps its description while the table's
  lock is held**: only a write under that lock drops it, and the unref
  follows the unlock. So a holder of the table's lock may use the
  description a slot names with no reference of its own -- and no
  `release()` to run, which might sleep.
- **An MPSAFE handler may decline** (`SYSCALL_DECLINE`, the handler told
  by `syscall_ctx.lockless`): having changed nothing, the locked path runs
  it again. `lseek` is the first: under its table's lock alone it answers
  a file's `SEEK_SET`/`SEEK_CUR`, and `ESPIPE` for a stream with no
  position, and declines a bad fd, `SEEK_END` (the size is the
  filesystem's, behind a sleeping lock) and a position lock somebody
  holds.
- **The position moves without taking its kmutex.**
  `kmutex_unheld_begin()` holds the mutex's own spinlock while the mutex
  is free: no thread can take it meanwhile, and nothing is taken, so no
  unlock wakes anyone -- `kmutex_unlock()`'s wake scans the process table
  under the kernel lock still.
- **One global lock was built first and measured wrong**: four copies
  seeking at once waited on 99.9% of its takes (5.3 s of waiting in 2 s),
  780-805 ns a seek. The table's own lock took that to 118-134 ns.

Proven by: KTESTs (`kernel/proc/fd_lock_test.c`) where every core claims
descriptions at once outside the kernel lock -- the preemption guard in
place of the lock panicked in `fd_desc_unref()`, two cores on one
description -- and other cores seek a descriptor while this one closes it
and reuses its description elsewhere: red (`moved`) with the table's lock
dropped from the lock-free path and its window widened, green with the
same window and the lock. The ring-3 version in `lockless_test` (threads
seeking while the main thread `dup2`s over the descriptor and opens
another file) is red on the same control, and checks the `lseek` row's
counts: lock-free for `SEEK_SET`, through the lock for `SEEK_END`.

**Measured** (`syscall_bench calls`, the `lseek` row, ns per call;
"through the lock" is `kernel.lockless_syscalls off` on the same boot):

| | 1 copy, lock-free | 1 copy, through the lock | 4 copies, lock-free | 4 copies, through the lock |
|---|---|---|---|---|
| KVM, four vCPUs, the TSC | 87 | 198 | 118-134 | 1185-1386 |
| ASUS | 208 | 545 | 210-310 | 1830-2420 |

Under 3 x `syscall_bench` on KVM, `lseek` left the kernel lock's holders;
`read` and `write` remain, 0.78 s and 0.57 s of 1.4 s held in 15 s.

**Built 2026-11-22 (later): a lock-free park, and a pipe's `read` and
`write` without the kernel lock.** FreeBSD's pipe mutex and its MPSAFE
`msleep()` are the shape (`docs/decisions/kernel.md`):

- **`read` and `write` are MPSAFE rows**, and a kind of description
  whose ops may run without the lock says so (`FD_OPS_MPSAFE`; a pipe's
  two ends). `rw_lockless()` holds the description, as the locked path
  does, and declines for every other kind. **A last reference is put
  under the kernel lock** (`put_lockless()`): a sibling's close during the
  call leaves this one the `release()`.
- **A spinlock per pipe**, every field under it. A waiter ARMS on the
  pipe's channel before letting the lock go and a waker wakes after, so
  both paths share one protocol; the preemption guard that was there
  excluded only its own core.
- **A wake's two scans, an arm, and a park's last test are under the
  scheduler lock** (`scheduler_wake_n()`, `scheduler_wait_arm()`,
  `block_common()`): with no kernel lock on either side, a wake testing
  "armed?" and "BLOCKED?" while the parker moved between them found
  neither.
- **The lock-free park** (`scheduler_block_current_lockless()`) parks at
  the syscall's entry, as a pipe's locked park does, and declines having
  written nothing for a signal or re-issue due or an RT process. **When
  the context it would switch into holds the kernel lock, it takes that
  lock first**, on its own stack before anything is picked or marked,
  picks again, and saves the parker at depth 0 (`bkl_take_for_switch()`);
  the switch hands the lock on. Declining instead never parked once: every
  locked park and every AP's idle loop saves depth 1, so nothing ever
  reached depth 0.
- **User memory is copied without faulting** (`vmm_copy_to_user_nofault()`),
  refusing instead, and that is safe because of the interrupts-off window
  -- a frame is freed only after a shootdown this core cannot answer --
  so no lock is taken between a range check and its copy. A read checks
  the destination before a byte leaves the pipe.

Proven by `lockless_rw_test` (ring 3): a two-thread ping-pong that parks
both ends every round, three writers' 64-byte records through one pipe,
a parked reader told of EOF and a parked writer of EPIPE, and the rows'
counts. Each red on its own control, KVM four vCPUs: the pipe lock
dropped from the lock-free write (the records phase stalls), the
lock-free park's "woken while arming" test skipped with its window
widened (ping-pong stalls; green with the window and the test), the wake
scan without the scheduler lock with the park's window widened (ping-pong
stalls; green locked), and every lock-free park declined (39281 reads take
the kernel lock against 3910). KTEST 1339/0 under TCG, the ring-3 suite
141/141 under KVM four vCPUs.

**Measured on KVM, four vCPUs, the TSC the clocksource** (`syscall_bench
200000 64`, ns; "through the lock" is `kernel.lockless_syscalls off` on
the same boot, two runs each):

| | 1 copy, lock-free | 1 copy, through the lock | 4 copies, lock-free | 4 copies, through the lock |
|---|---|---|---|---|
| `pipe 1 B` | 457-493 | 535-555 | 440-1640 | 4060-24700 |
| two processes, a pipe round trip | 9140-9330 | 18600-19300 | 4590-10500 | 41000-50200 |
| a /tmp read of 64 B (still locked) | -- | -- | 5350-7840 | 8380-11200 |

The ramfs read gains only because the pipes left the lock.

**On the ASUS** (UX305FA, two cores and four threads, the TSC), the same
rows and runs:

| | 1 copy, lock-free | 1 copy, through the lock | 4 copies, lock-free | 4 copies, through the lock |
|---|---|---|---|---|
| `pipe 1 B` | 1089-1100 | 1425-1437 | 1110-4630 | 2720-10100 |
| two processes, a pipe round trip | 7090-7100 | 10220-10260 | 7200-17400 | 26500-45600 |

**Built 2026-11-22 (later still): a ramfs file's `read` and `write`
without the kernel lock.** Linux's RCU path walk, which hands over to
ref-walk on any trouble, is the shape (`docs/decisions/kernel.md`):

- **Every sleeping lock is TRYLOCKED, and a busy one declines** --
  the description's position lock and the mount's
  (`fs_read_range_lockless()`, `fs_write_range_lockless()`). Both are
  taken before anything changes, so the locked path simply runs the call
  again; nothing sleeps without the kernel lock. A read or write over
  64 KiB takes the locked path: interrupts are off throughout.
- **A backend says its ops may run so** (`fs_ops.mpsafe`; ramfs, whose
  read, write and size are memory and the allocators' own locks). A disk
  file's call declines, as does a read-only mount, a mount with a device
  to settle, a path the image cache may hold, and any call while the
  cache simulator is on. **Nothing declines once the backend has
  written** -- a failed write may be partial -- so an mpsafe backend
  never asks `FS_OP()` to re-run it. **And it vets each lock-free write
  first** (`fs_ops.lockless_write_ok`): ramfs declines a missing file and
  one whose chunk table would grow past 128 KiB (a 64 MiB file), whose
  doubling copies and zeroes the whole table with interrupts off -- a byte
  written at 64 GiB would otherwise grow a 128 MiB table there.
- **A description says it may go lock-free once `open` has finished**
  (`open_file.file.lockless`, stored last with release, on an mpsafe
  mount only), so a sibling never uses a half-opened file and a disk
  file's call declines before any copy.
- **Nothing of a slot but its lock is read before the lock is held**: a
  root reprobe clears every slot, open files or not, and `fs` with it.
- **The mount table is read inside a sequence count**
  (`mount_resolve_lockless()`; Linux's `mount_lock` seqlock): `record()`,
  `slot_clear()` and a gone disk's flags bump it, under the kernel lock,
  and a resolve declines on an odd or moved count.
- **A write a watch covers owes its note**, sent under the kernel lock
  (`fs_note_watchers()`): the window server's broadcast is the lock's.
- **The I/O charge and the generation counters add atomically**: two
  cores may now be in them at once.

Proven by `lockless_rw_test`'s new phases, each red on its own control
under KVM's four vCPUs: readers sharing one description each get the
next word (the position lock taken out: 5 words wrong), writers sharing
one each take the next position (6096 of 16000 words), appenders with a
description each land at the end (the end read outside the mount lock:
11566 of 16000), a word written 128 MiB in takes the kernel lock (the
veto allowing it: lock-free +1), reads beside 200+ mount and unmount
cycles, and the
read and write rows' counts -- lock-free for `/tmp`, through the lock
for `/var/tmp`, past the bound and with the tunable off. Two mount
KTESTs: a lock-free resolve declines mid-write (red without the
odd-count test), and a lock-free write's answers, an owed note and the
far write's veto among them (red with either taken out).

**Measured on KVM, four vCPUs, the TSC** (`syscall_bench 200000 64`, a
64 B `/tmp` call, ns; "through the lock" is `kernel.lockless_syscalls
off` on the same boot, two runs each):

| | 1 copy, lock-free | 1 copy, through the lock | 4 copies, lock-free | 4 copies, through the lock |
|---|---|---|---|---|
| `read` | 546-558 | 754-766 | 2282-3118 | 5727-13426 |
| `write` | 703 | 846-850 | 1134-3105 | 7430-8769 |

**On the ASUS** (UX305FA, two cores and four threads, the TSC), the
same rows and runs:

| | 1 copy, lock-free | 1 copy, through the lock | 4 copies, lock-free | 4 copies, through the lock |
|---|---|---|---|---|
| `read` | 1572-1588 | 2092-2093 | 2947-3543 | 6284-11467 |
| `write` | 2020-2185 | 2368-2369 | 4292-9463 | 7029-10042 |

Under 3 x `syscall_bench` for 6 s (holds timed) the kernel lock was
held 8.9% of the time, against 61% before the descriptor split; `read`
took it 15 k times and ran lock-free 2.05 M times, `write` 14 k and
1.4 M. **What waits now is the heap lock** (41% of 7.1 M takes waited --
every lock-free call kmallocs its bounce buffer) and the scheduler lock
(30% of 7.3 M); the kernel lock's holders are `spawn`, `open` and page
faults. Per-core allocation caches are what the heap needs (stage 7's
first numbers said so). Still declining: strace, a sandboxed process, the
disk filesystems -- whose ops sleep in a disk wait, so they need a real
mid-call park -- and the stall and kstack tables, written under the lock.

**Built 2026-11-22 (last): a per-core bounce buffer for the lock-free
calls.** Every lock-free pipe and file call allocated its bounce buffer
and freed it again, taking the heap's lock twice -- 41% of 7.1 M takes
waited under 3 x `syscall_bench`. Each core now keeps one 64 KiB buffer
(`struct cpu`'s `bounce`, `fd_bounce_get()`/`fd_bounce_put()`), which a
lock-free call may use with no lock: it runs with interrupts off on one
core, never sleeps, and hands the buffer back before anything that could
park or switch (the lock-free write's owed fswatch note returns it before
waiting for the kernel lock). Linux's per-CPU slab freelists are the
general shape; this is the one allocation the measurement named. Proven
by `lockless_rw_test` with one buffer shared by every core: four checks
red (torn records, wrong words, writes out of order).

Measured the same way (KVM, four vCPUs, 3 x `syscall_bench`, 6 s, holds
timed): the heap lock 196 k takes, **none waited** (was 41% of 7.1 M);
what waits now is the scheduler lock (42% of 7.3 M) and the descriptor
pool's (32% of 7.1 M) -- per-core run queues and a per-table pool are
the next splits. One copy: a 1-byte pipe round 319 ns (410 before), a
64 B `/tmp` read 513 ns (546), write 677 ns (703).

**Built 2026-11-23: a ready list, then the wait channels on locks of
their own** -- the first two steps toward per-core run queues (picked
staged: the ready set as a structure, then its split per core with wake
placement, idle stealing and a periodic balancer; RT one global list).

*The ready list* (`sched_set_state()`, the one writer of a slot's run
state) replaced every whole-table walk the pick made. It changed
nothing measurable: 39-44% of ~7.3 M takes waited before, 47-49%
after, ~110 ns a hold either way. A pick's scan of a few dozen slots was
never the cost; how often the lock is taken was. **Measure holds with
`--cpu host,+invtsc`**: without it the guest's clock is acpi_pm, every
clock read is a VM exit, and holds read ~5 us a take.

Counted by caller over 4.2 M takes: `scheduler_wake_n` 54%,
`scheduler_wait_arm` 18%, the lock-free park 16%, the lock-free yield
8%, `scheduler_wake_one` 4%. Wakes and arms touch the run queue only
when somebody is actually parked, so *the wait channels went onto
chains* (`sched_sleepq.c`, FreeBSD's sleepqueue chains; Linux's
wait-queue lock is the same split): 64 chains hashed by channel, a
spinlock each. A slot is on its channel's chain from an arm or a park
until a disarm, a wake or leaving BLOCKED. An arm is the chain lock
alone; a wake stamps arms under the chain lock alone and takes the
scheduler lock only to make a parked slot READY. **Order: the scheduler
lock, then one chain lock, never two** -- a wake that finds a sleeper
lets its chain go, takes the scheduler lock and walks again. A park
holds its chain from "was I woken?" until BLOCKED, then lets it go
before the switch: a waker that sees BLOCKED waits on the scheduler
lock the parker still holds.

Measured the same way: scheduler-lock takes 7.35 M -> 1.7-2.0 M, time
waited on it ~0.9 s -> 0.44-0.57 s in 6 s; the per-call rows (`yield`,
a 1-byte pipe, a round trip) inside run-to-run noise -- what is left is
real parks and wakes, which per-core queues split.

**Built 2026-11-23: per-core run queues (2b)** (`kernel/proc/sched_rq.c`),
designed before building -- the points the code corrected are marked
*(built: ...)* below -- in Linux's shape, picked over a brlock (cold paths taking every queue's
lock) and over measuring a real load first. Linux: a lock per task
(`p->pi_lock`) and one per runqueue, `task_rq_lock()` taking both and
re-checking `task_cpu(p)`, `double_rq_lock()` in a fixed order for a
steal. FreeBSD ULE: a `tdq` lock per core, `thread_lock()` following
the thread's queue. Copied at toy size:

- *Per core* (`struct rq`, one per `struct cpu`): a lock, the fair
  ready list, its count, `min_vruntime`, `nr_running`. *Global*: the RT
  ready list, `rt_seq` and the throttle, under `rt_lock`. kthreads and
  the kernel context (`ROT_KERNEL`) live on the BSP's queue only;
  `kernel_vruntime` is placed against the BSP's floor.
- *Per slot*: `pi_lock`, and `rq_cpu` -- the queue a READY slot is on,
  the core a RUNNING one is on, the last core of a BLOCKED one (the
  wake's hint). *(built: a READY realtime slot's `rq_cpu` is `RQ_RT` --
  the RT list is a queue of its own, `rt_lock` its lock -- so "the list it
  is on is its `rq_cpu`'s" holds for both classes, and an RT pick taking
  one off it is an ordinary move between two queues.)*
- **THE LOCK ORDER**: kernel lock -> one wait-channel chain -> slot
  `pi_lock`s (several only within one thread group, by slot index) ->
  `rq` locks (two only for a move, by core index) -> `rt_lock`.
  *(built: ONE `pi_lock` at a time -- the group freeze locks each sibling
  in turn, so nothing ever holds two; a move holding its own queue takes
  a LOWER core's by trylock and skips on failure, as Linux's
  `double_lock_balance()` does, because letting its own go mid-pick lets
  a wake in behind it.)* The
  chain comes FIRST, the inverse of 2a's order, so a wake keeps its
  chain while it wakes and the drop-and-walk-again goes.
- **WHO MAY WRITE WHAT**: READY <-> RUNNING on this core, its own queue
  lock alone (the pick, `switch_to()`, a rotation's put-back -- Linux's
  `on_rq` changes under the rq lock only). Into or out of BLOCKED,
  ZOMBIE or UNUSED, and any `rq_cpu` change of a READY or RUNNING slot,
  `pi_lock` + its queue's lock (`task_rq_lock()`: take `pi_lock`, read
  `rq_cpu`, lock that queue, re-check, retry -- here `rq_lock_slot()`).
  *(built: a READY slot moving between queues -- a steal, the balancer,
  an RT pick -- holds BOTH queues' locks and no `pi_lock`, which comes
  before them in the order and which a stealer holding its own queue
  cannot take; Linux migrates a queued task the same way. Holding the
  queue it re-checked still pins the slot: no move can start.)* The `rq_cpu` of a slot on
  no list and not running is `pi_lock`'s alone. **Leaving BLOCKED, or
  dying while on a chain, holds that chain first** --
  `slot_lock_chained()`: read `sq_chan`, lock it, `pi_lock`, re-check.
  `stopped`, the class, `nice` and a `pending` that a lock-free switch-in
  reads are written under `task_rq_lock()`, which the pick's reader
  holds half of.
- *A park*: chain -> own `pi_lock` -> own queue; "was I woken?", the
  signal check and BLOCKED under all three; chain and `pi_lock` go, the
  queue lock is kept for the pick and released by the switch.
- *A wake* (`wake_slot()`, holding the chain): `pi_lock`, BLOCKED
  re-checked, a target chosen -- `rq_cpu` if that core is idle, else an
  idle core, else `rq_cpu`; core 0 for a kthread -- `vruntime` moved by
  the two floors' difference, enqueued under the target's lock, the
  preemption test against the TARGET's running context (its rank,
  vruntime, slice: per core, not this core's), and a kick when the
  target is remote. **A slot still live on its old core**
  (`on_core >= 0`, mid-switch-out) **goes back to that core**, which
  finds it after its switch -- never on a core that would skip it and
  halt.
- *Idle steal*: an AP whose queue has nothing runnable takes one READY
  slot (not a kthread, not live, not stopped) from the longest queue,
  both locks in core order, its own re-checked after. *(built: "longest"
  counts movable slots, `nr_movable`, so a stopped job or a kthread never
  draws an idle core into a lock every tick. And **core 0's queue holds
  no process while an AP schedules** -- a wake or spawn whose hint is core
  0 goes to the least-loaded AP, and neither the steal nor the balancer
  moves one onto it: placed there whenever it idled, the benches' pipe
  pairs made core 0 switch 137k times in 6 s against 2a's 6k, and since
  its kernel context holds the kernel lock whenever it runs, that lock's
  waits went from 0.2-0.3 s to 1-2.4 s.)* *The periodic
  balancer*: from the BSP's tick, every few slices, the longest and
  shortest queues read lock-free; when they differ by two or more, one
  slot moves, the one that ran longest ago, and an idle destination is
  kicked (Linux's `load_balance()`, one level, no domains).
- **A THREAD GROUP IS FROZEN BEFORE IT IS COUNTED** (exit, exec, kill
  of a group): each sibling under `task_rq_lock()` gets `stopped` --
  which no pick passes -- and its SIGKILL, and only then are those
  running on another core or parked mid-call counted.
  `group_wait_off_cores()` already does the freeze; what the global
  lock gave as "one view of every core" is that no frozen sibling can
  start running. *(built: and "is it running" is read UNDER the
  sibling's locks (`sched_running_core()`), never as a hint -- a rotation
  puts the running slot back READY under its queue lock and, declining,
  makes it RUNNING again, and a lock-free count that saw the gap freed a
  thread still running: the full KTEST suite hung on a held disk lock 2
  runs in 3. A sibling found still running is let go again rather than
  left frozen, or it switches out mid-call holding a lock and never runs.)*
- Every other cold site touches one slot and takes `task_rq_lock()`
  (stop, continue, a signal raise -- chained when it wakes --, set_class,
  set_priority, a kthread's start, a fabricated KTEST slot);
  *(built: a fabricated KTEST slot is pinned to core 0, `pin_bsp`, as a
  kthread is -- its frame is a test's local array, and the pick the test
  asks is core 0's -- and `sched_kick_core(0)` now sends core 0 the
  reschedule IPI, since a wake on another core may place work there;)*
  `scheduler_wake_timers()` scans unlocked and locks each candidate
  chained. Readers that report (`ps`, the hung check, next event) stay
  lock-free hints. `check_one_running()` checks this core's own
  RUNNING slot against its `rq_cpu`.
- **Off switches for the positive controls**: tunables for the steal
  and the balancer, as `kernel.lockless_sched` is one for the lock-free
  events.
- **Proven by**: a debug-build invariant check at every switch (a slot
  on at most one list, the list it is on is its `rq_cpu`'s, the counts
  agree); `sched_smp_test` with the steal and the balancer off going
  red on a placement that piles work on one core; the race tests
  (exit/group/kill/park), `kvm_soak.py --cpus 4`, the ring-3 suite on 4
  cores, the KTEST suite under TCG `-smp 4` several runs, on the X99 and
  the ASUS. Measured as 2a was (`--cpu host,+invtsc`, 3 x
  `syscall_bench`, `lockstat -i 6`) against 2a's 1.7-2.0 M takes and
  0.44-0.57 s waited.

**Measured, built** (KVM, four vCPUs, `--cpu host,+invtsc`, 3 x
`syscall_bench`, `lockstat -i 6`; 97f76a90 run alongside as the
baseline, the same command):

| | 2a (97f76a90), 2 runs | 2b, 3 runs |
|---|---|---|
| scheduler lock(s): takes | `sched` 1.70-1.79 M | `rq1`-`rq3` 1.88-1.91 M together; `rq0` ~130; `rt` ~200 |
| ...of which waited | 58-60% | 11-12% |
| ...time waited in 6 s | 0.40-0.44 s | 49-55 ms |
| kernel lock: takes, waited | 208-303 k, 0.19-0.34 s | 182-225 k, 77-112 ms |
| `yield` (median of the copies) | 531 ns | 162 ns |
| round trip (two processes, two pipes) | 6.46 us | 5.58 us |
| `spawn hello` | 204 us | 128 us |
| a 64 KiB `/tmp` read | 8.8 us | 4.5 us |

`getpid`, `lseek` and the clock rows are unchanged. The `switch` row is
derived (round trip less two `pipe 1 B`), so a faster pipe reads as a
slower switch there.

Proven: the full KTEST suite under TCG, four vCPUs, 10 runs green once the
fixes below were in, 3 of them on the final build (plus 2 with `option
lockdep`, its scheduler classes reporting nothing and the switch-time
invariant check run and clean);
the invariant check RED with `nr_movable` left unadjusted in
`slot_set_stopped()`; the piled-on-one-queue KTEST RED with
`kernel.sched.steal` and `kernel.sched.balance` both off, green with
either one on; the race tests (`-k race_test`, eleven of them) 5 x TCG
and 2 x KVM on four cores; the ring-3 suite on four cores; `kvm_soak.py
--cpus 4 -n 10` clean. Found on
the way: `pick_next()` dropped its own walk's answer on an AP (the APs
ran nothing from their queues); a lock-free count of a thread group saw
a rotation's READY-then-RUNNING gap and freed a thread still running,
which then kept the disk lock frozen (the hang the suite showed 2 runs in
3); and the call IPI on an AP in ring 3 never had its EOI -- in main's
code too, reached once 2b put ring-3 work on core 1 during the `smp`
KTESTs (`idt.c`). Found after, under KVM: the `smp` suite deadlocked 15
runs in 16 (main: 0 in 6) -- with every process on an AP, an AP was
often in its interrupt path's wait for the kernel lock when the lseek
KTEST's hammer call arrived, ran it nested there holding its ticket,
and the hammer looped until core 0, queued behind that ticket, said
stop. The hammer is a bounded burst now (`docs/conventions/kernel.md`,
"An `smp_call()` function never waits on another core"): 8 in 8 green,
and red on `moved` with the lseek's table lock dropped before its seek. And
`sleep_test` failed every run (a 1 ms sleep 19-41 ms late, main on time):
only the boot core's one-shot fires sleepers, and idling tickless it had
armed for the deadlines it saw -- with every process on an AP, every
sleep was set where it could not see it. A park on an AP with an earlier
deadline kicks the boot core now (`deadline_reaches_bsp()`,
`clockevent_armed_ns()`): 6 in 6 on time. Linux arms the sleeper's own
core's hrtimer; per-core timers for sleepers are the shape if a kick per
sleep ever measures.

The plan as written:

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
   without holding the kernel lock -- BUILT 2026-11-21 as one run-queue
   lock and an AP's lock-free events (above).
5. Everything else, when something measures it.

**Lock-order checking comes with it** (2026-11-21, `option lockdep`,
`docs/lockdep-design.md`): every path that leaves the kernel lock starts
relying on the order its own locks are taken in, and lockdep finds an
inversion from one run that takes them both ways, at TCG speed.

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
