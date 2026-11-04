# The source tree: roles, manifests and named libraries

A staged plan, in the shape `docs/cc-design.md` used. It answers one
question -- **does the source tree say what the OS is made of, and can a
new part be added without editing a 3,000-line Makefile?** -- and it
does not move anything ON THE DISK: `/bin`, `/bin/wm/...` and every row
of `docs/filesystem-layout.md`'s installed half stay where they are.
Only the source moves.

**Status: PLANNED, chosen 2026-11-04.** The maintainer picked all three
parts -- a role-based tree (B), a manifest per component (D) and
`libuapp` split into named libraries (C) -- with `kernel/` and
`userland/` kept at the top and third-party code as
`third_party/<x>/{upstream,patches,glue}`. **Stage 1 (third_party) is
BUILT** (2026-11-04); stages 2-5 are below, in order.

## What is wrong today, measured (2026-11-04)

1. **An app lives in two places.** Notepad is `userland/gui/apps/notepad.c`
   plus `userland/notepad/`; File Manager is `files.c` plus
   `userland/fm/`; Settings is `gui/system/settings.c` plus
   `userland/settings/`. Most apps of more than one file are split this way.
2. **`apps/` at the top is not the apps.** It is the ring-0 shell, the
   `edit` command and the desktop launcher, beside a `userland/gui/apps/`
   that is.
3. **`userland/lib/` and `userland/ui/` -- about 300 modules -- are ONE
   library**, `libuapp` (`.a` and `.so`), by a `find` in the Makefile.
   Every program maps all of it, the sandboxed parsers of hostile input
   (`thumb`, `fontd`, webcontent) included, and nothing can say "this
   library must not depend on the toolkit": the Makefile's workaround is
   `uhttp.c` living in `userland/dynlib/` "ON PURPOSE", because anything
   under `lib/` is globbed in.
4. **A port is two directories**: `userland/ports/<x>` (upstream) and
   `userland/backends/<x>` (our glue and patches), joined only by name.
5. **`kernel/drivers/` is half flat** (`ahci.c`, `ata.c`, `nvme.c`,
   `pci*.c`, `gfx.c`, `vga.c`) and half by class (`block/`, `usb/`,
   `net/`, `display/`).
6. **`userland/bin/` mixes command-line tools with daemons** (`init`,
   `netd`, `logd`, `fontd`, `clipboardd`, `soundd`); a ring-3 driver
   sits alone in `userland/snd/`; shared libraries in `userland/dynlib/`.
7. **The Makefile has a hand-written rule per program**, so a new
   component is a Makefile edit, and what a program links is written
   nowhere but its rule.

**What is already right, and stays**: `kernel/include/{api,abi,kernel}`
is Linux's `include/` vs `include/uapi/` split, enforced by the include
paths, and `data/` is the root filesystem's contents (SerenityOS's
`Base/`).

## What real systems do

- **SerenityOS**: `Kernel/`, `Userland/{Applications, Services,
  Libraries/LibGUI..., Utilities, Games, DevTools}`, `Base/`, `Ports/`,
  `Meta/`. One directory per component, a `CMakeLists.txt` in each.
- **Haiku**: `src/{system/kernel, servers, kits, apps, add-ons, bin}`;
  its libraries are KITS with a dependency direction (the Support Kit
  under the Interface Kit).
- **FreeBSD**: `sys/` with `sys/dev/<driver>`, then `lib/`, `bin/`,
  `sbin/`, `libexec/`, and `contrib/` for upstream code no one edits in
  place.
- **Linux's Kbuild, Fuchsia's GN**: every directory has a small
  manifest (`obj-y += foo.o`, `BUILD.gn`) saying what it builds and
  from what. **The manifest is where the modularity is**; the tree only
  makes it readable.
- **Chromium, Fuchsia**: `third_party/<name>/` with the upstream source
  and a metadata file beside it (`README.chromium`, `OWNERS`).

**SaunaOS follows the common shape** -- grouped by role, one directory
per component, one manifest per component -- and **keeps the ring split
at the top** (`kernel/`, `userland/`) where Haiku and FreeBSD flatten
it, because SaunaOS's include rules are by ring and the top level is
where a reader meets them.

