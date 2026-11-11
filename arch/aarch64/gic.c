/*
 * Interrupts on AArch64: the GICv3 (Generic Interrupt Controller) and the
 * generic timer's interrupt.
 *
 * The GIC has three parts:
 *   distributor     one per machine (memory mapped): global settings and the
 *                   shared peripheral interrupts (SPIs, INTID 32-1019)
 *   redistributor   one per core (memory mapped): that core's private
 *                   interrupts (PPIs, INTID 16-31, e.g. its timer)
 *   CPU interface   system registers (ICC_*): acknowledge an interrupt
 *                   (ICC_IAR1_EL1 returns its INTID) and signal its end
 *                   (ICC_EOIR1_EL1)
 * Interrupts here are "group 1": the kind an operating system handles at EL1.
 *
 * The timer is the core's virtual timer (CNTV), whose interrupt is a PPI
 * (INTID 27 per the GTDT). It fires when the counter reaches CNTV_CVAL_EL0;
 * every tick moves that compare value one interval ahead.
 *
 * (65c) Every core has its own redistributor, CPU interface and timer
 * (gic_init_core). Cores interrupt each other with SGIs (INTID 0-15, sent
 * through ICC_SGI1R_EL1): SGI_RESCHEDULE to look at the run queue,
 * SGI_STOP to halt (panic, power off).
 *
 * PCI devices signal interrupts with messages (MSI-X), memory writes. QEMU's
 * virt machine (without an ITS) has a "GICv2m" MSI frame for them: writing
 * an SPI's INTID to its SETSPI register raises that SPI. The SPIs it covers
 * are configured in the distributor as edge-triggered and routed to this
 * core.
 */
#include "gic.h"
#include "acpi.h"
#include "arch.h"
#include "cpu.h"
#include "kprintf.h"
#include "madt.h"
#include "memory.h"
#include "paging.h"
#include "string.h"
#include "cpus.h"
#include "percpu.h"
#include "sched.h"
#include "timer.h"

#define GICD_CTLR           0x0000
#define GICD_TYPER          0x0004
#define GICD_CTLR_GRP1      (1u << 1)       /* group 1 interrupts enabled */
#define GICD_CTLR_ARE       (1u << 4)       /* affinity routing (GICv3 mode) */
#define GICD_CTLR_RWP       (1u << 31)      /* register write pending */
#define GICD_IGROUPR        0x0080          /* 1 bit per INTID */
#define GICD_ISENABLER      0x0100          /* 1 bit per INTID */
#define GICD_ICENABLER      0x0180          /* (writing 1 disables) */
#define GICD_IPRIORITYR     0x0400          /* 1 byte per INTID */
#define GICD_ICFGR          0x0C00          /* 2 bits per INTID: edge or level */
#define GICD_IROUTER        0x6000          /* 8 bytes per SPI: target core */

#define V2M_MSI_TYPER       0x008           /* first SPI and how many */
#define V2M_MSI_SETSPI      0x040           /* devices write an INTID here */

#define GICR_TYPER          0x0008          /* 64 bits: affinity, last frame */
#define GICR_WAKER          0x0014
#define GICR_WAKER_SLEEP    (1u << 1)       /* ProcessorSleep */
#define GICR_WAKER_ASLEEP   (1u << 2)       /* ChildrenAsleep */
#define GICR_SGI_BASE       0x10000         /* second 64 KiB frame: SGI/PPI registers */
#define GICR_IGROUPR0       0x0080
#define GICR_ISENABLER0     0x0100
#define GICR_IPRIORITYR     0x0400
#define GICR_FRAME          0x20000         /* GICv3: 2 frames of 64 KiB per core */
#define GICR_FRAME_VLPI     0x40000         /* GICv4: 4 frames */

#define INTID_SPURIOUS      1020            /* 1020-1023: nothing to handle */
#define DEFAULT_TIMER_INTID 27
#define TIMER_PRIORITY      0x80
#define DEVICE_PRIORITY     0xA0
#define MAX_HANDLERS        32

#define CNTV_CTL_ENABLE     1

#define SGI_RESCHEDULE      1
#define SGI_STOP            2
#define SGI_PRIORITY        0x90

static volatile uint8_t *gicd, *gicr;
static uint64_t gicr_phys;
static uint32_t gicr_length;
static uint64_t boot_mpidr;                 /* (65c-2) where device interrupts go */
static uint32_t timer_intid;
static uint64_t interval;
static uint64_t next_ticks[MAX_CPUS];       /* (65c) each core's next compare value */
#define next_tick (next_ticks[arch_this_cpu()->index])
static uint64_t msi_frame;                  /* physical address, 0 if none */
static uint32_t msi_next, msi_end;          /* free SPIs: [msi_next, msi_end) */
static uint64_t unexpected;

