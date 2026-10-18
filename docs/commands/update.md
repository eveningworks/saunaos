# update

**a `/bin` program.**

**Category:** System administration

## Synopsis

    update [--check] [-v] [--go-back] [--from <url>] | --server [<url>] | --channel [<name>] | --channels | --log

## Options

- *(none)* -- check, then fetch and install everything that differs.
- `--check` (or `-n`) -- check and list what differs; change nothing.
- `-v` -- with `--check`, list every file rather than the first ten.
- `--from <url>` -- use this CHANNEL'S address (`http://<host>:8080/dev`)
  for this run only; the server's list is not consulted.
- `--go-back` -- install the channel's build even though it is OLDER than
  this machine's (see Channels, below).
- `--server` -- print the server address and the recently used ones.
- `--server <url>` -- set it (the `update.server` setting, also on
  System Settings' Updates page) and add it to the recent list.
- `--channels` -- list the server's channels, the one in use marked `*`,
  with each one's version, date and description, `[default]` and
  `[internal]`.
- `--channel` -- print the channel this machine follows.
- `--channel <name>` -- follow that one (the `update.channel` setting);
  `--channel default` follows the server's default again. A name the
  server does not list is refused.
- `--log` -- print the last run's log, `/var/log/update.log`.

## Description

`/bin/update` brings this machine up to date from an **update server**:
a static HTTP server carrying a manifest of files and their checksums,
which `tools/update_server.py` runs on the development host. The machine
PULLS; nothing listens on it. The **System Update** window
(`/bin/wm/system/sysupdate`, Start > System) is the same engine with a
progress bar, and what this prints *is* the log that window shows.

    $ update
    server http://10.0.2.2:8080
    manifest: 664 files, version 0.4.0-dev, built 2026-09-30 16:06
    compared 664 files: 3 to fetch
    3 files to fetch, 2.3M:
      /bin/hello                          169.1K
      /boot/kernel.bin                 2.0M  (restart needed)
      /lib/libhello.so                     16.2K
    fetching 3 files
    /bin/hello: 173176 bytes, crc 1130093803 ok
    ...
    staged 2 files for the next boot (a library is in the set)
    kernel: installed; the running kernel is kept as /boot/kernel.old
    done: 3 files staged, restart to finish
    update: restart to finish (`reboot`)

Run over `tools/remote.py exec "update"` it streams the same lines back
to the host.

## What's new

When something differs, `update --check` (and `update`) print the
server's **release notes** first -- only the ones NEWER than this
machine's build, as apt-listchanges cuts a changelog at the installed
version -- wrapped to the terminal:

    What's new
      Since this machine's build of 2026-10-02 12:51:

      New
      - When a program crashes you get a notice, then a dialog ...

      Fixed
      - Force-quitting System Update while it checked could freeze the
        desktop.

      Under the hood -- 2 changes you won't see: Sound 1, Desktop 1

      Sound
      - The sound path is 32 bits wide, so a 24-bit file reaches a
        24-bit card whole.

      Desktop
      - Say so when a buffer is taken back unreleased

This machine's build is the `# commit` of the manifest it last applied
(`/var/lib/update/installed`), or the commit `/bin/update` itself was
built from when there is no record yet. The notes are the commits'
`Release-note:` lines, or a `refs/notes/release` git note added later
(`docs/conventions/build.md`). A change to the OS that nobody sees is
listed under **Under the hood**, by area -- folded behind one row in the
window, printed open here -- and a commit that is not the OS's (docs,
tools, tests) is not shown at all. A server with no notes -- a build published before
they existed -- prints none, and that is not an error. The System
Update window shows the same text on its **What's new** tab, before and
after an install.

## What decides that a file changed

**Size and crc32, never dates.** A machine whose clock is wrong would
otherwise refuse every update or take every one.

**`/etc` and `/home` follow dpkg's conffile rule.** A file there that
is absent is installed. One that a release CHANGED is replaced only
while it still has the crc the last update shipped -- nobody edited it.
One edited here is **kept**, the run says
`<path>: changed on this machine -- kept`, and the shipped version is
fetched to `/var/lib/update/new<path>` to compare by hand. Not beside
it, as rpm's `.rpmnew` is: init loads every file in `/etc/services.d`,
so a second copy there would start the service twice. With no record
of an earlier update there is nothing to tell an edit by, so every
changed `/etc` file is treated as edited.

