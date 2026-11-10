# rm

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    rm [-rf] PATH...

## Options

- `-r`, `--recursive` -- remove a directory and everything in it.
- `-f`, `--force` -- a path that does not exist is not an error.
- `-h`, `--help` -- print the usage and exit.

## Description

Delete files. A directory is deleted only when it is empty, unless `-r`
is given.

`-f` is POSIX's, and what a Makefile's `clean` rule means by
`rm -f *.o`: a PATH that is not there is skipped silently (an unmatched
`*.o` reaches `rm` as the two characters), and `rm -f` with no PATH at
all succeeds. Any other failure -- a non-empty directory without `-r`,
a read-only volume -- is still printed, and still fails. There is no
prompt to suppress: `rm` never asks.

`-r` empties a tree from the leaves up and then removes each directory,
because the kernel's `fs_delete()` refuses a non-empty directory —
there is no recursive unlink behind it and there should not be. The walk
is breadth-first into a queue of at most 64 directories, then a reverse
pass over that queue, which is a post-order removal with no recursion;
each directory is read whole (`base/ulistdir.h`) before its files go, so
no listing size or name length leaves anything behind.

There is no prompt, and `rm` never uses the Recycle Bin -- that is
[`trash`](trash.md), and the File Manager's and the desktop's Delete.
`rm -r` on the wrong path is as final here as anywhere else.
