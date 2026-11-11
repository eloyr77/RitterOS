#include "ahci.h"
#include "arch.h"
#include "heap.h"
#include "kprintf.h"
#include "memory.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "timer.h"
#include "sleep.h"

/* Generic host control (AHCI 1.3.1, section 3.1). */
#define HBA_CAP         0x00    /* ports 4-0 (-1), staggered spin-up 27, 64-bit 31 */
#define HBA_GHC         0x04
#define HBA_IS          0x08    /* a bit per port with an interrupt pending */
#define HBA_PI          0x0C    /* ports implemented */
#define HBA_VS          0x10
#define HBA_CAP2        0x24
#define HBA_BOHC        0x28    /* BIOS/OS handoff */
#define CAP_SSS         (1u << 27)
#define CAP_S64A        (1u << 31)
#define GHC_HR          (1u << 0)   /* HBA reset */
#define GHC_IE          (1u << 1)
#define GHC_AE          (1u << 31)  /* AHCI mode (not the old IDE interface) */
#define CAP2_BOH        (1u << 0)
#define BOHC_BOS        (1u << 0)   /* the firmware owns the controller */
#define BOHC_OOS        (1u << 1)   /* the OS asks for it */
#define BOHC_BB         (1u << 4)   /* the firmware is busy with it */

/* Port registers (section 3.3), 0x80 bytes per port from 0x100. */
#define PORT(n)         (0x100 + (n) * 0x80)
#define PX_CLB          0x00    /* command list base (1 KiB aligned) */
#define PX_FB           0x08    /* received FIS base (256 bytes aligned) */
#define PX_IS           0x10
#define PX_IE           0x14
#define PX_CMD          0x18
#define PX_TFD          0x20    /* the disk's status and error registers */
#define PX_SIG          0x24
#define PX_SSTS         0x28    /* link: detection, speed, power state */
#define PX_SCTL         0x2C    /* (37g-6) link control: DET (reset), IPM (low-power states) */
#define PX_SERR         0x30
#define PX_CI           0x38    /* commands issued: a bit per slot */
#define CMD_ST          (1u << 0)   /* start processing the command list */
#define CMD_SUD         (1u << 1)   /* spin up the device */
#define CMD_POD         (1u << 2)   /* power on the device */
#define CMD_FRE         (1u << 4)   /* receive FISes */
#define CMD_FR          (1u << 14)
#define CMD_CR          (1u << 15)
#define TFD_ERR         (1u << 0)
#define TFD_DRQ         (1u << 3)
#define TFD_BSY         (1u << 7)
#define IS_TFES         (1u << 30)  /* task file error */
#define IE_COMPLETION   0x0000000Fu /* register, PIO setup, DMA setup FIS, set device bits */
#define IE_ERRORS       0x7DC00000u /* task file, host bus, interface and data errors */
#define SSTS_DET(s)     ((s) & 0xF)
#define SSTS_SPD(s)     (((s) >> 4) & 0xF)
#define DET_PRESENT     3           /* device there and the link up */

#define SIG_ATA         0x00000101u
#define SIG_ATAPI       0xEB140101u
#define SIG_PM          0x96690101u /* port multiplier */

/* ATA commands (ACS-3). */
#define ATA_IDENTIFY            0xEC
#define ATA_READ_DMA            0xC8
#define ATA_WRITE_DMA           0xCA
#define ATA_READ_DMA_EXT        0x25
#define ATA_WRITE_DMA_EXT       0x35
#define ATA_FLUSH_CACHE         0xE7
#define ATA_FLUSH_CACHE_EXT     0xEA
#define ATA_DATA_SET_MANAGEMENT 0x06    /* feature 1: TRIM */
#define DSM_TRIM                0x01
#define FIS_REG_H2D             0x27

#define MAX_HBAS        4
#define MAX_PORTS       32
#define BOUNCE_PAGES    16          /* 64 KiB per command at most */
#define TIMEOUT_MS      5000
#define FLUSH_MS        30000
#define SPIN_UP_MS      10000       /* hard disks need seconds to spin up */

