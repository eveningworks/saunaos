# display

**a `/bin` program.**

**Category:** System information

## Synopsis

    display off | on | status

## Options

- `-h`, `--help` -- print the usage and exit.

## Description

Turns the screen off or back on for now, or says which it is -- the
same switch the desktop's **Turn off the screen after** timer
(System Settings, Display > Screen) throws when nobody has touched the keyboard or
mouse for that long.

**Off is not saved.** On a laptop with an Intel panel the panel's own
power sequence goes down (backlight, then the link, then panel power);
elsewhere the backlight goes to 0. Either way `system.brightness` is
never written, a reboot comes up lit, and **any key or motion at the
desktop turns the screen back on** -- as does `display on`, which takes
about half a second while the panel powers up and its link retrains. A
panic turns it on to show itself.

Hardware with nothing to switch -- a virtual machine's display, a
monitor on a driver without a backlight -- says
`this screen cannot be turned off from here`.

`lsdisplay` shows `Screen: off for now` while it is off.
