/*
 * (67a) The kernel's module loader (kernel/module.h).
 *
 * After the root mount every NAME.mod in /system/<arch>/drivers/ is read
 * (in name order) and laid out: its allocated sections in three runs of
 * pages, code (read + execute), read-only data, and data with .bss (read
 * + write), from the next free place of the module region (MODULE_BASE,
 * near the kernel: common/vmlayout.h). Its symbols are the run's address
 * plus their value, or, undefined, the kernel's export of that name (no
 * other: a module may use only kernel/exports.txt). Its relocations are
 * applied (arch/<arch>/module.c) to a copy in contiguous physical pages,
 * written through the direct map. Its `.module` section is then read
 * there: when no PCI device matches its table the pages go back and
 * nothing is mapped; otherwise its range is mapped with each run's final
 * permissions (paging_map_kernel; ARM: the new code made visible to the
 * instruction fetch) and start() is called for each matching device. A
 * module that cannot be loaded (not ELF, another machine's, a relocation
 * or a name the loader does not know) is reported and left out. (67b) So
 * is one built for another major version of the driver interface, or a
 * newer minor one (drivers/include/driver.h).
 *
 * (67e) A USB module has a `.module_usb` section instead (driver_usb.h's
 * USB_MODULE): it is always kept (its devices may come later), offered
 * every device the USB core has found once it is loaded, and then each
 * one plugged in (modules_usb_offer, from the USB core's hotplug thread);
 * a device matches when its IDs and one of its interfaces fit an entry
 * of the module's table (USB_ANY: any). The configuration is selected
 * first (usb_select). Offers and removals go one at a time (usb_lock).
 */
#include "module.h"
#include "arch.h"
#include "elf.h"
#include "heap.h"
#include "kprintf.h"
#include "memory.h"
#include "mounts.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "string.h"
#include "sync.h"
#include "usb.h"
#include "vfs.h"
#include "vmlayout.h"

#define DRIVERS "/system/" ARCH_NAME "/drivers"
#define MODULES_MAX 32
#define MODULE_FILE_MAX (4u << 20)
#define DEVICES_MAX 8

enum { LOADED, NOT_NEEDED, REFUSED };

static struct module {
    char file[40];
    int state;
    char why[96];                       /* (REFUSED) */
    const struct module_info *info;     /* (LOADED) at its running address */
    const struct module_usb_info *usb;  /* (67e) a USB module's, instead */
    struct usb_device *usb_devices[DEVICES_MAX];
    int usb_device_count;
    uint64_t base, text, rodata, data;  /* its range and the runs' sizes in bytes */
    uint64_t file_size;
    int pci_count;
    struct pci_device *devices[DEVICES_MAX];
    int device_count;
} modules[MODULES_MAX];
static int module_count;
static uint64_t next_free = MODULE_BASE;
static bool looked, skipped;
static struct mutex usb_lock = MUTEX_INIT("modules usb");

static const Elf64_Shdr *section(const uint8_t *file, const Elf64_Ehdr *h, unsigned i)
{
    return (const Elf64_Shdr *)(file + h->e_shoff + (uint64_t)i * h->e_shentsize);
}

static void *export_of(const char *name)
{
    for (int i = 0; i < kernel_export_count; i++)
        if (!strcmp(kernel_exports[i].name, name))
            return kernel_exports[i].address;
    return 0;
}

static bool matches(const struct module_pci *ids, uint32_t count, const struct pci_device *d)
{
    for (uint32_t i = 0; i < count; i++) {
        if (ids[i].vendor == MODULE_BY_CLASS) {         /* (67g) a class of devices */
            if (ids[i].device == (uint16_t)(d->class_code << 8 | d->subclass))
                return true;
        } else if (ids[i].vendor == d->vendor_id && (ids[i].device == MODULE_ANY ||
                                                     ids[i].device == d->device_id)) {
            return true;
        }
    }
    return false;
}

