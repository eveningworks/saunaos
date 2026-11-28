/*
 * A battery, an AC adapter, a lid, a thermal zone and two wake devices for
 * QEMU, which emulates none of them (docs/aml-design.md, stage 4e).
 * Compiled with iasl by the tests that use it (power_test, thermal_test)
 * and appended to QEMU's tables with `vm.py --acpi-table`.
 *
 * The numbers start as the ASUS UX305FA's battery reads: 45 Wh design,
 * 38.2 Wh last full, charging at 78%. A test moves them through the T*
 * methods, which store the new state and then Notify() exactly as the
 * laptop's EC query methods do -- so the kernel's Notify path is what runs,
 * not a shortcut. They are called from a guest booted `acpieval` with
 * `acpi --eval \_SB.TSAC 0` and the like (SYS_ACPI_EVAL).
 */
DefinitionBlock ("", "SSDT", 2, "SAUNA", "TESTPWR", 1)
{
    Scope (\_SB)
    {
        Name (TACS, 1)        // the charger is in
        Name (TLID, 1)        // the lid is open
        Name (TBPR, 1)        // a battery is in the bay
        Name (TBST, 2)        // _BST state: 1 discharging, 2 charging, 4 critical
        Name (TBRT, 15000)    // the rate, mW
        Name (TBRC, 29800)    // the remaining energy, mWh
        Name (TBTP, 0)        // the last _BTP the kernel armed

        Device (ACAD)
        {
            Name (_HID, "ACPI0003")
            Name (_PCL, Package () { \_SB })
            Method (_PSR, 0) { Return (TACS) }
            Method (_STA, 0) { Return (0x0F) }
        }

        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, One)
            Name (_PCL, Package () { \_SB })
            Method (_STA, 0)
            {
                If (TBPR) { Return (0x1F) }
                Return (0x0F)
            }
            // Returned from a Name, as the ASUS's BIXT is: this iasl's static
            // check refuses a literal _BIX package of either revision.
            Name (PBIX, Package () {
                Zero,           // revision 0: 20 elements
                Zero,           // power unit: mWh
                45000,          // design capacity
                38200,          // last full charge
                One,            // rechargeable
                11400,          // design voltage, mV
                4500, 1500,     // warning, low
                212,            // cycle count
                100000,         // accuracy
                0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                10, 10,         // granularity
                "SAUNA-BAT", "0001", "LION", "SaunaOS fixture"
            })
            Method (_BIX, 0) { Return (PBIX) }
            Name (PBST, Package (4) { 0, 0, 0, 0 })
            Method (_BST, 0)
            {
                PBST [0] = TBST
                PBST [1] = TBRT
                PBST [2] = TBRC
                PBST [3] = 12100
                Return (PBST)
            }
            Method (_BTP, 1) { TBTP = Arg0 }
        }

        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID, 0) { Return (TLID) }
            Name (_PRW, Package () { 0x0D, 5 })   // wakes from soft off: armed
        }

        // A device that can wake from S3 only, as the ASUS's USB and
        // network devices can: listed, not armed.
        Device (WAK0)
        {
            Name (_HID, EisaId ("PNP0C02"))
            Method (_PRW, 0) { Return (Package () { 0x0E, 3 }) }
        }

        Name (TTMP, 3232)     // the zone, tenths of a kelvin: 50 C

        // The test's knobs.
        Method (TSAC, 1)      // the charger: 1 in, 0 out
        {
            TACS = Arg0
            Notify (ACAD, 0x80)
            Notify (BAT0, 0x80)
            Return (Arg0)
        }
        Method (TSLD, 1)      // the lid: 1 open, 0 closed
        {
            TLID = Arg0
            Notify (LID0, 0x80)
            Return (Arg0)
        }
        Method (TSBT, 3)      // the battery: state, rate mW, remaining mWh
        {
            TBST = Arg0
            TBRT = Arg1
            TBRC = Arg2
            Notify (BAT0, 0x80)
            Return (Arg2)
        }
        Method (TSBP, 1)      // the bay: 1 a battery in, 0 taken out
        {
            TBPR = Arg0
            Notify (BAT0, 0x81)
            Return (Arg0)
        }
        Method (TGBP, 0) { Return (TBTP) }
        Method (TSTZ, 1)      // the zone's temperature, whole degrees C
        {
            TTMP = (Arg0 * 10) + 2732
            Notify (\_TZ.TZ0, 0x80)
            Return (Arg0)
        }
    }

    // The ASUS's zone shape, its trips moved down to where a test can
    // reach them: passive at 80 C, critical at 100 C, sampled every second.
    Scope (\_TZ)
    {
        ThermalZone (TZ0)
        {
            Method (_TMP, 0) { Return (\_SB.TTMP) }
            Method (_CRT, 0) { Return (3732) }
            Method (_PSV, 0) { Return (3532) }
            Name (_PSL, Package () { \_SB })
            Name (_TC1, 2)
            Name (_TC2, 10)
            Name (_TSP, 10)
        }
    }
}