struct ahci {
    char name[8];                   /* "ahci0" */
    struct pci_device *pci;
    volatile uint8_t *regs;
    bool interrupts;                /* MSI; without it, waits poll */
    struct wait_queue waiters;
    volatile uint64_t interrupt_count;
    uint64_t commands;
    uint32_t cap, pi;               /* (37g-2) as the firmware left them: put back after a reset */
    struct port *ports[MAX_PORTS];  /* (37g-2) its disks */
    int port_count;
    uint32_t sleeps;                /* (37g-6) how the last wake went, for `disks` */
    char after_sleep[160];
};

struct port {
    struct block_device bd;         /* first member: drivers get it back by a cast */
    struct ahci *hba;
    unsigned index;
    volatile uint8_t *regs;
    uint8_t *page;                  /* command list (0), received FISes (1024), table (2048) */
    uint64_t page_phys;
    uint8_t *bounce;                /* physically contiguous transfer buffer */
    uint64_t bounce_phys;
    bool lba48;                     /* 48-bit addresses (else 28-bit) */
    uint8_t features;               /* the next command's features register */
    struct mutex lock;              /* one command at a time */
};

static struct ahci hbas[MAX_HBAS];
static int hba_count, disk_count;

static uint32_t hba_read(struct ahci *h, uint32_t off)
{
    return mmio_read32(h->regs + off);
}

static void hba_write(struct ahci *h, uint32_t off, uint32_t v)
{
    mmio_write32(h->regs + off, v);
}

static uint32_t port_read(struct port *p, uint32_t off)
{
    return mmio_read32(p->regs + off);
}

static void port_write(struct port *p, uint32_t off, uint32_t v)
{
    mmio_write32(p->regs + off, v);
}

/* Interrupt context: no console, no heap. Clears what the ports report and
 * wakes the waiting threads; they look at PxCI themselves. */
static void ahci_interrupt(void *context)
{
    struct ahci *h = context;
    h->interrupt_count++;
    uint32_t pending = hba_read(h, HBA_IS);
    for (unsigned n = 0; n < MAX_PORTS; n++) {
        if (pending & (1u << n))
            mmio_write32(h->regs + PORT(n) + PX_IS, mmio_read32(h->regs + PORT(n) + PX_IS));
    }
    hba_write(h, HBA_IS, pending);
    wait_queue_wake_all(&h->waiters);
}

/* Wait (bounded) until the register at off has (value & mask) == want. */
static bool wait_reg(volatile uint8_t *reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    for (uint32_t i = 0; i <= ms; i++) {
        if ((mmio_read32(reg) & mask) == want)
            return true;
        arch_delay_ms(1);
    }
    return false;
}

/* Done, or stopped by an error: the disk's error bit stays set until the
 * port restarts (PxIS may already have been cleared by the interrupt). */
static bool command_finished(struct port *p)
{
    return !(port_read(p, PX_CI) & 1) || (port_read(p, PX_TFD) & TFD_ERR);
}

/* Stop the port's command processing (ST, then FRE) and wait for it. */
static bool port_stop(struct port *p)
{
    port_write(p, PX_CMD, port_read(p, PX_CMD) & ~CMD_ST);
    if (!wait_reg(p->regs + PX_CMD, CMD_CR, 0, 500))
        return false;
    port_write(p, PX_CMD, port_read(p, PX_CMD) & ~CMD_FRE);
    return wait_reg(p->regs + PX_CMD, CMD_FR, 0, 500);
}

static void port_start(struct port *p)
{
    port_write(p, PX_CMD, port_read(p, PX_CMD) | CMD_FRE);
    port_write(p, PX_CMD, port_read(p, PX_CMD) | CMD_ST);
}

/*
 * Run one ATA command in slot 0: a Register Host-to-Device FIS, and bytes
 * of data to (write) or from the bounce buffer. Returns false on an error
 * or a timeout; the port is then restarted for the next command.
 */
