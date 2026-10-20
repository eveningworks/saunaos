# w16: running 16-bit Windows programs

A staged plan for `/bin/w16`, a ring-3 program that loads a Windows 3.x
executable (the NE format), runs its 16-bit code on an emulated CPU, and
implements the Win16 API it calls -- KERNEL, USER, GDI -- on top of
toywm. It answers "can toy-os run a Windows 3.11 program, and what is
the smallest honest route there?"

**STAGES 0-2 ARE BUILT (2026-10-18)**: `w16 --info` reads a program and names its imports, the CPU interpreter matches the host's own CPU on random instructions (`tools/cpu16_hostcheck.py`), and `w16 /usr/share/w16/hello.exe` opens a Windows 3.1 window in toywm that paints, moves, resizes and closes. **Since 2026-10-19** it minimizes, maximizes and opens its control menu (click the box, or Alt+Space), all through `WM_SYSCOMMAND` so a program may refuse, and a program that faults or calls something w16 lacks shows 3.1's Application Error box and leaves a crash report naming why (`/usr/share/w16/fault.exe`). **Minesweeper, from the maintainer's own Windows 3.11, is complete** -- it plays, and its menu bar, Best Times, Custom Field, About and Help all work (stage 3 below). **Notepad, too, is complete since that evening** -- it opens, edits, finds, word-wraps, uses the toy-os clipboard and saves, through a window tree (child windows), w16's EDIT, LISTBOX and COMBOBOX, KERNEL's file calls on drives C: and Z:, and COMMDLG's Open, Save As and Find in the 3.1 look ("Win16 Notepad" canvas, O1). Chosen that day: the emulated
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
  |   file16.c   drives C: and Z:, the file calls        |
  |   wnd16.c    the window tree: every HWND, its pixels |
  |   ctl16.c edit16.c list16.c scroll16.c               |
  |              w16's own classes (BUTTON, EDIT...)     |
  |   dialog16.c commdlg16.c  dialogs, the common ones   |
  |  MAIN thread (uapp)                                  |
  |   frame16.c  the 3.1 caption, frame and buttons (L3) |
  |   dlgwin16.c a dialog's frame, its own toywm window  |
  |   w16.c input16.c  input -> messages, pixels shown   |
  +-----|------------------------------------------------+
        |  uapp: a window per top-level HWND, drawn by w16
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
  blocks there; the uapp MAIN thread owns the toywm windows, draws their
  frames, copies the client pixels and turns input into messages for the
  window under the pointer or with the focus. They share the queue and
  the client buffers under one lock, and the task wakes the main thread
  with `uapp_post()`. EVERY CLIENT PIXEL IS PAINTED ON THE TASK THREAD,
  w16's controls included, because an EDIT's text lives in the
  program's memory, which the task may move.
- **One window tree** (`wnd16.c`): the main window, its dialogs and every
  child in one table with parents. A child has no pixels of its own --
  it paints into its top-level window's buffer at its place, clipped to
  its ancestors -- and w16's classes are C window procedures reached by
  SendMessage like a program's. `docs/decisions/gui.md`, "A Win16 window
  tree".
- **Two drives** (`file16.c`): C: is the Win16 folder, Z: the whole
  toy-os tree, names matched without case; a program starts in its own
  folder, and the common dialogs browse both.
- **The sandbox** (`userland/bin/w16.c`): the whole tree read-only (drive
  Z:), `/home`, drive C: and `/var/lib/win16` read-write, the clipboard,
  the compositor's and fontd's beacons by exact name, and `stdio rpath
  wpath cpath shm window settings query`; `--info` and `--check` keep
  only the file and `stdio rpath`. Settings are read BEFORE the
  sandbox, which is what decides drive C:.
- **Settings** (`userland/w16/w16prefs.c`): `/etc/win16.conf` the
  defaults, `/etc/win16/<program>.conf` one program's overrides, keyed by
  its path -- the shape of Windows' per-executable compatibility
  settings. Retro Launcher edits both and reads programs
  only through `w16 --check`, so the one parser of hostile files stays
  sandboxed.
