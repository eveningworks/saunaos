# keyboard

**a shell builtin.**

**Category:** Appearance and the console

## Synopsis

    keyboard <name>

## Description

Selects the keyboard layout, by the name of a file under `/usr/share/kbs/`.
With no argument it prints the usage line and the layout currently
active.

    # keyboard
    usage: keyboard <name>  (currently: us)
    # keyboard de
    Keyboard layout set to de.

The names are XKB's (`de`, `fr`, `se`...): 25 layouts, listed in
`tools/gen_kbs.py`'s `LAYOUTS` (`gen_kbs.py --choices` prints them with
their display names) -- Albanian `al`, German (Austria) `at`, Belgian
`be`, Portuguese (Brazil) `br`, French (Canada) `ca`, German
(Switzerland) `ch`, German `de`, Danish `dk`, Estonian `ee`, Spanish
`es`, Finnish `fi`, Faroese `fo`, French `fr`, English (UK) `gb`,
Icelandic `is`, Italian `it`, Spanish (Latin America) `latam`, Latvian
`lv`, Dutch `nl`, Norwegian `no`, Polish `pl`, Portuguese `pt`, Romanian
`ro`, Swedish `se` and English (US) `us`. System Settings >
Input > Keyboard shows them by name, A to Z; `ls /usr/share/kbs` is the list
on a given disk.

A layout is a DATA FILE, not compiled-in code: `/usr/share/kbs/<name>` maps
evdev keycodes to four characters each -- base, Shift, AltGr and
Shift+AltGr -- and carries its own dead keys. `tools/gen_kbs.py`
generates every one from Linux's own XKB data (its `LAYOUTS` table is
the list), so adding a region is a line there plus a re-seed rather than
an afternoon of tables. The kernel driver owns the wire and knows
nothing about regions; `kernel/lib/keyboard_layout.c` owns the tables.

**Dead keys work as on Windows.** On German, `´` then `e` types `é`;
`´` then Space, or `´` twice, types the accent alone; `´` then a letter
it does not combine with types both (`´x`); Backspace takes a pending
accent back. On Swedish and Finnish that means `~`, `^`, `` ` `` and
`´` are dead keys too, as on those keyboards -- press Space after one
to get the plain character.

The choice becomes the layout a boot **starts with**: it moves to the
front of `keyboard_layouts=` in `/etc/saunaos.conf` (added there if it was
not in the list), so it survives a reboot. The list itself, Super+Space
and dead keys on or off are System Settings' Input > Keyboard page, or
`config set system.keyboard.layouts us,fi,de` and
`config set system.keyboard.dead_keys off`.
Setting and saving are separate steps underneath, and only a FAILED
save is reported -- ` (NOT saved -- /etc unwritable, see dmesg)` is
appended when the write did not land, and silence means it did.

**A missing layout falls back rather than breaking the keyboard.** Ask
for a name with no file and it reverts to what was active and says so;
underneath, `keyboard_layout_load()` falls back to `/usr/share/kbs/us` and
then to a small compiled-in US table, so no state of the disk can leave
the machine unable to type.

**A builtin because it writes the kernel's own state** -- the loaded
table lives in ring 0 and there is no syscall to replace it, which is
the same reason `fontsize` and `cursor` are builtins. See
[the command index](README.md) for the rule.

## What it does not do

**It does not change what `kbd` reports below the character.** The
scancode and the keycode are the wire and the driver; a layout decides
only the character column. That is the split `kbd` prints, and swapping
layouts is the quickest way to see it.

**Only Caps Lock has a state.** It capitalises letters, accented ones
included, and lights the PS/2 LED; NumLock's off-state (the keypad as
arrows) is deliberately not modelled -- the keypad always types the
characters on its keycaps.

**A letter past Latin-1 is typed, and the physical console shows it as
`?`.** A layout's characters are Unicode codepoints (Polish AltGr+a is
`ą`, Romanian AltGr+s is `ș`) and reach a program as UTF-8
(docs/utf8-design.md, step 2), so a desktop window and the Terminal show
them; the kernel console's baked font is ASCII and Latin-1, and draws
anything past it as `?` -- the bytes the program received are the right
ones either way.

**It lists nothing.** There is no `keyboard --list`; the layouts are
whatever files exist, so `ls /usr/share/kbs` is the listing.

**It sets the active layout only.** The list Super+Space walks is
`system.keyboard.layouts`, and the picture of a layout is System
Settings' Keyboard page.