static bool run(struct port *p, uint8_t command, uint64_t lba, uint32_t sectors, uint32_t bytes,
                bool write, uint32_t timeout_ms)
{
    uint8_t *list = p->page, *table = p->page + 2048;
    uint64_t table_phys = p->page_phys + 2048;

    /* The command table: the FIS, then one data area (PRDT entry). */
    memset(table, 0, 0x80 + 16);
    uint8_t *fis = table;
    fis[0] = FIS_REG_H2D;
    fis[1] = 0x80;                  /* a command, not a control update */
    fis[2] = command;
    fis[3] = p->features;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 0x40;                  /* device: LBA addressing */
    if (command == ATA_READ_DMA || command == ATA_WRITE_DMA)
        fis[7] |= (uint8_t)((lba >> 24) & 0x0F);    /* 28-bit: LBA bits 24-27 here */
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)sectors;
    fis[13] = (uint8_t)(sectors >> 8);
    if (bytes) {
        uint32_t *prd = (uint32_t *)(table + 0x80);
        prd[0] = (uint32_t)p->bounce_phys;
        prd[1] = (uint32_t)(p->bounce_phys >> 32);
        prd[3] = (bytes - 1) | (1u << 31);  /* byte count - 1, interrupt when done */
    }

    /* Command header in slot 0: FIS length 5 dwords, direction, 0-1 areas. */
    uint32_t *header = (uint32_t *)list;
    header[0] = 5 | (write ? 1u << 6 : 0) | (bytes ? 1u << 16 : 0);
    header[1] = 0;                  /* bytes transferred, filled in by the controller */
    header[2] = (uint32_t)table_phys;
    header[3] = (uint32_t)(table_phys >> 32);

    port_write(p, PX_IS, 0xFFFFFFFF);
    arch_write_barrier();           /* the structures before the controller looks */
    port_write(p, PX_CI, 1);
    p->hba->commands++;

    uint64_t deadline = timer_ticks() + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 2;
    bool done;
    if (p->hba->interrupts) {
        done = wait_event_until(&p->hba->waiters, command_finished(p), deadline);
    } else {
        while (!(done = command_finished(p)) && timer_ticks() < deadline)
            thread_yield();
    }
    arch_read_barrier();            /* the data after the completion */
    uint32_t tfd = port_read(p, PX_TFD);
    if (done && !(port_read(p, PX_IS) & IS_TFES) && !(tfd & (TFD_ERR | TFD_BSY | TFD_DRQ)))
        return true;

    kprintf("  %s:         command 0x%02x at LBA %lu %s (status 0x%02x, error 0x%02x)\n",
            p->bd.name, command, lba, done ? "failed" : "timed out", tfd & 0xFF,
            (tfd >> 8) & 0xFF);
    port_stop(p);                   /* clears PxCI; the next command starts afresh */
    port_write(p, PX_SERR, 0xFFFFFFFF);
    port_write(p, PX_IS, 0xFFFFFFFF);
    port_start(p);
    return false;
}

static bool transfer(struct port *p, uint64_t lba, uint32_t count, bool write)
{
    uint8_t command = p->lba48 ? (write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT)
                               : (write ? ATA_WRITE_DMA : ATA_READ_DMA);
    return run(p, command, lba, count, count * p->bd.block_size, write, TIMEOUT_MS);
}

/* The most sectors one command moves: the bounce buffer, and 256 for the
 * 28-bit commands (whose count field has 8 bits). */
static uint32_t max_sectors(struct port *p)
{
    uint32_t n = BOUNCE_PAGES * PMM_PAGE_SIZE / p->bd.block_size;
    return p->lba48 ? n : (n < 256 ? n : 256);
}

static bool ahci_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buffer)
{
    struct port *p = (struct port *)dev;
    if (lba + count > dev->block_count)
        return false;
    mutex_lock(&p->lock);
    bool ok = true;
    uint8_t *out = buffer;
    while (ok && count) {
        uint32_t n = count < max_sectors(p) ? count : max_sectors(p);
        ok = transfer(p, lba, n, false);
        if (ok)
            memcpy(out, p->bounce, (size_t)n * dev->block_size);
        out += (size_t)n * dev->block_size;
        lba += n;
        count -= n;
    }
    mutex_unlock(&p->lock);
    return ok;
}

