# w16: several Windows programs in one w16 -- a shared VDM

**Status: DESIGN ONLY (2026-11-15).** Nothing here is built; the
maintainer asked for this document before choosing whether, and how far,
to build it. `docs/w16-design.md` is the w16 this changes.

## The finding that shapes the plan

w16 runs ONE program per process (`docs/decisions/gui.md`, "A Win16
program starts another through the compositor, as a w16 of its own") --
Windows NT's "separate memory space" for every program. That was enough
for Program Manager, and is the reason a crash, a sandbox and a trace are
each one program's.

WinTune 2.0 (2026-11-15) is the first program it stops. Its chart control,
WTGRAPH.VBX, calls GSWDLL.DLL (Bits Per Second's Graphics Server), which
starts GSW.EXE with WinExec and then waits for GSW.EXE to register itself
**in GSWDLL's own data segment**. Windows 3.1 kept ONE copy of a DLL's
data for every program that loaded it, so the server's write was the
client's read. Under w16 each program has its own copy: the client never
sees the server, the control fails ("Failed to open Graphics Server"),
and VB3 refuses the form. No missing call explains it; the process
boundary does.

The same boundary is behind what the earlier decision already listed as
the cost: FindWindow of another program's window, DDE between programs,
a program started hidden as another's agent (Hearts' NETDDE.EXE). Each
was acceptable alone; a shared DLL is the first that stops a program
outright.

## What real systems do

- **Windows 3.1**: one address space, one LDT, cooperative tasks. A DLL
  is loaded once, its DGROUP shared by every caller (instance data is
  the program's, a DLL's is everyone's). One system message queue feeds
  per-task application queues; SendMessage to another task's window
  switches to that task and back.
- **Windows NT (WOW)**: every 16-bit program in ONE NTVDM by default --
  one thread per task, but only one runs 16-bit code at a time (a global
  lock keeps 3.1's cooperative model). "Run in separate memory space" is
  the opt-out, at the cost of exactly what WinTune needs.
- **Wine / otvdm**: the same shape as WOW -- the 16-bit tasks of a
  session in one process, one thread each, serialised by the Win16 lock
  (`_EnterWin16Lock`), DLL data shared.

Every system that runs 3.1 programs well shares by default; w16 is the
odd one out, by choice, for isolation.

## The routes

**A. A shared w16 -- a VDM (WOW's and Wine's shape).** One w16 process
hosts several programs, each a TASK: its own interpreter state, stack,
queue, PSP, current directory and DOS handles; the window tree, atom
table, selector table, global heap and DLL table are the VDM's. Only one
task runs 16-bit code at a time (a Win16 lock, as WOW and Wine do), so
the code that today assumes one program needs no fine-grained locking --
it needs its globals split into per-task and per-VDM. A DLL is loaded
once and its DGROUP shared: GSWDLL works as on 3.1. FindWindow, SendMessage
between programs and DDE come with it. WinExec from a task starts a task
in the same VDM instead of asking the compositor.

**B. Share only a DLL's data, across processes.** Map a DLL's DGROUP in
shared memory (`shm`) in every w16 that loads it. Smaller, but what a DLL
keeps there is not position-free: selectors (each w16 numbers its own),
window handles (each w16's own tree), far pointers into a program's
memory. GSWDLL keeps a window handle to post to. It would work for a DLL
that keeps plain numbers and fail silently for the rest.

**C. Leave the boundary.** WinTune stays at its Graphics Server; programs
that share DLLs stay out of reach.

Recommendation: **A**, staged so that the first stage alone already runs
WinTune; B is rejected for being a correctness gamble that looks like
support.

## Route A in detail

### What is per task, what is per VDM

Today `struct w16 g_w16` is the program, and 167 file-scope `g_`
statics across 58 files (measured 2026-11-15) hold the rest. The split:

| Per task (`struct w16_task`) | Per VDM (`struct w16`) |
|---|---|
| `cpu16` (registers, steps, JIT state), the stack | the selector table (`desc`), the linear arena |
| the message queue, key state, capture | the window tree (`wnd16.c`), the atom table |
| `dgroup` and its local heap | the global heap, the DLL table and their DGROUPs |
| the PSP, command line, current directory, DOS handles | drive C:, the INI cache, fonts, the clipboard |
| the module (`ne`), its instance, resources | the sound ring, hlpbroker, the sandbox |
| a trace's and `--stats`' attribution | the main (uapp) thread and toywm's windows |

Most of the 235 uses of `g_w16` reach VDM state and stay; the work is the
per-task column, and every `g_` static in `file16.c`, `res16.c`,
`task16.c` and the queue that is really a task's.

### Running several tasks

One host thread per task, as WOW and Wine have, with a VDM-wide Win16
lock held while a task runs 16-bit code or a call that touches VDM
state, and dropped where 3.1 yielded: GetMessage, PeekMessage with
nothing waiting, Yield, WaitMessage, a modal loop's wait. SendMessage to
another task's window hands the message to that task and waits for its
reply with the lock dropped -- 3.1's inter-task send. The interpreter's
tick (`CPU16_TICK_STEPS`) does not preempt: 3.1 did not, and a program
that never yields hung 3.1 too.

### Which programs share a VDM

- A program started by a program (WinExec, ShellExecute, Program
  Manager's items, LoadModule) joins its parent's VDM. That alone covers
  GSW.EXE.
- A program started from SaunaOS (File Manager, the Retro Launcher, a
  terminal) starts its own VDM -- today's behaviour -- unless it names
  the same drive C: as a running VDM and a setting asks to join (NT's
  default, offered later and not first).
- Programs in one VDM share drive C:, as every program on a 3.1 machine
  did; a program whose own settings name a different drive C: does not
  join.

### What it costs

- **Fate is shared.** A fault in one task ends the VDM, as an
  application error could take down 3.1, and as a shared NTVDM did. The
  crash report names the task that faulted and lists the others.
- **The sandbox is the VDM's**: its unveil list is the union of what its
  programs need, which is one drive C: and the tasks' own folders on it.
- **toywm sees one client.** Each task's windows must still be its own
  taskbar entries, with their own icons and titles: the main thread
  tells toywm which task a window belongs to (an app identity per
  window, which `WIN_REQ` does not carry today -- the one new protocol
  piece).
- **Scale.** The per-task split touches most of w16's 58 files; it is
  the largest w16 change since the window tree.

### Stages

1. **Tasks inside one w16.** The per-task / per-VDM split, a thread per
   task under the Win16 lock, WinExec starting a task in the same VDM,
   DLLs loaded once with one DGROUP. Proof: WinTune draws its Performance
   chart through GSW.EXE; every program w16 runs today still runs
   (`w16_gui_test.py`, the reference comparisons).
2. **Programs see each other.** FindWindow and EnumWindows across tasks,
   SendMessage and PostMessage between tasks, GetModuleHandle and
   GetModuleUsage counting every task.
3. **DDE between tasks.** DDEML and raw WM_DDE_* between programs in a
   VDM; Hearts' NETDDE.EXE is no longer refused.
4. **Joining from outside**: the setting that lets a program started from
   SaunaOS join a running VDM with the same drive C:.

## Not planned

- Route B (shared DLL data across processes): see above.
- Preemption between tasks: 3.1 had none, and the lock is what keeps
  every API free of fine-grained locking.
- 32-bit programs (Win32s): a different loader and API, not a task.
