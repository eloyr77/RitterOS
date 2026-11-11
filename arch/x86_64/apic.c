/*
 * Interrupts on x86-64: the Local APIC.
 *
 * Every core has a Local APIC: the interrupt controller that delivers
 * interrupts to that core, with its own timer. Its registers are memory
 * mapped (normally at 0xFEE00000, as the MADT says); each is 32 bits wide,
 * 16 bytes apart. After handling an interrupt the kernel writes the EOI
 * (end of interrupt) register, so that the next one can be delivered.
 *
 * The original PC's interrupt controllers, two 8259 PICs, are still emulated
 * and the firmware leaves them delivering IRQ 0-15 at vectors 8-15 and
 * 0x70-0x77, which would collide with CPU exceptions. They are moved to 32-47
 * and then masked completely; the IO APIC takes their place later.
 *
 * The APIC timer counts down at the (unknown) bus frequency, so it is
 * measured against the PIT first, then started in periodic mode.
 *
 * PCI devices signal interrupts with messages (MSI-X): a memory write of the
 * vector number to 0xFEE00000 | (APIC ID << 12), which the Local APIC of
 * that core receives.
 *
 * Interrupt lines of the PC's built-in devices (ISA IRQs, e.g. 4 for COM1)
 * go to the IO APIC instead: it has one input per "global system interrupt"
 * (GSI) and a redirection table entry for each that says which vector to
 * send to which core. ISA IRQ n is GSI n, edge-triggered, unless the MADT
 * has an interrupt source override for it (on q35: IRQ 0 is GSI 2).
 */
#include "apic.h"
#include "arch.h"
#include "io.h"
#include "kprintf.h"
#include "madt.h"
#include "paging.h"
#include "string.h"
#include "percpu.h"
#include "sched.h"
#include "timer.h"

#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080   /* task priority: 0 = accept everything */
#define LAPIC_EOI           0x0B0
#define LAPIC_SVR           0x0F0   /* spurious vector + software enable */
#define LAPIC_ICR_LOW       0x300   /* interrupt command: writing it sends */
#define LAPIC_ICR_HIGH      0x310   /* bits 24-31: the destination's APIC ID */
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_TIMER_INITIAL 0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE  0x3E0
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360
#define LAPIC_LVT_ERROR     0x370

#define SVR_ENABLE          (1u << 8)
#define ICR_PENDING         (1u << 12)  /* delivery status: not yet sent */
#define ICR_ASSERT          (1u << 14)
#define ICR_INIT            (5u << 8)
#define ICR_STARTUP         (6u << 8)
#define ICR_NMI             (4u << 8)
#define ICR_ALL_BUT_SELF    (3u << 18)  /* destination shorthand */
#define LVT_MASKED          (1u << 16)
#define LVT_PERIODIC        (1u << 17)
#define DIVIDE_BY_16        0x3

#define CALIBRATE_MS        50

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define MSI_ADDRESS         0xFEE00000u

#define IOAPIC_SELECT       0x00    /* register index */
#define IOAPIC_WINDOW       0x10    /* its value */
#define IOAPIC_VERSION      0x01    /* bits 16-23: last redirection entry */
#define IOAPIC_REDIRECT     0x10    /* entry n: registers 0x10 + 2n (low), + 1 (high) */
#define REDIRECT_ACTIVE_LOW (1u << 13)
#define REDIRECT_LEVEL      (1u << 15)
#define REDIRECT_MASKED     (1u << 16)

static volatile uint32_t *lapic;
static uint32_t lapic_id;
void x86_tlb_serve(void);                   /* smp.c */
void x86_sleep_park(void);                  /* sleep.c */
static uint32_t timer_count;              /* (65c) every core's tick, in timer counts */
static volatile uint32_t *ioapic;
static uint32_t ioapic_gsi_base, ioapic_inputs;
static unsigned next_vector = VECTOR_DEVICE_FIRST;
static uint64_t unexpected;

static struct {
    irq_handler_fn handler;
    void *context;
} handlers[256];

static uint32_t lapic_read(uint32_t reg)
{
    return mmio_read32(&lapic[reg / 4]);
}

static void lapic_write(uint32_t reg, uint32_t value)
{
    mmio_write32(&lapic[reg / 4], value);
}

