/*
 * (37b-5) The firmware's side of the tests' embedded controller
 * (scripts/qemu/ritterec.c, QEMU's -device ritterec): an SSDT QEMU adds to
 * its tables (-acpitable), as a laptop's DSDT describes its controller.
 * The model's memory starts as byte i = i * 7 + 3, so: B000 3, B001 0xA,
 * W010 0x7A73, D020 0xF8F1EAE3; TWR1 writes 0x5A and reads it back; TWR2
 * writes the high nibble of byte 0x90 (0xF3) and gives 0x93 (Preserve).
 * (37c-1) A battery and an AC adapter behind it, as a laptop's: the
 * battery's remaining capacity W010 & 0xFFF (2675 mAh of 3500), its rate
 * B001 * 100 (1000 mA), charging; the adapter plugged in (B000 & 1).
 * (37c-2c) The controller's GPE is 7 (the model's sci-gpe), and its events
 * are a laptop's: 0x10 the lid (it closes: LIDV), 0x11 the adapter (B000's
 * bit 0 written to 0: unplugged), 0x12 the battery; 0x13 has no method.
 */
DefinitionBlock ("", "SSDT", 2, "RITTER", "TESTEC", 1)
{
    Scope (\_SB)
    {
        Device (EC0)
        {
            Name (_HID, EisaId ("PNP0C09"))
            Name (_UID, One)
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x62, 0x62, 0x00, 0x01)
                IO (Decode16, 0x66, 0x66, 0x00, 0x01)
            })
            Name (_GPE, 0x07)
            Name (ECOK, Zero)
            Method (_REG, 2)
            {
                If (Arg0 == 3)
                {
                    ECOK = Arg1
                }
            }
            OperationRegion (ERAM, EmbeddedControl, Zero, 0x100)
            Field (ERAM, ByteAcc, Lock, Preserve)
            {
                B000, 8,
                B001, 8,
                Offset (0x10),
                W010, 16,
                Offset (0x20),
                D020, 32,
                Offset (0x80),
                B080, 8,
                Offset (0x90),
                N900, 4,
                N904, 4
            }
            Method (TWR1)
            {
                B080 = 0x5A
                Return (B080)
            }
            Method (TWR2)
            {
                N904 = 9
                Return (N900 + (N904 << 4))
            }
            Method (_Q10)
            {
                \_SB.LIDV = Zero
                Notify (\_SB.LID0, 0x80)
            }
            Method (_Q11)
            {
                B000 &= 0xFE
                Notify (\_SB.AC0, 0x80)
            }
            Method (_Q12)
            {
                Notify (\_SB.BAT0, 0x80)
            }
        }

        Name (LIDV, One)
        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID)
            {
                Return (LIDV)
            }
        }

        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, Zero)
            Method (_STA)
            {
                Return (0x1F)
            }
            Method (_BIF)
            {
                Return (Package (13) { One, 4000, 3500, One, 11100, 400, 100, 10, 10,
                                       "TESTBAT", "0001", "LION", "RITTER" })
            }
            Method (_BST)
            {
                Local0 = Package (4) {}
                Local0 [0] = 2
                Local0 [1] = \_SB.EC0.B001 * 100
                Local0 [2] = \_SB.EC0.W010 & 0x0FFF
                Local0 [3] = 11400
                Return (Local0)
            }
        }

        Device (AC0)
        {
            Name (_HID, "ACPI0003")
            Method (_PSR)
            {
                Return (\_SB.EC0.B000 & One)
            }
        }
    }
}
