# A JIT for w16's 32-bit code, and the executable memory it needs

A staged plan, in the shape `docs/trace-design.md` used. It answers "what
would it take to run a Win386 program's 32-bit code translated rather
than interpreted, and what must the kernel give a sandboxed process
first?" Picked 2026-11-15, after the quick wins below, as "quick wins,
then a JIT design to review". **STAGES 0 AND 1 ARE BUILT (2026-11-15):
Lighthouse runs 53 MIPS on the ASUS against 19 interpreted. Stage 2,
chaining blocks, is the next lever (measured below).**

## The findings that shape the plan

**Lighthouse is CPU-bound in w16's interpreter, and the gap is a few
times, not a few percent.** Measured on the ASUS (`<asus-ip>`, an i5 of
2015) with a timing build of w16, 2026-11-15: ~90% of the task thread's
time is the program's own instructions at ~12 MIPS; Windows calls,
drawing, DPMI and DOS together are under 10%. Sierra asked for a
486DX2/66 and recommended a Pentium -- roughly 40-50 MIPS of the kind
of code SCI runs -- so the laptop is 3-4x short.

**Two causes outside the interpreter were found and fixed first**, and
the numbers above predate both:

- the scheduler gave a lone busy process HALF the CPU (the idle kernel
  handed the CPU back to itself; `docs/decisions/kernel.md`, "An idle
  kernel's hand-over is a yield") -- a lone process's work doubled under
  KVM;
- `PeekMessage` slept a millisecond on every empty answer, ~7% of
  Lighthouse's time in play and a quarter at its menus;
- and the interpreter's small copies were calls (`-ffreestanding`), ~6%.

**Re-measure before building any of this**: if Lighthouse is playable on
the ASUS after the scheduler fix, the JIT is a "later", not a "next".

**The interpreter has little left to give.** `tools/bench/cpu16_bench.py`
runs 89 MIPS on the dev host; its perf profile is `step()` itself
(~52%), `alu`, `decode_modrm` and `rm_get` -- decode and dispatch, which
an interpreter pays on every instruction by definition. Bochs, an
interpreter tuned for two decades, is the reference for how far that
road goes; DOSBox ships a dynamic recompiler beside its "normal" core
for exactly the 32-bit games this is about, and QEMU translates
everything (TCG).

**SaunaOS could map executable memory -- too easily -- until stage 0.**
`SYS_MMAP` took `PROT_WRITE | PROT_EXEC` on an anonymous mapping, the
weakest shape there is, and w16 is the worst process to give it to: a
Win16 program is untrusted CODE and w16 pledges. Stage 0 closed it the
way OpenBSD does (`docs/decisions/kernel.md`, "W^X for every mapping, and
executable memory a program wrote needs `prot_exec`").

## What real systems do for a JIT's memory

- **Flip with `mprotect()` / `VirtualProtect()`** -- write the code RW,
  then make it RX; Linux and Windows, V8's write-protected code space.
  One syscall per batch of new code, and a TLB flush.
- **Map one object twice** -- an RW view to write and an RX view to run,
  at different addresses; .NET's W^X mode and libffi's closures do this
  with a memfd. No flips, but the RW alias is a standing target if its
  address leaks.
- **A per-thread switch** -- Apple's `MAP_JIT` with
  `pthread_jit_write_protect_np()`, which flips the whole region's
  permission for the calling thread only.
- **Refuse W|X entirely, and gate exec on a promise** -- OpenBSD.

**Recommended for SaunaOS: `mprotect()`, W^X enforced for everyone, and
a `prot_exec`-shaped pledge promise.** It is the smallest kernel change
(the region list already carries `prot`, and the fault path already maps
NX from it), it is the guard-page item's prerequisite too, and enforcing
W^X closes the hole above for every sandboxed process, not just w16.
The dual mapping needs nothing new from the kernel beyond shm, but keeps
a writable alias of the code alive.

## The plan

### Stage 0 -- executable memory, W^X (kernel) -- BUILT 2026-11-15

- `SYS_MPROTECT(addr, len, prot)` on anonymous memory: regions split at
  both ends and folded back with same-prot neighbours, present leaves'
  W and NX rewritten and flushed (`mmap_protect()`, `vmm_protect_user_page()`);
  `munmap` spans the regions it leaves.
- W|X is -EACCES from `SYS_MMAP`, `SYS_MPROTECT` and the ELF loader; none
  of the 3,786 ELFs the build staged had a W+X segment.
- The `prot_exec` promise (`PROMISE_PROT_EXEC`): anonymous `PROT_EXEC`, or
  a file's outside `/bin`, `/lib`, `/tests`, kills a process that gave it
  up with SIGSYS.
- KTESTs (`kernel/mm/mprotect_test.c`, each seen red under a mutation):
  the flips, a page faulted in after the call, holes and files refused
  with nothing changed, a fork's shared frame never made writable and an
  RX page never left COW, the ceiling. Ring 3: `mmap_test.c` runs code
  written RW then made RX, a store into it and a call into RW memory
  fault; `sandbox_test.c` the promise both ways.

### Stage 1 -- translate a basic block (w16) -- BUILT 2026-11-15

`userland/utils/w16/jit16.c` (the arena, the block table, the
dispatcher, invalidation, the call-out) and `jit16_x86.c` (decoding and
emitting). What it became, where the plan below left room:

- **Blocks of 32-bit flat code, each instruction re-emitted as itself**
  -- a guest register is its 32-bit slot in `struct cpu16` (the
  registers were re-laid as one slot each first), a memory operand
  `[base+rax]` once checked -- with lazy flags as planned.
- **The CALL-OUT, which the plan did not have**: an instruction the
  translator lacks is run by the interpreter from inside the block, and
  the block goes on unless that loaded a segment register, dropped
  translated code or faulted -- QEMU's helper call, with the interpreter
  as the helper. Transfers of control, far calls, interrupts and the x87
  still end a block (they can run the program again, and a nested run
  could flush the arena under the waiting block).
- Self-modifying code by 64-byte LINES rather than whole pages: a page
  holds code and the data next to it; a page whose blocks keep being
  dropped is left to the interpreter (DOSBox's answer).
- **The dispatcher returns to the run loop at every tick** (64K
  instructions), however many blocks are left: the tick plays a wave
  buffer to its end and marks it done, and Lighthouse polls for that in
  a loop that calls nothing (New Game clicked early, Esc in the intro) --
  without the return it spun in translated code with a black screen.
- `--stats FILE`: time by module (exclusive), MIPS, and where runs fell
  out of translated code -- the measurement below.

**Measured.** The bench (`tools/bench/cpu16_bench.py --jit`, the dev
host): 88 -> 829 MIPS, every instruction translated. Lighthouse, its
title screen then New Game's intro, MIPS over its own time:

| | interpreted | JIT |
|---|---|---|
| dev host, KVM | 61 | 159 |
| the ASUS, bare metal | 19.3 | 53.3 |

On the ASUS, 85% of its instructions ran in blocks and 1.9% of those were
called out; a run left translated code mostly at a far call into Windows
(`FF /3`); the rest is its 16-bit thunk glue, which stage 1 leaves to the
interpreter. Blocks average ~5.3 instructions, and the ASUS ran 680
million of them in 75 s: the dispatcher's lookup between every two is
now the largest cost, which is stage 2.

**Picked 2026-11-15:** a JIT rather than native execution (below);
`mprotect` flips; 32-bit FLAT code only -- CS a 32-bit segment and DS, ES
and SS one flat selector, anything else (FS/GS included) exits; cpu16's
registers re-laid as 32-bit FIRST, a mechanical change guarded by
`cpu16_bench.py`'s checksum (`EAX` is `rh[0] << 16 | r[0]` today, which
translated code would pay a shift-or for); ONE code arena, bump-allocated
and flushed whole when full (QEMU's `tb_flush`), with flushes and
invalidations counted in the timing build so per-page reclaim (DOSBox's
free cache blocks) is added only if a program churns; a block translated
after ~50 entries (LuaJIT's hot-loop count is 56; `--jit-threshold N` to
measure); a `jit` key in w16prefs' table, ON by default with `jit=off`
per program and `--no-jit` -- no Retro Launcher control this stage.

- Hot-path discovery: the interpreter counts entries per block start
  (a small hash on linear address); past a threshold the block is
  translated.
- A block of 32-bit flat code becomes x86-64: guest registers in
  `struct cpu16` addressed off a fixed host register, guest memory
  through the segment's cached linear base and limit (the descriptor
  cache is already the interpreter's), a bounds check per access that
  exits to the interpreter rather than faulting.
- **Flags lazily**: most x86 instructions set flags nobody reads. The
  translator keeps the last result and its operation, and computes a
  flag only where a `jcc`, `adc`, `pushf` reads it -- the largest single
  win DOSBox's and QEMU's translators get.
- Anything the translator does not handle ends the block with an exit
  to the interpreter: correctness first, coverage grows by profile.
- **Self-modifying code**: Win386 code and data alias the same linear
  memory, so a store may hit translated code. A per-page "has
  translations" byte map over the arena; the interpreter's stores and
  translated stores test it and invalidate the page's blocks -- and so
  does `cpu16_ptr32(..., write)`, which every one of the C side's writes
  into program memory goes through (a file read over old code). QEMU's
  user-mode answer, write-protecting the page and catching SIGSEGV, needs
  the fault address, which SaunaOS's `struct sigframe` does not carry.

### Stage 2 -- chain blocks

- A direct jump or call patched to the next translated block; an
  indirect one through a small hash. Return-address prediction is a
  later refinement.

### Stage 3 -- only if a program needs it

- 16-bit code (most Win16 programs spend their time in USER and GDI,
  which are w16's own C), x87 (Lighthouse's SCI uses little).

## The oracle

Every stage keeps the interpreter as the reference:

- `tools/hostchecks/cpu16_hostcheck.py`'s 32-bit cases run through the
  translator as one-instruction blocks, and its fuzz as random blocks,
  against the host CPU and the interpreter both;
- `tools/bench/cpu16_bench.py`'s checksum must stay equal, and its MIPS
  is the number to beat;
- a `jit=off` program setting (the `/etc/win16/<program>.conf` file
  Lighthouse already has) and a `--no-jit` switch, so a program the JIT
  breaks still runs, and a bug can be bisected to it.

## What it is expected to buy

DOSBox's dynamic core runs 32-bit game code several times faster than
its normal core; a basic-block translator with lazy flags and chaining
is the same shape. A 3-5x gain would put the ASUS at Lighthouse's
recommended machine. NOT established: that is the expected range from
the systems named, not a measurement here.

## Not this plan

- **More cores.** Programs run on every core (`docs/smp-design.md`,
  stage 5), so w16 and the compositor can already land on different
  cores; one program's interpreter is still one thread. Splitting w16's
  drawing off its CPU thread is a separate lever.
- **Running Win386 code natively in a ring-3 32-bit segment** (weighed
  and declined 2026-11-15). x86-64 long mode has compatibility mode, so
  32-bit code CAN run directly -- Wine does on Linux, and
  `tools/w16/cpu16_check.c` does through `modify_ldt` -- but a
  compatibility-mode segment reaches only linear addresses below 4 GiB,
  and in every SaunaOS process that is PML4[0], the kernel's identity
  map (`kernel/uaddr.h`: user memory starts at 512 GiB). So it needs a
  higher-half kernel first, then per-process LDTs switched on every
  context switch, and every thunk and `INT 21h` turned from a #GP into a
  signal w16 handles. Faster than any JIT, and a different project.
