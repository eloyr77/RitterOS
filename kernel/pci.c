#include "pci.h"
#include "cpus.h"
#include "acpi.h"
#include "devpath.h"
#include "heap.h"
#include "kprintf.h"
#include "memory.h"
#include "panic.h"
#include "paging.h"
#include "string.h"

/* ACPI MCFG: one entry per PCI segment group. */
struct mcfg_entry {
    uint64_t base;             /* ECAM base address of start_bus */
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} __attribute__((packed));

struct mcfg {
    struct acpi_header header;
    uint64_t reserved;
    struct mcfg_entry entries[];
} __attribute__((packed));

#define VENDOR_NONE      0xFFFF
#define HEADER_MULTIFUNC 0x80
#define BRIDGE_SECONDARY_BUS 0x19

static struct pci_device *devices;
static int device_count, device_capacity;
static uint64_t ecam_base;
static uint8_t first_bus, last_bus;

static volatile uint8_t *config_space(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint64_t offset = ((uint64_t)(bus - first_bus) << 20) | ((uint64_t)dev << 15) |
                      ((uint64_t)fn << 12);
    return phys_to_virt(ecam_base + offset);
}

bool pci_config_access(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t fn, uint32_t offset,
                       int width, uint64_t *value, bool write)
{
    uint32_t bytes = (uint32_t)width / 8;
    if (!ecam_base || segment != 0 || bus < first_bus || bus > last_bus || dev > 31 || fn > 7 ||
        !bytes || offset % bytes || offset + bytes > 4096)
        return false;
    volatile uint8_t *c = config_space(bus, dev, fn) + offset;
    switch (width) {
    case 8:
        if (write)
            mmio_write8(c, (uint8_t)*value);
        else
            *value = mmio_read8(c);
        return true;
    case 16:
        if (write)
            mmio_write16(c, (uint16_t)*value);
        else
            *value = mmio_read16(c);
        return true;
    case 32:
        if (write)
            mmio_write32(c, (uint32_t)*value);
        else
            *value = mmio_read32(c);
        return true;
    case 64:                            /* (two 32-bit accesses, low first) */
        if (write) {
            mmio_write32(c, (uint32_t)*value);
            mmio_write32(c + 4, (uint32_t)(*value >> 32));
        } else {
            *value = mmio_read32(c) | (uint64_t)mmio_read32(c + 4) << 32;
        }
        return true;
    }
    return false;
}

uint32_t pci_read32(const struct pci_device *d, uint16_t off)
{
    return mmio_read32(d->kernel->config + off);
}

void pci_write32(const struct pci_device *d, uint16_t off, uint32_t v)
{
    mmio_write32(d->kernel->config + off, v);
}

uint16_t pci_read16(const struct pci_device *d, uint16_t off)
{
    return mmio_read16(d->kernel->config + off);
}

void pci_write16(const struct pci_device *d, uint16_t off, uint16_t v)
{
    mmio_write16(d->kernel->config + off, v);
}

uint8_t pci_read8(const struct pci_device *d, uint16_t off)
{
    return mmio_read8(d->kernel->config + off);
}

/*
 * Find each BAR's address and size: write all ones, read back which address
 * bits the device lets us change, restore. Decoding is switched off meanwhile
 * so the device does not respond at the temporary address.
 */
