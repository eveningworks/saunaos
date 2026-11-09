# `data/components/` -- the optional components

One file per OPTIONAL COMPONENT of the system: a named set of the
build's own files a machine may remove and add back -- Windows'
Optional features, ChromeOS's DLC. Why, and what each part does with
them: `docs/software-design.md`.

`<id>.component` is staged to `/usr/share/components/` by the seed step
(`tools/build/stage_components.py`), which appends `Size=` and `Files=`
summed over the component's files in the image. `/bin/component` and the
Software app read the staged copy (`userland/libs/base/ucomponent.h`);
the update server tags each manifest line with `component=<id>`.

```
Name=Web page engine
Summary=Draws web pages for the Web Browser, Help and System Update's notes
Project=NetSurf
Category=internet
License=GPL-2.0-only
Icon=browser
Needs=
Paths=/bin/webcontent /usr/share/netsurf/ /tests/nsmonkey
```

| Key | Meaning |
|---|---|
| `Name`, `Summary` | What a person reads: the card's title and its line. |
| `Project` | Optional: whose work it is ("NetSurf", "GNU make"), shown beside the category. |
| `Category` | `games`, `internet`, `devtools`, `win16`, `demos` (or `system`, `graphics`, `multimedia`, `utility`) -- the Software app's sidebar. |
| `License` | SPDX. Any `GPL-`/`AGPL-` id makes it a GPL component: its badge says so and **Show GPL-licensed apps** hides its apps. LGPL does not. |
| `Icon` | An icon NAME from `/usr/share/icons/` (data/icons/). |
| `Needs` | Other components, space-separated, ONE level deep: removing one removes what needs it; adding one adds what it needs. |
| `Paths` | Files, and directories ending `/` (everything beneath), space-separated on ONE line. |
| `Built` | `if-present`: a build may not carry it at all (the compiler, from `toolchain/native`) -- all of its paths or none. |

Rules the build holds you to (`tools/checks/check_optional_components.py`,
and the seed step itself):

- **A FILE BELONGS TO THE LONGEST PATH THAT COVERS IT.** `/tests/nsmonkey`
  in the Web page engine is carved out of Demos and tests' `/tests/`; the
  SAME path in two components is refused.
- **Every path is in the image**, except a `Built=if-present`
  component's, which is all or nothing.
- **A program built from a GPL-only port is in a GPL component**, and one
  useless without such a port (`NEEDS_PORT`) is in a component that is
  GPL or needs one -- so the GPL switch cannot miss one.
- **Never `/etc`, `/home` or `/var`** (the machine's own), and never
  `/usr/share/components/` or a component's icon: the definition and its
  icon stay in the BASE, so a removed component is still listed, by
  name and size, ready to add back. A component's licence notice in
  `/usr/share/licenses/` stays too.
- **No `Size=` or `Files=` here** -- the seed step writes them.