static bool refuse(struct module *m, const char *why)
{
    m->state = REFUSED;
    ksnprintf(m->why, sizeof(m->why), "%s", why);
    return false;
}

/* Lay out, relocate, match and (when its hardware is there) map and start
 * the module in file[0, size). */
static bool load(struct module *m, const uint8_t *file, uint64_t size)
{
    const Elf64_Ehdr *h = (const Elf64_Ehdr *)file;
    if (size < sizeof(*h) || *(const uint32_t *)file != ELF_MAGIC || file[4] != ELFCLASS64 ||
        file[5] != ELFDATA2LSB || h->e_type != ET_REL)
        return refuse(m, "not a relocatable ELF object");
    if (h->e_machine != ELF_MACHINE_NATIVE)
        return refuse(m, "made for another processor");
    if (h->e_shentsize != sizeof(Elf64_Shdr) || h->e_shoff + (uint64_t)h->e_shnum *
        sizeof(Elf64_Shdr) > size || h->e_shstrndx >= h->e_shnum)
        return refuse(m, "its section table is damaged");
    unsigned count = h->e_shnum;
    uint64_t *where = kzalloc(count * sizeof(uint64_t));    /* each section's running address */
    if (!where)
        return refuse(m, "no memory");
    /* The three runs: offsets within each, then the runs one after another. */
    uint64_t run[3] = {0, 0, 0};
    const Elf64_Shdr *info_section = 0;
    const char *names = (const char *)file + section(file, h, h->e_shstrndx)->sh_offset;
    for (unsigned i = 0; i < count; i++) {
        const Elf64_Shdr *s = section(file, h, i);
        if (!(s->sh_flags & SHF_ALLOC) || !s->sh_size)
            continue;
        if (s->sh_type != SHT_NOBITS && s->sh_offset + s->sh_size > size) {
            kfree(where);
            return refuse(m, "a section lies outside the file");
        }
        int r = s->sh_flags & SHF_EXECINSTR ? 0 : s->sh_flags & SHF_WRITE ? 2 : 1;
        uint64_t align = s->sh_addralign > 1 ? s->sh_addralign : 1;
        run[r] = (run[r] + align - 1) & ~(align - 1);
        where[i] = (uint64_t)r << 60 | run[r];      /* (the run, until the runs are placed) */
        run[r] += s->sh_size;
        if (!strcmp(names + s->sh_name, ".module") || !strcmp(names + s->sh_name, ".module_usb"))
            info_section = s;
    }
    if (!info_section) {
        kfree(where);
        return refuse(m, "no .module or .module_usb section (MODULE missing)");
    }
    uint64_t start_of[3], total = 0;
    for (int r = 0; r < 3; r++) {
        start_of[r] = total;
        total += (run[r] + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    }
    uint64_t pages = total / PAGE_SIZE;
    if (!pages || next_free + total > MODULE_BASE + MODULE_SIZE) {
        kfree(where);
        return refuse(m, "no room left in the module region");
    }
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) {
        kfree(where);
        return refuse(m, "no memory");
    }
    uint8_t *copy = phys_to_virt(phys);
    memset(copy, 0, total);
    uint64_t base = next_free;
    for (unsigned i = 0; i < count; i++) {
        const Elf64_Shdr *s = section(file, h, i);
        if (!(s->sh_flags & SHF_ALLOC) || !s->sh_size)
            continue;
        uint64_t at = start_of[where[i] >> 60] + (where[i] & ((1ull << 60) - 1));
        where[i] = base + at;
        if (s->sh_type != SHT_NOBITS)
            memcpy(copy + at, file + s->sh_offset, s->sh_size);
    }
    /* Relocations of loaded sections, against the symbol table. */
    const char *why = 0;
    for (unsigned i = 0; i < count && !why; i++) {
        const Elf64_Shdr *rs = section(file, h, i);
        if (rs->sh_type != SHT_RELA || rs->sh_info >= count || !where[rs->sh_info])
            continue;
        if (rs->sh_link >= count || rs->sh_offset + rs->sh_size > size) {
            why = "a relocation table is damaged";
            break;
        }
        const Elf64_Shdr *ss = section(file, h, rs->sh_link);
        const Elf64_Sym *syms = (const Elf64_Sym *)(file + ss->sh_offset);
        const char *strings = (const char *)file + section(file, h, ss->sh_link)->sh_offset;
        uint64_t nsyms = ss->sh_size / sizeof(Elf64_Sym);
        const Elf64_Rela *rel = (const Elf64_Rela *)(file + rs->sh_offset);
        for (uint64_t k = 0; k < rs->sh_size / sizeof(Elf64_Rela) && !why; k++) {
            uint64_t sym = rel[k].r_info >> 32, value = 0;
            uint32_t type = (uint32_t)rel[k].r_info;
            if (sym >= nsyms) {
                why = "a relocation names no symbol";
                break;
            }
            const Elf64_Sym *y = &syms[sym];
            if (y->st_shndx == SHN_UNDEF && y->st_name) {
                value = (uint64_t)export_of(strings + y->st_name);
                if (!value) {
                    static char missing[96];
                    ksnprintf(missing, sizeof(missing), "it needs %s, which the kernel does not "
                              "export", strings + y->st_name);
                    why = missing;
                    break;
                }
            } else if (y->st_shndx == SHN_ABS) {
                value = y->st_value;
            } else if (y->st_shndx == SHN_COMMON || y->st_shndx >= count) {
                why = "a common or unknown symbol";
                break;
            } else if (y->st_shndx != SHN_UNDEF) {
                value = where[y->st_shndx] + y->st_value;
            }
            uint64_t p = where[rs->sh_info] + rel[k].r_offset;
            int r = arch_module_relocate(type, copy + (p - base), p, value, rel[k].r_addend);
            if (r) {
                static char bad[64];
                ksnprintf(bad, sizeof(bad), "relocation type %u %s", type,
                          r == -1 ? "unknown to the loader" : "out of reach");
                why = bad;
            }
        }
    }
    const struct module_info *info = 0;
    const struct module_usb_info *usb = 0;
    if (!why) {
        const uint8_t *at = copy + (where[info_section - section(file, h, 0)] - base);
        static char version[96];
        uint32_t magic = *(const uint32_t *)at;
        uint16_t major = 0, minor = 0;
        if (magic == MODULE_MAGIC) {
            info = (const struct module_info *)at;
            major = info->api_major, minor = info->api_minor;
        } else if (magic == MODULE_USB_MAGIC) {
            usb = (const struct module_usb_info *)at;
            major = usb->api_major, minor = usb->api_minor;
        } else {
            why = "its .module section is not this kernel's";
        }
        if (!why && (major != DRIVER_API_MAJOR || minor > DRIVER_API_MINOR)) {
            ksnprintf(version, sizeof(version), "made for the driver interface %u.%u, this kernel "
                      "has %u.%u", major, minor, DRIVER_API_MAJOR, DRIVER_API_MINOR);
            why = version;
        }
    }
    kfree(where);
    if (why) {
        pmm_free_pages(phys, pages);
        return refuse(m, why);
    }
    /* The table, read in the copy (its pointer is the running address). */
    struct pci_device *found[DEVICES_MAX];
    int nfound = 0;
    if (info) {
        const struct module_pci *ids = (const struct module_pci *)(copy + ((uint64_t)info->pci - base));
        m->pci_count = (int)info->pci_count;
        for (int i = 0; i < pci_device_count() && nfound < DEVICES_MAX; i++)
            if (matches(ids, info->pci_count, pci_device_at(i)))
                found[nfound++] = pci_device_at(i);
    } else {
        m->pci_count = (int)usb->usb_count;         /* (its USB table's entries) */
    }
    if (info && !nfound) {
        pmm_free_pages(phys, pages);
        m->state = NOT_NEEDED;
        return false;
    }
    for (uint64_t off = 0; off < total; off += PAGE_SIZE) {
        int access = off < start_of[1] ? PAGING_TEXT : off < start_of[2] ? PAGING_RODATA : PAGING_DATA;
        if (!paging_map_kernel(base + off, phys + off, access))
            return refuse(m, "no memory for its page tables");      /* (its pages stay) */
    }
    arch_code_written(copy, run[0]);
    next_free = base + total;
    m->state = LOADED;
    if (info)
        m->info = (const struct module_info *)(base + ((const uint8_t *)info - copy));
    else
        m->usb = (const struct module_usb_info *)(base + ((const uint8_t *)usb - copy));
    m->base = base;
    m->text = run[0];
    m->rodata = run[1];
    m->data = run[2];
    for (int i = 0; i < nfound; i++)
        if (m->info->start(found[i]) && m->device_count < DEVICES_MAX)
            m->devices[m->device_count++] = found[i];
    return true;
}