static bool ahci_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buffer)
{
    struct port *p = (struct port *)dev;
    if (lba + count > dev->block_count)
        return false;
    mutex_lock(&p->lock);
    bool ok = true;
    const uint8_t *in = buffer;
    while (ok && count) {
        uint32_t n = count < max_sectors(p) ? count : max_sectors(p);
        memcpy(p->bounce, in, (size_t)n * dev->block_size);
        ok = transfer(p, lba, n, true);
        in += (size_t)n * dev->block_size;
        lba += n;
        count -= n;
    }
    mutex_unlock(&p->lock);
    return ok;
}

/* TRIM: the ranges (48-bit LBA, 16-bit count: 8 bytes each, 64 in a
 * 512-byte block) go in the data of DATA SET MANAGEMENT; one block per
 * command. */
static bool ahci_discard(struct block_device *dev, uint64_t lba, uint64_t count)
{
    struct port *p = (struct port *)dev;
    if (lba + count > dev->block_count)
        return false;
    mutex_lock(&p->lock);
    bool ok = true;
    while (ok && count) {
        uint64_t *entries = (uint64_t *)p->bounce;
        memset(entries, 0, 512);
        for (int i = 0; i < 64 && count; i++) {
            uint64_t n = count < 0xFFFF ? count : 0xFFFF;
            entries[i] = lba | n << 48;
            lba += n;
            count -= n;
        }
        p->features = DSM_TRIM;
        ok = run(p, ATA_DATA_SET_MANAGEMENT, 0, 1, 512, true, TIMEOUT_MS);
        p->features = 0;
    }
    mutex_unlock(&p->lock);
    return ok;
}

static bool ahci_flush(struct block_device *dev)
{
    struct port *p = (struct port *)dev;
    mutex_lock(&p->lock);
    bool ok = run(p, p->lba48 ? ATA_FLUSH_CACHE_EXT : ATA_FLUSH_CACHE, 0, 0, 0, false, FLUSH_MS);
    mutex_unlock(&p->lock);
    return ok;
}

/* An ATA string from IDENTIFY: words with their two bytes swapped, padded
 * with spaces. */
static void ata_string(const uint16_t *words, unsigned count, char *out)
{
    unsigned n = 0;
    for (unsigned i = 0; i < count; i++) {
        out[n++] = (char)(words[i] >> 8);
        out[n++] = (char)words[i];
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0))
        n--;
    out[n] = 0;
    unsigned start = 0;
    while (out[start] == ' ')
        start++;
    memmove(out, out + start, n - start + 1);
}

static const char *speed_text(uint32_t spd)
{
    switch (spd) {
    case 1: return "1.5 Gbit/s";
    case 2: return "3 Gbit/s";
    case 3: return "6 Gbit/s";
    default: return "unknown speed";
    }
}

/* Give back a port's memory when there is no usable disk on it. */
static struct port *drop(struct port *p, uint64_t page, uint64_t bounce)
{
    if (page)
        pmm_free_page(page);
    if (bounce)
        pmm_free_pages(bounce, BOUNCE_PAGES);
    kfree(p);
    return 0;
}

/*
 * Set up a port where a device is connected: its structures, receiving
 * FISes (only then does the device's signature arrive), the device ready.
 * If it is an ATA disk, identify it and start the port.
 */
/* Reset. CAP and PI partly are "write once" registers that the firmware
 * filled in and a reset clears on Intel controllers: put back afterwards,
 * as Linux does. */
static bool hba_reset(struct ahci *h)
{
    hba_write(h, HBA_GHC, hba_read(h, HBA_GHC) | GHC_AE);
    hba_write(h, HBA_GHC, hba_read(h, HBA_GHC) | GHC_HR);
    if (!wait_reg(h->regs + HBA_GHC, GHC_HR, 0, 1000))
        return false;
    hba_write(h, HBA_GHC, hba_read(h, HBA_GHC) | GHC_AE);
    hba_write(h, HBA_CAP, h->cap);
    hba_write(h, HBA_PI, h->pi);
    return true;
}

/* Spin up and power the ports (controllers with staggered spin-up leave
 * that to software), then give the links time to come up: at least 100
 * ms, until no port is still between "device seen" and "link up", at most
 * 1 s. Then the controller's interrupt, if it has one. */
