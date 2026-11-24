# stat

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    stat <path>

## Description

Type, size, permission bits, owner and group (by number and by name from
`/etc/passwd` and `/etc/group`), link count, inode number,
created/modified. A volume with no owners on disk -- FAT32, exFAT, ISO,
a network share -- reports the account it was mounted for, as Linux's
FAT driver reports its `uid=` mount option.