/* Initialize both 8259s (ICW1-ICW4) with vectors 32-47, then mask all lines. */
static void pic_disable(void)
{
    outb(PIC1_COMMAND, 0x11);       /* ICW1: initialize, ICW4 follows */
    outb(PIC2_COMMAND, 0x11);
    outb(PIC1_DATA, 0x20);          /* ICW2: vector base */
    outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);          /* ICW3: the second PIC hangs on line 2 */
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);          /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01);
    outb(PIC1_DATA, 0xFF);          /* mask every line */
    outb(PIC2_DATA, 0xFF);
}

void apic_interrupt(uint64_t vector)
{
    if (vector == VECTOR_SPURIOUS)
        return;                     /* no EOI for spurious interrupts */
    if (vector >= VECTOR_DEVICE_FIRST && vector <= VECTOR_DEVICE_LAST)
        arch_this_cpu()->interrupts++;      /* (65d) `cpus` counts devices' per core */
    if (vector == VECTOR_TIMER)
        timer_tick();
    else if (vector == VECTOR_RESCHEDULE)
        sched_reschedule_interrupt();
    else if (vector == VECTOR_TLB)
        x86_tlb_serve();
    else if (vector == VECTOR_SLEEP)
        x86_sleep_park();                   /* (37g-1) returns once awake again */
    else if (handlers[vector].handler)
        handlers[vector].handler(handlers[vector].context);
    else
        unexpected++;
    lapic_write(LAPIC_EOI, 0);
}

/* A free vector for a device interrupt that calls handler(context), or 0. */
static unsigned allocate_vector(irq_handler_fn handler, void *context)
{
    if (!lapic || next_vector > VECTOR_DEVICE_LAST)
        return 0;
    unsigned vector = next_vector++;
    handlers[vector].handler = handler;
    handlers[vector].context = context;
    return vector;
}

bool arch_msi_allocate(irq_handler_fn handler, void *context, uint64_t *address, uint32_t *data)
{
    unsigned vector = allocate_vector(handler, context);
    if (!vector)
        return false;
    *address = MSI_ADDRESS | lapic_id << 12;
    *data = vector;                 /* fixed delivery, edge triggered */
    return true;
}

static uint32_t ioapic_read(uint32_t reg)
{
    mmio_write32(&ioapic[IOAPIC_SELECT / 4], reg);
    return mmio_read32(&ioapic[IOAPIC_WINDOW / 4]);
}

static void ioapic_write(uint32_t reg, uint32_t value)
{
    mmio_write32(&ioapic[IOAPIC_SELECT / 4], reg);
    mmio_write32(&ioapic[IOAPIC_WINDOW / 4], value);
}

/* The first IO APIC from the MADT, with every input masked. */
static void ioapic_init(void)
{
    const uint8_t *e = madt_entry(MADT_IO_APIC, 0);
    if (!e) {
        kprintf("                 no IO APIC\n");
        return;
    }
    uint32_t address;
    memcpy(&address, e + 4, 4);
    memcpy(&ioapic_gsi_base, e + 8, 4);
    ioapic = (volatile uint32_t *)paging_map_mmio(address, 4096);
    ioapic_inputs = ((ioapic_read(IOAPIC_VERSION) >> 16) & 0xFF) + 1;
    for (uint32_t i = 0; i < ioapic_inputs; i++)
        ioapic_write(IOAPIC_REDIRECT + 2 * i, REDIRECT_MASKED);
    kprintf("                 IO APIC %p (id %u), %u inputs from GSI %u, all masked\n",
            (void *)(uint64_t)address, e[2], ioapic_inputs, ioapic_gsi_base);
}

bool apic_route_isa_irq(uint8_t irq, irq_handler_fn handler, void *context, uint32_t *gsi,
                        unsigned *vector)
{
    if (!ioapic)
        return false;
    /* ISA default: GSI = IRQ, edge-triggered, active high. */
    *gsi = irq;
    uint16_t flags = 0;
    const uint8_t *o;
    for (int i = 0; (o = madt_entry(MADT_SOURCE_OVERRIDE, i)) != 0; i++)
        if (o[3] == irq) {
            memcpy(gsi, o + 4, 4);
            memcpy(&flags, o + 8, 2);
        }
    if (*gsi < ioapic_gsi_base || *gsi >= ioapic_gsi_base + ioapic_inputs)
        return false;
    *vector = allocate_vector(handler, context);
    if (!*vector)
        return false;

    uint32_t low = *vector;                         /* fixed delivery, physical destination */
    if ((flags & 3) == 3)
        low |= REDIRECT_ACTIVE_LOW;
    if (((flags >> 2) & 3) == 3)
        low |= REDIRECT_LEVEL;
    uint32_t entry = IOAPIC_REDIRECT + 2 * (*gsi - ioapic_gsi_base);
    ioapic_write(entry + 1, lapic_id << 24);        /* destination: this core */
    ioapic_write(entry, low);                       /* unmasked */
    return true;
}

