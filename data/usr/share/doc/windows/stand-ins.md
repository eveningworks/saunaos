# Calls w16 answers only partly

**Category:** Windows programs

## Description

`w16` runs a Windows 3.x program by answering the Windows calls it
makes. Most are answered in full. A few are **stand-ins**: w16 answers
them the way Windows did when the thing asked for was not there -- no
printer, no other program, no Help viewer -- so the program carries on
rather than stopping. Retro Launcher marks these **Partly**, and its
**Why?** opens the section below for the call.

Each section says what Windows 3.1 did, what w16 does instead, why it
stops there, what a program may notice, and what would make it whole.

Every program runs in a `w16` of its own. Where Windows 3.1 had one
system that all programs shared, each program here sees a Windows with
nothing else running in it -- which is why several of these calls find
no one.

## COMMDLG.20 PrintDlg

### What Windows 3.1 does

Shows the Print dialog -- which printer, which pages, how many copies --
and hands the program a device context for the printer chosen, to draw
the pages into.

### What w16 does

Answers as Windows did on a machine with no printer installed: FALSE,
and `CommDlgExtendedError` says PDERR_NODEFAULTPRN. No dialog opens.

### Why only that

toy-os has no printing yet: no spooler and no printer drivers, so there
is nothing for the dialog to offer.

### What a program may notice

Its own message that no printer is installed, when you choose Print.

### What would make it whole

A printing system in toy-os, then this dialog over it. On the roadmap:
"w16: printing".

## DDEML.2 DdeInitialize

### What Windows 3.1 does

Registers the program with the DDE management library, which lets
programs talk to each other: one offers a service, another connects to
it and asks for data or sends commands.

### What w16 does

Succeeds and gives the program an instance id, so it can go on to name
services and make string and data handles. Nothing it does reaches
another program.

### Why only that

There is no other program to talk to: every program runs in its own
w16, and there is no DDE between them, nor NetDDE between machines.

### What a program may notice

Nothing at this call. Hearts uses DDE for network play: the dealer's
game starts, but a player finds no dealer to join.

### What would make it whole

DDE between w16 processes, and NetDDE over the network for Hearts. On
the roadmap: "w16: Hearts over the network".

## DDEML.7 DdeConnect

### What Windows 3.1 does

Opens a conversation with the program that offers a service and topic,
on this machine or -- through NetDDE -- on another.

### What w16 does

Finds no one: returns no conversation, and `DdeGetLastError` says
DMLERR_NO_CONV_ESTABLISHED, exactly as when nobody offers the service.

### Why only that

No other program is there to answer (see DdeInitialize).

### What a program may notice

A Hearts player cannot join a dealer; the program says it could not
connect.

### What would make it whole

As DdeInitialize: on the roadmap, "w16: Hearts over the network".

## DDEML.11 DdeClientTransaction

### What Windows 3.1 does

Sends a request, a poke, an advise or a command over an open
conversation, and returns the data or the result.

### What w16 does

Fails with DMLERR_NO_CONV_ESTABLISHED: there is never a conversation to
send in.

### Why only that

DdeConnect never opens one (see DdeInitialize).

### What a program may notice

Nothing on its own: a program only gets here after a connection it
never got.

### What would make it whole

As DdeInitialize: on the roadmap, "w16: Hearts over the network".

## DDEML.16 DdeGetData

### What Windows 3.1 does

Copies the bytes of a DDE data handle -- what another program sent --
into the program's memory.

### What w16 does

Copies nothing and answers 0 bytes.

### Why only that

No data ever arrives from another program (see DdeInitialize).

### What a program may notice

Nothing: a program asks only for data it was sent.

### What would make it whole

As DdeInitialize: on the roadmap, "w16: Hearts over the network".

## DDEML.27 DdeNameService

### What Windows 3.1 does

Announces a service by name, so other programs can connect to it -- a
Hearts dealer offering its game.

### What w16 does

Succeeds. The name is recorded nowhere, because no other program could
look for it.

### Why only that

See DdeInitialize.

### What a program may notice

A Hearts dealer waits for players who cannot reach it; start the game
and the computer plays the other hands.

### What would make it whole

As DdeInitialize: on the roadmap, "w16: Hearts over the network".

## GDI.38 Escape

### What Windows 3.1 does

Passes a device-specific request to a device's driver -- in 3.x mostly a
printer's: start a document, start or end a page, ask the paper size.

### What w16 does

Answers 0: the request is not supported.

### Why only that

There is no printer (see PrintDlg), and a screen device context takes
none of these requests.

### What a program may notice

Nothing beyond printing not working.

### What would make it whole

Printing. On the roadmap: "w16: printing".

## GDI.47 CombineRgn

### What Windows 3.1 does

Makes a region from two others -- their intersection, union, either but
not both, or one less the other -- of any shape the rectangles make.

### What w16 does

Exact while the answer is plainly a rectangle: the intersection of two
rectangles, the union of two where one holds the other, a difference
that takes all or none of the first away. Any other answer is kept as
its bounding rectangle -- a difference keeps the first rectangle whole
-- and reported COMPLEXREGION, as Windows would report it.

### Why only that

w16's regions are single rectangles. That covers the uses met so far:
Hearts combines rectangles to say what to redraw, and redrawing a little
more than needed draws the same picture.

### What a program may notice

A region used to clip may let a little more be painted than Windows
would.

### What would make it whole

Regions as lists of rectangles, as Windows kept them. On the roadmap:
"w16: regions as more than their bounding rectangle".

## GDI.53 CreateDC

### What Windows 3.1 does

