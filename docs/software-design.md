# Software: optional components in the update stream, and an app to choose them

STAGES 1-5 BUILT (2026-11-09), and the installer's Software step after
them; "What was built" at the end says where it departed from this plan. The picks this follows are the maintainer's, made
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

**`component`** is the command line for the same state, with stage 2:
`component list`, `component remove doom`, `component add doom`, with the app and the command sharing one engine in
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
- **Not a package chooser in the installer.** Installing picks whole
  components (below), never files or versions.

## Choosing components while installing (built 2026-11-09)

**What real systems do.** Calamares' `netinstall` module is a checklist
of package GROUPS, each ticked or not, with a group's `critical` and
`selected` flags, and its `packagechooser` a one-of-several page for a
desktop; both only choose what the next step fetches. Debian's `tasksel`
is a checklist of TASKS (a desktop, an SSH server, the standard
utilities), each a named set of packages. Anaconda's Software Selection
is one base environment plus add-on checkboxes, its size and "add-ons
for the selected environment" recomputed as you tick. None of the three
lets a person pick single packages there; each picks named, sized SETS.

**SaunaOS follows that shape**: a Software step on the installer's rail
(Welcome, Disk, Software, Ready, Installing, Finished -- the maintainer's
I1 on the Design canvas), a checklist of the optional components this
build has, ticked as THIS SESSION has them (one removed in the live
session starts unticked and cannot be ticked: there is nothing here to
copy), a running total -- "Copies N files, X MiB · Y MiB left out" --
and Select all / The system only. `install --without ID,...` and
`--only-system` are the same choice at the command line. Where it
DIFFERS from all three: nothing is fetched. A live session copies itself,
so leaving a component out means NOT COPYING its files, and the closure
is this design's one level of `Needs=` both ways (unticking the engine
unticks the Browser; ticking the Browser ticks the engine), through
`ucomponent_remove_closure()`/`_add_closure()`.

- **The engine is `base/uinstall.h`'s `struct uinstall_software`**, so
  the window and the CLI cannot choose two different ways: the set's
  `removed` mark means "left out of this install", which is what lets
  the closures apply unchanged. `uinstall_measure()` sums every file
  under its OWNER once, so a tick re-adds sums instead of walking `/`
  again, and the Welcome and Ready pages, the room a disk must have and
  the copy's progress all follow the choice.
- **A left-out component's files are not copied, by the longest-path
  rule**: a file whose owner is out is skipped; a directory whose owner
  is out is skipped WHOLE (nothing made, no empty `/bin/wm/demos`) unless
  a component going in claims a longer path under it -- `/tests` stays,
  holding `nsmonkey`, when only Demos and tests is out. That filter is
  `ufileop`'s new `leave_out` hook (rsync's `--exclude`).
- **The target's `/var/lib/update/components` gains `removed <id>`** for
  each left-out one (`ucomponent_state_set_at()`, under `/mnt`), so its
  updates keep it out until `component add` -- an install that cannot
  write that line FAILS, since the first update would fetch the
  component back. The session's own `removed` lines came over with `/var`.
- **The checklist is `uui_appcard`'s third layout, CHECKS**, not an
  installer-private list: a framed list, a checkbox per row, a click or
  Space parks the row for the caller to flip, Enter is left to the
  window's default button, and it scrolls when the window is short. The
  installer pins it to its rows' height when they fit, so a full-screen
  install does not draw five rows in a screen-tall box.

## What was built, and where it departed from the plan