static void read_bars(struct pci_device *d)
{
    int count = d->kernel->header_type == 0 ? 6 : d->kernel->header_type == 1 ? 2 : 0;
    uint16_t command = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));

    for (int i = 0; i < count; i++) {
        uint16_t off = (uint16_t)(0x10 + 4 * i);
        struct pci_bar *bar = &d->bars[i];
        uint32_t orig = pci_read32(d, off);
        pci_write32(d, off, 0xFFFFFFFF);
        uint32_t mask = pci_read32(d, off);
        pci_write32(d, off, orig);
        if (mask == 0)
            continue;                                   /* not implemented */

        if (orig & 1) {                                 /* I/O ports */
            bar->is_io = true;
            bar->address = orig & ~3u;
            bar->size = (uint16_t)(~(mask & ~3u) + 1);
            continue;
        }
        bar->prefetchable = orig & 8;
        uint64_t address = orig & ~0xFull;
        uint64_t size_mask = 0xFFFFFFFF00000000ULL | (mask & ~0xFu);
        if (((orig >> 1) & 3) == 2 && i + 1 < count) { /* 64-bit: uses two BARs */
            bar->is_64bit = true;
            uint32_t orig_hi = pci_read32(d, off + 4);
            pci_write32(d, off + 4, 0xFFFFFFFF);
            uint32_t mask_hi = pci_read32(d, off + 4);
            pci_write32(d, off + 4, orig_hi);
            address |= (uint64_t)orig_hi << 32;
            size_mask = ((uint64_t)mask_hi << 32) | (mask & ~0xFu);
            i++;
        }
        bar->address = address;
        bar->size = ~size_mask + 1;
    }
    pci_write16(d, PCI_COMMAND, command);
}

static void add_device(uint8_t bus, uint8_t dev, uint8_t fn)
{
    if (device_count == device_capacity) {
        int capacity = device_capacity ? device_capacity * 2 : 32;
        struct pci_device *grown = kzalloc((size_t)capacity * sizeof(*grown));
        if (!grown)
            panic("pci: out of memory");
        if (devices)
            memcpy(grown, devices, (size_t)device_count * sizeof(*grown));
        kfree(devices);
        devices = grown;
        device_capacity = capacity;
    }
    struct pci_device *d = &devices[device_count++];
    memset(d, 0, sizeof(*d));
    d->kernel = kzalloc(sizeof(*d->kernel));        /* (67b; its own: the array moves as it grows) */
    if (!d->kernel)
        panic("pci: out of memory");
    d->bus = bus;
    d->device = dev;
    d->function = fn;
    d->kernel->config = config_space(bus, dev, fn);
    d->vendor_id = pci_read16(d, 0x00);
    d->device_id = pci_read16(d, 0x02);
    d->revision = pci_read8(d, 0x08);
    d->prog_if = pci_read8(d, 0x09);
    d->subclass = pci_read8(d, 0x0A);
    d->class_code = pci_read8(d, 0x0B);
    d->kernel->header_type = pci_read8(d, 0x0E) & 0x7F;
    read_bars(d);
}

#define PCI_STATUS              0x06
#define PCI_STATUS_CAPABILITIES 0x0010
#define PCI_CAPABILITIES        0x34

#define MSIX_CONTROL            2       /* in the capability */
#define MSIX_TABLE              4
#define MSIX_ENABLE             0x8000
#define MSIX_FUNCTION_MASK      0x4000
#define MSIX_ENTRY_MASKED       1       /* vector control bit 0 */

uint8_t pci_find_capability(const struct pci_device *d, uint8_t id)
{
    if (!(pci_read16(d, PCI_STATUS) & PCI_STATUS_CAPABILITIES))
        return 0;
    uint8_t off = pci_read8(d, PCI_CAPABILITIES) & 0xFC;
    for (int n = 0; off >= 0x40 && n < 48; n++) {   /* bounded: no endless loops */
        if (pci_read8(d, off) == id)
            return off;
        off = pci_read8(d, (uint16_t)(off + 1)) & 0xFC;
    }
    return 0;
}

static bool spreading;                          /* (65e) see pci_spread_interrupts */
static void spread_one(struct pci_device *d);

