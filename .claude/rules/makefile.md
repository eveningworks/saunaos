---
paths:
  - "Makefile"
  - "mk/**"
---

# Editing the Makefile

Moved verbatim from the root CLAUDE.md (2026-10-23); it loads only when a matching file is read or edited.

- Makefile traps: **`LIVE` changes the PREREQUISITE as well as the
  command line**; **every definition is DEFERRED (`=`, never `:=`) with
  `$(if ...)`, not `ifeq`**; **`MENU=1` works by deriving
  `GRUB_TIMEOUT`**, baked into both media.
- **Source discovery is recursive** -- every `.c` under `kernel/` or
  `apps/` compiles, so a new directory needs no Makefile edit, and there
  is no scratch file the build ignores.
- **Headers are dependency-tracked; a CFLAGS change is NOT** -- `make
  clean` after one, and read a flag that half-works as stale objects.
