# netheal

**a `/bin` program.**

**Category:** Networking

## Synopsis

    netheal

## Description

Reboot, at most twice, if this machine came up with no network at all.

Started by init from `/etc/services.d/netheal` and **idle unless
`system.net.recover` is on**, which ships off. One switch, not two --
the same arrangement `ntpd` has beside it.

## What it is for

**Not a network fault.** The USB NIC on the bare-metal test laptop
sometimes enumerates at FULL speed instead of high speed, wedged, and
`address device` then fails for the rest of the boot -- see
`docs/bugs.md`, "A SUPERSPEED DEVICE CAN LAND ON THE USB2 COMPANION
PORT". A second signature, seen 2026-11-03, has the adapter link at
SuperSpeed and fail `address device` there (the UE300 entries beside
it). The machine boots perfectly and is simply unreachable, which is
expensive precisely because it is headless: nobody can look at it at
the moment something is wrong.

Every software lever on that controller has been tried and measured --
a port reset, a warm reset of the SuperSpeed companion, and the Intel
port-mux cycle all leave the device wedged, and `PPC=0` means there is
no VBUS to switch. What has cleared it, measured, is a reboot --
usually the first. **Not always**: on 2026-11-03 it survived both of
netheal's reboots and one more restart, with the firmware slow to POST
while it lasted, and only the restart after that cleared it -- so two attempts is a ceiling worth keeping rather than
raising.

So this makes the machine rescue itself. **It is not a fix and does not
pretend to be**: the bug entry stays open, and if the next occurrence
is cured by this, what that proves is that a reboot cures it -- which
was already known, and is not true every time.

## Never over a person at the machine

**Once the machine has been touched since boot -- a key, the pointer
(`config get input.events`, the kernel's count; Windows'
`GetLastInputInfo`) -- and a session is open (the desktop, or a
terminal's shell), it does not restart.** It puts up a notification
instead:

> **No network** -- The network adapter did not start. Restarting
> usually brings it back. **[Restart]** **[Not now]**

and counts no attempt. **Restart** goes through the desktop's own Leave
page (`WIN_REQ_LEAVE`, `ui/uleave.h`, asked by root alone): every app is
asked to close first, as when the person picks Restart themselves, and
the restart stops if one says no. **Not now** leaves it. The network
coming back while it waits withdraws the question and clears the
count. With only a terminal session there is no desktop to ask, and it
says so and does nothing.

**Untouched, it restarts as before** -- a headless boot, automatic
sign-in or not, or a login screen nobody signed in at -- at most twice.
Linux's and Windows' network tools never restart a session to fix the
network; on 2026-11-26 this one's reboot landed seconds after a person
signed in on the test laptop.

    netheal: no address 45s after boot -- someone is here, so asking instead of rebooting
    netheal: restarting through the Leave page, as asked

## What it deliberately does NOT do

- **It does not loop.** Attempts are counted in `/var/lib/netheal`, the
  count survives the reboot it causes (the file is `fsync`ed first, or
  a count in the write-back cache would be a count that never
  happened), and after two it says so once and stops. A machine with
  genuinely no NIC -- a VM started with no network -- would otherwise
  qualify on every boot, forever.
- **It does not act on a slow boot.** The count is CLEARED the moment
  any interface has an address, and it polls once a second rather than
  sleeping the whole wait, so a healthy boot clears it in the first few
  seconds. The measured DHCP lease on the test machine arrives about
  10 s in, well clear of the 45 s default.
- **It does not diagnose.** It asks one question -- does any interface
  have an address (`QUERY_NETDEV`, `ip != 0`) -- and nothing about
  why not.

## Settings

| Setting | Default | What it does |
|---|---|---|
| `system.net.recover` | `off` | Whether to reboot at all (a person at the machine is asked instead) |
| `system.net.recover.wait` | `45` (s) | How long to wait for an address first |
