# uACPI -- the kernel's AML interpreter

The first vendored code in the kernel: [uACPI](https://github.com/uACPI/uACPI),
release **7.0.0**, MIT. Why uACPI and not ACPICA or our own, and how it
is staged: `docs/aml-design.md`, stage 4, and `docs/decisions/kernel.md`,
"AML is run by a vendored uACPI".

| Directory | What it is |
|---|---|
| `upstream/` | The release, byte for byte, its own test suite included. **NEVER EDITED IN PLACE.** |
| `patches/` | Our changes to it, applied in name order by the vendoring tool. None yet (so the directory is absent). |
| `glue/` | SaunaOS's configuration headers (`uacpi_libc.h`, `UACPI_OVERRIDE_LIBC`). |
| `kernel/acpi/uacpi_*.c` | The callbacks (`uacpi_glue.c`), `kacpid` (`uacpi_boot.c`), the KTESTs (`uacpi_test.c`). |

Re-vendor (pinned by SHA-256 in the tool; reaches github.com):

    python3 tools/fetch/uacpi_vendor.py
    git status kernel/third_party/uacpi/   # nothing, if upstream/ was untouched

The build compiles `upstream/source/*.c` with the kernel's own flags and
`UACPI_FLAGS` (Makefile), and keeps `upstream/tests/` out of the kernel:
that suite runs on the host, `tools/hostchecks/uacpi_hostcheck.py`.
