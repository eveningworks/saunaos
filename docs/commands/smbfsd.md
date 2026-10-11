# smbfsd

**a `/bin` program.**

**Category:** Networking

## Synopsis

    smbfsd
    smbfsd --serve NAME
    smbfsd --url URL POINT [--user USER]

## Description

`/bin/smbfsd` serves network shares as ordinary files under `/net`, so
every program -- the shell, the File Manager, the Image Viewer -- reads
a NAS or a Windows PC's share as if it were a folder here. Read-only.
It is the service `smbfsd` (`/etc/services.d/smbfsd`); nobody needs to
run it by hand.

    ls /net/media
    cp /net/media/Photos/beach.jpg /home/Pictures/

**Saved shares are one file each in `/etc/mounts.d`**, and the file's
name is the share's name under `/net`. The File Manager's **Add network
share…** writes one; deleting the file forgets the share, and smbfsd
unmounts it within a couple of seconds.

    # /etc/mounts.d/media
    Source=//nas/media
    User=reader

| Key | Meaning |
|---|---|
| `Source` | `//server/share`, optionally with a folder inside it (`//nas/media/Films`). A port goes after the server: `//10.0.2.2:4450/media`. |
| `User` | Who to sign in as. Absent: anonymous. `guest`: the server's guest account. |
| `Domain` | The domain or workgroup. Absent: the server's own. |
| `Hash` | The password's NT hash, written when "Remember password" is ticked (or `smb login -r`). **Never the password itself** -- but still enough to sign in to that share as that user, for anyone who can read the file. |

**Three roles, one binary.** With no arguments it is the SUPERVISOR: it
starts one WORKER (`--serve NAME`) per file, restarts one that exits
(waiting longer each time, up to 30 seconds), stops and unmounts one
whose file is deleted, and starts a fresh one when a file's `Source`
changes. `User`, `Domain` and `Hash` are read again at every connect,
so editing them -- or ticking "Remember password", which writes them --
restarts nothing. `--url URL POINT` is a worker for a share nothing
saved -- what `mount -t smb` starts.

**A worker mounts at once and connects only when something opens the
share**, so a boot never waits for a server that is asleep or away. Its
state is what the File Manager shows beside the share, and what `mount`
lists:

    device               on          type  options
    //nas/media          /net/media  smb   ro,connected
    //nas/photos         /net/photos smb   ro,sign-in needed

`idle` (not connected yet), `connecting`, `connected`, `sign-in needed`
(no password, or the saved one was refused -- `smb login NAME`, or open
it in the File Manager), `unreachable` (no answer; the next access tries
again after a few seconds), `failed`, and `no server` (the worker
exited and the supervisor has not restarted it yet).

**Each worker is one process for one share**, as each FUSE filesystem is
on Linux: a server that hangs stalls only its own share, and a worker
that crashes is restarted without the others noticing. A request the
server does not answer gives up after 30 seconds. The protocol is
`/lib/libsmb.so` (`smb` shows what a server agreed to), and the kernel
side is the userfs bridge (`kernel/include/abi/userfs_abi.h`).

**A share opened by its address is a session share.** Typing
`smb://nas/media` in the File Manager's address bar, a file chooser's
path field or `open` starts a worker (`--url`) at `/net/<share>` with no
file behind it; the File Manager marks it "this session" and offers
**Save to Network**, which asks that worker (`USERFS_CTL_SAVE`) to write
its own `/etc/mounts.d` file -- keeping its connection and its sign-in;
the password is saved only by Remember. The supervisor then finds the
point served, retries every 10 seconds, and takes over only when that
worker exits.

**Reads are large and read ahead.** A server that allows large MTU
(SMB 2.1 and later) is read 1 MiB per request, and the kernel side of
the bridge fetches a window ahead of a small `read()` -- 128 KiB,
doubling to 1 MiB while the reads stay sequential -- so most of a
copy's reads never wake the worker. The numbers are in
`docs/decisions/storage.md`.

**A share's right-click menu** in the File Manager's tree -- Open, Sign
in..., Disconnect, Forget this share -- works on a share in any state,
including one that never connected. Forget deletes a saved share's file
(and smbfsd unmounts it); for a session share or one mounted by hand it
unmounts.

## Files

`/etc/mounts.d/<name>` (saved shares), `/net/<name>` (their mount
points, made by the worker), `/run/smbfsd/<name>.pid` (each worker, so
a restarted supervisor can stop the old ones).

## See also

`smb` (look at a share, copy a file, `smb login`), `mount` (`-t smb`).
