#include "ehci.h"
#include "arch.h"
#include "kprintf.h"
#include "paging.h"

#define CAP_LENGTH          0x00    /* byte: where the operational registers start */
#define CAP_HCCPARAMS       0x08    /* bits 8-15: EECP, first extended capability */
#define OP_USBCMD           0x00
#define OP_USBSTS           0x04
#define USBCMD_RUN          (1u << 0)
#define USBSTS_HALTED       (1u << 12)

/* Extended capability in PCI configuration space: USB legacy support. */
#define LEGSUP_ID           1
#define LEGSUP_BIOS_OWNED   (1u << 16)
#define LEGSUP_OS_OWNED     (1u << 24)
#define LEGCTLSTS           4       /* SMI enables (bits 0-15) and status */

#define TIMEOUT_MS          1000

static void release(struct pci_device *pci)
{
    struct pci_bar *bar = &pci->bars[0];
    if (!bar->size || bar->is_io)
        return;
    pci_write16(pci, PCI_COMMAND, pci_read16(pci, PCI_COMMAND) | PCI_COMMAND_MEMORY);
    volatile uint8_t *regs = paging_map_mmio(bar->address, bar->size);

    /* Ownership: ask the firmware to let go (it answers by clearing its
     * bit); then no more System Management Interrupts from this controller. */
    const char *owner = "not used by the firmware";
    uint8_t eecp = (uint8_t)(mmio_read32(regs + CAP_HCCPARAMS) >> 8);
    if (eecp >= 0x40 && (pci_read32(pci, eecp) & 0xFF) == LEGSUP_ID) {
        uint32_t legsup = pci_read32(pci, eecp);
        if (legsup & LEGSUP_BIOS_OWNED) {
            pci_write32(pci, eecp, legsup | LEGSUP_OS_OWNED);
            for (int ms = 0; ms < TIMEOUT_MS && (pci_read32(pci, eecp) & LEGSUP_BIOS_OWNED); ms++)
                arch_delay_ms(1);
            if (pci_read32(pci, eecp) & LEGSUP_BIOS_OWNED) {
                pci_write32(pci, eecp, (pci_read32(pci, eecp) & ~LEGSUP_BIOS_OWNED) |
                                       LEGSUP_OS_OWNED);
                owner = "taken from the firmware (it did not answer)";
            } else {
                owner = "taken over from the firmware";
            }
        }
        pci_write32(pci, eecp + LEGCTLSTS, 0);
    }

    /* Stop it: it halts within 16 microframes (2 ms). */
    volatile uint8_t *op = regs + mmio_read8(regs + CAP_LENGTH);
    mmio_write32(op + OP_USBCMD, mmio_read32(op + OP_USBCMD) & ~USBCMD_RUN);
    int ms = 0;
    while (ms < 20 && !(mmio_read32(op + OP_USBSTS) & USBSTS_HALTED)) {
        arch_delay_ms(1);
        ms++;
    }
    kprintf("  EHCI:          %02x:%02x.%u (%04x:%04x), USB 2 controller without a driver: "
            "%s, %s\n", pci->bus, pci->device, pci->function, pci->vendor_id, pci->device_id,
            owner, mmio_read32(op + OP_USBSTS) & USBSTS_HALTED ? "stopped" : "does not stop");
}

void ehci_release_all(void)
{
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        if (d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x20)
            release(d);
    }
}