Makes a device context for a named driver and device -- in practice a
printer's.

### What w16 does

Answers 0: there is no such device. The screen's device contexts come
from GetDC, CreateCompatibleDC and BeginPaint, which work in full.

### Why only that

There is no printer (see PrintDlg).

### What a program may notice

Nothing beyond printing not working.

### What would make it whole

Printing. On the roadmap: "w16: printing".

## KERNEL.166 WinExec

### What Windows 3.1 does

Starts another program from a command line, in the same Windows.

### What w16 does

Starts nothing and answers that the file was not found (2).

### Why only that

A second Windows program would need a second w16 and a way for the two
to see each other as one Windows -- which they do not have yet. Hearts
asks for NETDDE.EXE, the agent for network play.

### What a program may notice

Hearts goes on without network play.

### What would make it whole

w16 starting a second program -- and for Hearts, NetDDE. On the
roadmap: "w16: one program starting or finding another".

## SHELL.9 DragAcceptFiles

### What Windows 3.1 does

Tells File Manager that a window takes files dropped on it; a drop then
arrives as WM_DROPFILES.

### What w16 does

Accepts the call and does nothing: no drop is ever delivered.

### Why only that

Dropping a file from toy-os's File Manager onto a Windows program's
window needs toywm to carry the drop to w16, and w16 to turn it into
WM_DROPFILES with DOS paths.

### What a program may notice

A file dropped on its window is not opened. Open... in its File menu
works.

### What would make it whole

Drops delivered to Windows programs. On the roadmap: "w16: a file
dropped on a program (WM_DROPFILES)".

## SHELL.11 DragQueryFile

### What Windows 3.1 does

Says how many files a drop held, and each one's path.

### What w16 does

Answers 0: there is never a drop to ask about.

### Why only that

See DragAcceptFiles.

### What a program may notice

Nothing: it is only asked after a drop.

### What would make it whole

See DragAcceptFiles.

## SHELL.12 DragFinish

### What Windows 3.1 does

Frees what a drop held.

### What w16 does

Nothing, as there is nothing to free.

### Why only that

See DragAcceptFiles.

### What a program may notice

Nothing.

### What would make it whole

See DragAcceptFiles.

## USER.31 IsIconic

### What Windows 3.1 does

Says whether a window is minimized -- shown as an icon at the foot of
the screen in 3.x, hence the name.

### What w16 does

Always says no, even while the window is minimized.

### Why only that

w16 minimizes a program's window through toywm, which keeps the state;
w16 does not yet keep it on the window itself for this call to read.

### What a program may notice

A program that paints its own icon while minimized, or stops its timer
then, does neither. None run so far depends on it.

### What would make it whole

w16 keeping the minimized state it already sets. On the roadmap: "w16:
IsIconic answers from the window's state".

## USER.50 FindWindow

### What Windows 3.1 does

Finds a top-level window by its class or its title, in any program.
Programs use it to find a copy of themselves already running.

### What w16 does

Finds none.

### Why only that

Each program runs in its own w16, so there are no other programs'
windows to find. Its own windows are not searched either: a program
asks this for someone else's.

### What a program may notice

A program that allows only one copy of itself does not notice the first
one; a second copy starts beside it.

### What would make it whole

w16 programs that see each other. On the roadmap: "w16: one program
starting or finding another".

## USER.121 SetWindowsHook

### What Windows 3.1 does

Puts a function in a chain that Windows calls at a set point:
WH_CALLWNDPROC before each message a window is sent, WH_GETMESSAGE as a
message leaves the queue, WH_KEYBOARD for each key, WH_MSGFILTER inside
a dialog's or a menu's own loop, and others.

### What w16 does

Keeps every hook a program sets and answers with the one it replaced, as
Windows did. Only WH_CALLWNDPROC is called -- before each message w16
sends. The others are kept and never called.

### Why only that

An MFC program cannot make a window without WH_CALLWNDPROC: MFC ties
its C++ object to the window inside that hook, and Hearts is an MFC
program. The other kinds need w16's message, dialog and menu loops to
call out at the points 3.1 did, and no program run so far sets them.

### What a program may notice

Hearts sets only WH_CALLWNDPROC and notices nothing. A program that
watches keys or messages through WH_KEYBOARD or WH_GETMESSAGE would not
see them.

### What would make it whole

The other kinds called from the message, dialog and menu loops. On the
roadmap: "w16: the hooks besides WH_CALLWNDPROC called".

## USER.171 WinHelp

### What Windows 3.1 does

Opens the program's Help file (.HLP) in Windows Help, at its contents,
a topic or a keyword.

### What w16 does

Shows a box saying the file is a Windows Help file that w16 cannot show
yet, and answers that Help opened. The call a program makes as it
closes, to close its Help, simply succeeds.

### Why only that

A .HLP file is a compiled format of its own -- compressed topics, its
own fonts and pictures, hot spots -- and toy-os has no reader for it.

### What a program may notice

Its Help menu and F1 show that box instead of Help.

### What would make it whole

A viewer for .HLP files. On the roadmap: "w16: Windows Help files
shown".

## USER.291 SetWindowsHookEx

### What Windows 3.1 does

SetWindowsHook's newer form (3.1): the same hooks, with a handle the
program gives back to take its hook out.

### What w16 does

As SetWindowsHook: WH_CALLWNDPROC is called, the other kinds are kept
and never called.

### Why only that

See SetWindowsHook. Hearts sets its WH_CALLWNDPROC hook this way, once
for each window MFC makes.

### What a program may notice

See SetWindowsHook.

### What would make it whole

See SetWindowsHook.