/* (67e) Does a device fit one of a USB module's entries? Its IDs, and one
 * of its interfaces (the configuration's) for the class fields. */
static bool usb_fits(const struct module_usb_info *u, struct usb_device *dev)
{
    const struct usb_device_descriptor *d = usb_device_descriptor(dev);
    uint8_t *conf = 0;
    int length = -1;
    bool fits = false;
    for (uint32_t i = 0; i < u->usb_count && !fits; i++) {
        const struct module_usb *e = &u->usb[i];
        if ((e->vendor != USB_ANY && e->vendor != d->vendor_id) ||
            (e->product != USB_ANY && e->product != d->product_id))
            continue;
        if (e->interface_class == USB_ANY && e->interface_subclass == USB_ANY &&
            e->interface_protocol == USB_ANY) {
            fits = true;
            break;
        }
        if (!conf && (conf = kmalloc(4096)))
            length = usb_configuration(dev, conf, 4096);
        for (int pos = 0; conf && pos + 9 <= length && conf[pos] >= 2; pos += conf[pos])
            if (conf[pos + 1] == USB_DESC_INTERFACE &&
                (e->interface_class == USB_ANY || e->interface_class == conf[pos + 5]) &&
                (e->interface_subclass == USB_ANY || e->interface_subclass == conf[pos + 6]) &&
                (e->interface_protocol == USB_ANY || e->interface_protocol == conf[pos + 7]))
                fits = true;
    }
    kfree(conf);
    return fits;
}

