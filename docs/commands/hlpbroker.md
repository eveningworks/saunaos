# hlpbroker

**a `/bin` program.**

**Category:** Services and the system

## Synopsis

    hlpbroker

## Description

Opens Help Viewer for a Windows 3.x program. It is not something to type:
`w16` starts one for every program it runs and gives it the read end of a
pipe as standard input, and `WinHelp()` writes a line down the other end.

A line is tab-separated fields: `OPEN`, then any of `-c` NUMBER (a
HELP_CONTEXT topic), `-k` KEYWORD (HELP_KEY) and `-m` MACRO
(HELP_COMMAND), then the absolute path of the help file; or `QUIT`
(HELP_QUIT), or `WINHELP`, the path of a program named `winhelp.exe`, an optional `-i`ID (letters) and the help file's path, which starts `w16` on Windows' own Help. A new `OPEN` or `WINHELP` closes the window this started before. A line
that is anything else is ignored. It exits when the pipe closes, and
leaves the window open.

It takes no options.

## Why it is a program

`w16` runs a Windows program in a sandbox that gives up spawning, because
the program is untrusted code on an emulated CPU and its F1 key must not
be a way to start anything. A window has to be opened, though, so the
right to do so is split off: this process starts before `w16`'s sandbox
closes, runs exactly one program (`/bin/wm/apps/hlpview`), and checks
the options it passes. A `w16` that has been taken over can open a help
file and do no more -- the shape of OpenBSD's privilege separation
rather than a sandbox with a hole in it.

It is **not itself sandboxed**, and that is deliberate: a child inherits
its parent's promises and unveiled paths, so a broker that pledged would
start a viewer with no window. What keeps it harmless is how little it
does. The viewer pledges for itself before it reads a help file.
