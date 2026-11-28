#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

// ACPI tables, READ-ONLY, plus the three things this kernel acts on:
// turning the machine off, resetting it, and the fixed power button.
//
// **THE AML INTERPRETER IS uACPI, VENDORED, AND NOT THIS FILE'S**
// (docs/aml-design.md, stage 4; its interface is the uACPI section
// below). What this file owns is a fixed-layout table walk -- RSDP,
// RSDT/XSDT, FADT, MADT -- that runs before the heap, and one
// exception kept until stage 4d moves it onto uACPI: a BYTE SCAN of the DSDT
// for the `_S5_` object, because the sleep-type values a poweroff must
// write live in AML and nowhere else. Linux and Windows both evaluate
// `\_S5` with a real interpreter; this decodes the one encoding that
// object is allowed to have and REFUSES anything else, which is the
// difference between "works on QEMU" and "works on VirtualBox and real
// hardware". See docs/decisions.md.
//
// Every table pointer here is a physical address dereferenced through
// boot.asm's identity map of the low 4 GiB. Firmware places ACPI tables
// well below that, and a table above it is REFUSED rather than mapped:
// this runs at boot, before there is anything to map with.

// One table's header. Every ACPI description table starts with it, and
// `length` covers the header plus the body.
struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

// ACPI 2.0's Generic Address Structure: a register named by address
// space rather than assumed to be in I/O space. `space_id` 0 is system
// memory and 1 is system I/O; this kernel handles those two and refuses
// the rest (PCI config space, SMBus, and the embedded controller all
// need a driver that does not exist here).
#define ACPI_IOAPIC_MAX 4
#define ACPI_ISO_MAX    16

struct acpi_gas {
    uint8_t  space_id;
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

#define ACPI_GAS_MEMORY 0
#define ACPI_GAS_IO     1

// Where the RSDP came from. Worth reporting rather than assuming: a
// scanned RSDP means the bootloader passed no tag, which is the first
// thing to check when a machine's tables look wrong.
#define ACPI_RSDP_NONE      0
#define ACPI_RSDP_MULTIBOOT 1 // multiboot2 tag 14/15 -- the normal case
#define ACPI_RSDP_SCAN      2 // found by scanning the EBDA and 0xE0000+

// Discovers the RSDP, validates it, walks the RSDT/XSDT, and decodes the
// FADT, the DSDT's `_S5_` object and the MADT. Everything after this is
// a read of what it found; nothing here touches a device.
void acpi_init(void);

// The tables this boot found, in the order the RSDT/XSDT lists them.
int acpi_table_count(void);
const struct acpi_sdt_header *acpi_table_at(int index);

// The same tables PLUS the DSDT, which is not in the RSDT/XSDT at all
// -- the FADT points at it, so a machine listing 22 tables lists none of
// the ones carrying AML. What QUERY_ACPIDUMP enumerates.
int acpi_dumpable_count(void);
const struct acpi_sdt_header *acpi_dumpable_at(int index);

// The first table with this 4-character signature ("FACP", "APIC",
// "HPET"), or NULL. The signature is NOT NUL-terminated in the table,
// so this compares four bytes.
const struct acpi_sdt_header *acpi_find_table(const char *sig);

// What the tables said, decoded. Read by the QUERY_ACPI provider and by
// the power paths; a field that could not be established is zero, and
// the flags say which.
#define ACPI_F_TABLES     (1u << 0) // an RSDP was found and its tables walked
#define ACPI_F_FADT       (1u << 1) // a FADT was found
#define ACPI_F_S5         (1u << 2) // `_S5_` decoded -- poweroff can name a sleep type
#define ACPI_F_HW_REDUCED (1u << 3) // FADT flags bit 20: no PM1 blocks, use SLEEP_CONTROL_REG
#define ACPI_F_RESET      (1u << 4) // FADT flags bit 10: RESET_REG is usable
#define ACPI_F_ENABLED    (1u << 5) // SCI_EN is set, so the machine is in ACPI mode
#define ACPI_F_XSDT       (1u << 6) // the tables were reached through the 64-bit XSDT
#define ACPI_F_FW_LEGACY  (1u << 7) // the FIRMWARE handed over with ACPI mode off -- S5 hands it back first
#define ACPI_F_PWRBTN     (1u << 8) // the fixed power button is armed and its SCI routed
#define ACPI_F_PWRBTN_CM  (1u << 9) // FADT: the button is a control-method DEVICE, which needs AML
#define ACPI_F_EC_LEGACY  (1u << 10) // kept in legacy mode: an embedded controller needs AML (acpi_sci.c)
#define ACPI_F_PWRBTN_UACPI (1u << 11) // the power button is uACPI's fixed event (uacpi_power.c)
#define ACPI_F_S5_UACPI   (1u << 12) // power-off runs _PTS(5) and uACPI's S5 first (uacpi_power.c)

struct acpi_state {
    uint32_t flags;        // ACPI_F_*
    uint32_t rsdp_source;  // ACPI_RSDP_*
    uint32_t rsdp_revision; // 0 for ACPI 1.0, 2 for 2.0+
    uint32_t table_count;