static void ports_up(struct ahci *h)
{
    uint32_t pi = hba_read(h, HBA_PI), cap = h->cap;
    for (unsigned n = 0; n < MAX_PORTS; n++) {
        if (pi & (1u << n)) {
            volatile uint8_t *cmd = h->regs + PORT(n) + PX_CMD;
            mmio_write32(cmd, mmio_read32(cmd) | CMD_POD | ((cap & CAP_SSS) ? CMD_SUD : 0));
        }
    }
    for (int ms = 0; ms < 1000; ms += 10) {
        bool settling = false;
        for (unsigned n = 0; n < MAX_PORTS; n++) {
            if ((pi & (1u << n)) && SSTS_DET(mmio_read32(h->regs + PORT(n) + PX_SSTS)) == 1)
                settling = true;
        }
        if (ms >= 100 && !settling)
            break;
        arch_delay_ms(10);
    }
    if (h->interrupts)
        hba_write(h, HBA_GHC, hba_read(h, HBA_GHC) | GHC_IE);
}

/* A port's structures: command list at its page's start, received FISes
 * at 1024; then receiving (0, or why not). */
static const char *port_structures(struct port *p)
{
    if (!port_stop(p))
        return "does not stop";
    uint64_t page = p->page_phys;
    port_write(p, PX_CLB, (uint32_t)page);
    port_write(p, PX_CLB + 4, (uint32_t)(page >> 32));
    port_write(p, PX_FB, (uint32_t)(page + 1024));
    port_write(p, PX_FB + 4, (uint32_t)((page + 1024) >> 32));
    port_write(p, PX_SERR, 0xFFFFFFFF);
    port_write(p, PX_IS, 0xFFFFFFFF);
    port_write(p, PX_CMD, port_read(p, PX_CMD) | CMD_FRE);
    return 0;
}

/* ... then the disk ready (spinning, not busy). 0, or why not (the port
 * stopped). */
static const char *port_setup(struct port *p)
{
    const char *why = port_structures(p);
    if (why)
        return why;
    if (!wait_reg(p->regs + PX_TFD, TFD_BSY | TFD_DRQ, 0, SPIN_UP_MS)) {
        port_stop(p);
        return "the device stays busy";
    }
    return 0;
}

static uint32_t ms_since(uint64_t start)
{
    return (uint32_t)((arch_counter() - start) * 1000 / arch_counter_hz());
}

/* (37g-6) The link reset again, as Linux's libata does to every port on
 * a resume (ahci_do_hardreset, sata_link_hardreset, sata_link_resume,
 * sata_link_debounce with sata_deb_timing_normal): COMRESET through
 * SControl (DET 1, partial and slumber off) for 1 ms, the link resumed
 * (DET 0), 200 ms, SStatus' DET the same (and not 1) for 100 ms, read
 * every 5 ms, within 2 s; SError cleared; then the disk ready (its status
 * through the received FISes: receiving is on). In S3 the disk lost its
 * power: without this the VivoBook's SSD answered nothing after a wake. */
static const char *link_reset(struct port *p, uint32_t *settled_ms, uint32_t *ready_ms)
{
    uint32_t sctl = port_read(p, PX_SCTL);
    port_write(p, PX_SCTL, (sctl & 0x0F0) | 0x301);
    (void)port_read(p, PX_SCTL);
    arch_delay_ms(1);
    port_write(p, PX_SCTL, (sctl & 0x0F0) | 0x300);
    uint64_t start = arch_counter();
    timer_sleep_ms(200);
    uint32_t last = SSTS_DET(port_read(p, PX_SSTS)), since = ms_since(start);
    for (;;) {
        arch_delay_ms(5);
        uint32_t now = ms_since(start), det = SSTS_DET(port_read(p, PX_SSTS));
        if (det != last) {
            last = det, since = now;
            if (now > 2200)
                return "the link does not settle";
            continue;
        }
        if ((det != 1 || now > 2200) && now - since >= 100)
            break;
    }
    *settled_ms = ms_since(start);
    port_write(p, PX_SERR, port_read(p, PX_SERR));
    if (last != 3)
        return "no disk on the link after the reset";
    if (!wait_reg(p->regs + PX_TFD, TFD_BSY | TFD_DRQ, 0, SPIN_UP_MS))
        return "the disk stays busy after the reset";
    *ready_ms = ms_since(start);
    return 0;
}

