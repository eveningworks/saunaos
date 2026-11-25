# Users and privilege: who a process is, and what that lets it do

**Status: stages A, B and C BUILT (2026-11-24); stage D's plan decided 2026-11-25, D1 BUILT 2026-11-25, D2 next.** The maintainer
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
  walks `/home` once and gives it to `user`. `/etc` stays the desktop's
  to write until stage D splits the settings: root's, group `user`, mode
  3775 -- the desktop adds a file and rewrites its own, and cannot remove
  or replace root's (the accounts, the services, the shipped data).
- **A folder hands down** (`fs_new_attrs()`): a SETGID folder gives a
  new file its group, and when its group may write, group read and write
  too -- a POSIX default ACL's effect -- so a setting ROOT saves in /etc
  stays the desktop's to change; a SETUID folder owned by an account
  gives that account the file, FreeBSD's SUIDDIR, so what root (or the
  test harness) puts in /home is the person's. `/etc`, `/var` and
  `/var/lib` are 3775 root:user; `/home` 4755 user, its folders too.
- **A seat session starts in its account's home** -- init `chdir()`s to
  it for a `User=` service with `Seat=yes`, as login(1) does -- so what
  the desktop starts saves a bare name into /home, not root's `/`; and
  a file dialog given no folder opens in `$HOME`, as Windows' and
  GNOME's do (`uui_filedialog`; Notepad's untitled Save As with it).
- **The Software app restarts elevated**, as System Update does: it
  writes /bin, /usr and /var/lib/update; `component` is `sudo component`.
- **PERMISSIVE BY DEFAULT, for now.** The first run of the whole GUI
  suite under enforcement failed 48 tools, nearly all on fixtures the
  harness made as ROOT (the debug console's `sh`) for an app run as
  `user` to change; the rest -- /var/lib and /var/cache, the folders'
  inheritance, the home folder, Software -- are fixed above. So
  `fileperm=` defaults to `permissive`: every check is asked and each
  distinct refusal logged once (`fsperm: pid N (name) uid U refused ...
  -- allowed, permissive`), nothing refused. `fileperm=enforce` turns
  them on, and `CRED_PERM_ENFORCE` on a cred enforces one process tree
  whatever the boot says -- `fileperm_test` runs that way. The flip is
  on the roadmap, as `syscall_irq` was flipped: the harness's fixtures
  first, then a soak.
- **The tests' fixtures are the desktop's (2026-11-25).** A file a test
  makes for an app to change is MADE as `user` -- the debug console's
  `as <account> <line>` (`/bin/runuser -u`, util-linux's, root only),
  `DebugConsole.as_user()`, and `write_lines()` by default -- as Android's
  `run-as` makes an app's files as its uid; a scaffold folder at `/` is
  made `/home`-shaped (`user_dir()`: the account's, 4755). With that, the
  whole GUI suite and the usertests pass under `fileperm=enforce`; the
  default flips after daily use on the ASUS with the word on.
- **Downloaded game data is the person's** (the same day): DOOM's and
  ScummVM's cards write `$XDG_DATA_HOME/doom` and `.../games`, with their
  licences in `.../licenses`, and find a copy in `/usr/share` after it
  (`base/udatadir.h`) -- they run as `user`, which cannot write `/usr`.

### D. Several people

Picked 2026-11-25, in three parts, each shippable alone: **D1** a home
per account and the settings split, built with the one person there is;
**D2** passwords and a login screen; **D3** a second person, switching,
and the settings the kernel keeps applied per session.

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
  `Scope=user` or `Scope=machine` (the default, so a schema that says
  nothing keeps today's behaviour). A user key is READ from
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
  `resolv.conf`, `remote.conf`, `mounts.d`, `display.conf`,
  `devices.conf`, the services and the shipped data.
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
  Windows keep it per person, and D3 moves it with fontd into the
  session) and the kernel registry's own settings -- pointer, keyboard,
  font, scaling -- which with one person need no split. D3 applies the
  person's at login.
- **No migration.** Nobody runs SaunaOS on a disk that cannot be
  reimaged (`docs/decisions/build.md`, "No migration code for a machine
  nobody runs"), so an old disk's `/home` is not moved at boot: the test
  machines get a fresh image and the ASUS was moved by hand -- its
  entries into `/home/user`, the stored `/home/...` paths in its
  settings rewritten, `owners --all`.
- **Machine keys stay writable by the desktop** (`/etc` is 3775
  root:user) until D3, when a second person makes "change the machine"
  a privilege; then it goes through privd, as hostname1 and timedate1
  do under polkit.

**D2 -- passwords and the login screen.**

- Passwords hashed with a slow KDF (yescrypt or Argon2id, as current
  distributions use), in `/etc/shadow`, root's and 0600; the salt from
  `getrandom`, which the KDF refuses on `QUERY_RANDOM_JITTER` alone --
  `krandom.h` is not a CSPRNG.
- A login screen drawn by the compositor (mockup first) reading the
  account record above; auto-login kept as an option; lock on resume.
- The privd prompt gains the password field it lacks today.

**D3 -- several people.**

- More than one account: a Users page in Settings, `useradd`-shaped
  CLI through privd.
- Switching users (a session per seat, the logind shape).
- The kernel registry's per-person settings applied by the session at
  login, and `fontd` (with `display.scale`) run per session.
- Machine-scoped writes need the admin right (privd), as above -- which
  is also when the Configuration Editor's machine-wide values become
  editable (read-only there until then), and when its kernel-registered
  ones, editable now, split into the person's and the machine's.

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
