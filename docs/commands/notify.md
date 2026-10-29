# notify

**a `/bin` program.**

**Category:** The desktop

## Synopsis

    notify [OPTION]... TITLE [BODY]
      or:  notify --close ID

## Options

- `-a`, `--app NAME` -- who it is from (default `Shell`): the name its
  setting has in Settings > Desktop > Notifications.
- `-i`, `--icon ICON` -- an icon name, such as `drive` or `tray-bell`.
- `-u`, `--urgency LEVEL` -- `low`, `normal` (the default) or `critical`.
- `-A`, `--actions LIST` -- up to three button labels, comma-separated.
- `-t`, `--transient` -- not kept in the history.
- `-r`, `--replace ID` -- update notification ID in place.
- `-w`, `--wait` -- wait for the answer and print it.
- `-c`, `--close ID` -- take notification ID down, and do nothing else.
- `-h`, `--help` -- print the usage and exit.

## Description

Puts up a notification: a card above the tray that times out into the
history behind the tray's bell. Prints its id. The shape is freedesktop's
`notify-send`, through the same API every program uses
(`userland/lib/unotify.h`, `WIN_REQ_NOTIFY`).

With `--wait`, prints the label of the action taken, `clicked` for a
click on the card or its history row, or `closed` when it was dismissed;
the exit status is 0 for an action or a click, 1 for closed.

**Do Not Disturb** (the bell's switch, or Settings > Desktop >
Notifications) keeps a notification out of sight -- it goes straight to
the history -- unless it is `critical`, which shows anyway and stays
until answered. An app turned off in Settings shows nothing and is not
kept; one set to *history only* is kept without a card.

## Examples

    notify -a Backup -i drive "Backup finished" "3 files copied to usb0"
    12

    notify -A "Restart now,Later" -w "Update ready" "Restart to finish"
    13
    Restart now

    notify --close 12

## See also

`docs/features.md` (Notifications), `config` (`desktop.notifications.dnd`).