- **Names and kinds** (`userland/w16/w16_names.c`, generated by
  `tools/gen_w16_ordinals.py` from Wine's `.spec` files): every export
  of KERNEL, USER, GDI and the other DLLs a 3.x program imports, each
  with what it is for (Dialogs, Files, Clipboard...). `w16 --check`
  prints every import with both, and the launcher's calls window
  (`userland/retrolauncher/calls.c`) is that list -- what a program needs next,
  by module or by kind.
- **A stand-in is not a whole call.** A handler that answers without
  doing what Windows did (MessageBeep silent, PrintDlg with no printer,
  the SHELL drag calls) is listed in `task16.c`'s `PARTLY` table with
  what it does instead, which `--check` prints as `call partly` and the
  launcher shows as Partly -- so "has every call" never overstates how
  far a program runs. A handler that stops being one leaves the table.
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

### Comparing with real Windows 3.11

**The reference is a real Windows for Workgroups 3.11, measured, never
remembered.** It runs in QEMU on MS-DOS 6.22 (640x480 VGA, 16 colours,
no network card, no printer), built from the maintainer's own disks in
the local-only assets folder (`local_info.txt`), which also holds its
driver script (`win311vm.py`: `start`, `key`, `type`, `shot [--to]`,
`floppy`, `stop`) and a screenshot of every install step. None of it
enters this repo.

**How:**

1. **The same data on both.** One test file -- a heading, a blank line,
   a line longer than the window, punctuation -- copied onto the
   reference's C: (mtools, the VM stopped) and into `/home/Win16`.
2. **A fresh toy-os image every run.** toywm remembers each app's last
   size (`/etc/windows.conf`), so a reused image opens the program at
   whatever size it was left, not at its own default.
3. **The same keystrokes on both, one state per screenshot.** Keys, not
   the mouse: the reference's pointer is relative and Windows
   accelerates it, so a click cannot be aimed.
4. **Close programs by APP ID between runs, never by title** -- a wrong
   title leaves the old window open and the next run's keys go into it
   (Minesweeper's Esc is its boss key: it minimises).
5. **w16 at 1x** (`scale=1`): it reports a 640x480 screen, as the
   reference has, so a default-sized window is the same size on both
   and a pixel is a pixel.
6. **Diff, then dump.** Crop each pair to the window, take a PIL
   `ImageChops.difference` and count differing pixels per region
   (caption, menu bar, each scroll bar, client) -- the regions that are
   not zero say where to look. Then dump the region as a character map,
   one character per pixel class (`#` black, `.` white, `o` face grey,
   `+` shadow grey, `N` navy), from both, and compare row by row.
7. Fix, re-shoot on a fresh image, diff again; then the GUI test
   (`w16_gui_test.py`), whose expected colours and frame sizes are
   3.11's too.

**What to compare** -- each state, each of these:

| Area | Look at |
|---|---|
| The window as it opens | its default size; frame rows and columns; the corner notches; the caption's height, title, control-menu box, minimise/maximise buttons |
| The menu bar and each menu | bar height and its line; item rows, separators, check marks, grey items, accelerators; which item is lit when it drops from the keyboard |
| Scroll bars | arrows (head and stem), the button bevel, the thumb, the trough; a bar with nothing to scroll; where the bar meets the frame and the menu bar |
| Each dialog (Open, Save As, Find, About, the program's own) | outer size (it gives the base units), frame and caption, every control's font and place, list contents and case, combo buttons, drive and folder icons, which item is selected |
| The control menu (Alt+Space) | items, which are grey, which is lit |
| Text | the client's font, its row height and left margin; the caret; a selection's colours |
| The keyboard | Alt+letter, arrows across the bar, Esc from a menu (once, then again), F10, Enter on a grey item |

**3.11 at VGA, measured -- what w16 now follows:**

| Thing | 3.11 |
|---|---|
| Caption | 18 rows and a black line (W16_CAPTION 19) |
| Menu bar | 18 rows and a black line (W16_MENU_H 19) |
| Main frame | black, 2 grey, black: 4; notches 22 from each outer corner, on the caption buttons' edges |
| Dialog frame | black and 4 of navy all round; a caption the client's width, edged white on its top and sides, its black line stopping at the white |
| Control-menu box | 18 square; its bar 13 by 3, 2 in, 7 down, a shadow 1 down-right |
| Scroll-bar arrows | head 7 wide, stem 3 by 3, 7 long, centred; grey with a white shadow when there is nothing to scroll, over a white trough with no thumb |
| A top-level window's scroll bars | share their outer edge with the frame's inner line and the menu bar's line -- no second black line |
| Button bevel | white top and left, one deep (two on a push button); shadow two deep, the inner line stopping at the white |
| Push button | its label centred in the face left of the edge's shadow; a disabled default button loses its thick edge; disabled text a 50% dither (GrayString), not grey |
| Check box, radio button | 13 by 13, the label 5 past it and a row below centre; the circle 3.11's stepped one, its dot 7 wide |
| Group box | its frame from half a line down, the label a quarter line down, the gap 7 in |
| Mnemonic underline | a row below the baseline (ascent + 1), a blank row between it and the letters |
| Menu bar | titles from the bar's left edge, each 8 in from its box |
| List box | whole rows only, the foot moved up (unless LBS_NOINTEGRALHEIGHT); rows of folders or drives 16, the picture's height |
| Combo box | shut, the font's height and 7; its button 17 wide, the arrow with a stem over a bar |
| Colours | navy 0000A8, face grey C0C7C8, shadow grey 87888F (the VGA palette; ctl16.c's `w16_vga()`) |
| Base units | System font 8x16; a template's `FONT 8, "Helv"` 7x13, drawn bold |
| Units to pixels | an x from the template's own position, each end truncated; a y rounded; a width or height rounded (COMMDLG.DLL's templates: Open 264x134 at 36,24 is 462x218; Find 236x62 at 30,73 is 413x101) |
| Font cells | System 16, Helv 8 13, Fixedsys 15 |
| Edit text | a multi-line edit's at the client's left edge; a one-line field's 2 in |
| CW_USEDEFAULT | 632x408 on 640x480 |
| COMMDLG | names and the filter in lower case; "c: label" with a drive icon; the current folder not selected |
| Menus | dropped from the keyboard: the first item lit even when grey; Esc goes back to the bar, a second Esc out |

**Fonts and pictures are the user's own** (the decision "Win16's fonts
and pictures come from the user's own Windows files"): with 3.11's
`.FON` files and COMMDLG.DLL in drive C:'s `WINDOWS\SYSTEM`, Notepad's
text, caption and menus diff to ZERO pixels and Find to the caret and
one label's pixel; without them, toy-os's stand-ins at 3.11's cells.

**Still different, known:** the menu bar's title spacing (3.11's boxes
are not the text plus a constant: File, Edit, Search, Game measure 40,
40, 64, 54); a list box's top a row higher in 3.11; a disabled push
button's label a pixel right; the control menu's Move and Size (not
built) and Switch To; a dialog's place (toywm centres it; 3.1 used the
template's position); ShellAbout's layout; the other dark colours of
the palette, whose mapping is not yet measured.

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
   profiles and timers. **Begun 2026-10-19, aimed at Minesweeper, which
   PLAYS**: the NE resource table (`ne.c`, fuzzed), resources and
   LoadString (`res16.c`), the global heap (a selector a block, its
   handle the selector) and the local heap in DGROUP (`mem16.c`), GDI's
   objects -- DCs, bitmaps, pens, brushes -- with every ROP2 and ROP3
   from Windows' truth tables (`gdi16.c`, `blit16.c`), timers posted by
   the main thread's tick, the mouse capture, filtered Get/PeekMessage,
   MSG.pt, MoveWindow, and the PSP's environment a C runtime scans.
   **A display bitmap is 32-bit host pixels** (chosen that day over 3.1's
   device formats): a program treats a device bitmap as opaque, and a
   1-bit one keeps that it is one for the mask conversions. The INI
   calls read and write real files on drive C: (2026-10-19). **Menus and
   dialogs, 2026-10-19 -- Minesweeper complete**: the menu bar from a
   MENU resource (`menubar16.c`, drawn under the caption, its popups
   menu16.c's), TranslateAccelerator, and the dialog manager
   (`dialog16.c`): DialogBox from a DIALOG resource, BUTTON, STATIC and
   EDIT as w16's own classes, 3.1's dialog keyboard, MessageBox and
   ShellAbout; a dialog is an owned, modal toywm window of its own
   (`dlgwin16.c`, uapp's pixel dialog windows) -- `docs/decisions/gui.md`,
   "A Win16 dialog is an owned window w16 paints". KERNEL's constants `__AHSHIFT`, `__AHINCR` and `__WINFLAGS`
   are patched at load; SOUND's voices play nothing yet. **Notepad,
   2026-10-19**: ONE WINDOW TREE (`wnd16.c`) -- every HWND, a child
   painting into its top-level window's buffer on the task thread --
   with w16's classes as C window procedures (`ctl16.c` BUTTON and
   STATIC, `edit16.c` EDIT with its text in the program's local heap,
   `list16.c` LISTBOX and COMBOBOX, `scroll16.c` a window's scroll
   bars), IsDialogMessage and modeless dialogs, a local heap that grows
   and has real moveable handles (`mem16.c`), KERNEL's and DOS's file
   calls on drives C: and Z: (`file16.c`), and COMMDLG (`commdlg16.c`)
   -- `docs/decisions/gui.md`, "A Win16 window tree". **Fractional
   scales** (1.25x to 3x, `scale16.c`): a program's world is still drawn
   at 1x and scaled once, by the method its settings name -- Sharp,
   Pixels or Smooth. `w16 --trace FILE` logs every call into KERNEL, USER and
   GDI (Wine's `+relay`), which is how a stage-4 program's next stop is
   found.
4. **Real programs**, the user's own copies (none ship): run each,
   read its unimplemented list (the launcher's calls window), implement,
   repeat. A second top-level window that is not a dialog still needs
   uapp to grow past one main window.

## Not planned

- Running DOS programs (V86 mode, a DOS API) -- a different layer.
- Shipping any Microsoft file. Programs and fonts are the user's own.
- 32-bit Windows (Win32s, PE): another API, another project.
