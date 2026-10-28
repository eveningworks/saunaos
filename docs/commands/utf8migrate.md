# utf8migrate

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    utf8migrate [--root DIR] [--dry-run] [--force]

## Options

- `--root DIR` -- work on the tree under DIR as if it were / (its etc, home, var).
- `-n`, `--dry-run` -- list what would change; change nothing, write no log or marker.
- `--force` -- run even though the marker says it has run.
- `-h`, `--help` -- print the usage and exit.

## Description

The one-time move of a disk from Latin-1 to UTF-8
(`docs/utf8-design.md`, step 3). It runs ONCE, at the first boot after
the update that brought it, as the `utf8migrate` service -- before the
desktop and the other services, which read settings from `/etc` -- and
then never again: `/var/lib/utf8migrate` marks it done, and every later
boot logs one line, `utf8migrate: nothing to do -- converted ...`.

It does three things, on every writable TFS3 volume mounted at the time:

- **Names**: every file or folder name that is not valid UTF-8 is renamed
  to its UTF-8 spelling, each stray byte read as its Latin-1 letter
  (`K\xE4se` becomes `Käse`). When that name is already taken the new one
  gets ` (1)`, as a copy does.
- **Contents**: every file under `/etc`, and every file under `/home`
  ending `.txt`, `.md`, `.conf`, `.desktop` or `.ini`, that is not valid
  UTF-8 is converted the same way -- written beside itself, then swapped
  in by one atomic rename. It keeps its permissions; its times become
  the time of the conversion.
- **Logs** every change and every skip to `/var/log/utf8migrate.log`, and
  one summary line to the kernel log.

A name or file that is ALREADY valid UTF-8 is never touched, which is
what makes running it twice harmless. It leaves alone, saying so in the
log: w16's drives (C:, and D: to Y: when mapped -- their files are
Windows-1252, and a Windows program finds them by those bytes), a file
with a NUL byte (binary), a file with more than one hard link (a rewrite
would split them), a name whose UTF-8 would pass 255 bytes, and any
mount that is not a writable TFS3 volume (`/boot`, `/tmp`, a network
share).

A volume attached after the conversion -- an old disk or stick -- is
converted by hand: `utf8migrate --root /media/usb0 --dry-run` to see,
then without `--dry-run`. With `--root` the marker and the log are the
ones under DIR.

`convmv` is the model, with its default turned round: convmv only shows
what it would do until given `--notest`, which suits a person at a
prompt; this acts, because it runs unattended at boot, and `--dry-run`
is the way to look first.

## Examples

    utf8migrate --dry-run
    rename  /home/K\xE4se.txt -> /home/Käse.txt
    convert /etc/hostname
    utf8migrate: would do: 1 renamed, 1 converted, 0 skipped, 0 failed (dry run)

    cat /var/log/utf8migrate.log

## See also

`service`, `log`; `docs/utf8-design.md`.
