# chown

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    chown [OPTION]... OWNER[:[GROUP]] FILE...
    chown [OPTION]... :GROUP FILE...

## Options

- `-R`, `--recursive` -- change folders and everything in them.
- `-v`, `--verbose` -- say each file changed.
- `-h`, `--help` -- the options.

## Description

Changes who owns each FILE, GNU coreutils' way. OWNER and GROUP are names
from `/etc/passwd` and `/etc/group`, or numbers. `OWNER:GROUP` sets both,
`:GROUP` the group alone, and `OWNER:` the owner and that account's own
group.

**Giving a file away is root's** -- `sudo chown user /some/file`. A file's
owner may only move it into a group it belongs to; anyone else gets
`Operation not permitted` (Linux's rule, `docs/users-design.md`, stage C).

**A volume with no owners on disk** -- FAT32, exFAT, ISO 9660, a network
share -- has none to change, and says so:

    chown: /media/STICK/x: this filesystem stores no owners (its files belong to whoever it was mounted for)

Its files belong to its MOUNT: a chown of the mount point itself
(`chown user /media/STICK`) changes the owner every file on it reports,
until it is unmounted -- what `automountd` does for a stick it mounts.
The same holds for any mount point, `/tmp`'s included.

`stat` shows a path's owner and group.
