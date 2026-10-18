# exit

**a shell builtin** — `/bin/tosh` only, not the kernel shell.

**Category:** Processes and programs

## Synopsis

    exit [n]

## Description

Ends the shell, and with it the session: the GUI Terminal's window, or
the connection of a `telnetd` session (the server closes its end once
the shell is gone, so the client sees the connection close). Ctrl-D at
an empty prompt does the same.

`n` is the status the shell exits with. **With no argument it is the
last command's status**, as POSIX has it, so `false` then `exit` exits
1 — what a script ending on a failed command is expected to report.
A status is a byte: `exit 300` exits 44.

**In a pipeline it ends nothing.** `exit | cat` runs, prints nothing and
leaves the shell where it was — bash runs every stage of a pipeline in
a subshell, so `exit` there leaves only the subshell, and this shell
gives the same answer without having one.

`tosh -c "exit 3"` exits 3, so a script run through `tosh -c` can report
its own status.

## Exit status

`n` (or the last command's status). A word that is not a number —
`exit now` — ends nothing: the shell says `exit: now: not a number` and
stays, because the session staying up is the safe reading of a typo.

## See also

`jobs`, `fg`, `bg` — the other builtins; `telnetd` — a session over the
network.
