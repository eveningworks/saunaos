# acpi

**a `/bin` program.**

**Category:** System information

## Synopsis

    acpi [--dump <SIG> [--at N] [--len N]] [--eval <PATH> [INT...]] [--wake]

## Options

- `--dump <SIG>` -- hex-dump the table with that four-character
  signature instead of printing the summary; the two do not mix.
- `--at N` -- start the dump at byte offset `N`, decimal or `0x` hex;
  only with `--dump`.
- `--len N` -- dump `N` bytes rather than the rest of the table, decimal
  or `0x` hex; only with `--dump`.
- `--eval <PATH> [INT...]` -- run one AML method (or read one object) with
  up to seven Integer arguments, decimal or `0x` hex, and print its Integer
  result; `PATH` is from the root, with or without its `\` (a shell eats
  it). Root only, and refused unless the kernel was booted with
  `acpieval` (`SYS_ACPI_EVAL`, Linux's `acpi_call`): it can run anything
  the firmware has.
- `--wake` -- list every device whose `_PRW` names a wake event, instead
  of the summary: its `_HID`, the GPE, the deepest sleep state it wakes
  from, and whether it is armed at power-off (`acpi=uacpi`).

## Description

`/bin/acpi` — what this machine's firmware described in its ACPI tables,
and what the kernel will do with it when asked to stop.

The reason it exists is narrow and worth stating: `reboot --poweroff`
either turns the machine off or it does not, and when it does not there
is nothing on screen to say which half failed. This is that. The line
that matters is `poweroff:` — `yes` names the sleep type and the port
the write will go to, `no` says the machine will fall back to the legacy
port write and then halt.

One line says whether the AML interpreter ran: `AML interpreter: off`
on a default boot, or with the boot word `acpi=uacpi` -- uACPI, the
vendored interpreter (`docs/aml-design.md`, stage 4) -- how many
namespace nodes and devices it loaded and how long it took, or which
step failed with uACPI's status code. Under it, when the interpreter
found one, the embedded controller (stage 4b): its path, where it was
found (`ECDT` or `_CRS`), its ports and GPE, whether its events are on,
then its accesses, how many timed out, the longest wait and how many
waits parked past the 1 ms spin, and the events answered:

    Embedded ctrl:   \_SB_.PCI0.LPCB.EC0_ (ECDT), ports 0x66/0x62, GPE 0x0a, events on
                     64 access(es), 0 timed out, longest wait 2027 us, 1 nap(s); 32 event(s) of 5 kind(s), last 0xd4

With the interpreter, the Power section adds the batteries, the AC
adapter and the lid (stage 4e, `QUERY_POWER`/`QUERY_POWER_SUPPLY`):
each battery's charge, what it is drawn or charged at, its energy against
its last full charge and its design, then its maker, model, serial, type
and cycle count; `the bay is empty` for a slot with no battery; then the
lid, and whether the machine runs on battery and the charge it shuts
down at:

      AC adapter:    plugged in  (AC0_)
      battery BAT0   78%, charging at 15.0 W, 29.8 of 38.2 Wh (design 45.0 Wh)
                     SaunaOS fixture SAUNA-BAT, serial 0001, LION, 212 cycles
      lid:           open, 0 change(s) since boot
      on battery:    no; shuts down at 3%

`?` is a number the firmware did not give -- on the ASUS the battery
answers its default package, every number unknown, until its EC has
read the pack (`dmesg | grep power:` says "not ready yet").

Then the temperatures (`QUERY_THERMAL`): each firmware thermal zone
with its reading, the highest since boot and its critical and passive
trips (`acpi=uacpi`, `kernel/acpi/uacpi_thermal.c`), and the CPU's own
sensors -- the package and each core against TjMax -- on an Intel CPU
outside a virtual machine (`kernel/arch/x86_64/cputherm.c`). While a
zone's passive cooling is limiting the CPUs a line under it says how
far, and why the limit did not reach the hardware when it did not:

    Thermal:
      \_TZ_.THRM     48 C  highest 55 C  critical 98 C  passive 105 C
      \_TZ_.TZ01     30 C  highest 30 C  critical 105 C  passive 95 C
      Package        48 C  highest 48 C  TjMax 95 C
      Core 0         48 C  highest 48 C  TjMax 95 C

(the ASUS: its own `_PSV` is over its `_CRT`, so the firmware never asks
for passive cooling before the shutdown; `thermal.psv=` moves it).

`events OFF after a storm` means its GPE fired over 100 times in one
second and was left off for the boot; `dmesg | grep ec:` names each event's
`_Qxx`.

The other half is the processor list, which comes from the MADT rather
than from CPUID: CPUID describes the core running the instruction and
cannot count the others. `online: yes` marks a core the kernel started,
and every online core runs processes (`docs/smp-design.md`) -- unless
the boot said `smp=park` (the LAST `smp=` on the line wins) or the
kernel could not start a core scheduling, when a line says how many do
and which of the two it was. `nosmp` or `maxcpus=N` on the kernel
command line starts fewer.

## `--wake`: the devices that can wake the machine

    Device                          HID       GPE   wakes from  at power-off
    \_SB_.PCI0.XHC_                 -         0x6d  S3          -
    \_SB_.SLPB                      PNP0C0E   0x0b  S4          -

Linux's `/proc/acpi/wakeup`. Every listed GPE is marked for wake, so it is
not one of the runtime GPEs whose `_Lxx`/`_Exx` the firmware runs; a
power button, sleep button or lid that wakes from S5 is armed at power-off,
the rest are listed only. `block` is a GPE block device's event, listed and
not set up.

## `--eval`: one firmware method by hand

    acpi --eval _SB.PCI0.LPCB.EC0.GBTT 0      -> 255 (0xff)
    acpi --eval _TZ.RTMP                      -> 49 (0x31)

On a boot with `acpi=uacpi acpieval` only. An Integer result prints in
decimal and hex; anything else says `(not an Integer)` -- a Field read by
name is one, so ask a method that returns it. The QEMU fixture's battery
and lid are moved this way (`tools/tests/fixtures/acpi/power.asl`'s `T*`
methods). Every call is logged (`acpi: pid N evaluated ...`).

## `--dump`: the raw bytes of one table

`acpi --dump DSDT --at 0x1200 --len 256` hex-dumps a range of a table by
its four-character signature. With no range you get the whole table,
which for a real DSDT is tens of kilobytes.

It exists for the same reason `lsusb -D` does: **a parser for firmware
data has to be tested against real firmware data**, and a hand-written
fixture only ever agrees with the parser written beside it. These bytes
paste into a KTEST.

**`--dump` sees the DSDT; the table list above does not.** The DSDT is
not in the RSDT or XSDT — the FADT points at it — so a machine that
lists 22 tables lists none of the ones carrying AML. This flag has its
own enumeration with the DSDT appended, and every record carries its
signature.

**Do not ask for a whole big table through the debug console.** That
much hex outruns it and lines go missing, scattered rather than
truncated, which reads as a successful dump. `tools/dev/acpi_dump.py` reads
it in ranges and verifies the ACPI checksum, which is the check that
catches exactly that.

**A real DSDT can be very large** — 103,006 bytes on one laptop here,
against QEMU's 8,605 — so dumping one by hand is not the way to move
it. Where a whole table is genuinely needed, drive it with
`tools/dev/acpi_dump.py`; where the QUESTION is what the table says, it is
usually cheaper to make the kernel answer it.

## What it does not do

**Without the interpreter, its power line is a byte scan.** With
`acpi=uacpi` and the namespace loaded, power-off runs the firmware's
`_PTS(5)` and uACPI writes S5 (`poweroff: yes -- through uACPI, after
the firmware's _PTS(5)`), and the power button is uACPI's fixed event
(`SCI 9, uACPI's fixed event`). Otherwise it reads the sleep types from
a byte scan of the DSDT for the `_S5_` object, which accepts one
encoding and **refuses** everything else rather than guessing -- a
refusal shows up here as `poweroff: no` (`docs/aml-design.md`, stage 4c).