/* Offer dev to USB module m (usb_lock held). */
static void usb_offer(struct module *m, struct usb_device *dev)
{
    for (int k = 0; k < m->usb_device_count; k++)
        if (m->usb_devices[k] == dev)
            return;
    if (m->usb_device_count == DEVICES_MAX || !usb_fits(m->usb, dev))
        return;
    if (!usb_select(dev)) {
        usb_log_label(usb_device_name(dev));
        kprintf("%s: its configuration cannot be selected\n", m->usb->name);
        return;
    }
    if (m->usb->attach(dev))
        m->usb_devices[m->usb_device_count++] = dev;
}

void modules_usb_offer(struct usb_device *dev)
{
    mutex_lock(&usb_lock);
    for (int i = 0; i < module_count; i++)
        if (modules[i].state == LOADED && modules[i].usb)
            usb_offer(&modules[i], dev);
    mutex_unlock(&usb_lock);
}

void modules_usb_removed(struct usb_device *dev)
{
    mutex_lock(&usb_lock);
    for (int i = 0; i < module_count; i++) {
        struct module *m = &modules[i];
        for (int k = 0; k < m->usb_device_count; k++)
            if (m->usb_devices[k] == dev) {
                m->usb->detach(dev);
                m->usb_devices[k] = m->usb_devices[--m->usb_device_count];
                break;
            }
    }
    mutex_unlock(&usb_lock);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct module *)a)->file, ((const struct module *)b)->file);
}

