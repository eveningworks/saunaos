# /etc/realtime.d -- programs that may ask for realtime

One file per program that is NOT a service but may be made realtime when
started by hand, with `chrt` or through `uinitctl_realtime()` in the
program itself (`docs/commands/chrt.md`). A service's own program needs
no entry: its descriptor's `CPUSchedulingPolicy=` already lists it
(`data/etc/services.d/README.md`).

    Exec=/bin/myprog
    CPUSchedulingPolicy=fifo
    CPUSchedulingPriority=10

| Key | Required | Meaning |
|---|---|---|
| `Exec` | yes | The program's absolute path, as it is spawned. Matched exactly against the kernel's record of what the process runs. |
| `CPUSchedulingPolicy` | yes | `fifo` or `rr`: what init grants when the request names no policy. A request may name either. |
| `CPUSchedulingPriority` | yes | 1..99, the CEILING. A request above it is refused, not lowered. |

The keys and their spellings are a service descriptor's, so a line means
the same thing in both places. **Init reads this directory on every
request**, so an entry counts the moment it is written and nothing needs
a reload. A malformed entry is said in `dmesg` and lists nothing.

This README is not copied to the image; every other file here is
(`Makefile`, beside `services.d`). What ships is `README`, a
comment-only file that lists nothing: System Update delivers FILES, so
a directory that shipped empty would never reach an updated machine.
No entry ships: nothing that is not a service has needed one yet.
