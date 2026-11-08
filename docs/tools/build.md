# `tools/build/` -- Build and image plumbing

One entry per tool in `tools/build/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

- **`gen_kconfig.sh`** -- writes the kernel's timer configuration (`hz`,
  `tick`, `highres` from `build.conf` or the make line) as
  `build/gen/kconfig.h`, at Makefile PARSE time. Idempotent: the header
  is rewritten only when a value changed, so a plain rebuild recompiles
  nothing; a value outside the known set stops the build rather than
  ticking at a rate nobody chose.

- **`gen_version.sh`**/`set_version.sh` -- versioning — see CLAUDE.md's
  `version.h`/`VERSION` bullets

- **`ovmf.py`** -- finds QEMU's UEFI firmware (the CODE image and the
  variable-store template; Arch, Debian/Ubuntu and Fedora paths) and
  prints the two `-drive if=pflash` arguments with `--qemu-args VARS`,
  copying the template to `VARS` first. A per-guest store, because two
  firmwares writing one store land each other's boot entries. `vm.py`,
  `install_test.py`, `uefi_boot_test.py` and `make run UEFI=1` use it.
- **`build_conf.py`** -- resolves `build.conf` (`<name> = builtin |
  module`) into the source files the Makefile builds as `.ko` modules.
  A name is a file's basename under `kernel/drivers/`, found by walking
  that tree so there is no name-to-path table; an unknown, ambiguous or
  misspelt name is an ERROR rather than a driver silently becoming
  builtin.
- **`stage_sdk.py`** -- stages the SDK into `build/sysroot` (`make sdk`,
  part of `make all`): the C library's headers, `rt/sys.h`, the toolkit's
  headers, the kernel headers those reach, the static archives, the runtime
  objects and the linker scripts (`docs/filesystem-layout.md`'s `/usr/include`
  and `/usr/lib` rows say where each lands). ONE TREE, TWO USES: the seed step
  copies it to the disk, and it is the host cross compiler's sysroot
  (`docs/cc-design.md`). **Which kernel headers ship is the compiler's `-M`
  closure, not a list**, and they go FLAT into `/usr/include/saunaos/`, searched
  after `/usr/include` -- the tree's `-I` order, which matters because
  `string.h` and `errno.h` exist on both sides; it refuses the day two of
  them share a name. `--list` prints what would ship and from where.
  Archives are copied `--strip-debug` (deterministic, ~17 MiB -> ~6).
- **`tfs3_writer.py`** -- the host-side TFS3 tool: format
  (writes superblock backups + GDT snapshots, wipes a stale TFS2
  signature per the wipefs rule, keeps images sparse by skipping/
  hole-punching the zeroed inode tables) / ls / read / write / mkdir /
  delete / sync (`once/` + `sync/` convention; the directories a
  `<seed>/exact` file names are MIRRORED, pruned of what `sync/` no
  longer holds -- the SDK's, so a deleted header leaves the image) / trim / info /
  corrupt (`--leak`, `--free-referenced`, `--bad-link-count`,
  `--smash-superblock`, `--stage-journal[-torn]` -- known damage for
  fsck/backup/journal-replay testing, same reasoning as
  tfs2_writer's). Spec: `docs/tfs3-spec.md`; the kernel backend
  (`kernel/fs/tfs3.c`) is kept in lockstep for v1/v2. **It does not
  read or write v3**, the kernel's lazy, volume-sized format (2026-10-20):
  a v3 volume is REFUSED BY NAME, never reported as "not TFS3", because
  the seeder formats what it takes for blank. The images it seeds are a
  few GiB, where v3's geometry is v2's anyway.

  **A NAME CROSSES FROM THE HOST AS ITS BYTES** (`disk_path()`): inside,
  a name is a str of the disk's bytes, and until 2026-10-28 a host name
  was encoded to Latin-1 on the way in, so `Käse` (C3 A4 on the host)
  would have landed on every seeded image as `K\xE4se` -- a Latin-1 name
  on an OS that reads UTF-8. `todisk.py` goes through the same helper.

  **IT WRITES THE WHOLE BLOCK MAP** -- direct, single, double and triple
  indirect. It stopped at single-indirect (~4.05 MB) until 2026-08-31,
  described in its own docstring as "a deliberate cap", which it was
  right up until `/install/kernel.bin` (~4.7 MB) meant a live image could
  not carry the payload it installs from. Adding one level would have
  moved the cliff to 4 GiB rather than removing it, so all three went in,
  behind one recursive walker/builder pair. **The triple level is
  UNEXERCISED** -- it starts past 4 GiB -- and says so where it is
  defined. The general shape is worth keeping: a second implementation of
  a format is only as complete as the biggest thing anyone has fed it.
  **`read` reads a SPARSE file** -- a zero pointer at any level is a
  hole and reads as zeros (`file_blocks(holes=True)`), since that is
  how fstorture_test.py checks TFS3 from the host; every WRITE path
  still refuses one.
  **`format --fs-version {1,2}`** picks the on-disk layout: v2 (32
  journal slots, GDT at 42, group 0 at 58) is the default and what a
  fresh image gets; v1 (four slots, GDT at 14, group 0 at 30) exists so
  the layout the kernel still mounts stays PRODUCIBLE and therefore
  testable -- `tools/tests/sweep/tfs3_v1_test.py` is its caller.

  **`trim` CORRUPTED EVERY LIVE IMAGE FOR AS LONG AS disk.img HAS BEEN
  PARTITIONED, and two bugs were stacked so that neither showed.** It
  punches holes through free blocks, and it does so with `fallocate()`
  on the raw fd -- so unlike every other read and write in that file it
  does NOT go through `Tfs3Image`, which is what adds `base_lba * 512`.
  It punched at a VOLUME-relative offset, which on a partitioned image
  is one partition-start early: 1 MiB, 256 blocks, landing on blocks
  that were in use.

  The second bug hid the first. It walked each group's bitmap to
  `BLOCKS_PER_GROUP`, but **TFS3's last group may be PARTIAL** and the
  bits past the volume's end read as free -- so it asked to punch ~71 MB
  beyond the volume, which the offset error pulled back INSIDE the file.
  A trim-past-the-end became a corrupt-the-beginning, and the two
  errors together produced a plausible-looking `punched N blocks` line.

  What it destroyed: `/usr/wm/applications`, `/etc/services.d` and
  `/usr/wm/startup`. So the live CD booted with a full `/bin`, a
  working shell, and a **desktop with no apps** -- the Start menu is
  built from those `.desktop` files. Reported from real hardware.
  `disk.img` was never affected because **`trim` runs only on the live
  image** (one line in the Makefile), which is why a disk boot looked
  perfect throughout.

  Nothing here caught it: `live_boot_test.py`'s "a directory the image
  shipped is readable" reads `/bin/wm/apps`, which sat far enough from a
  free run to survive. It names the three that did not now. The fix adds
  a bounds guard, because **a punch is destructive and silent** -- it
  cannot fail loudly the way a bad write can, and that guard is what
  surfaced the second bug.
  **TFS3 is the default format for FRESH images** (blank-disk policy
  in `vfs.c` and `seed_disk.py`); an existing TFS2 disk.img keeps
  mounting as TFS2 -- `make clean-disk && make iso` is the deliberate
  move. Use whichever writer matches the image's magic (both refuse
  the other's images; `trim` before gzipping a release image means
  the MATCHING tool's trim).
- **`strip_staged.py`** -- `objcopy --strip-debug` over every program
  and shared library in `seed/sync`, run by the Makefile's `seed` unless
  `STRIP=0`. The copies under `build/` keep their DWARF for gdb and
  `panic_resolve.py`; the staged ones were 131 of 186 MiB of the tree,
  read off a CD by GRUB on every live boot. Keeps `.symtab` (crash
  reports name functions from it) and leaves kernel modules alone.
- **`check_lib_layers.py`** -- run by the Makefile after it links each
  toolkit library (and libhttp, libsmb): every undefined symbol must be
  defined by a library to its LEFT in base < net < media < ui, by libc,
  or by what every executable carries (rt/'s objects, libc_nonshared.a);
  otherwise the library is deleted and the build fails, naming the
  module the symbol is in. It is `ld -z defs` with the executable's half
  named -- plain `-z defs` fails every library here on the `sys_*`
  wrappers and pthread that live in the executable on purpose. Shown red
  on a libbase call into libmedia's `uimg_free`.
- **`seed_disk.py`** -- the format-aware seeding front-end the
  Makefile's `seed` target calls: probes the image's magic, delegates
  `sync` to the matching writer, and formats a blank image with the
  default (tfs3) -- the same policy the kernel's blank-disk path
  applies at boot.

  **A BLANK IMAGE IS PARTITIONED AND BOOTABLE NOW** -- a GPT with a 1 MiB
  BIOS boot partition, a 64 MiB FAT32 `/boot` and TFS3 in the rest, which
  is what `make iso` produces for a fresh `disk.img`. `--partition mbr`
  writes the legacy table, which needs no BIOS boot partition
  (`core.img` goes in the pre-partition gap) and so gets two. **There is
  no `--flat` any more**: the kernel refuses a whole-disk volume
  (`docs/rootfs-design.md`), so this tool has no way to write one, and
  even the live ISO's RAM image carries a table. An image that already
  IS flat is still seeded -- a host tool may legitimately want files in
  one -- with a loud warning that the kernel will not mount it. `--layout`
  sizes and TYPES the partitions (`1M:bios,64M:esp,rest`). It prints
  `filesystem in partition N -- LBA ..., ... sectors`, which is what
  `partition_test.py` reads rather than assuming partition 1.
  `install_grub.py` fills the other two.

  **`--fbs-done` writes First-boot setup's marker** (`/var/lib/fbs/done`)
  when it is missing, so the image boots straight to the desktop. `make
  iso`, the USB image, `fresh_disk.py` and `partition_test.py` pass it;
  the live image does not, and must show setup. **A new tool that seeds a
  bootable image must pass it too**, or the wizard covers the desktop and
  every GUI check after it fails (`docs/commands/fbs.md`).

  **An image it FORMATS gets `utf8migrate`'s marker**
  (`/var/lib/utf8migrate`), always: its every file came from the UTF-8
  seed tree, so the first boot has nothing to convert -- and without the
  marker that walk held every service back on each fresh test image
  (`gui_regress.py`, eight guests at once), while tools that expect a
  desktop at the prompt failed three checks. An existing image is never
  marked, so a dev `disk.img` with history is converted at its boot.
  `utf8migrate_boot_test.py` deletes the marker from its copy.

  **An EXISTING image keeps its shape.** It asks `mkpart_test.py`'s
  `volume_of()` and seeds into whatever it finds, so a checkout does not
  change layout under anybody -- `make clean-disk && make iso` is the
  opt-in, the same one that moved TFS2 to TFS3. A partitioned image whose
  partition 1 is empty (a `mkpart`ed disk nobody formatted) gets formatted
  IN the partition rather than treated as blank, which is the same
  refusal the kernel makes at boot.
- **`install_grub.py`** -- puts GRUB **and the kernel** onto `disk.img`,
  which is what makes an ordinary `make run` a DISK boot rather than a CD
  boot. Three writes: `boot.img` at LBA 0 (patched with `core.img`'s LBA,
  and keeping the existing partition table), `core.img` embedded in the
  BIOS boot partition -- or, on an MBR, in the gap before the first
  partition -- with its own block list patched, the part
  `grub-bios-setup` normally does, and `kernel.bin` + `grub/` written at
  the FAT32 partition's root (SaunaOS's `/boot`) with `mtools` -- no root,
  no loop device, no mount. A volume still holding the old `::/boot/`
  tree has it moved up and removed once the new core image is written;
  **`--legacy-layout`** writes that old tree instead, as a test fixture.

  **Why `/boot` is FAT32 at all:** GRUB cannot read TFS3 and never will
  without a module written into its own GPLv3 source tree. Every real
  system with this problem answers it the same way (Linux's separate
  `/boot`, UEFI's FAT32 ESP, Windows' System Reserved) -- see
  `docs/decisions.md`.

  **The UEFI loader is a fourth write**: GRUB's `x86_64-efi` build at the
  ESP's removable-media path, `EFI/BOOT/BOOTX64.EFI`, so the same disk
  boots with CSM off. Its prefix is `/grub` with NO device -- GRUB fills
  in the partition it was loaded from -- so one image serves GPT and MBR.
  A host without the `x86_64-efi` modules writes a BIOS-only disk and
  says so. **`--check-efi`** and **`has_efi_loader(disk)`** ask whether
  the loader is there.

  **`--stage-payload <dir>`** writes the files `/bin/install` needs
  -- `kernel.bin`, `grub.cfg`, `boot.img`, `core.img`, and
  `bootx64.efi` when the host has GRUB's EFI target -- into a directory
  instead of onto a disk. The build points it at `seed/sync/install`, so
  every SaunaOS filesystem carries `/install` and can install itself. The
  `core.img` it makes has `(hd0,gpt2)/grub` baked in, which is what
  makes the ESP-is-partition-2 layout a constraint rather than a
  preference; `core.prefix` says so, and `install --bootloader` refuses
  a payload without it. A checkout without GRUB's BIOS target stages nothing and
  says so; that build boots, it just cannot install itself.

  **`--check`** answers "is this image bootable" for a script;
  **`boot_medium(disk)`** is the same question as a function and is what
  the Makefile, `vm.py`, `qmp_test.py`, `boot_smoke_test.py` and
  `serial_console.py` all ask before choosing `-boot order=c` or the CD.
  **`--optional`** is what the build passes: an image with no boot
  partition is reported and skipped rather than being an error, so a
  `disk.img` predating this layout keeps working (booting the ISO).
  It never partitions and never reformats an existing `/boot`.

  **`add_boot_word(img, word)` / `drop_boot_word(img, prefix)`** edit
  the multiboot2 lines of an image COPY's grub.cfg (over
  `patch_grub_cfg()`), so a test gets its own boot line without `make
  iso KCMDLINE=...` rebuilding the media everyone shares. `core.img`
  carries `probe` and `true` for grub.cfg's `bootpart=` lines -- a
  module the config needs must be in `CORE_MODULES`, because an
  installed target has no module directory to load it from.
- **`release_assets.sh <version> [outdir]`** -- a GitHub Release's
  assets, made only from a build that IS the tag: the live ISO, the USB
  image gzipped, `saunaos-<version>-source.tar.gz` (`git archive` of the
  tag) and `run_release.sh`, with `SHA256SUMS` over them, in
  `build/release/<version>/`. It refuses before writing anything unless
  VERSION matches, the tree is clean INCLUDING untracked files (every
  `.c` compiles whether tracked or not, and `gen_version.sh`'s `-dirty`
  sees tracked changes only), the tag is HEAD and on origin at the same
  commit, `version.h` was generated from HEAD, and both images are newer
  than it. The source archive is an asset of its own because GitHub's
  automatic "Source code" links follow the tag and the repository, so a
  history rewrite or a re-pointed tag would silently change what they
  give; the asset stays fixed to the images beside it.
- **`run_release.sh`** -- standalone QEMU launcher shipped as a GitHub
  Release asset (not part of the build), for running from just a
  release download with no checkout. Gunzips `disk.img.gz` if needed,
  boots with `make run`'s same device/display flags. When cutting a
  release: rebuild `disk.img` fresh (`make clean-disk` first), then
  `gzip -k -9 disk.img` before attaching it -- it's a large SPARSE file
  (9GB apparent, ~2MB of real data on a freshly-trimmed image), and
  GitHub's 2GB-per-asset limit plus plain bandwidth sense both rule out
  the raw file. Run the matching writer tool's trim
  (`tools/build/tfs3_writer.py trim disk.img` for a fresh-built image,
  `tfs2_writer.py` for an old TFS2 one) before gzipping
  -- sparseness is only ever lost, and an untrimmed image compresses
  whatever stale data it is still carrying. See
  `docs/decisions.md`'s versioning entry for the full v0.0.9 writeup.
- **`todisk.py`** -- copies the files in `todisk/` (git-ignored, the
  guest's root as written) over a disk image's TFS3 volume: folders made
  on the disk as needed, identical files skipped, changed ones replaced,
  NOTHING deleted. It creates no folder of its own and knows nothing of
  the contents. `--dry-run` says what would be written; `--disk` and
  `--dir` pick another image or folder. It finds the volume with
  `volume_of()`, refuses an image with no TFS3 and one a running
  `qemu-system` has open. `make iso TODISK=1` (or `TODISK=<folder>`) runs
  it after seeding; the live ISO and USB image never do. For files that
  must not be published. Convention in `docs/conventions/build.md`.