bool pci_msix_enable(struct pci_device *d, irq_handler_fn handler, void *context,
                     uint64_t *address, uint32_t *data, uint16_t *entries)
{
    uint8_t cap = pci_find_capability(d, PCI_CAP_MSIX);
    if (!cap)
        return false;
    uint16_t control = pci_read16(d, cap + MSIX_CONTROL);
    uint32_t table = pci_read32(d, cap + MSIX_TABLE);
    const struct pci_bar *bar = &d->bars[table & 7];  /* BAR index, offset in it */
    uint16_t size = (uint16_t)((control & 0x7FF) + 1);
    if (!bar->size || bar->is_io || (table & ~7u) + 16u * size > bar->size)
        return false;
    if (!arch_msi_allocate(handler, context, address, data))
        return false;

    /* Table entries: address low, address high, data, vector control. They
     * are written while the whole function is masked. */
    volatile uint32_t *t = paging_map_mmio(bar->address + (table & ~7u), 16u * size);
    pci_write16(d, cap + MSIX_CONTROL, control | MSIX_ENABLE | MSIX_FUNCTION_MASK);
    for (uint16_t i = 1; i < size; i++)
        mmio_write32(&t[4 * i + 3], mmio_read32(&t[4 * i + 3]) | MSIX_ENTRY_MASKED);
    mmio_write32(&t[0], (uint32_t)*address);
    mmio_write32(&t[1], (uint32_t)(*address >> 32));
    mmio_write32(&t[2], *data);
    mmio_write32(&t[3], mmio_read32(&t[3]) & ~(uint32_t)MSIX_ENTRY_MASKED);
    pci_write16(d, cap + MSIX_CONTROL, (control | MSIX_ENABLE) & ~MSIX_FUNCTION_MASK);
    *entries = size;
    d->kernel->msix_cap = cap;
    d->kernel->msix_entry = t;
    d->kernel->msi_address = *address;
    d->kernel->msi_data = *data;
    if (spreading)
        spread_one(d);                          /* (65e) switched on after the boot's pass */
    return true;
}

#define MSI_CONTROL             2       /* in the capability */
#define MSI_ADDRESS             4
#define MSI_ENABLE              0x0001
#define MSI_MULTIPLE            0x0070  /* bits 4-6: vectors enabled (log2) */
#define MSI_64BIT               0x0080
#define MSI_MASKABLE            0x0100  /* has a mask register per vector */

bool pci_msi_enable(struct pci_device *d, irq_handler_fn handler, void *context,
                    uint64_t *address, uint32_t *data)
{
    uint8_t cap = pci_find_capability(d, PCI_CAP_MSI);
    if (!cap)
        return false;
    uint16_t control = pci_read16(d, cap + MSI_CONTROL);
    bool wide = control & MSI_64BIT;
    if (!arch_msi_allocate(handler, context, address, data))
        return false;
    /* A 32-bit capability cannot hold a message address above 4 GiB, nor a
     * 16-bit data field more (neither happens on x86 or QEMU virt; the
     * allocated vector then stays unused). */
    if ((!wide && *address >> 32) || *data > 0xFFFF)
        return false;

    /* Layout: address low, [address high,] data (16 bits), [mask bits]. */
    uint8_t data_at = wide ? 12 : 8;
    pci_write16(d, cap + MSI_CONTROL, control & ~(MSI_ENABLE | MSI_MULTIPLE));
    pci_write32(d, cap + MSI_ADDRESS, (uint32_t)*address);
    if (wide)
        pci_write32(d, cap + MSI_ADDRESS + 4, (uint32_t)(*address >> 32));
    pci_write16(d, cap + data_at, (uint16_t)*data);
    if (control & MSI_MASKABLE)
        pci_write32(d, cap + data_at + 4, 0);           /* vector 0 not masked */
    pci_write16(d, cap + MSI_CONTROL, (control & ~MSI_MULTIPLE) | MSI_ENABLE);
    d->kernel->msi_cap = cap;
    d->kernel->msi_address = *address;
    d->kernel->msi_data = *data;
    if (spreading)
        spread_one(d);                          /* (65e) switched on after the boot's pass */
    return true;
}

static const char *vendor_name(uint16_t vendor);

/* (65d) Give the device its message interrupt's new address: MSI-X with the
 * entry masked meanwhile; MSI with its vector masked where it can be (a
 * capability without masking takes the address low word in one write). */