All five stages, each tested red before green (`components_hostcheck`,
`component_test`, `engineless_test`, `gpl_switch_test` with
`appentry_test`, `software_test` and `scroll_test`'s two Software views).

- **A component's definition and icon stay in the BASE**, not in its
  `Paths=` as "The data" said: a removed component must still be listed,
  by name, licence and size, for `component list` and the app to offer
  it back with no network. `check_optional_components.py` refuses a
  path under `/usr/share/components` or `/usr/share/icons`.
- **A file belongs to the LONGEST covering path**, so `/tests/nsmonkey`
  (built from NetSurf) is the engine's, carved out of Demos and tests'
  `/tests/`; the same path in two components is still refused. The
  GPL rule became two: a program BUILT from a GPL-only port
  (`PORT_PROGRAMS_<port>`, which DOOM now declares) lies in a GPL
  component; one that only NEEDS one (`NEEDS_PORT`, the Browser) lies in
  a component that is GPL or needs one. So the switch hides the
  engine and DOOM, and the Browser -- MIT -- stays listed, as the
  mockup has it.
- **`Built=if-present`** marks the compiler and make: a checkout with no
  `toolchain/native` has no such component, rather than a failing
  build; all of its paths or none. `NOGPL=1` stages no GPL component.
- **The engine is two halves**, not one in libbase: `base/ucomponent.h`
  (definitions, owner, the state file, Remove, the closures, the GPL
  view) and `common/upd.h`'s `upd_component_add()` and the update's
  skip -- adding fetches, and libbase cannot link libhttp. The command
  and the app link both, as `update` and System Update do.
- **The per-build routes match by `# commit`** across every kept
  snapshot on `main` and `release`, and on `dev` only the build it
  serves. `upd_component_check()` is the app's Check server: does the
  server still have this machine's build.
- **An update from an older server**, whose manifest carries no tags, is
  tagged on the machine from its own definitions, so it cannot undo a
  removal either; an add's per-build manifest is tagged the same way.
- **A removed id is never dropped from the state**, unlike "How each
  part uses it" planned: a build that merely does not STAGE a component
  (`Built=if-present`, `NOGPL=1`) ships neither its lines nor its
  definition, so it looks exactly like one that dropped it, and the next
  build may have it again. `upd.c` reads the removed ids from the state
  file itself, not through the definitions; an id no build has costs a
  line nothing reads.
- **Remove marks first, then deletes**: one stopped part of the way
  stays marked removed and names what is left (`component remove` again
  tries those) -- never files gone with the state saying installed,
  which the next update would quietly undo.
- **Add goes back to the channel the build came from**: each update
  writes `# from <channel address>` at the head of its record
  (`upd_machine_source()`), and an add with no `--from` tries that, then
  the configured server and channel. The ASUS was updated from main
  with `--from` while configured for dev, and an add asked dev for a
  main build. Safe to try both: the route is keyed by the commit.
- **A file's failed transfer is tried once more** (`retrying once` in
  the log), in `fetch_one()`, for update and add alike. On the ASUS one
  add in two lost one file of 233 to "cannot connect ... connection
  reset by peer" -- which this stack reports when a connect's deadline
  passes with no acceptable SYN-ACK, not for a RST (that is "refused") --
  and the request never reached the server. Why no answer came is NOT
  established; QEMU's SLIRP terminates the guest's TCP itself, so the
  host server never sees the guest's ports or sequence numbers and the
  case cannot be reproduced there.
- **toywm's scan did not move onto `uappentry`**: it reads `ShowIn=`,
  `Handles=` and `AppId=` and keeps every value in one allocation per
  entry, which `struct uappentry` does not carry. Both ask
  `ucomponent_view_hides()`. `uappentry_find_exec()` -- naming a program
  already running, for a crash report -- does not follow the switch, nor
  does `uappentry_each_all()`, Task Manager's list of names.
- **The switch is also System Settings > System > Software**, being a
  declared setting; the app's own switch writes the same setting, and
  with it off the app hides the GPL components' cards too, under a
  banner that says what is hidden and offers "Show them".
- **The queue is kept whole both ways**: after every change nothing that
  will be installed needs something that will not, and the component
  just chosen wins -- Keep on the Browser keeps the engine, Install on
  the Browser takes back the engine's queued removal.
- **Apply runs the removals first** (a dependent before what it needs),
  then EVERY addition in ONE `upd_component_add()` -- one manifest, and a
  set holding a library staged for the restart as a whole; the app then
  says "Restart to finish". It reads the GPL switch on the settings
  signal (`uapp_desc.on_setting`), not by polling.
- **The view is remembered** (2026-11-09) in `/etc/software-app.conf`
  (`view=cards|list`, a `base/uprefs.h` table -- not `/etc/software.conf`,
  which is the settings registry's file for `software.show_gpl`), and the
  details pane's Source names the channel the build came from (the
  record's `# from`), falling back to `<server>/<channel>/source/` on a
  machine whose record predates that line. Enter in the Apply dialog
  confirms, and `software_test` covers it.
- **Choosing components while installing** was built after the five
  stages (2026-11-09): "Choosing components while installing" above.