**It does not dump table bodies** in the summary; `--dump` is the way to
the bytes.

**It changes nothing.** Every number is read through `SYS_QUERY`; there
is no `acpi enable` verb. The kernel enters ACPI mode at boot, for the
power button, unless booted with `acpimode=legacy`; `(firmware booted in
legacy mode)` after it means power-off hands ACPI mode back first.
`power button: no -- left to the firmware` is a legacy-mode machine with
an embedded controller, kept in legacy mode unless booted with
`acpi=uacpi`, which runs the controller's AML (`docs/aml-design.md`).

## Output

    RSDP:            revision 2, from the multiboot2 tag
    Root table:      XSDT at 0x7ffe1a2b, 8 table(s)
    ACPI mode:       yes  (firmware booted in legacy mode)

    Power:
      poweroff:      yes  -- S5 type 0 to PM1a port 0x604
      reset:         yes  -- value 0xf to port 0xcf9
      power button:  yes  -- SCI 9, 0 press(es) since boot
      SMI command:   port 0xb2, enable 0xf1

    Tables:
      FACP  0x7ffe0000     244 bytes  rev 3    BOCHS  BXPC
      APIC  0x7ffe0100     120 bytes  rev 1    BOCHS  BXPC
      ...

    Processors (MADT):
      cpu0    apic   id 0     acpi id 0     enabled   online: yes
      cpu1    apic   id 1     acpi id 1     enabled   online: yes
      2 listed, 2 online, 1 I/O APIC(s), local APIC at 0xfee00000

A machine with no ACPI at all prints that plainly and says the poweroff
will halt instead — a real supported state, not a failure.

`RSDP: ... from the BIOS-area scan` means the bootloader passed no ACPI
tag and the kernel found the pointer by scanning the EBDA and the BIOS
ROM area itself. Everything still works; it is worth noticing because it
is the first thing to check when a machine's tables look wrong.

## See also

`reboot` (and `reboot --poweroff`), which is what these numbers are for;
`lscpu`, whose "Logical CPUs" line reads the same processor list;
`dmesg`, where the same facts are logged at boot under `acpi:`.