static void set_message_address(struct pci_device *d, uint64_t address)
{
    if (d->kernel->msix_cap) {
        volatile uint32_t *t = d->kernel->msix_entry;
        uint32_t ctl = mmio_read32(&t[3]);
        mmio_write32(&t[3], ctl | MSIX_ENTRY_MASKED);
        mmio_write32(&t[0], (uint32_t)address);
        mmio_write32(&t[1], (uint32_t)(address >> 32));
        mmio_write32(&t[3], ctl);
    } else if (d->kernel->msi_cap) {
        uint16_t control = pci_read16(d, d->kernel->msi_cap + MSI_CONTROL);
        bool wide = control & MSI_64BIT;
        uint8_t mask_at = (uint8_t)(d->kernel->msi_cap + (wide ? 16 : 12));
        uint32_t mask = 0;
        if (control & MSI_MASKABLE) {
            mask = pci_read32(d, mask_at);
            pci_write32(d, mask_at, mask | 1);
        }
        pci_write32(d, d->kernel->msi_cap + MSI_ADDRESS, (uint32_t)address);
        if (wide)
            pci_write32(d, d->kernel->msi_cap + MSI_ADDRESS + 4, (uint32_t)(address >> 32));
        if (control & MSI_MASKABLE)
            pci_write32(d, mask_at, mask);
    }
    d->kernel->msi_address = address;
}

/* Network cards' interrupts, once the cores run (65d): one card per core
 * after the boot core, in turn. A card that switches its interrupt on later
 * (the MT7921, after its firmware: 65e) is routed then. */
static int spread_next = 1;

static void spread_one(struct pci_device *d)
{
    int cores = cpus_online();
    if (cores < 2 || d->class_code != 0x02 || (!d->kernel->msi_cap && !d->kernel->msix_cap))
        return;
    if (spread_next >= cores)
        spread_next = 1;
    struct cpu *c = cpu_online_at(spread_next);
    uint64_t address = d->kernel->msi_address;
    if (!arch_msi_route(&address, d->kernel->msi_data, c))
        return;
    if (address != d->kernel->msi_address)
        set_message_address(d, address);
    d->kernel->msi_core = c->index;
    const char *vendor = vendor_name(d->vendor_id);
    kprintf("  interrupts:    %02x:%02x.%u (%s%s%04x:%04x, %s) to core %d\n", d->bus, d->device,
            d->function, vendor, *vendor ? " " : "", d->vendor_id, d->device_id,
            d->kernel->msix_cap ? "MSI-X" : "MSI", c->index);
    spread_next++;
}

void pci_spread_interrupts(void)
{
    spreading = true;
    for (int i = 0; i < device_count; i++)
        spread_one(&devices[i]);
}

static const char *vendor_name(uint16_t vendor)
{
    switch (vendor) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x1234: return "QEMU/Bochs";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x1AF4: return "Red Hat (virtio)";
    default:     return "";
    }
}

static const char *class_name(const struct pci_device *d)
{
    switch (d->class_code << 8 | d->subclass) {
    case 0x0106: return d->prog_if == 0x01 ? "SATA controller (AHCI)" : "SATA controller";
    case 0x0108: return d->prog_if == 0x02 ? "NVMe controller" : "non-volatile memory controller";
    case 0x0100: return "SCSI controller";
    case 0x0101: return "IDE controller";
    case 0x0200: return "Ethernet controller";
    case 0x0300: return "VGA display controller";
    case 0x0380: return "display controller";
    case 0x0600: return "host bridge";
    case 0x0601: return "ISA bridge";
    case 0x0604: return "PCI-to-PCI bridge";
    case 0x0C03: return "USB controller";
    case 0x0C05: return "SMBus controller";
    case 0x00FF: return "unclassified device";
    default:     return d->class_code == 0xFF ? "vendor-specific device" : "device";
    }
}

void pci_print_device(const struct pci_device *d)
{
    kprintf("    %02x:%02x.%u %04x:%04x  %-24s %s\n", d->bus, d->device, d->function,
            d->vendor_id, d->device_id, class_name(d), vendor_name(d->vendor_id));
    for (int i = 0; i < 6; i++) {
        const struct pci_bar *b = &d->bars[i];
        if (!b->size)
            continue;
        uint64_t kib = b->size / 1024;
        kprintf("               BAR%d %s 0x%lx, %lu %s%s\n", i,
                b->is_io ? "I/O " : b->is_64bit ? "mem64" : "mem32", b->address,
                kib ? kib : b->size, kib ? "KiB" : "bytes",
                b->prefetchable ? ", prefetchable" : "");
    }
}

