# w16: running 16-bit Windows programs

A staged plan for `/bin/w16`, a ring-3 program that loads a Windows 3.x
executable (the NE format), runs its 16-bit code on an emulated CPU, and
implements the Win16 API it calls -- KERNEL, USER, GDI -- on top of
toywm. It answers "can toy-os run a Windows 3.11 program, and what is
the smallest honest route there?"

**STAGES 0-2 ARE BUILT (2026-10-18)**: `w16 --info` reads a program and names its imports, the CPU interpreter matches the host's own CPU on random instructions (`tools/cpu16_hostcheck.py`), and `w16 /usr/share/w16/hello.exe` opens a Windows 3.1 window in toywm that paints, moves, resizes and closes. **Since 2026-10-19** it minimizes, maximizes and opens its control menu (click the box, or Alt+Space), all through `WM_SYSCOMMAND` so a program may refuse, and a program that faults or calls something w16 lacks shows 3.1's Application Error box and leaves a crash report naming why (`/usr/share/w16/fault.exe`). Chosen that day: the emulated
CPU in ring 3 (W1 below), the Windows 3.1 look all the way (L3 on the
"Win16 windows in toywm" mockup canvas), the code in `userland/w16/`,
and the name **w16** -- not WOW, which is Microsoft's term. "Win16" is
used only as the descriptive name of the API.

## The finding that shapes the whole plan

**The CPU is the easy part; the API is the project.** A Win16 program
is 16-bit protected-mode code: segments, far calls, no paging, no
virtual-8086 mode. An interpreter for that is a few thousand lines, and
speed does not matter -- this software was written for a 25 MHz 386.
What the program CALLS is the size of the work: USER and GDI export
several hundred functions between them, with window classes, messages,
painting, menus, dialogs and the standard controls behind them. Every
route below except emulating a whole PC pays that cost in full, and a
native CPU does not make it smaller.

## What real systems do

- **Windows NT, 32-bit (WOW).** Win16 code ran natively inside NTVDM,
  one shared address space for all 16-bit tasks, with USER and GDI
  thunked onto Win32. **64-bit Windows dropped it**: NTVDM needs
  virtual-8086 mode, which long mode does not have.
- **Wine on Linux.** Runs the 16-bit code natively in LDT segments
  (`modify_ldt`), its 16-bit DLLs thin wrappers over its Win32 ones.
  Linux carries `espfix64` because an `iret` to a 16-bit stack segment
  leaks the upper bits of the kernel stack pointer.
- **otvdm/winevdm on 64-bit Windows.** Wine's 16-bit layer with an
  EMULATED CPU, because the native route is closed there.
- **DOSBox.** Emulates the whole PC; real Windows 3.11 runs inside it.

## The routes, and the case against each

