---
paths:
  - "tools/gdb/**"
  - "kernel/debug/**"
  - "kernel/arch/x86_64/kdebug_x86.c"
  - "tools/kdebug_bridge.py"
---

# Debugging with GDB

Moved verbatim from the root CLAUDE.md (2026-10-23); it loads only when a matching file is read or edited.

`make debug` boots frozen for QEMU's own GDB stub; **the kernel's OWN
stub (`kdebug=ttySN`, `make run KDEBUG=1`) is the one that works on bare
metal** -- `docs/testing.md`, "Debugging with GDB", has both. Its stopped
path takes no lock, allocates nothing and does not log. `-x
tools/gdb/toyos.py` adds `toy-dmesg`, `toy-ps`, `toy-symbols` (modules,
user programs) and a `bt` past `isr_common`.