static struct {
    uint32_t intid;
    irq_handler_fn handler;
    void *context;
} handlers[MAX_HANDLERS];
static int handler_count;

static uint32_t r32(volatile uint8_t *base, uint32_t off)
{
    return mmio_read32(base + off);
}

static void w32(volatile uint8_t *base, uint32_t off, uint32_t value)
{
    mmio_write32(base + off, value);
}

/* The redistributor whose affinity matches this core's MPIDR, or 0. */
static volatile uint8_t *find_redistributor(uint64_t base, uint64_t length)
{
    uint64_t mpidr = read_sysreg(MPIDR_EL1);
    uint32_t affinity = (uint32_t)((mpidr & 0xFFFFFF) | ((mpidr >> 8) & 0xFF000000));
    volatile uint8_t *frame = paging_map_mmio(base, length);
    for (uint64_t off = 0; off < length;) {
        uint64_t typer = mmio_read64(frame + off + GICR_TYPER);
        if ((uint32_t)(typer >> 32) == affinity)
            return frame + off;
        if (typer & (1u << 4))                      /* Last: no more frames */
            break;
        off += (typer & (1u << 1)) ? GICR_FRAME_VLPI : GICR_FRAME;
    }
    return 0;
}

void gic_handle_irq(void)
{
    uint32_t intid = (uint32_t)read_sysreg(ICC_IAR1_EL1) & 0xFFFFFF;
    if (intid >= INTID_SPURIOUS && intid <= 1023)
        return;
    if (intid >= 32)
        arch_this_cpu()->interrupts++;      /* (65d) `cpus` counts devices' (SPIs) per core */
    if (intid == SGI_STOP) {
        __asm__ volatile ("msr daifset, #0xf");
        for (;;)
            __asm__ volatile ("wfe");
    }
    if (intid == SGI_RESCHEDULE) {
        sched_reschedule_interrupt();
    } else if (intid == timer_intid) {
        /* Next compare value. If this interrupt came late (the host did not
         * run this virtual CPU for a while), the ticks that fell due
         * meanwhile still count: the kernel's clock does not fall behind. */
        next_tick += interval;
        uint64_t now = read_sysreg(CNTVCT_EL0);
        unsigned late = 0;
        while (next_tick <= now && late < 100) {
            next_tick += interval;
            late++;
        }
        if (next_tick <= now)
            next_tick = now + interval;
        write_sysreg(CNTV_CVAL_EL0, next_tick);
        isb();
        timer_tick();
        while (late--)
            timer_tick();
    } else {
        int i = 0;
        while (i < handler_count && handlers[i].intid != intid)
            i++;
        if (i < handler_count)
            handlers[i].handler(handlers[i].context);
        else
            unexpected++;
    }
    write_sysreg(ICC_EOIR1_EL1, intid);
    isb();
}

bool gic_route_spi(uint32_t intid, bool edge, irq_handler_fn handler, void *context)
{
    if (!gicd || intid < 32 || intid >= 1020 || handler_count == MAX_HANDLERS)
        return false;
    handlers[handler_count].intid = intid;
    handlers[handler_count].handler = handler;
    handlers[handler_count].context = context;
    handler_count++;

    /* Group 1, edge- or level-triggered, routed to the boot core (65c:
     * device interrupts are its alone, whichever core asks), enabled. */
    uint64_t mpidr = boot_mpidr;
    w32(gicd, GICD_IGROUPR + 4 * (intid / 32), r32(gicd, GICD_IGROUPR + 4 * (intid / 32)) |
                                               1u << (intid % 32));
    mmio_write8(gicd + GICD_IPRIORITYR + intid, DEVICE_PRIORITY);
    uint32_t cfg = r32(gicd, GICD_ICFGR + 4 * (intid / 16)) & ~(2u << (2 * (intid % 16)));
    w32(gicd, GICD_ICFGR + 4 * (intid / 16), cfg | (edge ? 2u << (2 * (intid % 16)) : 0));
    mmio_write64(gicd + GICD_IROUTER + 8 * (uint64_t)intid,
                 (mpidr & 0xFFFFFF) | ((mpidr >> 32) & 0xFF) << 32);
    w32(gicd, GICD_ISENABLER + 4 * (intid / 32), 1u << (intid % 32));
    return true;
}

/* (37c-2b) A GSI is the GIC's interrupt ID (ACPI 6.5 5.2.12.14); an SPI
 * is active high or rising (the GIC has no polarity). */
