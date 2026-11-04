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
`third_party/<x>/{upstream,patches,glue}`. **Stages 1 (third_party), 2 (the
kernel and the root), 3 (a directory per GUI program, with
manifests) and 4 (services, utilities, drivers) are BUILT** (2026-11-04);
5 is below.

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
5. **PCI is the one shared bus without its own directory.**
   `kernel/drivers/` is the class registries plus the shared buses, and
   a driver with no family stays flat -- `kernel/README.md`'s rule, which
   stays (a first draft of stage 2 sorted the flat files into
   `storage/`, `bus/pci/` and `base/` against it, and was reverted) --
   but `pci*.c` sat flat beside `virtio/` and `usb/`.
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
    │   │   ├── pci/               the PCI bus, beside virtio/ and usb/
    │   │   ├── block/ display/ input/ net/ sound/ usb/ virtio/
    │   │   └── ahci.c ata.c nvme.c gfx.c vga.c ...  no family: flat
    │   ├── shell/                 the ring-0 shell (from apps/)
    │   ├── samples/modules/       the example modules (from modules/)
    │   └── core/ fs/ mm/ net/ proc/ tty/ lib/ include/ ...  unchanged
    ├── userland/
    │   ├── apps/<class>/<name>/   ONE directory per GUI program, toywm included
    │   ├── services/<name>/       daemons: init, netd, logd, fontd, telnetd, ...
    │   ├── utils/                 command-line programs (from bin/), one file
    │   │                          each; w16/, webcontent/ of several
    │   ├── drivers/<name>/        ring-3 drivers (from snd/)
    │   ├── common/                code several programs link (w16prefs, upd)
    │   ├── libs/<name>/           libc, rt, ldso, and the named libraries
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
| 2 | `kernel/drivers/pci*.c` | `kernel/drivers/pci/` |
| 3 | `userland/gui/apps/<x>.c` + `userland/<x>/` | `userland/apps/<x>/` |
| 3 | `userland/gui/system/<x>.c` (+ its dir) | `userland/apps/<x>/`, `KIND := system-app` |
| 3 | `userland/gui/{demos,savers,wallpapers}/` | `userland/apps/{demos,savers,wallpapers}/` |
| 3 | `userland/webcontent/` | `userland/apps/browser/webcontent/` |
| 4 | `userland/bin/<daemon>.c` (+ `userland/remoted/`) | `userland/services/<daemon>/` |
| 4 | `userland/bin/` (the rest) | `userland/utils/<x>.c` |
| 4 | `userland/bin/{w16,webcontent}.c` + `userland/{w16,webcontent}/` | `userland/utils/{w16,webcontent}/` |
| 4 | `userland/wm/` | `userland/apps/system/toywm/` (toywm stays an app) |
| 4 | `userland/snd/` | `userland/drivers/snd/` |
| 4 | `userland/w16/w16prefs.*`, `userland/update/` | `userland/common/` |
| 5 | `userland/{libc,rt,ldso,include}/` | `userland/libs/{libc,rt,ldso}/` |
| 5 | `userland/{lib,ui,dynlib}/` | `userland/libs/<named library>/` -- the split, below |

**Stage 5 moves and splits in one step**, so no module moves twice.

## Manifests (stage 3 introduced them)

A `component.mk` in each GUI program's directory -- and since stage 4 in
each service's and each command's of several files -- a Makefile fragment
saying only what the directory cannot (chosen 2026-11-04 over a
key=value file with a generator, and over no manifest at all):

    # userland/apps/apps/doom/component.mk
    EXTRA      = $(DOOM_BACKEND_OBJS) $(DOOM_PORT_OBJS)
    ULIBS      = $(LIBHTTP_SO) $(LIBSSL_SO) $(BUILD)/lib/libhash.so
    NEEDS_PORT = doom

    # userland/apps/apps/terminal/component.mk
    INSTALL_AS = uterm