void modules_load(void)
{
    mutex_lock(&usb_lock);                      /* (67e) no offer while the table is made */
    looked = true;
    struct vfs *fs = mounts_root();
    uint64_t dir_id, pos = 0, id;
    struct rfs_inode dir;
    if (!fs || !vfs_lookup(fs, DRIVERS, &dir_id, &dir)) {
        kprintf("modules:       none: no %s\n", DRIVERS);
        mutex_unlock(&usb_lock);
        return;
    }
    char name[RFS_NAME_MAX + 1];
    uint8_t length, type;
    while (module_count < MODULES_MAX && vfs_dir_next(fs, dir_id, &dir, &pos, name, &length, &id,
                                                      &type) == 1) {
        name[length] = 0;
        if (length < 5 || strcmp(name + length - 4, ".mod") || length >= sizeof(modules[0].file))
            continue;
        ksnprintf(modules[module_count++].file, sizeof(modules[0].file), "%s", name);
    }
    for (int i = 1; i < module_count; i++)          /* (name order: insertion sort) */
        for (int j = i; j > 0 && by_name(&modules[j - 1], &modules[j]) > 0; j--) {
            struct module t = modules[j];
            modules[j] = modules[j - 1];
            modules[j - 1] = t;
        }
    for (int i = 0; i < module_count; i++) {
        struct module *m = &modules[i];
        char path[sizeof(DRIVERS) + sizeof(m->file) + 1];
        ksnprintf(path, sizeof(path), "%s/%s", DRIVERS, m->file);
        struct rfs_inode inode;
        if (!vfs_lookup(fs, path, &id, &inode)) {
            refuse(m, "cannot be read");
            continue;
        }
        if (inode.size > MODULE_FILE_MAX) {
            refuse(m, "larger than 4 MiB");
            continue;
        }
        uint8_t *file = kmalloc(inode.size ? inode.size : 1);
        if (!file) {
            refuse(m, "no memory");
            continue;
        }
        m->file_size = inode.size;
        if (vfs_read(fs, id, &inode, 0, file, inode.size) != (int64_t)inode.size)
            refuse(m, "cannot be read");
        else
            load(m, file, inode.size);
        kfree(file);
        if (m->state == LOADED && m->usb)        /* (67e) the devices found so far */
            for (int k = 0; k < usb_device_count(); k++)
                usb_offer(m, usb_device_at(k));
    }
    /* The boot line: what was loaded (and for which devices), how many not. */
    char line[256];
    size_t at = 0;
    int loaded = 0, unneeded = 0;
    for (int i = 0; i < module_count; i++) {
        struct module *m = &modules[i];
        if (m->state == REFUSED)
            kprintf("modules:       %s: %s\n", m->file, m->why);
        if (m->state == NOT_NEEDED)
            unneeded++;
        if (m->state != LOADED || at >= sizeof(line))
            continue;
        at += (size_t)ksnprintf(line + at, sizeof(line) - at, "%s%s (", loaded++ ? ", " : "",
                                m->info ? m->info->name : m->usb->name);
        if (m->usb && !m->usb_device_count && at < sizeof(line))
            at += (size_t)ksnprintf(line + at, sizeof(line) - at, "no device yet");
        for (int k = 0; k < m->usb_device_count && at < sizeof(line); k++)
            at += (size_t)ksnprintf(line + at, sizeof(line) - at, "%s%s", k ? ", " : "",
                                    usb_device_name(m->usb_devices[k]));
        for (int k = 0; k < m->device_count && at < sizeof(line); k++)
            at += (size_t)ksnprintf(line + at, sizeof(line) - at, "%s%02x:%02x.%u", k ? ", " : "",
                                    m->devices[k]->bus, m->devices[k]->device,
                                    m->devices[k]->function);
        if (at < sizeof(line))
            at += (size_t)ksnprintf(line + at, sizeof(line) - at, ")");
    }
    kprintf("modules: %s loaded; %d not needed\n", loaded ? line : "none", unneeded);
    mutex_unlock(&usb_lock);
}

