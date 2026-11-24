# Users and privilege: who a process is, and what that lets it do

**Status: stage A BUILT (2026-11-24); stage B next.** The maintainer
picked the shape on 2026-11-23: four stages, stage A first, a ROOT
BROKER for elevation (no setuid bits), ONE user account before several.
Roadmap: "Multi-user & file permissions".

## The finding that shapes the plan

Everything runs as uid 0 today, and nothing says so: `getuid()` and
friends are stubs answering 0 (`userland/libs/libc/unistd.c`), `<pwd.h>`
knows one entry, and no process carries an identity. But most of the
pieces a privilege model needs are already here, and that decides the
order:

- **Mode bits exist.** TFS3 stores `mode` at inode byte 92 and `chmod`
  works; bytes 100..127 are checksummed and always written as zero, so a
  32-bit uid and gid fit with no format revision (`mode`'s own trick,
  `kernel/fs/tfs3_internal.h`).
- **A "needs root" set exists.** The sandbox's `PROMISE_ADMIN` group is
  the 23 syscalls a normal user must not reach: mount and umount, mkfs,
  the partition and boot-sector writes, setting the clock, power-off,
  loading modules, the network's configuration. Gating a group is one
  check in the syscall layer, which already gates by promise.
- **The comments are waiting.** `abi/mount_abi.h` and
  `abi/partition_abi.h` say where the uid check goes "when uids arrive".
- **What is MISSING is the identity and the split of shared state.**
  Settings live system-wide in `/etc` (`docs/settings-and-queries.md`),
  and `/home` is "the one user's home" (`docs/filesystem-layout.md`).
  A second person needs both split -- the largest piece of work here,
  and the reason several people come LAST.

## What real systems do

- **Linux.** A process carries real/effective/saved uid and gid and a
  supplementary group list (`struct cred`, shared by a thread group,
  replaced whole rather than edited). Files carry owner, group and mode;
  `inode_permission()` in the VFS checks once for every filesystem, and
  FAT/exFAT get an owner from `uid=,gid=,umask=` mount options. Root is
  split into capabilities(7). Elevation is setuid `sudo`/`su`, or
  `pkexec`, where polkit -- a root daemon -- decides by group membership
  and a password prompt from the session's agent. A desktop session is
  started by a display manager (GDM, SDDM, greetd) through PAM, and
  logind tracks it.
- **Windows NT.** A process has an access token (user SID, group SIDs,
  privileges); objects carry ACLs. Under UAC an administrator logs on
  with a FILTERED token, and a consent prompt (consent.exe, on a secure
  desktop) starts one elevated process with the full one. Services run
  as SYSTEM or a service account.
- **macOS.** Unix uids underneath; admin users approve through
  `authorizationhost` (an Authorization Services right), the polkit
  shape.

**SaunaOS follows the Unix identity** (uid/gid in the process, root =
0) because ported programs expect it -- dash, SDL, ScummVM and GCC call
`getuid`, `stat` and `getpwuid` -- **and the polkit/UAC elevation**,
not setuid. A setuid binary is a privileged program parsing an
unprivileged caller's argv, environment and fds; most local root
exploits on Linux have been one. A broker keeps the privileged code in
one long-running root service that takes a narrow request.

## The stages

### A. Identity (nothing enforced)

- `struct cred { uid, euid, gid, egid, groups[] }` on the thread-group
  LEADER, as the sandbox is ("the leader's is the group's"); inherited
  through spawn, fork and exec.