bool arch_route_gsi(uint32_t gsi, bool level, bool active_low, irq_handler_fn handler,
                    void *context, unsigned *vector)
{
    if (!ioapic || gsi < ioapic_gsi_base || gsi >= ioapic_gsi_base + ioapic_inputs)
        return false;
    *vector = allocate_vector(handler, context);
    if (!*vector)
        return false;
    uint32_t low = *vector | (active_low ? REDIRECT_ACTIVE_LOW : 0) | (level ? REDIRECT_LEVEL : 0);
    uint32_t entry = IOAPIC_REDIRECT + 2 * (gsi - ioapic_gsi_base);
    ioapic_write(entry + 1, lapic_id << 24);        /* destination: this core */
    ioapic_write(entry, low);                       /* unmasked */
    return true;
}

bool arch_route_sci(uint32_t irq, irq_handler_fn handler, void *context, uint32_t *gsi,
                    bool *level, bool *active_low, unsigned *vector)
{
    /* ACPI 6.5 5.2.12.5: flags bits 0-1 polarity, 2-3 trigger mode; 00
     * conforms to the bus, which for the SCI is level, active low. */
    *gsi = irq;
    uint16_t flags = 0;
    const uint8_t *o;
    for (int i = 0; (o = madt_entry(MADT_SOURCE_OVERRIDE, i)) != 0; i++)
        if (o[3] == irq) {
            memcpy(gsi, o + 4, 4);
            memcpy(&flags, o + 8, 2);
        }
    *active_low = (flags & 3) != 1;
    *level = ((flags >> 2) & 3) != 1;
    return arch_route_gsi(*gsi, *level, *active_low, handler, context, vector);
}

void arch_gsi_mask(uint32_t gsi)
{
    if (!ioapic || gsi < ioapic_gsi_base || gsi >= ioapic_gsi_base + ioapic_inputs)
        return;
    uint32_t entry = IOAPIC_REDIRECT + 2 * (gsi - ioapic_gsi_base);
    ioapic_write(entry, ioapic_read(entry) | REDIRECT_MASKED);
}

void arch_gsi_unmask(uint32_t gsi)
{
    if (!ioapic || gsi < ioapic_gsi_base || gsi >= ioapic_gsi_base + ioapic_inputs)
        return;
    uint32_t entry = IOAPIC_REDIRECT + 2 * (gsi - ioapic_gsi_base);
    ioapic_write(entry, ioapic_read(entry) & ~REDIRECT_MASKED);
}

bool arch_msi_route(uint64_t *address, uint32_t data, const struct cpu *c)
{
    (void)data;
    if (c->hw_id > 0xFF)
        return false;                       /* (a destination beyond xAPIC's 8 bits) */
    *address = (*address & ~0xFF000ULL) | c->hw_id << 12;    /* bits 12-19: the APIC ID */
    return true;
}

uint32_t apic_boot_id(void)
{
    return lapic_id;
}

/* Send an interprocessor interrupt; the APIC sends one at a time. */
static bool send_ipi(uint32_t id, uint32_t command)
{
    if (!lapic)
        return false;
    for (int i = 0; i < 100000 && (lapic_read(LAPIC_ICR_LOW) & ICR_PENDING); i++)
        arch_cpu_relax();
    lapic_write(LAPIC_ICR_HIGH, id << 24);
    lapic_write(LAPIC_ICR_LOW, command);
    for (int i = 0; i < 100000; i++) {
        if (!(lapic_read(LAPIC_ICR_LOW) & ICR_PENDING))
            return true;
        arch_cpu_relax();
    }
    return false;
}

bool apic_send_init(uint32_t id)
{
    return send_ipi(id, ICR_INIT | ICR_ASSERT);
}

