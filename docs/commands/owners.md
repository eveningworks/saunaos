# owners

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    owners [OPTION]...

## Options

- `-a`, `--all` -- walk every rule's tree, the `once` ones too.
- `-n`, `--dry-run` -- say what would change; change nothing.
- `-v`, `--verbose` -- say each file changed.
- `-c`, `--config FILE` -- read FILE instead of /etc/owners.conf.
- `-h`, `--help` -- the options.

## Description

Gives the system volume's files the owners and modes `/etc/owners.conf`
says (`docs/users-design.md`, stage C). It runs at EVERY boot as the
`owners` service, first and before the desktop and the
shells, because two things would otherwise go wrong:

- **an old disk is root's throughout** -- an inode written before stage C
  reads owner 0 -- so the desktop, which runs as `user`, could not write
  its own home;
- **a file a root service makes in `/etc` is root's**, and the desktop
  must still be able to rewrite it.

Only root may run it for real; `--dry-run` works for anyone.

### The file

One rule a line: `PATH OWNER GROUP MODE SCOPE [once]`. OWNER and GROUP
are names or numbers; MODE is octal or `-` to keep each file's own;
SCOPE is `self` (the path), `below` (everything under it) or `tree`
(both). **The most specific rule wins**, whatever the order -- so
`/etc/passwd`'s line beats `/etc`'s `below`. A path no rule covers is
left as it is, which on every disk is root's. A line that does not parse
stops the whole run, with its number, rather than apply half a policy.

`once` marks a big tree (`/home/user`): it is walked only when the file
changes -- `/var/lib/owners` holds the hash of the file it last applied
in full -- or with `--all`. The every-boot rules cover `/etc`, a few
hundred files; a boot spends one stat on each and a chown only where an
owner is wrong (~70 ms in QEMU).

The image seeder reads the same file (`tools/build/tfs3_writer.py`'s
`owner_for()`), so a disk built from source and an old one end up alike.

It never follows a symlink, never enters another mount, and stops a
folder's walk at what it cannot list. systemd-tmpfiles' `z` and `Z`
lines are the model, in a format whose scope is explicit.
