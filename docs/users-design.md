# Users and privilege: who a process is, and what that lets it do

**Status: stages A, B and C BUILT (2026-11-24); stage D's plan decided 2026-11-25, D1 BUILT 2026-11-25, D2 (D2a, D2b, D2c below) BUILT 2026-11-25, D3a (several people, the login screen's list) BUILT 2026-11-26, D3b1 (the machine's settings root's, asked through privd) BUILT 2026-11-26, D3b2a (`/etc` and `/var/lib` root's, the desktop's writes there routed) and D3b2b (a Standard account's change signed by an administrator in the prompt) BUILT 2026-11-27; D3c next.** The maintainer
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
root alone is still found. Both files shipped as SYNCED data, like
`/etc/timezones`, while nothing on the machine added an account; since
D3a they are seeded ONCE and the machine's (an account a later build
needs would arrive as systemd-sysusers adds one, not by a sync). `User=` starts nothing for an unknown account
(systemd's rule) and sets `HOME`, `USER`, `LOGNAME` and `SHELL` from the
passwd line, so the desktop's `HOME` is `/home` while services keep `/`.
Task Manager's User column is HIDDEN by default -- shown, it opened the
window past a 1280-wide screen (1196 px before it) -- with View > Show
(All / My / System processes, KDE System Monitor's filter; "system" is
uid < 1000, login.defs' UID_MIN) and the user in the details subtitle.
`/tests/cred_test` and `taskmgr_test` cover it.

### B. Privilege and the broker

**Decided 2026-11-24** (the maintainer's picks; the mockup canvas was
https://claude.ai/artifact/S2MjCXxtMr5sfPDVLFgkNy, option P2).

- **The gate.** A `PROMISE_ADMIN` syscall from euid != 0 is `-EPERM`,
  checked in the dispatcher beside the sandbox's promise check
  (`kernel/proc/privilege.c` holds which rows are exceptions).
- **SEAT RIGHTS** -- what the person at the machine may do without
  asking, logind's and udisks2's default polkit policy (`allow_active`):
  power off and reboot, turn the screen off, read the shutdown status,
  and mount, unmount and eject REMOVABLE drives, disc images and network
  shares, at points under `/media/` and `/net/` only (udisks keeps to
  `/media/<user>`; a stick mounted over `/bin` would be root). The seat
  is a mark on the cred, `CRED_SEAT`, that init sets for a service with
  `Seat=yes` (toywm, the console shell, the VT shells) and that every
  descendant inherits -- Windows' INTERACTIVE group in the token, where
  Linux asks logind which session is active. A telnet shell runs as
  `user` WITHOUT it.
- **`SYS_KILL` follows Linux's rule**: the sender's real or effective
  uid equals the target's real uid, or the sender is root. A group kill
  reaches the members it may and is `-EPERM` only when it may reach
  none. Settings writes and a public `shm_open` stay as they are until
  stage D splits the settings.
- **`privd`**, a root service on the uchan `privd`. Its policy: a caller
  with euid 0 is granted; a caller whose account is in `wheel`
  (`/etc/group`, as polkit's `unix-group:` reads it) AND holds the seat
  is ASKED; anyone else is refused, since there is no password to ask
  for before stage D. Two kinds of request:
  - **an ACTION** from a fixed table, run by privd itself: set the
    clock, load or unload a module, signal a process the caller may not,
    mount or unmount anything the seat may not.
  - **an ELEVATED SPAWN**, the `pkexec`/UAC "run as administrator" half:
    privd grants the CALLER one spawn of exactly that path and argv as
    root (`SYS_CRED_GRANT`, root only), and the caller makes it with
    `SPAWN_ELEVATED`. So the elevated program is the caller's own child,
    with its terminal, its foreground group and its exit status -- as
    Windows' AppInfo service makes the elevated process a child of the
    one that asked. A grant binds the path and a hash of the argv, lasts
    30 seconds, is used once, and dies with its process.
- **The prompt** is drawn by the COMPOSITOR, as polkit's agent dialog
  (KDE's shape, option P2): a modal "Authentication Required" window,
  the action in plain words, a details grid (action, program and pid,
  the asking account), Cancel and Allow. toywm registers with privd as
  the AGENT, and privd believes only an agent whose cred holds the seat
  and whose program is `/bin/wm/system/toywm`. With no agent -- a text boot --
  privd asks on the caller's controlling VT itself, which the caller
  cannot answer for it (pkexec's text agent is the caller's own).
- **Who calls it**: Disks, the Installer and System Update's install
  step restart ELEVATED (a desktop entry's `Admin=true`; the launcher
  asks before it starts them); Date & Time sets the clock and Device
  Manager loads a module as ACTIONS; Task Manager's End on another
  account's process is the signal ACTION; Files needs nothing (seat).
- **`sudo`** is privd's command-line client: `sudo CMD ARGS` asks for an
  elevated spawn of CMD and runs it. It is NOT setuid sudo -- no
  sudoers, no `-u`; its page says so.
- **telnetd and tftpd run as `user`** (`User=user`, no seat): the
  network gets an ordinary account and no route to a prompt.

**As built (2026-11-24).** The gate is `kernel/proc/privilege.c`
(`SEAT_ROWS`, `ANY_ROWS`), the broker `userland/services/privd/`, its
client `base/upriv.h`, the agent `toywm`'s `wm_priv.c`, and `sudo`.
Beyond the plan above, building it found:

- **The grant carries the child's ENVIRONMENT** -- privd's own, root's --
  and the kernel substitutes it at the elevated spawn, so a PATH or HOME
  the asker set never reaches a root program (sudo's `env_reset`).
- **A verdict lands only on the process that asked**: privd re-reads the
  asker's uid and program before granting, since its pid may have gone
  to someone else while the person read the prompt. A grant dies with
  its process (`slot_free()`).
- **A command line too long for the prompt is refused**, never shown cut.
- **Some actions are the seat's without a prompt** (logind's
  `allow_active`): the next boot entry and the screen-size trial
  (`bootenv`, `bootmode`), and ending or muting a remote session
  (`remote`, only a `QUERY_REMOTESESS` leader, only TERM/USR1/USR2).
  The rest ask: `settime`, `modload`/`modunload`, `kill`, `bootcfg`
  (grub.cfg, parsed by privd), `netlink` (a card's speed, EEE, flow,
  moderation).
- **The desktop's own Force Quit** of an elevated window goes to privd as
  the agent's kill, not asked again: the person chose it there.
- **A signal to init is root's now**, and the SIGHUP doorbell meant
  "somebody is reading" to Task Manager and `service`: init's channel
  grew `INITCTL_STATUS`, which any account may send.
- **`gui prompt` and `gui kill` refuse a ring-3 sender that is not root**
  -- the kernel marks such a debug command `DIAG_F_UNPRIVILEGED` -- or any
  program could have switched the prompt off.
- **A GUI program asks off its frame** (`ui/uapp.h`'s `uapp_run_then()`),
  or the window is marked Not Responding while the person reads; the
  Network adapter widget still asks on its frame (`base/unetlink.c`).

**What stage B did not make safe** -- files carried no owner, so `user`
could still rewrite `/bin` or `/etc/group` itself -- is stage C's. Stage
B makes the privileged CALLS ask; stage C makes the files hold.

### C. Files

- TFS3 inode: uid at byte 100, gid at 104; zero reads as root, so a disk
  from before stays root's and boots.
- One check in `vfs.c` (owner, group, other; root passes), as Linux's
  `inode_permission()`; ramfs keeps uid/gid in memory; FAT32/exFAT/ISO
  get an owner from the mount (Linux's `uid=`), SMB the share's.
- `chown`, `umask`, `stat`'s `st_uid`/`st_gid` for real.

**As built (2026-11-24).** Where it differs from the plan above, the code
is right and this is what it does:

- **The check is at the SYSCALL BOUNDARY, not inside `vfs.c`**
  (`kernel/fs/fsperm.c`, `kernel/fsperm.h`). Every path syscall names its
  question as a parameter of `resolve_user_path()`, beside the sandbox's
  -- search, read, write, exec, create, delete, own -- and `open()`,
  `rename()`, `chown()` and the descriptor calls ask theirs where they
  know enough. The VFS stays unchecked, so the kernel's own reads (an
  ELF's libraries, init's files) are never refused: Linux's kernel
  threads run with root's credentials for the same reason.
- **The rule is Linux's `generic_permission()`**: the FIRST class that
  matches (owner, a group of the cred's, other) decides; SEARCH on every
  folder above the path; root reads and writes anything and runs a file
  only if some x bit is set. The STICKY bit: in a 1777 folder only the
  file's owner or the folder's removes or replaces it. chmod and an
  explicit utime are the owner's; "now" is a writer's. chown: root gives
  a file to anyone, an owner moves it into a group it is in. A name that
  is already there is the operation's EEXIST before any mode is asked.
- **A MOUNT ROOT's owner and mode are the MOUNT's** (`struct mount`'s
  `uid`, `gid`, `root_mode`): no backend here gives a root an entry, so a
  chmod or chown of a mount point changes the record, in memory. The
  mounting process owns a mount; `mount -o mode=,uid=,gid=` sets it, so
  the `tmpfs` service mounts `/tmp` 1777. A volume with no owners on
  disk (FAT32, exFAT, ISO, SMB) reports its mount's for every file, and
  `udrive_mount()` and `smbfsd` hand a ROOT mount to the desktop's
  account (`uusername_desktop()`, toywm's `User=`) -- udisks2's `uid=`.
- **ramfs keeps a mode and an owner per node** (`FS_CAP_MODE`,
  `FS_CAP_OWNER`), files 0755 like tfs3's default.
- **A new file's mode is the backend's default (0755) less the creating
  process's umask** (`SYS_UMASK`, inherited, 022): `open()`'s and
  `mkdir()`'s mode arguments do not reach the kernel, because the tools
  that write programs -- a shell's `>`, System Update, `remote.py sync`
  -- never chmod what they write, and an honoured 0666 would leave every
  binary they write unrunnable (the T3_MODE_DEFAULT measurement). The
  owner is the creator's effective ids (`fs_new_attrs()`).
- **`SYS_STATX` carries the owner**: `struct sys_stat` is 48 bytes for
  good, so libc's `stat()`/`fstat()` ask the new call -- Linux grew
  statx(2) for the same reason. `SYS_CHOWN`, `SYS_FCHOWN`, `SYS_UMASK`;
  `access()` answers R/W/X for the real uid.
- **WHO OWNS WHAT is a file, `/etc/owners.conf`**, applied at every boot
  by the `owners` service (before the desktop) and by the image seeder.
  An old disk is root's throughout, so the first boot after the update
  walks `/home` once and gives it to `user`. `/etc` stayed the desktop's
  to write until stage D split the settings -- root's, group `user`, mode
  3775 -- and is root's alone, 0755, since D3b2a (below).
- **A folder hands down** (`fs_new_attrs()`): a SETGID folder gives a
  new file its group, and when its group may write, group read and write
  too -- a POSIX default ACL's effect; a SETUID folder owned by an account
  gives that account the file, FreeBSD's SUIDDIR, so what root (or the
  test harness) puts in /home is the person's. `/home` is 0755 root, each
  home 4755 its account's, its folders too; `/etc`, `/var` and `/var/lib`
  were 3775 root:user until D3b2a made them root's 0755.
- **A seat session starts in its account's home** -- init `chdir()`s to
  it for a `User=` service with `Seat=yes`, as login(1) does -- so what
  the desktop starts saves a bare name into /home, not root's `/`; and
  a file dialog given no folder opens in `$HOME`, as Windows' and
  GNOME's do (`uui_filedialog`; Notepad's untitled Save As with it).
- **The Software app restarts elevated**, as System Update does: it
  writes /bin, /usr and /var/lib/update; `component` is `sudo component`.
- **It shipped PERMISSIVE.** The first run of the whole GUI
  suite under enforcement failed 48 tools, nearly all on fixtures the
  harness made as ROOT (the debug console's `sh`) for an app run as
  `user` to change; the rest -- /var/lib and /var/cache, the folders'
  inheritance, the home folder, Software -- are fixed above. So
  `fileperm=` defaulted to `permissive`: every check asked and each
  distinct refusal logged once (`fsperm: pid N (name) uid U refused ...
  -- allowed, permissive`), nothing refused. `CRED_PERM_ENFORCE` on a
  cred enforces one process tree whatever the boot says --
  `fileperm_test` runs that way -- and the flip came as `syscall_irq`'s
  did: the harness's fixtures first, then a soak.
- **The tests' fixtures are the desktop's (2026-11-25).** A file a test
  makes for an app to change is MADE as `user` -- the debug console's
  `as <account> <line>` (`/bin/runuser -u`, util-linux's, root only),
  `DebugConsole.as_user()`, and `write_lines()` by default -- as Android's
  `run-as` makes an app's files as its uid; a scaffold folder at `/` is
  made `/home`-shaped (`user_dir()`: the account's, 4755). With that, the
  whole GUI suite and the usertests pass under `fileperm=enforce`.
- **ENFORCED BY DEFAULT since 2026-11-26**, after sixteen boots of
  daily use on the ASUS with the word on: three refusals, all correct
  (a home folder not made yet, `/etc/shadow` opened in Files and
  Notepad). `fileperm=permissive` is the way back.
- **Downloaded game data is the person's** (the same day): DOOM's and
  ScummVM's cards write `$XDG_DATA_HOME/doom` and `.../games`, with their
  licences in `.../licenses`, and find a copy in `/usr/share` after it
  (`base/udatadir.h`) -- they run as `user`, which cannot write `/usr`.

### D. Several people

Picked 2026-11-25, in three parts, each shippable alone: **D1** a home
per account and the settings split, built with the one person there is;
**D2** passwords and a login screen; **D3** a second person (D3a), the
machine's settings an administrator's (D3b), and the settings the kernel
keeps applied per session (D3c); switching between sessions later.

**What real systems do.** Linux keeps the account in `/etc/passwd` and
the hash in `/etc/shadow`, and a display manager's greeter (GDM) reads
everything else it shows -- picture, language, the session to start --
from **AccountsService**, `/var/lib/AccountsService/users/<name>`, root's
and written through a D-Bus daemon, so the greeter never opens a home.
Settings are LAYERED: an XDG program reads `~/.config/<file>` over
`/etc/xdg/<file>`, dconf reads the user's database over `/etc/dconf`'s,
and Windows reads HKCU over HKLM -- the machine's copy is the default
and the administrator's policy, the person's the choice. Windows also
applies the per-person halves of what the kernel holds (the pointer
speed, the keyboard layout) at logon from HKCU.

**The account record** (decided 2026-11-25, the AccountsService shape):

- The real name stays in passwd's GECOS field -- `getpwnam()` callers
  and `finger`-shaped tools already read it there.
- Everything else the login screen shows is `/var/lib/accounts/<name>`,
  a `name=value` file (`Picture=`, `Language=`), root's and 0644, so the
  greeter reads every account's without reading anyone's home; the
  picture is copied beside it as `/var/lib/accounts/<name>.picture`. A
  person changes theirs through a privd action -- the polkit
  `org.freedesktop.accounts.change-own-user-data` right, allowed without
  a prompt for the seat's own account.
- No reader exists before D2's login screen, so D1 builds none of it.

**D1 -- a home each, and the settings split.** D1a, the home, D1b, `Scope=`, and D1c, the apps' own files and state, all BUILT 2026-11-25:

- **`/home/<name>`.** The account's home is `/home/user`, not `/home`;
  `/home` itself becomes root's 0755, Linux's shape. `XDG_CONFIG_HOME`,
  `XDG_DATA_HOME` and `XDG_CACHE_HOME` leave `/etc/environment` and are
  set by init from the passwd line beside `HOME`, so a root service has
  none and a `User=` service gets its own.
- **A setting has a SCOPE.** A schema in `/etc/settings.d` says
  `Scope=user`, `Scope=seat` (stored as the machine's, changed by the
  person at the machine without asking; users D3b) or `Scope=machine`
  (the default, so a schema that says nothing keeps today's behaviour). A user key is READ from
  `$XDG_CONFIG_HOME/<file>` and then `/etc/<file>` -- the machine's
  value is the default -- and WRITTEN to the person's file only; a
  machine key reads and writes `/etc` alone. The file keeps its
  basename, so `File=/etc/desktop.conf` is `~/.config/desktop.conf`
  for the person's half.
- **One layered lookup, in `libbase`.** The person's half of a file is
  read through one helper over `uconf` (the user file, then `/etc`), and
  `usetting_schema` routes a write by scope. Without `XDG_CONFIG_HOME`
  -- a root service -- the person's layer is skipped, so `fontd`
  and `privd` see the machine's values and nothing a person writes can
  steer a root service.
- **As built (D1b):** a raw reader asks `uconf_get_layered()` or
  `uconf_load_layered()` -- key by key, the person's file then the
  machine's, no schema consulted (XDG's whole-file overlay); the schema
  routes writes and unsets by scope, and reads a MACHINE key from `/etc`
  alone. User keys: the look and behaviour (`desktop.*` but
  `display.scale` and the two debugging keys), `locale.*`,
  `shortcuts.*`, `browser.*`, `system.cursor.*`, `system.double_click`,
  `system.trash.*`; the machine keeps the computer's name, time sync,
  network recovery, updates and `software.show_gpl`. **`config
  --machine`** reads and writes the machine's layer -- the default a
  person without a value of their own follows (GNOME's admin-edited dconf
  system database, Windows' Default User hive). Setting a user key to
  the value the layers already give writes nothing, so it keeps
  following the machine's. The person's directory is made 0700 by the
  first write. A disk from before D1b needs nothing moved: its `/etc`
  values became the machine's defaults. A root GUI app (First-boot setup
  started from `sh`) follows the machine's values. **Root never has a
  person's layer**, whatever its environment says: `sudo` and privd's
  elevated tools inherit `XDG_CONFIG_HOME`, and a root write there would
  leave a root-owned file in a home -- so `uconf_user_path()` answers
  none for euid 0, as Linux's `sudo` drops the variable by `env_reset`.
  A person's path too long to open refuses the write rather than
  falling back to `/etc`.
- **The apps' own files are the person's.** Notepad's, Task Manager's,
  the File Manager's, the Terminal's, the Start menu's pins and the
  window positions (`windows.conf`) -- every `/etc/<app>.conf` an app
  writes for itself -- move to `$XDG_CONFIG_HOME`, read through the
  same layered call; the shell's history moves to
  `$XDG_STATE_HOME/tosh_history` (bash's `~/.bash_history`, XDG's state
  dir). What describes the MACHINE stays in `/etc`: `net.conf`,
  `resolv.conf`, `remote.conf`, `display.conf`, `devices.conf`, the
  services and the shipped data (`mounts.d` stayed too, until D3b2a made
  the saved shares the person's).
- **As built (D1c):** an app names its own file by its BARE NAME --
  `"notepad.conf"`, `"savers/starfield.conf"` -- and `uconf_load()`,
  `uconf_get*()` and `uconf_set*()` take it where they take a path,
  KConfig's `openConfig("notepadrc")` shape: a read merges the person's
  keys over `/etc/<name>`'s, key by key and section by section (KDE's
  cascade over `/etc/xdg`), a write goes to `~/.config/<name>` alone with
  its folders made, and root -- no person's layer -- uses `/etc`'s. An
  unset of a key `/etc` also has leaves a TOMBSTONE in the person's file,
  `<key>[$d]=1` (KConfig's deletion mark), so a "forget" does not bring
  the machine's value back; a set removes it. A LINE-LIST file (`places.conf`) is whole-file:
  `uconf_own_read_path()` answers the person's file if it exists, else
  `/etc`'s, and `uconf_own_write_path()` the person's. A file name built
  at run time is a bare name too (`savers/`, `effects/`, `wallpapers/`,
  `win16/<program>.conf`), and the compositor watches `~/.config` and
  `~/.config/effects` beside `/etc` and `/etc/effects`. PER-PERSON STATE
  left `/var/lib` for the XDG folders through `base/udatadir.h`, root
  keeping the `/var` path: `recent`, Notepad's `recent` and `session`,
  Mines' best times, Disk Mark's history, w16's per-program records and
  `tosh_history` in `~/.local/state`; the Web Browser's bookmarks,
  history, permissions and cookie jar in `~/.local/share/browser` (data,
  not state, by XDG's test: they are worth a backup); the thumbnails in
  `~/.cache/thumbnails`, the freedesktop spec's place. The machine's
  state stays: `update/`, `owners`, `fbs/`, `netheal`, `logd.seq`.
  `soundd`'s per-app volumes (`/etc/sound.conf`) stay the machine's while
  soundd is a root service, as `display.scale` does with fontd. No
  migration: a disk from before D1c keeps its `/etc/<app>.conf` files as
  the machine's defaults, and the ASUS's were moved by hand.
- **What stays machine-scoped in D1, though a person's elsewhere:**
  `display.scale` (`fontd`, root, renders for the screen; GNOME and
  Windows keep it per person, and D3c moves it with fontd into the
  session) and the kernel registry's own settings -- pointer, keyboard,
  font, scaling -- which with one person need no split. D3c applies the
  person's at login.
- **No migration.** Nobody runs SaunaOS on a disk that cannot be
  reimaged (`docs/decisions/build.md`, "No migration code for a machine
  nobody runs"), so an old disk's `/home` is not moved at boot: the test
  machines get a fresh image and the ASUS was moved by hand -- its
  entries into `/home/user`, the stored `/home/...` paths in its
  settings rewritten, `owners --all`.
- **Machine keys stayed writable by the desktop** (`/etc` was 3775
  root:user) until D3b, when a second person made "change the machine"
  a privilege; it goes through privd since, as hostname1 and timedate1
  do under polkit (D3b1 the settings, D3b2a the files).

**D2 -- passwords and the login screen.** Picked 2026-11-25: the
centred login screen (L1 on the mockup canvas), a greeter SESSION rather
than a lock over the person's desktop, yescrypt, and a password that is
OPT-IN -- a machine nobody set one on boots and behaves as before. Three
parts, each shippable alone: **D2a** the password, **D2b** the greeter,
**D2c** the lock screen and remote logins.

**What real systems do.** Linux keeps the hash in `/etc/shadow` (root's,
0600) and checks it in PAM, inside a ROOT process -- GDM's worker,
`login(1)`, `sudo` (setuid) -- or, for a program that is not root, through
the setuid `unix_chkpwd`, which answers only for the CALLER's own
account. Current distributions write yescrypt (`$y$`, libxcrypt); RFC 9106
recommends Argon2id. GDM and SDDM run the GREETER as an unprivileged
account in a session of its own and start a new compositor as the person
who signed in; the LOCK screen (kscreenlocker, GNOME Shell's) is the
person's own session drawing over itself. Windows has the same split:
LogonUI on Winlogon's secure desktop for sign-in, LSASS the one checker.
telnetd runs `login(1)` as root, which drops to the account.

**D2a -- the password.**

- **`/etc/shadow`**, Linux's format (`name:hash:lastchg:::::::`), root's
  and 0600. An EMPTY hash field is an account with no password -- the
  shipped `user` -- and `!` or `*` one that cannot sign in by password
  (root's: `sudo` asks for the ASKER's password, as Ubuntu's does).
  `<shadow.h>`'s `getspnam()` in the C library reads it, so only root can.
- **yescrypt**, Openwall's reference implementation vendored byte for
  byte (`userland/third_party/yescrypt/`), libxcrypt's default cost
  (`$y$j9T$`, N=4096 r=32: 16 MiB, a fraction of a second on hardware).
  A hash written by a Linux box verifies here and the other way round;
  the host check holds it against the host's own libxcrypt. The salt is
  `getrandom`'s: a salt must be UNIQUE, not secret, so the jitter source
  is enough for it (`krandom.h` is still no CSPRNG, and nothing here
  makes a key from it).
- **privd is the ONE checker** (LSASS's shape; no setuid, so no
  `unix_chkpwd`): only privd links yescrypt and reads `/etc/shadow`.
  `UPRIV_T_AUTH` (an account and a password) answers yes or no to root,
  to the greeter account, and to anyone else for THEIR OWN account
  only; a wrong answer costs the asker two seconds before the reply
  (`pam_faildelay`), so a loop guesses slowly. `UPRIV_ACT_PASSWD` sets
  or removes a password: root for any account, an account for itself
  with its current password; the file is replaced whole (written beside
  it, then renamed), so a power cut leaves the old one or the new one.
- **Setting a password turns auto-login off** for that account; Settings
  can turn it back on (Windows' "netplwiz" checkbox). Auto-login is the
  machine key `login.autologin` -- an account name, or empty for the
  login screen -- shipped as `user`.
- **The consent prompt asks for the ASKER's password** once the account
  has one (KDE's polkit dialog: "Password for User"), in the dialog
  stage B drew: privd marks the prompt, the agent sends the typed
  password with Allow, privd checks it -- wrong, and the agent is told to
  ask again, three times before the request is refused. With no password
  set, Allow alone still answers, as today. **`sudo` on a text boot
  sends the password it read** with echo off, and privd checks it, which
  turns the stage-B typo guard into a real boundary.
- **`passwd`**: the account's own password (current, new, new again),
  `passwd -d` to remove it, `passwd <name>` as root -- through privd.
- **As built (D2a, 2026-11-25):** a shadow file that is there but NOT
  root's alone (0600) locks every account -- someone wrote it who should
  not have -- while no file at all (a disk from before D2a) is no
  password, root locked; the file is seeded ONCE, so `make iso` keeps a
  password set on the machine. A password sent WITH a request is
  consent only from an account that HAS one: "" from an account with
  none would otherwise be root for the asking, from anywhere. While an
  account's wrong answer is pending, its next check is refused unhashed
  (EAGAIN), so requests in flight cannot be hashed back to back. An
  agent's Allow with no password reads as Cancel, so the debug console's
  `gui priv allow` never answers a password question. Under
  `fileperm=permissive` the file's mode is logged, not enforced: the
  hashes are only as private as enforcing makes them.

**D2b -- the greeter.**

- **A `greeter` account** (uid 90, no shell, no home of its own:
  `/var/lib/greeter`) runs the compositor as `toywm --greeter`, with the
  SEAT mark -- the login screen may power the machine off, as logind
  allows a greeter -- and draws the login screen alone: no taskbar, no
  apps, no desktop. It asks privd `UPRIV_T_LOGIN` (an account and a
  password); privd checks it and asks init to START THE SESSION for that
  account (a new `INITCTL_SESSION`, root's only). init stops the greeter
  and starts the desktop service as the account -- `User=` in
  `services.d/toywm` becomes `User=@session`, the account the session
  belongs to -- with the seat mark and the login variables, as today.
- **At boot**, `login.autologin` naming an account that exists makes
  init start that account's session directly (today's boot); empty, it
  starts the greeter. **Signing out** (the Start menu's new item) ends
  the desktop and init starts the greeter again; "Exit to shell" keeps
  its meaning.
- **The login screen is L1**: the clock and date at the top, the
  account's picture, real name (GECOS) and a password field in the
  middle, a Caps Lock warning under it, the keyboard layout and the
  computer's name bottom-left, Restart and Shut down bottom-right. It is
  one overlay in the compositor, `wm_login.c`, so the lock screen (D2c)
  is the same drawing over the person's own desktop.
- **Settings > Users** (D3a adds Other accounts under it): the name, Change
  password, Remove password, and "Sign in automatically". **First-boot
  setup gains an optional Password step**; skipped, the machine is as
  before.

- **As built (D2b, 2026-11-25):** init keeps the session's account
  (`User=@session`, `Session=none`, `INITCTL_SESSION` from root alone,
  the desktop's exit code `INITCTL_EXIT_SIGN_OUT` ending it); a greeter
  stopped for a session is stopped as `service stop` stops one, so its
  SIGTERM is no failure. `toywm --greeter` sets up no tray, no app list
  and no privd agent, and polls none of them. The login screen is the
  overlay `wm_login.c` over a blurred, darkened copy of the wallpaper --
  never the windows. `login.autologin` is SEEDED ONCE as `user` in
  `/etc/login.conf` and declares an EMPTY default, because the settings
  layer reads an empty value as the default: with `Default=user` the
  login screen could not be chosen at all. Sign out is the Leave page's
  fourth action. First-boot setup's Password step comes after Network,
  not after the computer's name as drawn: `ufbs`'s answered-pages bits
  are the first six steps in order. The new password asked twice is
  `ui/uui_newpassword.h`, shared by both. The Caps Lock warning drawn on
  the mockup is not built: ring 3 cannot read the lock state yet.
  The console shell and the VT shells are `User=@session` too, so no
  Ctrl+Alt+F2 reaches a shell past the login screen; a session's end
  (Sign out, or its desktop given up on) stops them with it, and the next
  session starts them again. An ABSENT `autologin` key means the login
  screen to init as it does to the settings layer -- the seeded file says
  `user`. `/etc/login.conf` is root's: Settings changes it through privd's
  `autologin` action -- switching it off is the seat's right, switching it
  on asks (for the password once there is one), and only for the asker's
  own account -- so no program running as the person can turn automatic
  sign-in back on behind a password. A TEXT boot with the login screen
  chosen has no greeter to start a session; its console asks through
  `/bin/login` (D2c).

**D2c -- the lock screen and remote logins.**

- **Lock**: Win+L, the Start menu's Lock, and waking from the screen
  saver or a turned-off screen when the account has a password
  (`desktop.lock_on_resume`, a user key, on by default: Windows'
  "On resume, display logon screen"). The person's own compositor draws
  L1 over everything, takes all input, and unlocks through
  `UPRIV_T_AUTH` for its own account -- the compositor never reads
  `/etc/shadow`.
- **`/bin/login`**, run as root: asks for the account and the password
  (`saunaos login:`), checks them through privd, and starts the
  account's shell with its identity and login variables -- what init
  does for a `User=` service. **telnetd** runs as root and starts
  `login` on the pty once any account has a password; with none, it
  starts the shell as `user` (no seat) as before. The text virtual
  terminals and the console shell do the same through init.
- **`tools/dev/remote.py`** answers the login prompt from the
  machine's entry in `local_info.txt`, so a laptop with a password
  stays reachable.
- **What the prompt asks (2026-11-26): `login.admin_prompt`**, a machine
  key in root's `/etc/login.conf`, one drop-down on Settings > Users --
  the password every time (the default), the password then Allow alone
  for 5 or 15 minutes, or Allow alone (UAC's consent prompt, Windows'
  default for an administrator). Safe only because the desktop draws the
  prompt and no program can press it (only `remoted` injects input).
  Inside the remembered minutes the prompt is still SHOWN, saying when
  the password was given -- never silent, as sudo's timestamp and
  polkit's `auth_admin_keep` are. Remembered per account in privd's
  memory; forgotten on a lock, a return from the saver or a dark screen
  (`UPRIV_T_FORGET` from the agent), a sign-out (the agent goes) and a
  boot. `sudo` and remote sessions always ask. Asking LESS is asked, with
  the password even under consent; asking more is the seat's.
- **One privd channel per process** (`base/uchan.h` names a ring by
  pid): the lock screen's password check, made from toywm, replaced the
  agent's ring, and privd had no agent after every unlock -- each prompt
  answered ENXIO until toywm restarted (found 2026-11-26). The agent
  registers again after its own calls (`wm_priv_reopen()`); a channel
  per connection is on the roadmap.

As built (2026-11-25), and where it differs from the plan:

- **The login screen (2026-11-26)**: with more than one keyboard layout,
  the bottom-left "Keyboard: FI" is a button opening the tray's own layout
  list above it (no "Keyboard settings..." footer there), and Super+Space
  switches as on the desktop. Every password field -- the login and lock
  screens, the administrator prompt, Settings' and first-boot setup's --
  has an EYE inside its right edge once it holds text: a click shows the
  letters, a second the stars, and new text starts hidden (`uui_textbox`'s
  `revealed`; KDE's login field and GTK's peek icon). A sign-in and a
  sign-out HAND OVER the screen: the compositor arms `WIN_ROLE_HANDOVER`
  first, and the kernel holds its last frame, as it is, until the next
  compositor claims the screen (15 s, then the console) -- where the
  console had shown for half a second.
- **Lock**: Super+L, and resuming from the saver or a dark screen when
  `desktop.lock_on_resume` is on and the account has a password (asked
  of privd only then, never per frame). `wm_login.c`'s LOCK mode, the
  same overlay as the greeter's. The Start menu's Lock is NOT built yet
  (the roadmap).
- **`/bin/login [--seat] [-f ACCOUNT]`** (`docs/commands/login.md`).
  `-f` signs in at once only while the account has NO password -- so
  telnetd always starts `login -f <first person>` and needs no "does
  any account have one" test of its own. The identity and login
  variables are `base/ulogin.h`'s, which init's `User=` uses too.
- **The console and VT shells have two forms**: `User=@session` with
  `NoSessionExec=/bin/login` and `NoSessionArgs=--seat` -- the shell as
  the session's account while one is on, `login` as root while none is
  (the login screen up, or a text boot). init stops the one form and
  starts the other at a session's start and end (`switching`, which
  is no failure), logind's shape of a getty per VT.
- **`remote.py`** reads a `login <host> <account> <password>` line from
  `local_info.txt` (or `SAUNAOS_REMOTE_ACCOUNT` / `_PASSWORD`), answers
  `login:`, then runs `sudo /bin/tosh` with the password, so its
  commands still run as root.
- **A machine's own copy of a descriptor survives System Update**
  (`/etc` files changed on the machine are kept, `update -v` lists
  them): one whose `/etc/services.d/tosh` predates `User=@session` keeps
  a ROOT console shell behind the login screen. The ASUS was migrated
  by hand on 2026-11-25 (there are no migrations; nobody else runs it).

**D3 -- several people.** Picked 2026-11-26, in three parts as D1 and
D2 were: **D3a** adding and removing people and the login screen listing
them (BUILT 2026-11-26); **D3b** the machine's settings an
administrator's right; **D3c** the kernel registry's per-person settings
applied at sign-in, and `fontd` per session.

**What real systems do.** Windows keeps every signed-in session running
and switches between them (Fast User Switching); GNOME and KDE under
logind run a session per VT, and the hidden one's compositor gives up
DRM master (logind's PauseDevice). Accounts are made by a root service
the settings panel asks through polkit (`accounts-daemon`'s CreateUser,
`org.freedesktop.accounts.user-administration`), or by `useradd` as
root; a new home is copied from `/etc/skel`. GDM lists the people
(AccountsService's ListCachedUsers) and preselects the last one;
Windows 11 puts the last one in the middle and the others bottom-left.

- **ONE SESSION AT A TIME, for now.** Another person signs in after the
  first signs out; the login screen lists everyone. Concurrent sessions
  -- the compositor role handed between paused compositors, init keeping
  a list of sessions -- are their own later stage (roadmap), and "Switch
  user" waits for them: with one session it would only be Sign out.
- **D3a: privd adds, removes and changes accounts** (`accounts.c`, the
  `useradd`/`userdel`/`usermod` actions; `base/uusers.h` the client half),
  an administrator's right, asked as any other. `/etc/passwd` and
  `/etc/group` are replaced whole beside themselves, as `/etc/shadow`
  is; an add writes group, passwd, then shadow, a removal the reverse,
  so a power cut between them leaves an unused group or a hash with no
  account, never an account with no group. A new uid is one above every
  person's uid and gid (useradd's max+1), so a removed account's files
  never fall to a newcomer -- except that an account added again over a
  home that was KEPT takes that home's old owner back, when nobody has
  it. The home is made `04700` and the account's, and `/etc/skel` is
  copied into it: the setuid folder gives each copied file to the
  account (`fs_new_attrs()`), so nothing is chowned after. Removing is
  refused while the account has a process (signed in, or a program left
  running: `userdel`'s rule) and for the last administrator. An
  administrator is a member of `wheel`; Standard is not.
- **D3a: the front ends.** Settings > System > Users keeps the signed-in
  person's own rows and adds **Other accounts** under them (mockup S1,
  GNOME's and Windows' shape): a row per person with a Standard /
  Administrator drop-down and Remove... (keep or delete the files), and
  Add account... with `uui_newaccount` -- full name, the account name
  made from it until edited, the type, an optional password -- which
  First-boot setup and the installer can use for the first account.
  `user` is the command line, one command with verbs (as `component`
  and `part` are), asking as `sudo` does.
- **D3a: the login screen** (mockup LA, Windows 11's shape): the person
  who signed in last in the middle, every person in a list bottom-left
  when there is more than one; a click puts another in the middle.
  privd writes who signed in, `/var/lib/greeter/last`, at the sign-in:
  init stops the greeter the moment the session begins, before the
  greeter would hear the answer.
- **Homes are private from D3a on**: a new one is `04700`. `/home/user`
  stays `4755` as stage C made it (roadmap).
- **D3b: machine-scoped writes need the admin right** (privd), as
  hostname1 and timedate1 do under polkit. Picked 2026-11-26 (mockup
  canvas M1): a shield on each machine row in System Settings, and ONE
  prompt per Apply listing every machine change (Windows 11's shield,
  KDE's KAuth); a Standard account's change signed by an administrator in
  the prompt (polkit's auth_admin, UAC's credential prompt); the kernel's
  per-person settings free until D3c; the desktop's own `/etc` files
  through NAMED privd actions per domain, so a prompt says what a change
  means, not which file. In two parts:
  - **D3b1, the settings (BUILT 2026-11-26).** Who may change a setting
    is the registry's answer (`SETTING_ABI_SF_PERSON`/`_SEAT`, a
    kernel setting's `who`, a schema's `Scope=`): a person's is anyone's,
    a seat's the person at the machine's (the volume, the brightness, the
    screen mode, `display.scale`), the rest root's. The kernel refuses a
    SET or UNSET of a machine setting at `SYS_SETTING` with EACCES --
    the kernel writes `/etc` itself, so no file mode would -- and
    `libbase` the same for a declared one. privd's `settings` action
    makes the changes as root after one prompt whose rows name each
    (`Time zone: Helsinki -> Stockholm`); System Settings' Apply,
    the Configuration Editor's machine values and `config` (asking on
    its terminal, as `sudo`; `-n` never asks) go through it.
  - **D3b2a, the files (BUILT 2026-11-27).** `/etc`, `/var` and
    `/var/lib` are root's, 0755 (`/etc/owners.conf`), and their `below`
    rules give back to root, at the next boot, whatever the desktop's
    account wrote there before -- so a synced image needs nothing moved.
    Everything the desktop wrote there went one of three ways (picked
    2026-11-27, polkit's shape: `allow_active=yes` for the seat's,
    `auth_admin` for the machine's):
    - **The person's own things went home.** The desktop's icon positions
      and the undo card's note are the bare name `desktop.conf` (the
      person's over `/etc`'s); a theme made by Save as, Import or the
      Theme Editor is `~/.local/share/themes/<stem>.theme`, looked up
      before the machine's `/etc/themes` (now an administrator's) and the
      shipped `/usr/share/themes` -- `desktop.theme`'s `ChoiceDir=` names
      the person's with `$XDG_DATA_HOME/themes`, read for anyone but root;
      a saved network share is the person's `~/.config/mounts.d/<name>`
      (the folder 0700, each file 0600, the NT hash in it), and smbfsd
      serves the shares of the account whose session holds the seat,
      stopping every one of the last session's when it changes hands --
      the account read from the seat's processes, logind's active session
      (`uusername_session()`). A root worker writes no one's file:
      Remember password is saved by the asking program, from the hash the
      worker hands back only to an asker of the share's own account
      (USERFS_CTL now carries the asker's uid to the server and the
      server's answer back); a session share's worker, the person's own
      process, writes its file itself on Save to Network.
    - **The seat's things are privd's, granted without asking**: a
      program's volume (`appvolume`, sent by the compositor on the
      agent's own channel, so the frame never waits and the agent keeps
      its ring), a sound card's format (`soundformat`), and a
      `settings` request whose every setting is the seat's
      (`display.scale`) -- libbase routes a seat holder's SET or UNSET of
      a `Scope=seat` schema setting there.
    - **The machine's things ask an administrator**: remote desktop
      (`remotedesktop`: key and value pairs over a fixed key set, a row
      per change in the prompt, a password's value never shown; the VNC
      password moved to root's 0600 `/etc/remote.secret`, `remote.conf`
      keeping only `password_set`; Settings' Remote Desktop page stages
      every edit behind D3b1's shield and Apply, its Connected rows still
      the seat's `remote`); a network card's link settings (`netlink`
      now applies AND saves `/etc/net.conf`, one request); Device
      Manager's disable and enable (`device`, off the frame, the prompt
      naming the device). `config register`/`unregister`, `service
      enable`/`disable` and `bootcfg resolution forget` refuse a
      non-root caller up front -- "needs administrator rights -- run it
      with sudo".
    - privd checks every argument of these actions itself before anyone
      is asked: a volume's program name and 0..100, a card name and the
      format's three words strictly, the remote keys and their values,
      the device by its id in a fresh list, the card by its name.
  - **D3b2b, the administrator-signed prompt (BUILT 2026-11-27).** A
    Standard account at the machine is no longer refused: whatever an
    administrator would be asked -- an action, an elevated tool, a
    machine setting, an account change -- is ASKED, and an
    administrator signs it in the prompt (polkit's `auth_admin`, UAC's
    credential prompt). Picked 2026-11-27 from a mockup round:
    - **Who signs: an Administrator drop-down** above the field (KDE's
      polkit agent), listing every account in `wheel` with a password it
      can sign with (set and not locked), the first chosen; choosing another relabels the field
      ("Password for Mika Laine") and empties it. privd sends the list in
      the prompt's `admins` field; the agent's verdict names the signer
      after the password, and privd checks that one is still listed,
      still in `wheel` and still has such a password before it checks
      the password. A wrong one costs the SIGNER's failure delay. A
      prompt whose body cannot also say "An administrator must approve
      this." is refused (E2BIG), as any prompt that cannot be shown whole
      is.
    - **No administrator with a password: refused, saying why.** The
      prompt has no field and one button, Close, and the request is
      refused (EPERM). Allow alone would let anyone at a Standard account
      approve their own change, and the default image's `user` has no
      password.
    - **Never remembered.** `login.admin_prompt`'s minutes are an
      administrator's own; each Standard request is signed afresh, as
      UAC's credential prompt asks every time.
    - **Not on a terminal.** `sudo` and `upriv_action_tty()` (`user`,
      `config`) refuse a Standard account before asking anyone, and a
      remote session is refused as before: the signature is the desktop
      prompt's.
- **D3c: the kernel registry's per-person settings** (pointer,
  keyboard, font, scaling) applied by the session at sign-in, as Windows
  applies HKCU's at logon, and `fontd` (with `display.scale`) run per
  session.

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
BSDs keep them. The RICHER record -- a picture and a language for the
login screen -- was decided at the start of stage D (2026-11-25): GECOS
for the name, `/var/lib/accounts/<name>` for the rest (stage D above).

## Not decided yet

- Nothing open for stage B. Settings writes wait for stage D's split
  (decided 2026-11-24).
