# config

**a `/bin` program.**

**Category:** Configuration

## Synopsis

    config COMMAND [ARG]...

## Options

- `-h`, `--help` -- every command and what it does; `config help` and a
  bare `config` print the same.
- `-r`, `--recursive` -- with `get`: everything under a namespace rather
  than one level (`config get -r desktop`).
- `--machine` -- the machine's values in `/etc`, not this person's: a
  `Scope=user` setting is read and written in `/etc`, the default every
  person without a value of their own follows. What a root service sees
  anyway, having no `XDG_CONFIG_HOME`.
- `-n`, `--non-interactive` -- never ask for administrator rights: a
  machine setting set by an account that is not root is refused at once
  (`sudo -n`'s rule), never asked for on the terminal.

**The machine's settings are root's** (users D3b): `config set` or `unset`
of one from any other account asks for administrator rights as `sudo`
does -- the password on the terminal, or the desktop's prompt -- and privd
makes the change, listed in the prompt; a Standard account is refused
(its administrator signs in Settings' Apply instead). A person's setting, and the seat's
(the brightness, the volume, the screen mode, `display.scale`) for the
person at the machine, never ask.

## Description

Every registered setting, its value, and **the file it lives in**. Flags any whose file no longer matches what is live.

Settings live as plain `name=value` text under `/etc` and can be edited
in `edit` — this is the index over those files, which is the part a
directory of text cannot provide about itself. See
`docs/decisions.md`'s settings-registry entry.

| Command | Notes |
|---|---|
| `config list` | Every registered setting, its value, and **the file it lives in**. Flags any whose file no longer matches what is live. |
| `config get [<name>]` | One value -- or, for a BRANCH, every name under it: `config get desktop.transparency` prints the switch's own value and then every transparency setting. A bare namespace (`config get desktop`) prints ONE level, each child with its value and how many names sit under it; `-r` prints all of them; no name at all lists every namespace. Kernel facts are in the same tree (`config get mem`), marked `(fact)`. A leaf falls back to the FACT registry when no setting has that name, so `config get mem.frame.free` answers — **labelled as read-only kernel state**, since a setting survives a reboot and a fact does not exist between them. A list-shaped fact says so and names a tool that can show it. |
| `config set <name> <value>` | Validate, apply and persist. `name=value` works too. A refusal says what WOULD be accepted — the named choices for an enum, the range and step for a number (`try 25..300 % (in steps of 25)`); a value that applied but did **not** save says so rather than reporting success. |
| `config unset <name>` | Removes the key, so the built-in default applies at the next boot. For a person's setting, removes it from their file only, so the machine's value applies again. |
| `config where <name>` | Just the owning file's path — scriptable. |
| `config diff` | Settings whose file differs from what is in effect, i.e. exactly what a hand edit changed and what `reload` would apply. |
| `config reload` | Re-read every file after editing by hand. Reports how many values were **refused**. |
| `config files` | Every known config file, its path, its description, and whether it is built in or declared in `/etc/config.d`. |
| `config show <name\|path>` | Print one config file verbatim. |
| `config find <text>` | Search key names **and** values across every registered config file, `file:key=value` per hit. Case-insensitive. |
| `config register <name> <path> [description]` | Declare a new config file by writing a descriptor into `/etc/config.d`. Picked up live. Root's: another account is refused up front -- run it with `sudo`. |
| `config unregister <name>` | Remove that descriptor. A built-in cannot be unregistered. Root's, as `register`. |
| `config help` | Every command and what it does -- the same page as `-h`. |

**System Settings is GENERATED from the same registry** — it holds no
list of settings and no list of categories, it asks
(`SETTING_OP_COUNT`/`INFO`/`CHOICE`) and draws what comes back. A
setting registered anywhere in the kernel -- or declared by a file in
`/etc/settings.d` -- gains a sidebar home, a page
and a `config` entry with no edit to any of them. It deliberately shows
settings only, not facts: it is the "what can I change" screen, and
read-only counters would bury the settings.

**Settings are named `<namespace>.<name>`** — the namespace being the
registered name of the file the setting lives in, so `font.size` in
`/etc/saunaos.conf` is `system.font.size`. A bare name still works when
only one setting has it; when several do, `config` lists them and
refuses rather than picking. `config set` and `config unset` refuse an
ambiguous name outright, because writing the wrong setting changes
something you did not mean to change. `config list` prints the qualified
form, which is the one that always works.

**A name is a path** (`docs/settings-and-queries.md`, "Names"). A name
with a value of its own prints that value FIRST, alone on its line as a
leaf's does, so a script reading the first line still reads the value:

    $ config get desktop
    desktop.corners                        small  (+5 under it)
    desktop.taskbar.*                      7 settings
    desktop.transparency                   off  (+10 under it)
    ...
    $ config get desktop.transparency
    off
    desktop.transparency.menus             85
    ...
    desktop.transparency.taskbar           75
    desktop.transparency.taskbar.glass     frosted
    ...
    $ config get desktop.transparency.taskbar
    75

A name from before the dotted names (`desktop.transparency_taskbar`)
finds nothing, and a file line under one is not read
(`docs/settings-and-queries.md`, "Names").