int pci_init(void)
{
    const struct mcfg *m = (const struct mcfg *)acpi_find_table("MCFG");
    if (!m || m->header.length < sizeof(*m) + sizeof(struct mcfg_entry)) {
        kprintf("  PCI:           no ACPI MCFG table, PCI Express not available\n");
        return 0;
    }
    /* Segment group 0 is enough for PCs with one PCI domain. */
    ecam_base = m->entries[0].base;
    first_bus = m->entries[0].start_bus;
    last_bus = m->entries[0].end_bus;
    /* 1 MiB of configuration space per bus (32 devices x 8 functions x 4 KiB). */
    paging_map_mmio(ecam_base, ((uint64_t)(last_bus - first_bus) + 1) << 20);

    for (unsigned bus = first_bus; bus <= last_bus; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            volatile uint8_t *c = config_space((uint8_t)bus, dev, 0);
            if (mmio_read16(c) == VENDOR_NONE)
                continue;
            int functions = (mmio_read8(c + 0x0E) & HEADER_MULTIFUNC) ? 8 : 1;
            for (uint8_t fn = 0; fn < functions; fn++)
                if (mmio_read16(config_space((uint8_t)bus, dev, fn)) != VENDOR_NONE)
                    add_device((uint8_t)bus, dev, fn);
        }
    }

    kprintf("  PCI:           ECAM %p, buses %u-%u, %d devices\n", (void *)ecam_base,
            first_bus, last_bus, device_count);
    for (int i = 0; i < device_count; i++)
        pci_print_device(&devices[i]);
    return device_count;
}

int pci_device_count(void)
{
    return device_count;
}

struct pci_device *pci_device_at(int index)
{
    return index >= 0 && index < device_count ? &devices[index] : 0;
}

/* The bridge (a PCIe port, mostly) whose secondary bus dev is on. */
static struct pci_device *port_above(const struct pci_device *dev)
{
    for (int i = 0; i < device_count; i++)
        if (devices[i].kernel->header_type == 1 && pci_read8(&devices[i], BRIDGE_SECONDARY_BUS) == dev->bus)
            return &devices[i];
    return 0;
}

#define PCI_CAP_EXPRESS     0x10
#define PCIE_LINK_CONTROL   0x10        /* in the PCIe capability */
#define LINK_ASPM           0x0003      /* L0s (bit 0), L1 (bit 1) */

static int aspm_clear(struct pci_device *dev)
{
    uint8_t cap = dev ? pci_find_capability(dev, PCI_CAP_EXPRESS) : 0;
    if (!cap)
        return -1;
    uint16_t control = pci_read16(dev, cap + PCIE_LINK_CONTROL);
    if (control & LINK_ASPM)
        pci_write16(dev, cap + PCIE_LINK_CONTROL, control & ~LINK_ASPM);
    return control & LINK_ASPM;
}

void pci_aspm_off(struct pci_device *dev, int *was_dev, int *was_port)
{
    *was_dev = aspm_clear(dev);
    *was_port = aspm_clear(port_above(dev));
}

void pci_enable_dma(struct pci_device *dev)
{
    pci_write16(dev, PCI_COMMAND, pci_read16(dev, PCI_COMMAND) | PCI_COMMAND_MEMORY |
                                  PCI_COMMAND_BUS_MASTER);
    /* Each bridge on the way to the root passes the device's memory
     * accesses upstream only with its own bus master bit, which firmware
     * sets only where it used a device itself (Linux sets it too). */
    uint8_t bus = dev->bus;
    for (int depth = 0; depth < 8 && bus != first_bus; depth++) {
        struct pci_device *up = 0;
        for (int i = 0; i < device_count && !up; i++)
            if (devices[i].kernel->header_type == 1 && pci_read8(&devices[i], BRIDGE_SECONDARY_BUS) == bus)
                up = &devices[i];
        if (!up)
            break;
        uint16_t command = pci_read16(up, PCI_COMMAND);
        uint16_t wanted = command | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
        if (wanted != command) {
            pci_write16(up, PCI_COMMAND, wanted);
            kprintf("  pci:           %02x:%02x.%u: bus master on, for %02x:%02x.%u below it\n",
                    up->bus, up->device, up->function, dev->bus, dev->device, dev->function);
        }
        bus = up->bus;
    }
}