    uint64_t rsdt_phys;    // whichever of the two was used
    uint64_t dsdt_phys;
    // Where the RSDP was found -- GRUB's copy inside the multiboot info
    // (which pmm_init() reserves) or the firmware's own. uACPI starts here.
    uint64_t rsdp_phys;

    uint32_t pm1a_cnt;     // I/O ports, 0 when hardware-reduced
    uint32_t pm1b_cnt;
    // The EVENT blocks, whose FIRST HALF is PM1_STS -- the wake-status
    // bits. They must be cleared before a sleep write or the machine
    // enters S5 with a wake already pending and comes straight back up,
    // which reads as "it restarts instead of shutting down".
    uint32_t pm1a_evt;
    uint32_t pm1b_evt;
    uint8_t  pm1_evt_len;  // the WHOLE block; PM1_STS is the low half
    // The GENERAL PURPOSE EVENT blocks, same [STS][EN] shape. A laptop
    // has GPEs for its lid, its embedded controller, USB and the power
    // button; one of them enabled and pending is a wake, so S5 means
    // turning all of them off first.
    uint32_t gpe0_blk, gpe1_blk;
    uint8_t  gpe0_len, gpe1_len;
    uint32_t pm_tmr;       // the PM timer's I/O port, 0 when there is none
    uint8_t  pm_tmr_bits;  // 24 or 32 (FADT flags bit 8, TMR_VAL_EXT)
    uint32_t smi_cmd;
    uint8_t  acpi_enable;  // the value written to smi_cmd to enter ACPI mode
    uint8_t  acpi_disable; // ...and to leave it, handing the events back to the firmware
    uint16_t sci_int;      // the SCI's ISA IRQ (or GSI); 9 on every PC seen so far
    uint32_t pwrbtn_presses; // power-button events taken since boot
    uint8_t  slp_typ_a;    // from `_S5_`; meaningless unless ACPI_F_S5
    uint8_t  slp_typ_b;
    uint8_t  reset_value;

    struct acpi_gas reset_reg;
    struct acpi_gas sleep_control; // ACPI 5.0 hardware-reduced sleep
    struct acpi_gas sleep_status;

    uint64_t lapic_phys;   // MADT's Local APIC address (with the type-5 override applied)
    uint32_t ioapic_count;
    uint32_t cpu_count;

