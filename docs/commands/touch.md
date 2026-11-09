# touch

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    touch <file> [file...]

## Description

Sets each file's modification time to now, and creates an empty file
for a name that does not exist. `touch util.h && make` is how a
Makefile is told a header changed (`docs/commands/make.md`).

Only a TFS3 volume can be told a time. On FAT32 or exFAT (a USB stick,
`/boot`) touching an existing file fails with "not supported" rather
than pretending; creating a new file there works.