## The tree, as it ends up

    saunaos/
    ├── boot/                      grub.cfg, grub-live.cfg
    ├── kernel/
    │   ├── arch/x86_64/           + linker.ld (from the root)
    │   ├── drivers/
    │   │   ├── base/              dev_claim, the driver registry
    │   │   ├── bus/pci/           pci, pci_bind, pci_irq, pci_msi
    │   │   ├── storage/           ahci, ata, nvme (controllers)
    │   │   ├── block/             the block_device layer, partitions
    │   │   ├── display/           + gfx, vga, font_ttf, splash
    │   │   └── input/ net/ sound/ usb/ virtio/
    │   ├── shell/                 the ring-0 shell (from apps/)
    │   ├── samples/modules/       the example modules (from modules/)
    │   └── core/ fs/ mm/ net/ proc/ tty/ lib/ include/ ...  unchanged
    ├── userland/
    │   ├── apps/<name>/           ONE directory per GUI program
    │   ├── services/<name>/       daemons: init, netd, logd, fontd, toywm, ...
    │   ├── utils/                 command-line programs (from bin/)
    │   ├── drivers/<name>/        ring-3 drivers (from snd/)
    │   ├── libs/<name>/           libc, rt, ldso, and the named libraries
    │   ├── subsystems/w16/
    │   ├── third_party/<x>/{upstream,patches,glue}
    │   └── tests/
    ├── data/  tools/  docs/

## The mapping, by stage

Each stage is one script run (below) and one commit, gated on its own.
The exact file list of a stage is generated when that stage runs; the
table is the rule the script follows.

| Stage | From | To |
|---|---|---|
| 1 | `userland/ports/<x>/` | `userland/third_party/<x>/upstream/` |
| 1 | `userland/backends/<x>/patches/` | `userland/third_party/<x>/patches/` |
| 1 | `userland/backends/<x>/` (the rest) | `userland/third_party/<x>/glue/` |
| 2 | `apps/` | `kernel/shell/` (same audience: `kapi.h` only) |
| 2 | `modules/` | `kernel/samples/modules/` |
| 2 | `linker.ld`; `grub*.cfg` | `kernel/arch/x86_64/`; `boot/` |
| 2 | `kernel/drivers/*.c` (flat) | `kernel/drivers/{base,bus/pci,storage,block,display,sound}/` |
| 3 | `userland/gui/apps/<x>.c` + `userland/<x>/` | `userland/apps/<x>/` |
| 3 | `userland/gui/system/<x>.c` (+ its dir) | `userland/apps/<x>/`, `KIND := system-app` |
| 3 | `userland/gui/{demos,savers,wallpapers}/` | `userland/apps/{demos,savers,wallpapers}/` |
| 3 | `userland/webcontent/` | `userland/apps/browser/webcontent/` |
| 4 | `userland/bin/<daemon>.c` | `userland/services/<daemon>/` |
| 4 | `userland/bin/` (the rest) | `userland/utils/` |
| 4 | `userland/remoted/`, toywm + `userland/wm/` | `userland/services/{remoted,toywm}/` |
| 4 | `userland/snd/` | `userland/drivers/snd/` |
| 4 | `userland/w16/` | `userland/subsystems/w16/` |
| 5 | `userland/{libc,rt,ldso,include}/` | `userland/libs/{libc,rt,ldso}/` |
| 5 | `userland/{lib,ui,dynlib}/` | `userland/libs/<named library>/` -- the split, below |

**Stage 5 moves and splits in one step**, so no module moves twice.

## Manifests (stage 3 introduces them)

A `component.mk` in each component's directory, a Makefile fragment:

    COMPONENT := notepad
    KIND      := app             # app | system-app | service | util | driver | lib | test
    INSTALL   := /bin/wm/apps/notepad
    LIBS      := ui fs base      # named libraries; libuapp until stage 5

The Makefile `include`s every `component.mk` and generates the rules
from one `$(eval)` template per KIND -- Kbuild's shape, kept inside
Make so the build stays one tool. **What a program links becomes data**,
so stage 5's split changes manifests, not rules. Third-party code keeps
hand-written rules: each port's build is bespoke (NetSurf's
`sources.mk`, dash's generated headers), the way SerenityOS's ports
each carry their own `package.sh`.

**`tools/checks/check_components.py`** (stage 3) refuses: a component
directory without a manifest; a `LIBS` naming an app or a service; a
library depending on one ABOVE it in the declared order (below). It
joins `tools/runners/checks.py` like the other static checks.

## Named libraries (stage 5)

**First cut, to be confirmed by the include graph when the stage runs**
(a tool computes it; the assignment is not guessed by hand):