    // The I/O APICs (MADT type 1) and the INTERRUPT SOURCE OVERRIDES
    // (type 2): an ISA IRQ whose I/O APIC input is not the same number,
    // or whose trigger/polarity is not ISA's edge/high. IRQ 0 -> GSI 2
    // is on every PC; the SCI is the usual level/low one. Ignoring these
    // works on QEMU's default machine and fails on real hardware.
    struct acpi_ioapic { uint8_t id; uint32_t phys; uint32_t gsi_base; } ioapics[ACPI_IOAPIC_MAX];
    struct acpi_iso { uint8_t bus, irq; uint32_t gsi; uint16_t flags; } isos[ACPI_ISO_MAX];
    uint32_t iso_count;
};

// MPS INTI flags, the low nibble of an override's flags: polarity in
// bits 0-1, trigger in bits 2-3, 0 meaning "as the bus does" (ISA:
// active-high, edge).
#define ACPI_ISO_POLARITY(f) ((f) & 3)
#define ACPI_ISO_TRIGGER(f)  (((f) >> 2) & 3)
#define ACPI_ISO_ACTIVE_LOW  3
#define ACPI_ISO_LEVEL       3

const struct acpi_state *acpi_get_state(void);

// Did this machine's own tables name a way to power off? The question
// power.c asks before reaching for a hardcoded port: a machine that
// named one and did not stop is a machine to HALT, not one to guess at.
int acpi_poweroff_known(void);

// One logical processor, from the MADT. `apic_id` is what an
// INIT-SIPI-SIPI would be addressed to; nothing starts one yet.
#define ACPI_CPU_ENABLED       (1u << 0) // the firmware says this one is usable
#define ACPI_CPU_ONLINE_CAPABLE (1u << 1) // ...or could be brought up later
#define ACPI_CPU_X2APIC        (1u << 2) // came from a type-9 entry, not type 0

struct acpi_cpu {
    uint32_t acpi_id;
    uint32_t apic_id;
    uint32_t flags; // ACPI_CPU_*
};

int acpi_cpu_count(void);
const struct acpi_cpu *acpi_cpu_at(int index);

// Turns the machine off through ACPI. Does not return on success.
// Returns 0 when the machine stayed up; acpi_poweroff_known() then says
// whether a named path was tried -- no FADT, no `_S5_` (and no uACPI
// S5), or a register in an address space this kernel does not drive is
// what makes system_poweroff()'s legacy fallback reachable.
//
// With uACPI's namespace ready it runs the firmware's `_PTS(5)` and lets
// uACPI write S5 (uacpi_power.c); otherwise, or when that returns, it
// writes SLP_TYP|SLP_EN into PM1a_CNT (and PM1b_CNT when the FADT names
// one), or into SLEEP_CONTROL_REG on a hardware-reduced platform. May
// park (AML reaches the embedded controller): a syscall's context, with
// interrupts on.
int acpi_poweroff(void);

// Resets the machine through the FADT's RESET_REG. Same contract:
// does not return on success, returns 0 when there is nothing to try,
// which leaves system_reboot()'s 8042 pulse as the fallback.
int acpi_reset(void);

// ACPI MODE AND THE SCI, for the fixed power button: enters ACPI mode,
// masks every GPE (no AML here to service one), arms PM1's PWRBTN_EN
// and routes the SCI. A press sends SIGPWR to init, which shuts down.
// After ioapic_init(), so the line is routed on the controller that
// will deliver it. `acpimode=legacy` skips it. kernel/acpi/acpi_sci.c.
void acpi_sci_init(void);

// Arm PWRBTN_EN again, if acpi_sci_init() armed it. uACPI's event set-up
// disables every fixed event, the power button's among them, before its
// own fixed event takes the button (stage 4c); its start and its teardown
// call this. A press in the gap is lost.
void acpi_sci_rearm(void);
// `acpi=uacpi` entered ACPI mode on a legacy-mode EC machine for
// uacpi_ec.c to serve the controller; when it cannot -- the interpreter
// failed, or the EC's events could not be turned on -- this hands the
// machine back to the firmware, as if the word were absent. A no-op
// anywhere else. Logs `why`.
void acpi_sci_ec_unserved(const char *why);

// --- uACPI, the AML interpreter (docs/aml-design.md, stage 4) --------
//
// Behind `acpi=uacpi`, off by default. acpi_uacpi_start() runs once
// kernel threads exist (after the scheduler, beside fs_threads_start())
// and starts `kacpid`, which loads the namespace, runs _STA/_INI, takes
// the power button and the embedded controller, and then serves uACPI's
// deferred work. Boot does not wait for it -- but it
// runs on core 0 with interrupts off, giving the core up only between
// steps (uacpi_boot.c's top comment).
enum acpi_uacpi_state {
    ACPI_UACPI_OFF = 0,     // not asked for (or torn down)
    ACPI_UACPI_LOADING,     // kacpid is loading the namespace
    ACPI_UACPI_READY,       // loaded and initialised
    ACPI_UACPI_FAILED,      // a step refused; `failed_at` says which
};

struct acpi_uacpi_info {
    uint32_t state;          // enum acpi_uacpi_state
    int32_t  status;         // uACPI's status for a FAILED step, else 0
    const char *failed_at;   // "initialize", "load", "namespace init", or 0
    uint32_t nodes;          // namespace nodes, root excluded
    uint32_t devices;        // of which Devices
    uint64_t load_us;        // initialize + load + namespace init
};

void acpi_uacpi_start(void);
void acpi_uacpi_info(struct acpi_uacpi_info *out);

// The embedded controller on uACPI (uacpi_ec.c, stage 4b). `via` 0 is
// none found, or no interpreter.
enum { ACPI_EC_VIA_NONE = 0, ACPI_EC_VIA_ECDT, ACPI_EC_VIA_CRS };
enum { ACPI_EC_EVENTS_OFF = 0, ACPI_EC_EVENTS_ON, ACPI_EC_EVENTS_STORM };
struct acpi_ec_info {
    uint32_t via;              // ACPI_EC_VIA_*
    uint32_t events;           // ACPI_EC_EVENTS_*
    uint16_t cmd, data;        // the command/status port and the data port
    int32_t  gpe;              // -1: none known
    uint64_t transactions, timeouts, flushed, naps;
    uint64_t longest_wait_ns;  // the longest wait for IBF/OBF that came back in time
    uint64_t gpes, queries;    // GPEs taken, events answered
    uint32_t last_query, distinct;   // the last event's number; how many numbers seen
    char     path[48];         // the EC device, e.g. \_SB.PCI0.LPCB.EC0
};
void acpi_ec_info(struct acpi_ec_info *out);

// Batteries, the AC adapter and the lid (uacpi_supply.c, stage 4e), Linux's
// power_supply shape: one record per supply, energy in mWh whatever unit
// the firmware counts in. ACPI_SUPPLY_UNKNOWN for a number nobody knows.
#define ACPI_SUPPLY_UNKNOWN  UINT64_MAX
#define ACPI_SUPPLY_PATH_MAX 48
enum { ACPI_SUPPLY_MAINS = 1, ACPI_SUPPLY_BATTERY = 2 };
#define ACPI_SUPPLY_PRESENT     (1u << 0)  // a battery in its bay; an adapter device exists
#define ACPI_SUPPLY_ONLINE      (1u << 1)  // mains: plugged in
#define ACPI_SUPPLY_DISCHARGING (1u << 2)
#define ACPI_SUPPLY_CHARGING    (1u << 3)
#define ACPI_SUPPLY_CRITICAL    (1u << 4)  // _BST's own critical bit
#define ACPI_SUPPLY_MAH         (1u << 5)  // the firmware counts mAh; converted

struct acpi_supply {
    uint32_t type;             // ACPI_SUPPLY_MAINS / _BATTERY
    uint32_t flags;            // ACPI_SUPPLY_*
    int32_t  percent;          // of the last full charge; -1 unknown
    uint64_t design_mwh, full_mwh, now_mwh, rate_mw, mv, cycles;
    char     name[8];          // the device's own name, e.g. BAT0
    char     model[32], serial[32], kind[32], maker[32];
};

#define ACPI_POWER_AC_PRESENT    (1u << 0)
#define ACPI_POWER_AC_ONLINE     (1u << 1)
#define ACPI_POWER_BATTERY       (1u << 2)  // a battery is in
#define ACPI_POWER_ON_BATTERY    (1u << 3)
#define ACPI_POWER_LID_PRESENT   (1u << 4)
#define ACPI_POWER_LID_OPEN      (1u << 5)
#define ACPI_POWER_SHUTDOWN_SENT (1u << 6)  // the critical charge sent SIGPWR
#define ACPI_POWER_CRITICAL_PCT  3          // on battery at or below this, a clean shutdown
struct acpi_power {
    uint64_t generation;       // moves with every change below or in a supply
    uint32_t flags;            // ACPI_POWER_*
    uint64_t lid_changes, notifies, polls;
};
void acpi_power_info(struct acpi_power *out);
int  acpi_supply_get(int index, struct acpi_supply *out);   // 0 past the last

// For the KTEST: bring uACPI up in THIS context when it is OFF (it then
// owns that instance) -- 1 on success -- and take that instance down
// again. Waits never park here, they poll. It RUNS THE FIRMWARE'S
// _STA/_INI/_REG, so the KTEST does it only under a hypervisor.
int acpi_uacpi_bring_up_inline(void);
void acpi_uacpi_tear_down_inline(void);

// The query providers. Called from acpi_init(), not from kernel_main().
void acpi_query_init(void);

// Parses a `_S5_` package out of `aml` (a DSDT or SSDT body). Exposed
// for the KTESTs, which build the encodings this has to accept and
// reject without needing a machine that has them. Returns 1 and fills
// both types on success, 0 when the object is absent or encoded in a
// way this does not decode.
int acpi_scan_s5(const uint8_t *aml, uint32_t len, uint8_t *out_a, uint8_t *out_b);

// Sums `len` bytes; an ACPI structure is valid when this is 0.
uint8_t acpi_checksum(const void *p, uint32_t len);

#endif
