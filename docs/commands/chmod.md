# chmod

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    chmod <octal-mode> <file> [file...]

## Options

None. The mode is positional and must be octal.

## Description

Changes a file's permission bits. `chmod 755 /bin/thing` makes it
executable; `chmod 644 notes.txt` does not.

**Octal only — there is no `u+x` symbolic form.** That syntax is a small
parser over the umask, which nothing here needed yet; the octal form says
exactly what it sets.

**Only the file's owner, or root, may change its mode** (`EPERM`
otherwise, `docs/users-design.md`, stage C). A mount point's mode is its
mount's, in memory until it is unmounted -- what makes `/tmp` 1777.

**A mode that is not one to four octal digits is REFUSED**, not guessed
at. `chmod 8 f` and `chmod rwx f` both mean the caller expected
behaviour this does not have, and applying a misread number is worse
than failing.

**The type bits are ignored.** POSIX says chmod changes permissions, and
one that could turn a file into a directory would be a corruption
primitive rather than a convenience — so passing a whole `st_mode` back
does what you meant.

**Not every filesystem can store a mode.** TFS3 and ramfs can
(`FS_CAP_MODE`); FAT32, exFAT and ISO 9660 cannot, and say so rather than
failing vaguely:

    chmod: /boot/x: this filesystem stores no permission bits

`stat` shows whether a mode came off the disk or is the default for the
type.

## Why it exists

Until this existed, a file was created with the default for its type and
stayed there forever — which made the permission bits a seeded constant
rather than a fact about the file. That was not merely incomplete: on
real hardware it made `dash` unable to run any external command, because
files written by `remote.py sync` go through the kernel at the default
mode and dash's exec path refuses a file with no execute bit. See
`kernel/fs/tfs3_internal.h`'s `T3_MODE_DEFAULT` for the measurement.