/* (37g-2) Sleep: each disk's lock kept until resume (no command), its
 * written blocks flushed, the port stopped; awake, the controller as at
 * the boot (reset, ports up), each disk's port set up and started again. */
static bool ahci_suspend(void *ctx)
{
    struct ahci *h = ctx;
    for (int i = 0; i < h->port_count; i++) {
        struct port *p = h->ports[i];
        mutex_lock(&p->lock);
        run(p, p->lba48 ? ATA_FLUSH_CACHE_EXT : ATA_FLUSH_CACHE, 0, 0, 0, false, FLUSH_MS);
        port_stop(p);
    }
    return true;
}

static bool ahci_resume(void *ctx)
{
    struct ahci *h = ctx;
    bool ok = hba_reset(h);
    if (ok)
        ports_up(h);
    h->sleeps++;
    size_t at = (size_t)ksnprintf(h->after_sleep, sizeof(h->after_sleep), "%s",
                                  ok ? "" : "the controller's reset does not finish");
    for (int i = 0; i < h->port_count; i++) {
        struct port *p = h->ports[i];
        uint32_t settled = 0, ready = 0;
        const char *why = !ok ? "not started" : port_structures(p);
        if (!why)
            why = link_reset(p, &settled, &ready);
        if (!why) {
            port_write(p, PX_IE, IE_COMPLETION | IE_ERRORS);
            port_start(p);
        } else {
            port_stop(p);
            ok = false;
        }
        uint32_t ssts = port_read(p, PX_SSTS), tfd = port_read(p, PX_TFD) & 0xFF;
        if (at < sizeof(h->after_sleep) && why)
            at += (size_t)ksnprintf(h->after_sleep + at, sizeof(h->after_sleep) - at,
                                    "%s%s: %s (SStatus 0x%03x, task file 0x%02x)", at ? "; " : "",
                                    p->bd.name, why, ssts, tfd);
        else if (at < sizeof(h->after_sleep))
            at += (size_t)ksnprintf(h->after_sleep + at, sizeof(h->after_sleep) - at,
                                    "%s%s: the link at %s after %u ms, the disk ready after %u ms "
                                    "(SStatus 0x%03x, task file 0x%02x)", at ? "; " : "",
                                    p->bd.name, speed_text(SSTS_SPD(ssts)), settled, ready, ssts,
                                    tfd);
        kprintf("  %s: after the sleep: %s\n", p->bd.name, why ? why : "the disk back");
        mutex_unlock(&p->lock);
    }
    if (!at)                                    /* (a controller without disks: q35's own) */
        ksnprintf(h->after_sleep, sizeof(h->after_sleep), "started again, no disk on it");
    return ok;
}

static struct port *disk_on_port(struct ahci *h, unsigned n)
{
    struct port *p = kzalloc(sizeof(*p));
    uint64_t page = pmm_alloc_page(), bounce = pmm_alloc_pages(BOUNCE_PAGES);
    if (!p || !page || !bounce) {
        kprintf("  %s port %u:  out of memory\n", h->name, n);
        return drop(p, page, bounce);
    }
    bool wide = hba_read(h, HBA_CAP) & CAP_S64A;
    if (!wide && (page + PMM_PAGE_SIZE > ((uint64_t)1 << 32) ||
                  bounce + BOUNCE_PAGES * PMM_PAGE_SIZE > ((uint64_t)1 << 32))) {
        kprintf("  %s port %u:  the controller needs memory below 4 GiB (no 64-bit addressing)\n",
                h->name, n);
        return drop(p, page, bounce);
    }
    p->hba = h;
    p->index = n;
    p->regs = h->regs + PORT(n);
    p->page = phys_to_virt(page);
    p->page_phys = page;
    p->bounce = phys_to_virt(bounce);
    p->bounce_phys = bounce;
    p->lock = (struct mutex)MUTEX_INIT("ahci port");
    memset(p->page, 0, PMM_PAGE_SIZE);

