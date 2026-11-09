# run

**a shell builtin.**

**Category:** Processes and programs

## Synopsis

    run <name> [args...]

## Description

Runs a program by name -- the explicit form. A bare name RESOLVES
identically (one resolver, `shell_exec_name()`) and starts the same way:
as a real scheduled process, waited on. So either form runs a dynamic
executable or a `#!` script, and a program that sleeps, spawns children
or claims a device works under both.

**The one difference is reporting.** `run` prints
`Process finished. Exit code: N` afterwards; a bare name prints nothing
on success and one terse line on failure. That exit code is the entire
assertion
`tools/tests/gate/usertest_run.py` makes -- see `docs/conventions/shell.md`.

**Use `spawn`, not `run`, for anything meant to keep running** -- `run`
waits for it, as a bare name does.
