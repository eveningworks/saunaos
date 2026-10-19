# netd

**a `/bin` program.**

**Category:** Networking

## Synopsis

    netd

## Description

`/bin/netd` is the network daemon: it decides **what each card is
called** and **which cards get an address**. init starts it from
`/etc/services.d/netd`. It takes no arguments.

**Naming.** The kernel gives a card one name, made from the last three
bytes of its MAC — `net-718ebf` — and that is a bootstrap, not a
policy. The rules that turn it into `lan` live in `/etc/net.conf` and
are read here, in ring 3; the kernel only validates and applies a name
through `SYS_NET_RENAME` and has no opinion about what it should be.
That is udev renaming what the Linux kernel called `eth0`, and it is the
same split this project already made for NTP, DHCP and DNS.

**Addresses.** netd runs a lease per card in one loop: `udhcp_step()`
advances one interface and returns when it wants to be called again, so
the loop sleeps until the earliest deadline across every card and no
card blocks another. `dhcp -k` supervises exactly ONE card, which is why
the boot-time client is this and not that: on a two-NIC machine the
second card's lease would be renewed by nobody.

**It polls carrier rather than waiting on it.** `/bin/dhcp` waits up to
ten seconds for the wire, which is right for a command typed at a
prompt. Here it would stall every other card behind a port with no cable
in it, so netd simply does not step a card whose driver reports the link
down. A driver that cannot report carrier is not "down" and is stepped
normally. A lease that runs out while the cable is out is still given
up on time.

**A lease that ends takes its address with it.** Expiry, or a server
refusing a renewal, removes the address from the card and starts over
with a DISCOVER; a lease or a link-local address REPLACES the card's
address, netmask and gateway together, so a gateway from an earlier one
cannot outlive it.

**Cards come and go.** A card absent from a pass is forgotten (its
lease stays on disk), so a fifth adapter is not refused because four
others were once plugged in. One unplugged and put back quicker than a
pass reappears under the kernel's own name, and is treated as new: its
naming rule applies again and its lease starts over under that name.

## The rules file

`/etc/net.conf`. Absent, every card keeps the name the kernel gave it
and every card is leased an address.

The file is re-read on every pass, so `dhcp = none` takes effect at
once. **A NAME, THOUGH, IS APPLIED ONCE PER CARD, AT DISCOVERY** —
renaming a live interface would let a socket bound by name silently
follow to whatever now holds that name, which is the hazard this naming
scheme exists to remove. A changed name takes effect on the next boot,
or when the card is unplugged and put back.

    scheme = mac        how to name a card this file does not name:
                        mac | location | driver | kernel
    prefix = net        what a built name starts with
    dhcp   = all        which cards to lease for: all | none

    [02:00:00:00:00:01] one section per card, keyed by MAC, by
    name = lan          location or by driver -- most specific wins
    dhcp = no           overrules the global `dhcp` for this card

**A CARD IS A SECTION, AND THAT IS WHAT LETS IT CARRY MORE THAN A NAME.**
A card keyed directly, outside any section, is also read -- that form
gives a card its name and nothing else, so `dhcp` there can only be a
machine-wide answer.

    02:00:00:00:00:01 = lan       this exact card, wherever it is plugged
    pci3.0            = builtin   whatever card is in that slot
    r8153             = usb       any card this driver claims

**Most specific wins** — a MAC, then a location, then a driver — so one
adapter can be named individually while a slot or a whole driver is
named by a line below it.

**A NAME IS AN IDENTITY AND FOLLOWS THE CARD.** `scheme = mac` is the
default because moving a USB adapter to another port must not rename it;
`location` is offered for anyone who wants the opposite, which is what
systemd's `enp3s0` does. Where a card currently sits is reported by
`netctl` as `at pci3.0` either way, so it is still findable.

## Being asked: `netctl renew|down|up`

netd answers on a control channel (`lib/unetctl.h`, uchan -- the shape
init's `service` channel has), the way networkd answers `networkctl`.
**`renew`** starts the card's lease over, asking for the remembered
address first; **`down`** switches the card off in the kernel (nothing
sent or received), keeping its address for `up`, and netd then
leaves it alone; **`up`** switches it back on and leases at once. Each is
answered "accepted" immediately and carried out on the next pass,
because a DHCP exchange can block for seconds and every card shares one
loop -- `netctl` watches the card for the outcome. A card renewed by
hand also stops counting as "addressed by hand" until its lease binds.

## Being watched: what each card is doing

A client that sends `NETCTL_WATCH` is told every card's PHASE at once,
then each change as it happens, in its uchan inbox (`struct
netctl_event`): `unmanaged` (`dhcp = no`, or an address set by hand),
`off`, `no cable` (a driver that knows the link is down), `asking` (no
lease yet), `bound`, or `gone`, with when it began. It is pushed, not
polled -- NetworkManager's `StateChanged`, not a status call -- because
a DHCP exchange blocks this loop for seconds and "asking" is said
BEFORE the exchange starts. The taskbar's network item is the watcher
today: asking is what makes its icon move.

A card whose link just went -- switched on and renegotiating, or a
cable pulled -- is looked at every second for 30 seconds rather than
every ten, so the lease follows the link within a second of it coming
back. **A LINK THAT COMES UP ASKS AT ONCE**, the way Windows and
dhclient restart DHCP on a link-up event: netd notices the edge by
polling -- a link it saw down and now sees up, or a card whose packet
counters went backwards, which only one replugged and registered afresh
does -- and a card still asking stops sitting out its backoff (up to a
minute).

## What it deliberately does not do

**Not a route table or a firewall.** Routing is two rules per device and
lives in the kernel; there is nothing here to configure.

**No hot-plug notification.** Nothing tells ring 3 that a card appeared
or a cable went in, so the loop wakes at least every ten seconds to
look. That is a poll, and it is honest about being one.

**It does not name a card it cannot name well.** `scheme = location` on
a driver that reports no location leaves the kernel's name alone rather
than inventing one that says nothing.

**It does not take over a card that already has an address.** One
configured by hand with `netctl address` is left alone, which is
`dhcp`'s rule too. The cost is real and worth knowing: if netd itself is
restarted while a card it leased is up, it sees an addressed card and
leaves it — so that lease is not renewed until the next boot, or until
`netctl renew` asks. `Restart=always` makes
that reachable, and telling the two apart needs the lease file to record
when it was granted, which it does not.

**It is not `/bin/dhcp`.** That command runs the same library and is the
way to lease a card by hand. Running both at once means two clients on
one port, and the second will fail to bind.

## See also

- `dhcp` — the same DHCP client, as a command
- `netctl` — what the cards are called and where they are, and renew/down/up
- `docs/decisions.md`, "An interface name is the card's identity"