bool apic_send_startup(uint32_t id, uint8_t vector)
{
    return send_ipi(id, ICR_STARTUP | ICR_ASSERT | vector);
}

bool apic_send_ipi(uint32_t id, uint8_t vector)
{
    return send_ipi(id, ICR_ASSERT | vector);
}

void apic_nmi_others(void)
{
    send_ipi(0, ICR_NMI | ICR_ASSERT | ICR_ALL_BUT_SELF);
}

/* (37g-1) The IO APIC's redirection entries (low, high), kept for after a
 * sleep (the chipset forgets them). */
static uint32_t kept[2 * 240];

void apic_sleep_save(void)
{
    for (uint32_t i = 0; ioapic && i < ioapic_inputs && i < 240; i++) {
        kept[2 * i] = ioapic_read(IOAPIC_REDIRECT + 2 * i);
        kept[2 * i + 1] = ioapic_read(IOAPIC_REDIRECT + 2 * i + 1);
    }
}

void apic_wake(void)
{
    const struct madt *m = madt_get();
    if (m && (m->flags & 1))
        pic_disable();
    for (uint32_t i = 0; ioapic && i < ioapic_inputs && i < 240; i++) {
        ioapic_write(IOAPIC_REDIRECT + 2 * i, REDIRECT_MASKED);
        ioapic_write(IOAPIC_REDIRECT + 2 * i + 1, kept[2 * i + 1]);
        ioapic_write(IOAPIC_REDIRECT + 2 * i, kept[2 * i]);
    }
}

void apic_mask_all(void)
{
    for (uint32_t i = 0; ioapic && i < ioapic_inputs; i++)
        ioapic_write(IOAPIC_REDIRECT + 2 * i, REDIRECT_MASKED);
}

void apic_shutdown_core(void)
{
    if (!lapic)
        return;
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VECTOR_TIMER);
    lapic_write(LAPIC_TIMER_INITIAL, 0);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);
    lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) & ~SVR_ENABLE);
}

void apic_init_core(void)
{
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, SVR_ENABLE | VECTOR_SPURIOUS);
    lapic_write(LAPIC_TIMER_DIVIDE, DIVIDE_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | VECTOR_TIMER);
    lapic_write(LAPIC_TIMER_INITIAL, timer_count);
}

bool arch_interrupts_init(uint32_t hz)
{
    const struct madt *m = madt_get();
    if (!m) {
        kprintf("  APIC:          no ACPI MADT\n");
        return false;
    }
    if (m->flags & 1)
        pic_disable();
    lapic = (volatile uint32_t *)paging_map_mmio(m->local_apic_address, 4096);

    lapic_id = lapic_read(LAPIC_ID) >> 24;
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, SVR_ENABLE | VECTOR_SPURIOUS);
    kprintf("  APIC:          Local APIC %p (id %u, version 0x%02x), %d core(s), "
            "8259 PICs %s\n", (void *)(uint64_t)m->local_apic_address,
            lapic_id, lapic_read(LAPIC_VERSION) & 0xFF,
            madt_count(MADT_LOCAL_APIC), m->flags & 1 ? "masked" : "absent");
    ioapic_init();

    /* Measure the timer: count down from the maximum during a PIT delay. */
    lapic_write(LAPIC_TIMER_DIVIDE, DIVIDE_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VECTOR_TIMER);
    lapic_write(LAPIC_TIMER_INITIAL, 0xFFFFFFFF);
    uint64_t tsc_start = arch_counter();
    arch_delay_ms(CALIBRATE_MS);
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURRENT);
    uint64_t tsc_hz = (arch_counter() - tsc_start) * 1000 / CALIBRATE_MS;
    timer_set_counter_hz(tsc_hz);
    uint64_t frequency = (uint64_t)elapsed * 1000 / CALIBRATE_MS;
    if (frequency < hz) {
        kprintf("  APIC:          timer does not count\n");
        return false;
    }
    uint32_t count = (uint32_t)(frequency / hz);
    timer_count = count;
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | VECTOR_TIMER);
    lapic_write(LAPIC_TIMER_INITIAL, count);
    kprintf("                 timer %lu kHz (bus / 16, measured with the PIT), "
            "vector 0x%02x every %u counts\n", frequency / 1000, VECTOR_TIMER, count);
    kprintf("                 time stamp counter %lu MHz (measured with the PIT)\n",
            tsc_hz / 1000000);
    return true;
}