bool arch_route_gsi(uint32_t gsi, bool level, bool active_low, irq_handler_fn handler,
                    void *context, unsigned *vector)
{
    (void)active_low;
    if (!gic_route_spi(gsi, !level, handler, context))
        return false;
    *vector = gsi;
    return true;
}

bool arch_route_sci(uint32_t irq, irq_handler_fn handler, void *context, uint32_t *gsi,
                    bool *level, bool *active_low, unsigned *vector)
{
    (void)irq, (void)handler, (void)context, (void)gsi, (void)level, (void)active_low,
        (void)vector;
    return false;                                   /* (hardware-reduced ACPI: no SCI) */
}

void arch_gsi_mask(uint32_t gsi)
{
    if (gicd && gsi >= 32 && gsi < 1020)
        w32(gicd, GICD_ICENABLER + 4 * (gsi / 32), 1u << (gsi % 32));
}

void arch_gsi_unmask(uint32_t gsi)
{
    if (gicd && gsi >= 32 && gsi < 1020)
        w32(gicd, GICD_ISENABLER + 4 * (gsi / 32), 1u << (gsi % 32));
}

bool arch_msi_allocate(irq_handler_fn handler, void *context, uint64_t *address, uint32_t *data)
{
    if (!msi_frame || msi_next >= msi_end)
        return false;
    /* A message is an event, not a level: edge-triggered. */
    if (!gic_route_spi(msi_next, true, handler, context))
        return false;
    *address = msi_frame + V2M_MSI_SETSPI;
    *data = msi_next++;
    return true;
}

/* The MSI frame and its SPIs, if the MADT has one. */
static void find_msi_frame(void)
{
    const uint8_t *e = madt_entry(MADT_GIC_MSI_FRAME, 0);
    if (!e) {
        kprintf("                 no MSI frame: PCI devices cannot interrupt\n");
        return;
    }
    uint64_t phys;
    uint32_t flags;
    uint16_t count, base;
    memcpy(&phys, e + 8, 8);
    memcpy(&flags, e + 16, 4);
    memcpy(&count, e + 20, 2);
    memcpy(&base, e + 22, 2);
    if (!(flags & 1)) {                             /* the frame itself tells */
        volatile uint8_t *frame = paging_map_mmio(phys, 0x1000);
        uint32_t typer = r32(frame, V2M_MSI_TYPER);
        base = (uint16_t)((typer >> 16) & 0x3FF);
        count = (uint16_t)(typer & 0x3FF);
    }
    msi_frame = phys;
    msi_next = base;
    msi_end = (uint32_t)base + count;
    kprintf("                 MSI frame %p (GICv2m): SPIs %u-%u for PCI devices\n",
            (void *)phys, base, msi_end - 1);
}

/* This core's redistributor awake, its SGIs and the timer's PPI on, and
 * its CPU interface taking every priority. */
static void init_this_core(volatile uint8_t *r)
{
    w32(r, GICR_WAKER, r32(r, GICR_WAKER) & ~GICR_WAKER_SLEEP);
    while (r32(r, GICR_WAKER) & GICR_WAKER_ASLEEP)
        arch_cpu_relax();
    volatile uint8_t *sgi = r + GICR_SGI_BASE;
    uint32_t mine = 1u << SGI_RESCHEDULE | 1u << SGI_STOP | 1u << timer_intid;
    w32(sgi, GICR_IGROUPR0, r32(sgi, GICR_IGROUPR0) | mine);
    mmio_write8(sgi + GICR_IPRIORITYR + timer_intid, TIMER_PRIORITY);
    mmio_write8(sgi + GICR_IPRIORITYR + SGI_RESCHEDULE, SGI_PRIORITY);
    mmio_write8(sgi + GICR_IPRIORITYR + SGI_STOP, SGI_PRIORITY);
    w32(sgi, GICR_ISENABLER0, mine);

    /* CPU interface: system registers on, every priority let through. */
    write_sysreg(ICC_SRE_EL1, read_sysreg(ICC_SRE_EL1) | 1);
    isb();
    write_sysreg(ICC_PMR_EL1, 0xFF);
    write_sysreg(ICC_BPR1_EL1, 0);
    write_sysreg(ICC_IGRPEN1_EL1, 1);
    isb();
}

/* This core's timer: the first compare value one interval from now. */
static void start_timer(void)
{
    next_tick = read_sysreg(CNTVCT_EL0) + interval;
    write_sysreg(CNTV_CVAL_EL0, next_tick);
    write_sysreg(CNTV_CTL_EL0, CNTV_CTL_ENABLE);
    isb();
}