    const char *why = port_setup(p);
    if (why) {
        kprintf("  %s port %u:  %s (status 0x%02x)\n", h->name, n, why, port_read(p, PX_TFD) & 0xFF);
        return drop(p, page, bounce);
    }
    uint32_t sig = port_read(p, PX_SIG);
    if (sig != SIG_ATA) {
        kprintf("  %s port %u:  %s, %s (not supported)\n", h->name, n,
                speed_text(SSTS_SPD(port_read(p, PX_SSTS))),
                sig == SIG_ATAPI ? "CD/DVD drive (ATAPI)"
                : sig == SIG_PM  ? "port multiplier" : "unknown device");
        port_stop(p);
        return drop(p, page, bounce);
    }
    port_write(p, PX_IE, IE_COMPLETION | IE_ERRORS);
    port_start(p);
    if (h->port_count < MAX_PORTS)
        h->ports[h->port_count++] = p;              /* (37g-2) */

    /* IDENTIFY DEVICE: 512 bytes about the disk. */
    p->bd.block_size = 512;
    ksnprintf(p->bd.name, sizeof(p->bd.name), "sata%d", disk_count);
    if (!run(p, ATA_IDENTIFY, 0, 0, 512, false, TIMEOUT_MS))
        return 0;
    const uint16_t *id = (const uint16_t *)p->bounce;
    char model[41], serial[21], firmware[9];
    ata_string(id + 27, 20, model);
    ata_string(id + 10, 10, serial);
    ata_string(id + 23, 4, firmware);
    p->lba48 = id[83] & (1u << 10);
    uint64_t sectors = p->lba48 ? (uint64_t)id[100] | (uint64_t)id[101] << 16 |
                                  (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48
                                : (uint64_t)id[60] | (uint64_t)id[61] << 16;
    /* Word 106: logical sectors longer than 512 bytes (words 117-118). */
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12)))
        p->bd.block_size = 2 * ((uint32_t)id[117] | (uint32_t)id[118] << 16);
    if (p->bd.block_size < 512 || p->bd.block_size > PMM_PAGE_SIZE) {
        kprintf("  %s port %u:  unsupported sector size %u\n", h->name, n, p->bd.block_size);
        return 0;
    }
    p->bd.block_count = sectors;
    p->bd.kind = "SATA";
    ksnprintf(p->bd.model, sizeof(p->bd.model), "%s", model);
    p->bd.read = ahci_read;
    p->bd.write = ahci_write;
    p->bd.flush = ahci_flush;
    /* TRIM: word 169 bit 0 (48-bit commands: DATA SET MANAGEMENT is one). */
    p->bd.discard = p->lba48 && (id[169] & 1) ? ahci_discard : 0;
    p->bd.driver_data = p;
    disk_count++;

    char label[16], kind[24];
    ksnprintf(label, sizeof(label), "%s:", p->bd.name);
    if (id[217] == 1)
        ksnprintf(kind, sizeof(kind), "SSD");
    else if (id[217] > 0x400 && id[217] < 0xFFFF)
        ksnprintf(kind, sizeof(kind), "%u rpm", id[217]);
    else
        ksnprintf(kind, sizeof(kind), "rotation unknown");
    kprintf("  %-15s'%s' serial '%s' firmware '%s' on %s port %u (%s)\n", label, model, serial,
            firmware, h->name, n, speed_text(SSTS_SPD(port_read(p, PX_SSTS))));
    kprintf("                 %lu blocks of %u bytes (%lu MiB), %s, %s addresses%s\n",
            sectors, p->bd.block_size, sectors * p->bd.block_size / (1024 * 1024), kind,
            p->lba48 ? "48-bit" : "28-bit", p->bd.discard ? ", TRIM" : "");
    return p;
}

