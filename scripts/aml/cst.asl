/*
 * (37d) The processors' sleep states for the tests (cstates_test): an SSDT
 * QEMU adds to its tables (-acpitable), giving both processors the _CST
 * the old laptop's firmware gives (C1 by MWAIT with hint 0, C2 and C3 by
 * reading I/O ports 0x414 and 0x415) on q35's Processor objects
 * (\_SB.CPUS.C000). x86 only: QEMU's ARM machine takes no -acpitable.
 */
DefinitionBlock ("", "SSDT", 2, "RITTER", "TESTCST", 1)
{
    External (\_SB.CPUS.C000, ProcessorObj)
    External (\_SB.CPUS.C001, ProcessorObj)
    Scope (\_SB.CPUS.C000)
    {
        Method (_CST)
        {
            Return (\_SB.CSTS)
        }
    }
    Scope (\_SB.CPUS.C001)
    {
        Method (_CST)
        {
            Return (\_SB.CSTS)
        }
    }

    Scope (\_SB)
    {
        Name (CSTS, Package (4)
        {
            3,
            Package (4)
            {
                ResourceTemplate () { Register (FFixedHW, 0x02, 0x02, 0x0000000000000000, 0x00) },
                1, 1, 0
            },
            Package (4)
            {
                ResourceTemplate () { Register (SystemIO, 0x08, 0x00, 0x0000000000000414, 0x01) },
                2, 18, 0
            },
            Package (4)
            {
                ResourceTemplate () { Register (SystemIO, 0x08, 0x00, 0x0000000000000415, 0x01) },
                3, 350, 0
            }
        })
    }
}