**A file a release stops shipping is removed** -- dpkg's rule. Each
successful run keeps the manifest it applied in
`/var/lib/update/installed`; the next run removes what that listed and
the new one does not, from the managed trees only (`/bin`, `/lib`,
`/usr`, `/tests`, `/install`, `/etc/settings.d`), and only while the
file still has the crc it was shipped with. A file edited here is kept
and the log says so; a file no manifest listed is never touched. With
no record yet (the first run after this existed) nothing is removed and
the record is written. A stale library waits for the restart like a
changed one.

## When it waits for a restart

Everything is **downloaded and verified first**, as `<path>.upd`, so a
failed or cancelled download changes nothing. Then:

- **No library and no kernel in the set**: each file is renamed over
  its target, atomically (`SYS_RENAME2`). A program is read whole when it
  starts, so replacing one under a running copy is safe.
- **Any `/lib` file, or the kernel**: the files stay as `.upd` and are
  listed in `/var/lib/update/pending`; the **next boot** renames them
  into place before it starts anything. A running program pages its
  libraries in by path, so replacing one live would mix old code and
  new -- Windows applies in-use files at boot for the same reason.

**The kernel** is written to `/boot` straight away, with the running one
kept as `kernel.old` -- the GRUB rescue entry, *toy-os (previous
kernel)*. So `update` **refuses to install anything** while
`/boot/grub/grub.cfg` has `set timeout=0`: with no menu, a new
kernel that fails to boot could not be undone. A VM built with plain
`make iso` is in that state; `make iso MENU=1` gives it a menu. The
gzipped kernel is sent instead of the ELF when this machine's GRUB
records `gzio` (`/etc/grub-core.modules`).

A second `update` before the restart says an update is already staged
and installs nothing.

## Channels: one address, the server's own list

A machine names the SERVER (`update.server`, `http://<host>:8080`) and
follows one of the channels it lists (`update.channel`; empty is the
server's default). `GET <server>/channels` is that list. The development
host runs `tools/update_server.py` as a systemd user service
(`--install-service` sets it up) on port 8080, with three:

| Channel | What it serves |
|---|---|
| `release` | released versions only: a `main` build PROMOTED, never rebuilt |
| `main` | the last build somebody PUBLISHED after its tests (the default) |
| `dev` | the checkout's latest `make iso`, live, before any gate -- `internal` |

`dev` is marked internal: a development build lists it, a released one
only when `update.show_internal` is on. An address from before the list
that names one channel (`http://<host>:8080/stable`) is split into the
server and that channel the first time it is used; `/stable` still
answers as `main` for a machine whose updater predates the list, and a
server without a list is used as one channel, as before.

**A channel BEHIND this machine is not installed silently.** When the
channel's build is older than the one this machine last installed (by
version, then by build date), `update` says so and changes nothing;
`--go-back` installs it -- only the files that differ, `/etc` and `/home`
kept as always. The System Update window asks the same question when
such a channel is chosen: go back now, or wait until it passes this
build.

    update --channels
    update --channel release
    update --go-back

A build reaches `main` only when it is published, after its tests, and
`release` only when a published one is promoted:

    python3 tools/update_server.py --publish                  # this build -> main
    python3 tools/update_server.py --list                     # M = main, R = release
    python3 tools/update_server.py --promote <name>           # roll main back or forward
    python3 tools/update_server.py --promote <name> --to release

A publish is a COPY (under `~/.local/share/toy-os/updates`), its
unchanged files HARD-LINKED to the build before it, so rebuilding the
checkout never changes what `main` serves, and it is
refused unless `tools/preflight.sh` passed on exactly this tree
(`--force` overrides). `dev` refuses (503) while `seed/sync` is older
than the build or a `make iso` is still writing it, so a machine cannot
install the previous build, or half of the next one, by accident; an
error from the server is printed with its reason.

## What it is not

**Not authenticated.** The crc32 proves a file arrived intact, not who
built it: anyone who can answer on the server's address can install
anything. Use it on a network you trust.

**Not a package manager** -- no packages, no dependencies, no removal.

**Not atomic as a whole.** Each file is replaced atomically; a machine
that loses power while the list is being applied at boot finishes it on
the boot after (the list is idempotent), but a power cut during a live
install leaves some files new and some old.

## See also

`docs/update-design.md` for the design, `tools/update_server.py` for
the server (`docs/tools.md`), `reboot` to finish a staged update, `sum` for the checksums
it compares.