| Library | From | Holds |
|---|---|---|
| `libbase` | `lib/` | strings, UTF-8, paths, args, containers, formatting (`ubase*`, `uutf8`, `upath`, `uargs`, `ulru`, ...) |
| `libsys` | `lib/` | settings, processes, crash, logging, clipboard, IPC (`usetting*`, `uprocio`, `ucrash`, `uchan`, `uclip`, ...) |
| `libfs` | `lib/` | files, trash, walk, archives, volumes (`ufile*`, `utrash`, `uwalk`, `uzip`, `utar`, ...) |
| `libmedia` | `lib/` | image, sound, video decoders (`uimg*`, `usnd*`, `uvid*`, `ttf`) |
| `libnet` | `lib/`, `dynlib/` | DHCP, DNS, SMB, HTTP (`udhcp`, `uresolv`, `usmb*`, `uhttp*`) |
| `libui` | `ui/`, `lib/`'s drawing | the toolkit and its widgets |

**The order is the table's**: a library may use the ones above it, never
below. **Measured 2026-11-04: only 8 `lib/` files include a `ui/`
header** (`icon_cache.c`, `ulivewall`, `unotify.c`, `upile.h`,
`usaver.c`, `usolid.h`, `uthumb.c`), so `lib` already sits almost
cleanly under `ui`; those eight move up into `libui` or have their
drawing call moved down.

**What the split buys**: a sandboxed parser maps `libbase` + `libmedia`,
not the toolkit; the dependency direction is a build failure, not a
comment; a change to the toolkit no longer relinks the network stack.
**What it costs**: each library is its own `.so` under `/lib`, every
program's `DT_NEEDED` list grows, and `ld-toy.so` resolves several
libraries where it resolved one -- measure start-up time before and
after.

## How a move is done

**One script, `tools/dev/tree_move.py`, and one mapping table per
stage**, never hand edits:

1. `git mv` every path the stage's table names, so `git log --follow`
   and `blame` follow the rename (no history rewrite);
2. rewrite every reference to an old path -- Makefile, `tools/`,
   `docs/`, `.claude/`, the CLAUDE.md files, comments and `#include`s
   in source -- longest prefix first;
3. refuse to finish while `git grep` still finds an old path outside
   commit messages.

**Build outputs move with the source** (`build/userland/<path>`), and
the dependency files of the old build still name the old paths: `make
clean` before the first build after a stage, which is then a full one,
ccache warm.

**What stage 1 found the script cannot see** -- check each by hand at
every stage:

- **A path built from parts in code** -- `os.path.join(REPO,
  "userland", "ports")`, a skip list naming the bare directory
  (`{"ports", "tests"}`). Grep `tools/` for the old directory's NAME, not
  only its path. A skip list that stops matching scans vendored code
  silently, or a new name skips our own.
- **A relative `#include "../../.."`** whose depth the move changed:
  NetSurf's glue forwards four libdom headers this way, and a move one
  level deeper broke all four.
- **References relative to `userland/`** -- `#include "ports/cjson/cJSON.h"`,
  `EXTRA_OBJS_x = ports/...`, a `$(BUILD)/userland-pic/ports/...` target.
  The table carries those forms as rows of their own.

## The honest case against

- **Muscle memory and every session's notes.** ~260 references move in
  stage 1 alone; the auto-memory files outside the repo name paths too,
  and go stale.
- **Open branches conflict.** Every worktree open at a stage's commit
  rebases across the move; git's rename detection carries most edits,
  not all. Merge or close them before a stage.
- **It is churn that ships nothing a person sees.** Its return is the
  next hundred changes, not this one.

## What would make us stop

- **A stage's build is not identical in code** to the build before it.
  The check: `.text` size and hash per binary against the previous
  build; for any binary whose hash differs, the disassembly with
  RIP-relative displacements masked must match exactly -- a longer
  `__FILE__` string shifts `.rodata`, and with it every `lea` into it,
  and nothing else. Stage 1: 365 of 368 binaries identical, the other
  three (the NetSurf and DOOM programs) identical once masked. A move
  that changes code is not a move.
- **Stage 5's include graph has cycles that are not a handful of
  files.** Then the layering is aspirational and the split waits for the
  code to be untangled first.

## Open questions

- **Names**: `utils/` (Serenity) or `bin/` kept; `services/` (Serenity)
  or `servers/` (Haiku). Settled before stage 4.
- **`userland/wm/`** is both toywm's code and the `wm/wm.h` boundary
  GUI apps include. Stage 4 splits it: toywm to `services/toywm/`, the
  app-facing half to `libui` at stage 5.
- **Does `userland/include/`** (libc's public headers) become
  `libs/libc/include/`? Yes unless stage 5 finds a reason.
