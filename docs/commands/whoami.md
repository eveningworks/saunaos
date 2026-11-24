# whoami

**a `/bin` program.**

**Category:** System information

## Synopsis

    whoami

## Options

- `-h`, `--help` -- the usage.

## Description

The name of the user this process runs as -- its effective uid, named
from `/etc/passwd` (`id -un`). `user` in the desktop's Terminal and at
the console, `root` in anything a service or the kernel started. A uid
with no name in the file is said on stderr and exits 1.
