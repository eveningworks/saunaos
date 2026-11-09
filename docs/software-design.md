# Software: optional components in the update stream, and an app to choose them

PLANNED (2026-11-09). The picks this follows are the maintainer's, made
on the Design canvas (store cards S1 with S2's list and details as a
view option, queued changes with one Apply, a licence badge on every app
and a "Show GPL-licensed apps" switch). This document is the order of
work and the data that decides it.

## What real systems do

- **Windows "Optional features"** (DISM's `/Enable-Feature`,
  `/Disable-Feature /Remove`): the OS image is ONE build; a feature is a
  named set of its files with a state per machine (Enabled, Disabled,
  Disabled with payload removed). Windows Update services what is
  enabled and leaves the rest alone. A removed payload comes back from
  Windows Update, at the machine's own build.
- **ChromeOS DLC**: a component is part of the OS release, versioned
  WITH it, fetched by the same `update_engine` from the same payload
  server, and only for the release the machine runs -- a DLC newer than
  its OS is never installed.
- **Fedora Silverblue's `rpm-ostree override remove`**: a base-image
  package removed on one machine stays removed across every later
  upgrade until reset. The state is the machine's, the image is shared.
- **GNOME Software / KDE Discover** are front ends over a packaging
  system (PackageKit, Flatpak). SaunaOS has no packages, so the app is a
  front end over the component state below, nothing more.

**SaunaOS follows Windows and ChromeOS**: components are tagged files of
the one build the update server already publishes, a machine keeps which
it has removed, `/bin/update` fetches only the rest, and adding one back
fetches it AT THE MACHINE'S OWN BUILD. Not real packages: no separate
versions, no dependency solver beyond a `Needs=` line, no third-party
repository. Not hide-only either: Remove frees the space.

## The data

**A component is a file in `data/components/<id>.component`**, the
desktop-entry `key=value` shape (`base/uconf.h` reads it), staged to
`/usr/share/components/` so a machine knows its components with no
network:

    Name=C and C++ compiler
    Summary=cc, c++, as and ld -- build programs on this machine
    Category=devtools
    License=GPL-3.0-or-later
    Icon=cc
    Needs=
    Paths=/usr/bin/cc /usr/bin/c++ /usr/bin/gcc ... /usr/libexec/gcc/ /usr/lib/gcc/

- `Paths=` are files, or directories ending `/` (everything beneath).
  A path in no component is the BASE system and is always installed.
- `Needs=` names other components; removing one removes what needs it
  (the Web Browser goes with the Web page engine), adding one adds what
  it needs. One level of names, checked acyclic at build time.
- `License=` is SPDX, and is what the badge and the GPL switch read. A
  component's own `.component` file and its Start-menu entries are in
  its `Paths=`, so a removed component leaves nothing behind.

**First round** (the maintainer's picks): `doom`, `webengine` (NetSurf:
`/bin/webcontent`, `/usr/share/netsurf/`), `browser` (Needs=webengine),
`cc` (GCC + binutils), `make`, `win16` (w16, the Retro Launcher, the
Windows installer), `demos` (`/bin/wm/demos/*`, `/tests`). Their
licence texts under `/usr/share/licenses/` stay in the BASE: a removed
GPL program's notice costing a few KiB is not worth a rule.

**The build checks it** (a `tools/checks/` step): every path exists in
the staging tree; no file is in two components; every `Needs=` names a
component; and every program built from a `GPL_ONLY_PORTS` port lies in
a component whose `License=` is GPL -- so the switch cannot miss one.

**The manifest tags each line** with `component=<id>` in its options
(the `new-only` / `if-missing` slot). Sizes in the app are sums over
the manifest, never a number someone has to keep true.

**The machine's state is `/var/lib/update/components`**, one line per
DIFFERENCE from the default: `removed doom`, and (for a later
default-off component) `added <id>`. A difference list, not an
installed list, so a component a later build introduces arrives by
default, as a new Windows feature does, and a removed one stays removed.

## How each part uses it