- `SYS_GETCRED` answers it whole (one record, `getresuid`'s shape);
  libc's `getuid`/`geteuid`/`getgid`/`getegid`/`getgroups` read it.
- `SPAWN_CREDS`, a spawn flag with `uid`/`gid` fields appended to the
  spawn message (gated like `SPAWN_SANDBOX`): the ONE way to start a
  process as someone else, honoured only from euid 0 -- systemd's
  `User=`. This is the only check stage A enforces, since otherwise the
  identity means nothing.
- `/etc/passwd` and `/etc/group`, shipped (`root:0`, `user:1000`, group
  `wheel`), read by `<pwd.h>`/`<grp.h>`. Ported code reaches them
  through those calls (dash and sdl2-compat do; no vendored port opens
  the files itself), so the FILES are this system's choice of store --
  see "Why plain files, not a user database" below.
- `services.d` gains `User=`; toywm and the console shell run as
  `user`, every other service stays root.
- `id`, `whoami`; a USER column in `ps` (and Task Manager, after a
  mockup).

What could break: a program that checks `geteuid() == 0` and behaves
differently (dash's prompt, a port's "do not run as root"). Measured
before it ships, by running the GUI suite with the desktop as `user`.

**As built (2026-11-24).** The cred is `kernel/proc/cred.c`: refcounted,
NEVER CHANGED once made (Linux's rule -- a new identity is a new cred),
NULL meaning root, on the leader beside the sandbox; the group list is
ALLOCATED to fit, with `CRED_GROUPS_MAX` (65536, Linux's NGROUPS_MAX)
only the bound on SPAWN_CREDS' copy (`abi/cred_abi.h`). `SPAWN_CREDS`
from anyone but euid 0 is `-EPERM`, never `-EINVAL`, which libsys reads
as an old kernel. `proc_info` grew `uid`/`euid`/`gid`/`egid`. libc reads
`/etc/passwd` and `/etc/group` on every call (glibc's files backend);
when the file cannot be opened -- a sandbox that did not unveil it --
root alone is still found. Both files ship as SYNCED data, like
`/etc/timezones`, while nothing on the machine adds an account; stage D
makes them the machine's. `User=` starts nothing for an unknown account
(systemd's rule) and sets `HOME`, `USER`, `LOGNAME` and `SHELL` from the
passwd line, so the desktop's `HOME` is `/home` while services keep `/`.
Task Manager's User column is HIDDEN by default -- shown, it opened the
window past a 1280-wide screen (1196 px before it) -- with View > Show
(All / My / System processes, KDE System Monitor's filter; "system" is
uid < 1000, login.defs' UID_MIN) and the user in the details subtitle.
`/tests/cred_test` and `taskmgr_test` cover it.

### B. Privilege and the broker

- `PROMISE_ADMIN` syscalls require euid 0 (`-EPERM`), checked in the
  syscall layer beside the promise check.
- **`privd`**, a root service on a uchan: a request names an ACTION from
  a fixed table (format a volume, write the boot loader, install an
  update, set the clock, mount), never a command line. A caller in
  `wheel` is asked for consent; anyone else for root's password. The
  prompt is drawn by the compositor, not the requesting app, so an app
  cannot fake the dialog (UAC's secure desktop, in the compositor's own
  layer).
- Disks, the installer, System Update, Date & Time and `mount` call it;
  a CLI `sudo`-shaped front end for the shell.
- telnetd and tftpd then need a login: they hand the network a machine
  with the run of itself today (`docs/decisions/shell.md`).
- The calls whose ABI comments say "there is no user model" to justify
  being unprivileged -- `SYS_KILL`, `SETTING_OP_RELOAD`, `SYS_SHM_OPEN`
  -- are re-argued here: there is one now, only unenforced.

### C. Files

- TFS3 inode: uid at byte 100, gid at 104; zero reads as root, so a disk
  from before stays root's and boots.
- One check in `vfs.c` (owner, group, other; root passes), as Linux's
  `inode_permission()`; ramfs keeps uid/gid in memory; FAT32/exFAT/ISO
  get an owner from the mount (Linux's `uid=`), SMB the share's.
- `chown`, `umask`, `stat`'s `st_uid`/`st_gid` for real.

### D. Several people

- Passwords hashed with a slow KDF (yescrypt or Argon2id, as current
  distributions use), in `/etc/shadow`; the salt from a CSPRNG --
  `krandom.h` is not one, so this needs a real `getrandom` first.
- A login screen drawn by the compositor (mockup first), auto-login
  kept as an option; lock on resume.
- `/home/<name>`, and the settings split: what is the person's
  (wallpaper, theme, pins, recent files) moves to their home, the
  machine's stays in `/etc`, by a `Scope=` in each setting's schema --
  dconf's user database against `/etc/dconf`, or HKCU against HKLM.
- Switching users.
- First, DECIDE the richer account record ("Why plain files, not a user
  database"): what the login screen and the settings split need beyond
  a passwd line, and whether it is a userdb-shaped record or a field or
  two beside `/etc/passwd`.

## Why plain files, not a user database

Considered 2026-11-24, after stage A shipped, and dropped by the
maintainer. POSIX specifies only `<pwd.h>` and `<grp.h>`; where records
live is the system's choice. glibc's NSS puts `/etc/passwd` behind
pluggable backends; systemd's userdb adds JSON user records
(`/etc/userdb/<name>.user`, a root-only `<name>.user-privileged`,
`<user>:<group>.membership` files) that `nss-systemd` serves -- but they
do not override `/etc/passwd`, which every distribution still boots
from; FreeBSD compiles `master.passwd` into `pwd.db`; macOS keeps Open
Directory and Windows the SAM. The JSON records would have cost a JSON
parser in libc run on every lookup, one account spread over several
files with no atomic rename over them, a generated `/etc/passwd` that
goes stale or silently ignores a hand edit, and on TFS3 -- no symlinks,
so no `<uid>.user` -- a uid lookup that parses every record; what they
buy (a real name, icon, language, a per-user secrets file) has no
reader before stage D. So the store stays `/etc/passwd` and
`/etc/group`, with `/etc/shadow` for stage D's hashes as Linux and the
BSDs keep them. Whether an account also gets a RICHER record -- a real
name, icon and language for the login screen, a pointer to its settings
-- is decided at the start of stage D, when those have a reader.

## Not decided yet

- Whether settings writes need `privd` in stage B or wait for the split
  in D.
