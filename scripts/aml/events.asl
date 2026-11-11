/*
 * (37c-2b) The firmware's side of the events test (power_button_test): an
 * SSDT QEMU adds to q35's tables (-acpitable). GPE 6 is level-triggered:
 * its method counts and presses the power button device (Notify 0x80),
 * as laptops whose button is a device do; GPE 7 is edge-triggered and
 * only counts. GPE 5 has a method too, but a device's _PRW names it: it
 * is for waking only and must stay off. The test raises GPEs through the
 * ritterec model's "gpe" property.
 */
DefinitionBlock ("", "SSDT", 2, "RITTER", "TESTEVT", 1)
{
    Scope (\_SB)
    {
        Name (GPEL, Zero)
        Name (GPEE, Zero)
        Name (GPEW, Zero)

        Device (PWRB)
        {
            Name (_HID, EisaId ("PNP0C0C"))
            Name (_UID, One)
        }

        Device (WAKD)
        {
            Name (_HID, "RTR0001")
            Name (_PRW, Package (2) { 0x05, 0x03 })
        }
    }

    Scope (\_GPE)
    {
        Method (_L05)
        {
            \_SB.GPEW++
        }

        Method (_L06)
        {
            \_SB.GPEL++
            Notify (\_SB.PWRB, 0x80)
        }

        Method (_E07)
        {
            \_SB.GPEE++
        }
    }
}