void modules_print(void)
{
    if (skipped || !looked) {
        kprintf("modules not loaded%s\n", skipped ? ": the command line says nomodules" : " yet");
        return;
    }
    int loaded = 0;
    for (int i = 0; i < module_count; i++)
        loaded += modules[i].state == LOADED;
    kprintf("%d module(s) in %s, %d loaded; the kernel exports %d names to them\n", module_count,
            DRIVERS, loaded, kernel_export_count);
    for (int i = 0; i < module_count; i++) {
        const struct module *m = &modules[i];
        kprintf("  %-16s ", m->file);
        if (m->state == REFUSED) {
            kprintf("refused: %s\n", m->why);
            continue;
        }
        if (m->state == NOT_NEEDED) {
            kprintf("not needed: none of its %d PCI IDs is here (file %lu KB)\n", m->pci_count,
                    (m->file_size + 1023) / 1024);
            continue;
        }
        kprintf("%s at 0x%lx: code %lu KB, read-only %lu KB, data %lu KB (file %lu KB); %d %s "
                "IDs; took", m->info ? m->info->name : m->usb->name, m->base,
                (m->text + 1023) / 1024, (m->rodata + 1023) / 1024, (m->data + 1023) / 1024,
                (m->file_size + 1023) / 1024, m->pci_count, m->usb ? "USB" : "PCI");
        if (!m->device_count && !m->usb_device_count)
            kprintf(" no device%s", m->usb ? " yet (it is offered every USB device plugged in)" : "");
        for (int k = 0; k < m->usb_device_count; k++) {
            const struct usb_device_descriptor *d = usb_device_descriptor(m->usb_devices[k]);
            kprintf(" %s (%04x:%04x)", usb_device_name(m->usb_devices[k]), d->vendor_id,
                    d->product_id);
        }
        for (int k = 0; k < m->device_count; k++)
            kprintf(" %02x:%02x.%u (%04x:%04x)", m->devices[k]->bus, m->devices[k]->device,
                    m->devices[k]->function, m->devices[k]->vendor_id, m->devices[k]->device_id);
        kprintf("\n");
    }
}

void modules_skip(void)
{
    skipped = true;
    kprintf("modules: not loaded: the command line says nomodules\n");
}

/* ---- (37g-2) Sleep: the modules' devices ---- */

const char *modules_cannot_sleep(void)
{
    for (int i = 0; i < module_count; i++) {
        struct module *m = &modules[i];
        if (m->state == LOADED && m->info && m->device_count && !m->info->suspend)
            return m->info->name;
    }
    return 0;
}

/* Every device suspended, the last module's last device first; on a
 * refusal those suspended are resumed and its name returned. */
const char *modules_suspend(void)
{
    for (int i = module_count - 1; i >= 0; i--) {
        struct module *m = &modules[i];
        if (m->state != LOADED || !m->info)
            continue;
        for (int k = m->device_count - 1; k >= 0; k--)
            if (!m->info->suspend(m->devices[k])) {
                for (int j = k + 1; j < m->device_count; j++)
                    m->info->resume(m->devices[j]);
                for (int n = i + 1; n < module_count; n++)
                    for (int j = 0; modules[n].state == LOADED && modules[n].info &&
                                    j < modules[n].device_count; j++)
                        modules[n].info->resume(modules[n].devices[j]);
                return m->info->name;
            }
    }
    return 0;
}

int modules_resume(void)
{
    int failed = 0;
    for (int i = 0; i < module_count; i++) {
        struct module *m = &modules[i];
        for (int k = 0; m->state == LOADED && m->info && k < m->device_count; k++)
            if (!m->info->resume(m->devices[k])) {
                kprintf("sleep: %s did not bring %02x:%02x.%u back\n", m->info->name,
                        m->devices[k]->bus, m->devices[k]->device, m->devices[k]->function);
                failed++;
            }
    }
    return failed;
}