/* (65c) A started core's own part of the GIC and its tick. */
bool gic_init_core(void)
{
    volatile uint8_t *r = find_redistributor(gicr_phys, gicr_length);
    if (!r)
        return false;
    init_this_core(r);
    start_timer();
    return true;
}

/* An SGI to the core with this affinity (MPIDR's Aff3.Aff2.Aff1.Aff0), or
 * with others set, to every core but this one. */
static void send_sgi(uint64_t affinity, uint32_t intid, bool others)
{
    uint64_t v = (uint64_t)intid << 24;
    if (others)
        v |= 1ULL << 40;                            /* IRM: all but self */
    else
        v |= (1ULL << (affinity & 15)) | ((affinity & 0xF0) >> 4) << 44 |   /* list, range */
             ((affinity >> 8) & 0xFF) << 16 | ((affinity >> 16) & 0xFF) << 32 |
             ((affinity >> 32) & 0xFF) << 48;
    __asm__ volatile ("dsb ishst; msr s3_0_c12_c11_5, %0; isb" : : "r"(v) : "memory");
}

bool arch_msi_route(uint64_t *address, uint32_t data, const struct cpu *c)
{
    (void)address;                          /* (the message stays: the GIC routes the SPI) */
    if (!gicd || data < 32 || data >= 1020)
        return false;
    mmio_write64(gicd + GICD_IROUTER + 8 * (uint64_t)data, c->hw_id);   /* Aff3.Aff2.Aff1.Aff0 */
    return true;
}

bool arch_wake_core(struct cpu *c)
{
    send_sgi(c->hw_id, SGI_RESCHEDULE, false);
    return true;
}

static volatile bool stopping;

void arch_stop_other_cores(void)
{
    if (cpus_online() < 2 || stopping)
        return;
    stopping = true;
    send_sgi(0, SGI_STOP, true);
}

bool arch_interrupts_init(uint32_t hz)
{
    const uint8_t *d = madt_entry(MADT_GIC_DISTRIBUTOR, 0);
    const uint8_t *r = madt_entry(MADT_GIC_REDISTRIBUTOR, 0);
    if (!d || !r) {
        kprintf("  GIC:           no GIC distributor or redistributor in the ACPI MADT\n");
        return false;
    }
    uint64_t gicd_phys;
    boot_mpidr = read_sysreg(MPIDR_EL1);
    memcpy(&gicd_phys, d + 8, 8);
    memcpy(&gicr_phys, r + 4, 8);
    memcpy(&gicr_length, r + 12, 4);
    unsigned version = d[20];
    if (version != 3 && version != 4) {
        kprintf("  GIC:           GICv%u, only GICv3 is supported\n", version);
        return false;
    }

    /* Distributor: GICv3 mode, group 1 on. */
    gicd = paging_map_mmio(gicd_phys, 0x10000);
    w32(gicd, GICD_CTLR, GICD_CTLR_ARE | GICD_CTLR_GRP1);
    while (r32(gicd, GICD_CTLR) & GICD_CTLR_RWP)
        arch_cpu_relax();
    unsigned lines = 32 * ((r32(gicd, GICD_TYPER) & 0x1F) + 1);

    /* The virtual timer's interrupt, from the GTDT (offset 64). */
    timer_intid = DEFAULT_TIMER_INTID;
    const struct acpi_header *gtdt = acpi_find_table("GTDT");
    if (gtdt && gtdt->length >= 68)
        memcpy(&timer_intid, (const uint8_t *)gtdt + 64, 4);
    if (timer_intid < 16 || timer_intid > 31) {
        kprintf("  GIC:           timer interrupt %u is not a private one\n", timer_intid);
        return false;
    }

    /* This core's redistributor and CPU interface. */
    gicr = find_redistributor(gicr_phys, gicr_length);
    if (!gicr) {
        kprintf("  GIC:           no redistributor for this core\n");
        return false;
    }
    init_this_core(gicr);
    kprintf("  GIC:           GICv%u, distributor %p (%u interrupt lines), redistributor %p,"
            " %d core(s)\n", version, (void *)gicd_phys, lines,
            (void *)virt_to_phys((const void *)(uintptr_t)gicr),
            madt_count(MADT_GIC_CPU));

    find_msi_frame();

    uint64_t frequency = read_sysreg(CNTFRQ_EL0);
    interval = frequency / hz;
    start_timer();
    kprintf("                 timer %lu kHz (virtual timer, INTID %u), every %lu counts\n",
            frequency / 1000, timer_intid, interval);
    return true;
}