| | Route | For | Against |
|---|---|---|---|
| **W1** | Ring-3 process, emulated CPU, our own KERNEL/USER/GDI (otvdm's shape) | No kernel change; sandboxable; a fault in the program is a fault in a user process | The whole API is ours to write |
| W2 | Native 16-bit segments (Wine's shape) | Speed | A per-process LDT, an espfix, a 16-to-64 call bridge: kernel attack surface for software that does not need the speed; the API work is unchanged |
| W3 | A whole PC emulated in a window (DOSBox's shape) | Runs real Windows 3.11, so the most compatible | One window with Program Manager inside, nothing integrated; a C++ runtime to port DOSBox, or a PC emulator to write; needs Microsoft's disks |

**W1 was chosen.** Its own risk is breadth: the first real program will
call something unimplemented, so an unimplemented import must fail
LOUDLY and by name (below), never as a silent zero.

## How it fits together

```
  a Win16 program (NE .exe, untrusted)
        |  16-bit code, far calls to imported ordinals
  +-----v------------------------------------------------+
  | /bin/w16  (ring 3, pledged)                          |
  |  TASK thread                                         |
  |   ne.c       the NE format                           |
  |   task16.c   loading, the imports, the message queue |
  |   cpu16.c    the interpreter: 8086/186/286 integer,  |
  |              a selector table instead of an LDT      |
  |   kernel16.c user16.c gdi16.c  the API, in C         |
  |  MAIN thread (uapp)                                  |
  |   frame16.c  the 3.1 caption, frame and buttons (L3) |
  |   w16.c      input -> messages, client pixels shown  |
  +-----|------------------------------------------------+
        |  uapp: one window, pixels drawn by w16
  +-----v------+        toywm: an UNDECORATED window,
  |   toywm    |  <---  a client-started move/resize   [NEW]
  +------------+
```

- **Selectors, not an LDT.** The interpreter keeps its own table:
  selector -> (host base, limit, code/data, present). A segment load
  checks it, so a bad selector is a #GP the interpreter raises, which
  ends the program with a crash report naming CS:IP -- never a host
  fault.
- **Imports are thunk selectors.** A FAR_ADDR relocation to
  `USER.41` (CreateWindow) points at a reserved selector whose offset
  names the function; a far call into it leaves the interpreter loop
  and runs the C implementation against the 16-bit stack (Pascal
  convention: arguments pushed left to right, the callee pops).
- **Callbacks re-enter the CPU.** DispatchMessage calls the program's
  window procedure: the C side pushes a far return to a sentinel
  selector and runs the interpreter until it returns there.
- **An unimplemented import ends the program, NAMING it**: `w16: FILE:
  Windows call failed at 1:00bb -- USER.123 (Foo) is not implemented`
  on stderr, exit status 1. The first run of a new program prints the
  next thing on its to-do list.
- **Two threads.** The program -- interpreter, message loop, GDI -- runs
  on a TASK thread, so a modal loop inside a window procedure simply
  blocks there; the uapp MAIN thread owns the window, draws the frame,
  copies the client pixels and turns input into messages. They share
  the queue and the client buffer under one lock, and the task wakes
  the main thread with `uapp_post()`.
- **The sandbox** (`userland/bin/w16.c`): the program file, `/usr/share`
  and `/etc` read-only, the compositor's and fontd's beacons by exact
  name, and `stdio rpath shm window settings query`; `--info` keeps
  only the file and `stdio rpath`.
- **Far pointers are translated at the boundary.** An API taking an
  `LPSTR` gets segment:offset, checked against the selector's limit
  before C reads a byte -- the same bounds discipline as `ttf.h`.

## The look (L3)

The program's window is drawn the way Windows 3.1 drew it: a thick
frame, the navy caption with the control-menu box and the arrow
buttons, the menu bar, grey bevelled controls. toywm draws no chrome
for it. That needs two things toywm lacks today, both with a Wayland
shape to copy:

- **An undecorated window**, the client drawing its own frame --
  `xdg-decoration`'s client-side mode. The taskbar button, Alt+F4 and
  Alt+Tab-style focus still come from toywm.
- **A client-started move and resize** -- `xdg_toplevel.move` /
  `.resize(edges)`: on a press in its caption or frame the client asks
  the WM to take over the drag, so moving a 3.1 window feels like
  moving any other.

## Stages

0. **The format.** `tools/gen_ne.py` builds NE executables from
   NASM-assembled 16-bit segments, so the test corpus needs no foreign
   toolchain and no Microsoft binary; `w16 --info FILE` prints a
   program's segments, imports and resources (the to-do list for a
   target program). The NE parser pledges before the first byte.
1. **The CPU.** `cpu16.c`: the 8086/186/286 integer instruction set
   and protected-mode segment loads against the selector table. A host
   build runs the test programs; a **foreign check** compares it with
   the real CPU running the same code in a 16-bit LDT segment on the
   Linux host (`modify_ldt`), undefined flags masked.
2. **A first window.** The toywm additions above; KERNEL's task
   start-up, USER's classes, CreateWindow, the message loop and
   DefWindowProc, GDI's BeginPaint/TextOut; the L3 frame. A NASM
   "hello" opens a 3.1-looking window in toywm that moves, repaints and
   closes.
3. **Standard controls and menus** (BUTTON, EDIT, LISTBOX, STATIC,
   menus from resources), dialogs from resources, then files, INI
   profiles and timers.
4. **Real programs**, the user's own copies (none ship): run each,
   read its unimplemented list, implement, repeat. Several top-level
   windows per process need uapp to grow past one main window.

## Not planned

- Running DOS programs (V86 mode, a DOS API) -- a different layer.
- Shipping any Microsoft file. Programs and fonts are the user's own.
- 32-bit Windows (Win32s, PE): another API, another project.