int ahci_init(struct pci_device *pci, struct block_device **out, int max)
{
    struct pci_bar *bar = &pci->bars[5];
    if (hba_count == MAX_HBAS || !bar->size || bar->is_io)
        return 0;
    struct ahci *h = &hbas[hba_count];
    ksnprintf(h->name, sizeof(h->name), "ahci%d", hba_count);
    h->pci = pci;
    pci_write16(pci, PCI_COMMAND, pci_read16(pci, PCI_COMMAND) | PCI_COMMAND_MEMORY |
                                  PCI_COMMAND_BUS_MASTER | PCI_COMMAND_INTX_OFF);
    h->regs = paging_map_mmio(bar->address, bar->size);
    hba_count++;

    /* The firmware may still use the controller (to boot from it): ask it
     * to let go, as with USB, where the controller supports that. */
    const char *handoff = "";
    if (hba_read(h, HBA_CAP2) & CAP2_BOH) {
        hba_write(h, HBA_BOHC, hba_read(h, HBA_BOHC) | BOHC_OOS);
        bool released = wait_reg(h->regs + HBA_BOHC, BOHC_BOS, 0, 25);
        if (released && (hba_read(h, HBA_BOHC) & BOHC_BB))
            released = wait_reg(h->regs + HBA_BOHC, BOHC_BB, 0, 2000);
        handoff = released ? ", taken over from the firmware" : ", the firmware did not let go";
    }

    h->cap = hba_read(h, HBA_CAP);
    h->pi = hba_read(h, HBA_PI);
    if (!hba_reset(h)) {
        kprintf("  AHCI:          %02x:%02x.%u: reset does not finish\n", pci->bus, pci->device,
                pci->function);
        return 0;
    }
    uint32_t cap = h->cap, pi = hba_read(h, HBA_PI);

    uint64_t msi_address = 0;
    uint32_t msi_data = 0;
    h->interrupts = pci_msi_enable(pci, ahci_interrupt, h, &msi_address, &msi_data);
    /* Version: major in bits 31-16, then minor digits, e.g. 0x00010301 = 1.3.1. */
    uint32_t vs = hba_read(h, HBA_VS);
    char version[16];
    if (vs & 0xFF)
        ksnprintf(version, sizeof(version), "%u.%u.%u", vs >> 16, (vs >> 8) & 0xFF, vs & 0xFF);
    else
        ksnprintf(version, sizeof(version), "%u.%u", vs >> 16, (vs >> 8) & 0xFF);
    sleep_register(h->name, ahci_suspend, ahci_resume, h);     /* (37g-2) */
    kprintf("  AHCI:          %02x:%02x.%u, version %s, %s, %u port(s) of %u, "
            "%s addressing%s\n", pci->bus, pci->device, pci->function, version, h->name,
            (unsigned)__builtin_popcount(pi), (cap & 0x1F) + 1,
            cap & CAP_S64A ? "64-bit" : "32-bit", handoff);
    if (h->interrupts)
        kprintf("                 interrupts: MSI, message 0x%x to %p\n", msi_data,
                (void *)msi_address);
    else
        kprintf("                 interrupts: none (no MSI): commands polled\n");

    ports_up(h);

    int found = 0;
    for (unsigned n = 0; n < MAX_PORTS && found < max; n++) {
        if (!(pi & (1u << n)))
            continue;
        uint32_t ssts = mmio_read32(h->regs + PORT(n) + PX_SSTS);
        if (SSTS_DET(ssts) != DET_PRESENT)
            continue;               /* nothing connected */
        struct port *p = disk_on_port(h, n);
        if (p)
            out[found++] = &p->bd;
    }
    if (!found)
        kprintf("                 no disks\n");
    return found;
}

void ahci_sleep_print(void)
{
    for (int i = 0; i < hba_count; i++)
        if (hbas[i].sleeps)
            kprintf("%s: after sleep %u: %s\n", hbas[i].name, hbas[i].sleeps,
                    hbas[i].after_sleep);
}

void ahci_report(void)
{
    for (int i = 0; i < hba_count; i++) {
        char label[16];
        ksnprintf(label, sizeof(label), "%s:", hbas[i].name);
        kprintf("  %-15s%lu commands, %lu interrupts\n", label, hbas[i].commands,
                hbas[i].interrupt_count);
    }
}
