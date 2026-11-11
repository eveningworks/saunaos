# spawn

**a `/bin` program.**

**Category:** Processes and programs

## Synopsis

    spawn <path> [args...]    (typing the name instead waits for it)

## Description

`/bin/spawn` starts a program and **does not wait for it**. It prints
the child's pid and returns to the prompt immediately, leaving the
program running:

```
/# spawn /bin/toywm
started as pid 3 -- /bin/toywm
/#
```

Typing a program's name instead — `toywm`, or `run toywm` — runs it and
**blocks the shell until it exits**. That is the right default for
`ls` or `cat`, and the wrong one for anything meant to keep running.

The child is **reparented to init** when `spawn` itself exits and
**leads a session and a process group of its own**, with no terminal, so
it survives the shell that started it -- and the hangup a session
leader's exit sends its terminal's foreground group. That is `nohup` and
`setsid` elsewhere, folded into one program.

## Why it is not only a convenience

A name and `spawn` start the same kind of process -- a real scheduled
one with a pid, a parent, a process group and a slot. They differ in who
WAITS: a name blocks the shell until the program exits, `spawn` returns
at once and the child is reparented to init when `spawn` exits. Anything
meant to keep running -- a service, a window, a sleeper -- is started
this way.

## Standard output, and arguments

**stdout is inherited, not redirected.** A spawned program writes where
its parent was writing, which is what you want when starting something
to watch. A caller that needs a pipe passes a real descriptor through
`SYS_SPAWN` directly; that is not this program's job.

**Started from the serial debug console, the child DETACHES from its
terminal** and gets the machine console as stdin and stdout instead:
that terminal is one command's session, and a background job would
otherwise print into the next command's reply. Anywhere else -- a
Terminal window, the physical console -- stdout is inherited as above.

**stderr goes to the kernel log** (`dmesg`), as `nohup` takes a
detached job's output off the terminal: the prompt is back before the
child has said anything, so its errors land where they can be read
later. A program typed by name instead keeps its errors on your
terminal.

Arguments after the path are **rejoined into one string** with single
spaces, because `SYS_SPAWN` takes them that way — the shell split them
and this puts them back. The original spacing is not recoverable from
`argv` and nothing depends on it. The rejoined string is capped; past
that the request is refused rather than truncated, with `spawn:
arguments too long`.

## Exit status

0 if the child was created, 1 if it was not — a missing path, an image
that is not a loadable ELF, or arguments that do not fit. `spawn`'s own
exit status says nothing about how the child fared, and by design cannot:
it has already returned by then. Use `ps` to see whether it is still
running, and `kill` to stop it.

## See also

`ps` for what is running, and its PID and PGID columns. `kill` to stop
a spawned program. `service` for the things init supervises, which are
started this way and then kept alive. In `/bin/tosh` a trailing `&`
backgrounds a job with the shell still tracking it — see `jobs`, `fg`
and `bg`; `spawn` is the blunter tool that hands the child to init
instead.
