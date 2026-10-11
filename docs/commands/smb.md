# smb

**a `/bin` program.**

**Category:** Networking

## Synopsis

    smb [OPTION]... COMMAND //SERVER/SHARE[/PATH] [FILE]
    smb -u USER [-r] login NAME

## Commands

- `ls URL` -- list a folder: size, date and name, a `/` after a folder.
- `get URL [FILE]` -- copy a file into the current folder, under its own
  name or as `FILE`.
- `cat URL` -- print a file to the terminal (or a pipe).
- `stat URL` -- one file's or folder's type, size and dates.
- `info URL` -- what the server agreed to: the SMB dialect, whether
  every message is signed, who you are signed in as, how much one read
  may carry (1 MiB from a server that allows large MTU, 64 KiB
  otherwise), and the share's size and free space.
- `login NAME` -- sign in to a MOUNTED share: `/net/NAME` for a saved
  one (`/etc/mounts.d/NAME`), or any mount point given as a path. It
  hands the user and password to the share's `smbfsd` worker -- the same
  message the File Manager's Sign-in dialog sends -- and says whether
  the server accepted them. With `-r` the password is saved, as a hash,
  for the next boot.

## Options

- `-u USER`, `--user USER` -- sign in as `USER`. `-u guest` uses the
  server's guest account; with no `-u` the login is anonymous.
- `-p PW`, `--password PW` -- the password. Given `-u` without `-p`,
  `smb` asks for it with the echo off, which keeps it out of the
  history.
- `-W DOM`, `--domain DOM` -- the domain or workgroup. The default is
  the server's own, which is right for a NAS or a Windows PC that is
  not in a domain.
- `-t SECS`, `--timeout SECS` -- how long to wait for each answer
  (default 10).
- `-r`, `--remember` -- with `login`: save the password in
  `/etc/mounts.d/NAME` as the NT hash it is turned into, so the share
  opens without asking after a reboot. Anyone who can read that file
  can sign in to the share as you -- there are no user accounts here to
  keep it from them.
- `-h`, `--help` -- the commands and options.

## Description

`smb` reads files from a Windows PC, a NAS or a Samba server. **It is
read-only**: there is no `put`. The protocol is `/lib/libsmb.so`
(`userland/include/usmb.h`), which `/bin/smbfsd` also uses to serve a
share as ordinary files under `/net` -- a share you open often belongs
there; `smb` is for a quick look, a one-off copy, and for telling a
sign-in problem from a mount problem.

    smb -u reader ls //nas/media
    smb -u reader get //nas/media/Photos/2026/beach.jpg
    smb info //10.0.2.2:44500/public

**A URL** is `//server/share/path`, `\\server\share\path` (quote it --
the shell eats backslashes) or `smb://server:port/share/path`. The
server is a name `uresolv` can look up (DNS) or a dotted address; the
port defaults to 445.

**What it speaks.** SMB 2.0.2, 2.1, 3.0, 3.0.2 and 3.1.1, signing in
with NTLMv2. Once signed in as a user, EVERY message is signed (Windows
11 24H2 requires that); a guest or anonymous login has no key to sign
with, so a server that requires signing refuses it -- `smb` says so in
those words rather than "access denied". Encryption is not supported:
a share that requires it is refused, and the server says "access
denied" for it, so that is what you see, with the encryption named as
the likely cause.

**A refusal it has no words for names the server's code** --
`the server refused (NTSTATUS 0xc000000d)` -- so a report from a server
nobody here has seen can still be read.

**The password is never stored** -- it is hashed (NTLM's NT hash) as it
is read and the plain text cleared.

## Exit status

0 on success, 1 for a failure the server or the network reported, 2 for
a usage error.

## See also

`smbfsd` (saved shares under `/net`), `mount` (`-t smb`), `wget`.