void pci_print_path(const struct pci_device *dev)
{
    const struct pci_device *path[8];
    int n = 0;
    path[n++] = dev;
    while (n < 8 && path[n - 1]->bus != first_bus) {
        const struct pci_device *up = 0;
        for (int i = 0; i < device_count && !up; i++)
            if (devices[i].kernel->header_type == 1 &&
                pci_read8(&devices[i], BRIDGE_SECONDARY_BUS) == path[n - 1]->bus)
                up = &devices[i];
        if (!up)
            break;
        path[n++] = up;
    }
    char line[256];
    size_t used = 0;
    for (int i = n - 1; i >= 0 && used < sizeof(line); i--)
        used += (size_t)ksnprintf(line + used, sizeof(line) - used, "%s%02x:%02x.%u command 0x%04x status 0x%04x",
                                  i == n - 1 ? "" : ", ", path[i]->bus, path[i]->device,
                                  path[i]->function, pci_read16(path[i], PCI_COMMAND),
                                  pci_read16(path[i], 0x06));
    kprintf("       pci: %s\n", line);
}

struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if)
{
    for (int i = 0; i < device_count; i++)
        if (devices[i].class_code == class_code && devices[i].subclass == subclass &&
            devices[i].prog_if == prog_if)
            return &devices[i];
    return 0;
}

static struct pci_device *find_at(uint8_t bus, uint8_t dev, uint8_t fn)
{
    for (int i = 0; i < device_count; i++)
        if (devices[i].bus == bus && devices[i].device == dev && devices[i].function == fn)
            return &devices[i];
    return 0;
}

struct pci_device *pci_find_by_device_path(const uint8_t *path, uint64_t size)
{
    uint8_t devs[8], fns[8];
    int n = devpath_pci_nodes(path, size, devs, fns, 8);
    if (n == 0)
        return 0;

    /* Start on the root bus; every Pci() node but the last is a bridge whose
     * secondary bus holds the next one. */
    uint8_t bus = first_bus;
    for (int i = 0; i < n; i++) {
        struct pci_device *d = find_at(bus, devs[i], fns[i]);
        if (!d || i == n - 1)
            return d;
        bus = pci_read8(d, BRIDGE_SECONDARY_BUS);
    }
    return 0;
}

/* ---- (37g-1) Across a sleep: in S3 every function loses its
 * configuration (BARs, the command register, its message interrupt). As
 * Linux's pci_save_state and pci_restore_state: the first 256 bytes kept
 * before, and after it the PCI Express controls, then the header from its
 * last dword down (only those that differ: the command register after the
 * BARs; a bridge's windows written whatever they read, Linux's "force"),
 * bridges before the devices behind them, then MSI's address and data
 * before its enable, MSI-X's entry 0 (the kernel uses that one) before its
 * control. ---- */

#define PCIE_CAP                0x10
static uint32_t (*kept)[64];
static int kept_count;

void pci_sleep_save(void)
{
    if (!kept || kept_count < device_count) {
        kfree(kept);
        kept = kmalloc((size_t)device_count * sizeof(*kept));
        kept_count = kept ? device_count : 0;
    }
    for (int i = 0; i < kept_count; i++)
        for (int k = 0; k < 64; k++)
            kept[i][k] = mmio_read32((volatile uint32_t *)(devices[i].kernel->config + 4 * k));
}

/* (37g-5d) As Linux's pci_restore_config_dword: a BAR (retry) written
 * again until it reads back, up to 10 times 1 ms apart (a function just
 * out of S3 may not take it yet). */