- **`/bin/update` (`common/upd.c`)** skips every manifest line whose
  component is removed -- neither compared nor fetched -- and deletes
  nothing it would not have replaced. A component renamed or dropped by
  the server is dropped from the state file with a log line.
- **Remove** deletes the component's `Paths=` on the machine (offline;
  the component file says what), through the same staged commit the
  update uses, and writes the state. A component holding a `/lib` file
  waits for the next boot, as an update does (none in the first round).
- **Add** fetches the component's lines from the manifest of the build
  the machine RUNS (`# commit` in its record), never the channel's
  newest -- ChromeOS's rule; a component newer than its base is two
  builds in one machine. So the server gains **`GET
  <channel>/builds/<build>/manifest` and `.../files/<path>`** over the
  snapshots `--publish` already keeps. A build the server has pruned
  cannot add: the app says "Update first, then add it".
- **The GPL switch** is the setting `software.show_gpl` (on by default).
  Off, a GPL-licensed component's apps leave the Start menu and the
  desktop: toywm's own scan of `/usr/wm/applications/` (`gui_apps.c`,
  with its live reload) and `base/uappentry.h`'s walk both skip an entry
  whose file lies in a hidden component, through a small
  `base/ucomponent.h` that reads `/usr/share/components/` and the state
  -- the first caller to move toywm's scan onto `uappentry` if it fits
  (fix as touched). The files stay and the
  command line still runs them; it is a VIEW, and a component other
  apps need (`webengine`) is hidden but still works, which the app's
  banner says.
- **The Web page engine can be removed.** Help and System Update's notes
  then show their Markdown through `uui_markdown` instead of
  `uui_webview`, which already knows when it has no engine (the NOGPL
  build's branch in `start_process()`). The details pane warns first.

## The app

`userland/apps/system/software/`, "Software" in the Start menu
(`installer` and `wininstall` are taken).

- **Cards (S1)** by default: categories left (All, Installed, Available,
  then each `Category=`), a grid of `uui_appcard`, search, the GPL
  switch, Check server. **List (S2)** is a View option: columns Name,
  Licence, Size, Status, with a details pane (licence, size, Needed by,
  Needs, a link to the build's source for a GPL one).
- **Changes are queued**: Install / Remove mark a card, a footer names
  each change and its size, Discard drops them, **Apply** runs them in
  order through `uui_steplist` (Disks' shape), one row per component.
- **`uui_appcard`** in `libs/ui/`, card and row layouts of one widget:
  icon tile, name, kind, summary, licence badge, size, one button, a
  pending state. Reusable for anything listing installable things.

A command line for the same state comes with stage 2 (name to agree):
list, remove, add, with the app and the command sharing one engine in
`libbase`, as `bootcfg` and Boot Manager do.

## Stages

Each stage is useful alone, and each is tested red before green.

1. **Components in the build**: the `.component` files, staging to
   `/usr/share/components/`, the check, and `component=` in the
   manifest. Host tests only; nothing on a machine changes.
2. **The engine and the command**: the state file, `upd.c` skipping
   removed components, Remove, Add from the machine's own build (the
   server's per-build routes). A VM test: remove `demos`, update, still
   absent; add it back, the files match the manifest.
3. **The engine-less fallback**: Help and System Update show Markdown
   with no `/bin/webcontent`. Before stage 4 lets anyone remove it.
4. **The GPL switch**: `software.show_gpl`, `ucomponent.h`, the
   `uappentry` filter, the Start menu and desktop refreshing on change.
5. **`uui_appcard` and the Software app**: cards, the list view, the
   queue, Apply. GUI tests per `userland/CLAUDE.md`, a scroll test.

## What this is NOT

- **Not third-party software.** Everything a component holds is built
  from this repo and served by the update server with the base system.
  An app from elsewhere is a different design (a signed bundle, a
  sandbox profile per app), not a fourth kind of manifest line.
- **Not per-user.** Components are the machine's, like `/bin`.
- **Not the installer's choice yet.** A fresh install copies what the
  live machine has; picking components while installing is later.
