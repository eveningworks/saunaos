# component

**a `/bin` program.**

**Category:** System administration

## Synopsis

    component [OPTION]... [COMMAND [ID]...]

## Options

- `list` (or nothing) -- every optional component this build has: its
  id, name, licence, size and whether it is installed or removed, and
  what it needs.
- `remove <id>...` -- delete the component's files from this machine,
  and those of every installed component that NEEDS it (the Web Browser
  goes with the Web page engine), then mark each removed. Offline: the
  component's own definition says what to delete.
- `add <id>...` -- fetch the component back, and any removed one it
  needs, from the update server, **at the build this machine runs**.
- `--from <url>` -- with `add`, fetch from this CHANNEL'S address
  (`http://<host>:8080/main`) rather than the `update.server` setting's.
- `-h`, `--help` -- every command and option in one page.

`<id>` is the first column of `component list`. A usage error exits 2;
a failed remove or add exits 1.

## Description

An **optional component** is a named set of the system's own files a
machine may do without: DOOM (`doom`), the Web page engine (`webengine`,
NetSurf), the Web Browser (`browser`), the C and C++ compiler (`cc`) and
GNU make (`make`) when the build carries them, Windows 3.x support
(`win16`), and the demo apps and `/tests` (`demos`). The **Software**
app (Start > System) is the same engine with cards and a queue of
changes; `docs/software-design.md` is the why.

    $ component list
      ID         Name                   Licence                 Size  State
      browser    Web Browser            MIT                   278.0K  installed, needs webengine
      demos      Demos and tests        MIT                    13.6M  installed
      doom       DOOM                   GPL-2.0-or-later      688.1K  installed
    ...
    $ component remove demos
    component: removing demos (Demos and tests)
      demos: removed 233 files
    $ component add demos
      adding at this machine's build, 103559db: http://10.0.2.2:8080/main/builds/103559db
      ...
      added 1 component

**A removed component stays removed through every update.** The
machine keeps a DIFFERENCE list, `/var/lib/update/components` -- one
`removed <id>` line each -- and `update` neither compares nor fetches
those files, so a component a later build introduces still arrives by
default (Windows' Optional features and Fedora Silverblue's `rpm-ostree
override remove` keep the same kind of state). A component the server's
build no longer has is dropped from the list once an update is applied.

**Add fetches the machine's OWN build, never the channel's newest** --
ChromeOS's rule for its DLC: a component newer than the rest of the
system would be two builds in one machine. So it asks the server for
`<channel>/builds/<commit>/manifest` and its files, `<commit>` this
machine's (its record's `# commit`). A server that no longer keeps that
build answers 404, and `add` says **update first, then add it**. It is
refused, too, while an update waits for a restart.

A file under `/lib` (none in the first components) is removed or added
at the next boot, as an update's library is, since a running program
may page it in by path.

**Removing the Web page engine** removes the Web Browser too; Help and
System Update then show their pages as plain Markdown. The setting
`software.show_gpl` (the Software app's **Show GPL-licensed apps**)
hides GPL-licensed components' apps from the Start menu and the desktop
without removing anything; `component list` says when it is off.

## Files

- `/usr/share/components/<id>.component` -- each definition, with the
  build's `Size=` and `Files=` (data/components/README.md).
- `/var/lib/update/components` -- what this machine removed.
- `/var/log/update.log` -- an `add`'s log, as an update's.