static void restore_range(struct pci_device *d, const uint32_t *k, int from, int to, bool force,
                          int retry)
{
    volatile uint8_t *c = d->kernel->config;
    for (int i = to; i >= from; i--) {
        volatile uint32_t *r = (volatile uint32_t *)(c + 4 * i);
        if (!force && mmio_read32(r) == k[i])
            continue;
        mmio_write32(r, k[i]);
        for (int t = 0; t < retry && mmio_read32(r) != k[i]; t++) {
            arch_delay_ms(1);
            mmio_write32(r, k[i]);
        }
    }
}

static void restore_one(struct pci_device *d, const uint32_t *k)
{
    uint8_t e = pci_find_capability(d, PCIE_CAP);
    for (int r = 0; e && r < 4; r++) {              /* device, link, device 2, link 2 controls */
        static const uint8_t at[4] = { 0x08, 0x10, 0x28, 0x30 };
        uint16_t off = (uint16_t)(e + at[r]);
        pci_write16(d, off, (uint16_t)(k[off / 4] >> (off % 4 * 8)));
    }
    if (d->kernel->header_type == 1) {
        restore_range(d, k, 12, 15, false, 0);
        restore_range(d, k, 9, 11, true, 0);
        restore_range(d, k, 1, 8, false, 0);
    } else {
        restore_range(d, k, 10, 15, false, 0);
        restore_range(d, k, 4, 9, false, 10);
        restore_range(d, k, 1, 3, false, 0);
    }
    uint8_t m = d->kernel->msi_cap;
    if (m) {
        uint16_t control = (uint16_t)(k[m / 4] >> 16);
        uint8_t data_at = control & MSI_64BIT ? 12 : 8;
        pci_write16(d, m + MSI_CONTROL, control & ~MSI_ENABLE);
        pci_write32(d, m + MSI_ADDRESS, k[(m + MSI_ADDRESS) / 4]);
        if (control & MSI_64BIT)
            pci_write32(d, m + MSI_ADDRESS + 4, k[(m + MSI_ADDRESS + 4) / 4]);
        pci_write16(d, m + data_at, (uint16_t)k[(m + data_at) / 4]);
        if (control & MSI_MASKABLE)
            pci_write32(d, m + data_at + 4, k[(m + data_at + 4) / 4]);
        pci_write16(d, m + MSI_CONTROL, control);
    }
    uint8_t x = d->kernel->msix_cap;
    if (x && d->kernel->msix_entry) {
        uint16_t control = (uint16_t)(k[x / 4] >> 16);
        volatile uint32_t *t = d->kernel->msix_entry;
        uint32_t low = (uint32_t)d->kernel->msi_address;
        uint32_t high = (uint32_t)(d->kernel->msi_address >> 32);
        pci_write16(d, x + MSIX_CONTROL, control | MSIX_ENABLE | MSIX_FUNCTION_MASK);
        /* (37g-8) Written until it reads back, as the BARs: the VivoBook's
         * xHCI lost the first write after S3 (the address read 0, the data
         * right), and its interrupts went nowhere. */
        for (int try = 0; try < 10; try++) {
            mmio_write32(&t[0], low);
            mmio_write32(&t[1], high);
            mmio_write32(&t[2], d->kernel->msi_data);
            if (mmio_read32(&t[0]) == low && mmio_read32(&t[1]) == high &&
                mmio_read32(&t[2]) == d->kernel->msi_data)
                break;
            arch_delay_ms(1);
        }
        mmio_write32(&t[3], mmio_read32(&t[3]) & ~(uint32_t)MSIX_ENTRY_MASKED);
        pci_write16(d, x + MSIX_CONTROL, control);
    }
}

int pci_wake(void)
{
    int restored = 0;
    for (int pass = 0; pass < 2; pass++)            /* bridges first */
        for (int i = 0; i < kept_count && i < device_count; i++)
            if ((devices[i].kernel->header_type == 1) == (pass == 0)) {
                restore_one(&devices[i], kept[i]);
                restored++;
            }
    return restored;
}
