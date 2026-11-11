/*
 * example: the template of a RitterOS kernel module (drivers/CLAUDE.md is
 * the guide, drivers/include/driver.h the interface). It drives QEMU's PCI
 * test device (`-device pci-testdev`, 1b36:0005), which nothing else
 * does: it maps the device's first BAR, reads its first register and says
 * so. A real driver would set its device up here and register it with its
 * class (a network card: driver_net.h).
 *
 * Copy this folder, rename the module (DRIVER in the Makefile, MODULE
 * below), put your hardware's IDs in the table, and build with `make`.
 */
#include "driver.h"

struct example {
    struct pci_device *pci;
    volatile uint8_t *regs;             /* BAR 0, uncached */
};

/* Called for each device in the table: true when it takes the device. */
static bool start(struct pci_device *pci)
{
    const struct pci_bar *bar = &pci->bars[0];
    if (!bar->address || bar->is_io || !bar->size) {
        kprintf("example: %02x:%02x.%u: BAR 0 is not memory\n", pci->bus, pci->device,
                pci->function);
        return false;
    }
    struct example *e = kzalloc(sizeof(*e));
    if (!e)
        return false;
    e->pci = pci;
    e->regs = paging_map_mmio(bar->address, bar->size);
    uint32_t first = mmio_read32(e->regs);
    kprintf("example: QEMU PCI test device at %02x:%02x.%u, BAR 0 at 0x%lx (%lu KB), first "
            "register 0x%08x; driver interface %u.%u\n", pci->bus, pci->device, pci->function,
            bar->address, bar->size / 1024, first, DRIVER_API_MAJOR, DRIVER_API_MINOR);
    return true;
}

static const struct module_pci ids[] = {
    { 0x1B36, 0x0005 },                 /* QEMU's PCI test device */
};

MODULE(example, start, 0, 0, ids)