The program's name is its directory, its sources every `.c` in it, its
destination the class directory above -- so none of those is a field,
and a one-file program's manifest is a comment. The Makefile includes
each with the four fields reset and copies them into the per-program
keys the link and seed rules already read, through `$(value)` so every
one stays deferred (the Makefile's rule) -- Kbuild's shape, kept inside
Make so the build stays one tool. **What a program links becomes data**,
so stage 5's split changes manifests, not rules. The layout is
`apps/<class>/<name>/` (chosen over a flat `apps/<name>/` with a KIND
field) because the class directory being the destination was already
the Makefile's rule; the main file keeps the program's name
(`notepad/notepad.c`, not `main.c`) so a crash report's file:line names
one file.

**The trade**: a stray `.c` dropped into a program's directory is now
linked in, where the hand-written `EXTRA_OBJS_files` list made it fail
to link. Membership of the directory is the statement now. Third-party code keeps
hand-written rules: each port's build is bespoke (NetSurf's
`sources.mk`, dash's generated headers), the way SerenityOS's ports
each carry their own `package.sh`.

**`tools/checks/check_components.py`** (built in stage 3, over
`services/` and `utils/` too since stage 4) refuses a
program directory without a manifest (it would silently not be built), a
`.c` one level too deep or too shallow to be read,
a manifest with no `<name>.c`, a line outside the four fields, an
`EXTRA` naming another program's part, and two programs sharing a name
when either has parts (the per-program keys are by bare name). Stage 5
adds the library order to it: a library depending on one ABOVE it.

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

**And what stage 2 found:**

- **A short name at the root is also the tail of longer paths.** A row
  for `apps/` would have rewritten `userland/gui/apps/` too; the table
  names each file, anchored at the root (`/apps/shell.c`).
- **A name that usually means something else** -- most mentions of
  `grub.cfg` are the INSTALLED file -- is moved by a `=` row and its
  few real references (the Makefile's `sed` lines) fixed by hand.
- **Prose about the past keeps the old path.** Mentions of `apps/ui/`,
  `apps/wm/` or `apps/editor.c` describe files long deleted; dated
  decision entries and struck roadmap items stay as written. This table
  is their legend.
- **A list of source trees still needs the old name for history**:
  `update_server.py` sorts commits by the paths they touched, so
  `apps/` stays in its list for the commits made before the move.

**And what stage 3 found:**

- **A directory row rewrites BUILD paths too, and can get them wrong.**
  `userland/gui` -> `userland/apps` turned `build/userland/gui/system/toywm.elf`
  into a path one level short of where the ELF now is
  (`apps/system/toywm/toywm.elf`); grep for the new tree's ELF and
  object paths after a run.
- **A check that scans a moved directory may stop checking.**
  `check_key_routing.py` read `userland/gui/` by name; it refused the
  missing directory, but a check that skips one quietly would have
  passed with nothing scanned. Grep `tools/checks/` for every old
  directory name.
- **The identity check needed two fixes of its own**: ELFs that moved
  are paired with `--rename`, and a moved target can flip a
  displacement's sign (`-0x2a62(%rip)` -> `0x6ede(%rip)`), which the
  mask now covers. Stage 3: 352 binaries identical, 16 once masked.

**And what stage 4 found:**

- **A path relative to its OWN directory breaks when a file leaves
  it**: `grp16.c` and `shell16.c` included `"w16prefs.h"` bare, which
  only worked while w16prefs sat beside them -- no row can see it,
  the compiler does. Grep a split directory's bare `#include "x.h"`s.
- **A dry run refuses a directory row whose TO exists**: `userland/wm`
  -> `userland/apps/system/toywm` is a merge, so the directory rows are
  `~` (rewrite-only) and the files move one by one.
- **The brief's place for shared code was a BUILD directory.**
  `build/userland/shared/` is `kernel/lib/`'s ring-3 copies, and a
  source `userland/shared/x.c` would build to the same place with a
  rule (`shared/%.o: kernel/lib/%.c`) ahead of it; `userland/common/`
  instead.
- **A directory's membership can include a file the old list left
  out**: `w16/missing16.c` (documentation in C comments, no code) was
  not in `EXTRA_OBJS_w16` and is linked now -- an empty object, which
  `code_identity.py` confirms adds no function. The WM's 60 `.c` files
  were exactly its old `EXTRA` list.
- **`os.path.join(REPO, "userland", "w16")`** in `gen_w16_calls.py`,
  `gen_w16_ordinals.py`, `w16_hostcheck.py`, two tests' headers, and
  `check_docs.py`'s command checks over `"userland", "bin"` -- which
  returned quietly when the directory was gone, so the synopsis and
  option checks would have passed with nothing read. They read every
  command's `main()` now, and say so when they find none.
- **The directory wildcard changes link order** (w16, webcontent,
  toywm, remoted), which moves functions and `.bss` padding: 355
  binaries identical, 13 once masked (those four, and the ones carrying
  the build stamp), 0 problems; the seeded tree's 1,830 paths identical.

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

- **A stage's build is not identical in code** to the build before it:
  `tools/dev/code_identity.py OLD_BUILD`, against a build of the commit
  before the stage (a scratch `git worktree`). Two things legitimately
  differ and it masks both: a longer `__FILE__` string shifts `.rodata`
  and every `lea` into it (stage 1: the NetSurf and DOOM programs), and
  a changed link order -- `find kernel | sort` over moved files --
  moves functions and the alignment padding between them (stage 2: the
  kernel's 5,382 functions, identical once masked). A move that changes
  code is not a move.
- **Stage 5's include graph has cycles that are not a handful of
  files.** Then the layering is aspirational and the split waits for the
  code to be untangled first.

## Open questions

- **Names** were settled at stage 4: `utils/` and `services/`
  (Serenity's), and w16 a command, `utils/w16/`, rather than
  `subsystems/w16/`.
- **`wm.h`** is both toywm's header and the boundary GUI apps include.
  Stage 4 put it with toywm in `apps/system/toywm/` (toywm stays an app);
  the app-facing half goes to `libui` at stage 5.
- **Does `userland/include/`** (libc's public headers) become
  `libs/libc/include/`? Yes unless stage 5 finds a reason.
