/*
 * (37d-2) The idle loop's sleep on x86: the processors' sleep states from
 * the firmware's _CST, entered and chosen as Linux's acpi_idle does.
 *
 * C1 is MWAIT with its hint where the processor has MWAIT, else HLT; a
 * deeper state is MWAIT or a read of its I/O port (the chipset stops the
 * core until an interrupt, even with interrupts masked; Intel's old
 * chipsets want a read of the PM timer after it, AMD's do not). States
 * below C1 are used only where the APIC timer keeps running in them
 * (ARAT: the tick) and the time stamp counter does not stop (invariant
 * TSC, or a hypervisor's: the clock). Before C3 the bus-master status
 * (PM1 BM_STS) is looked at: a device's DMA since the last look takes the
 * deepest state above C3 instead (Linux's bm_check, set for AMD Zen; its
 * ARB_DIS is not used there); BM_RLD is set once, as Linux does.
 *
 * Choosing: each core keeps a running average of its idle periods (the
 * last weighing 1/8) and takes the deepest state whose target residency,
 * twice its latency (Linux's latency_factor), fits in it.
 */
#include "arch.h"
#include "acpi.h"
#include "cpu.h"
#include "cpus.h"
#include "io.h"
#include "kprintf.h"
#include "percpu.h"
#include "string.h"

#define PM1_BM_STS  (1u << 4)           /* status: a bus master was active */
#define PM1_BM_RLD  (1u << 1)           /* control: bus masters wake from C3 */

enum { BY_HLT, BY_MWAIT, BY_IO };
static const char *const by_names[] = { "HLT", "MWAIT", "reading I/O" };

static struct state {
    uint8_t type, by;
    uint32_t hint;                      /* MWAIT's hint, or the I/O port */
    uint32_t latency_us;
    uint64_t target;                    /* counter ticks the idle should last */
} states[IDLE_STATES];
static int state_count;                 /* (0 until set up: HLT) */
static int safe;                        /* the deepest state above C3 */
static uint16_t pm1_status, pm_timer;
static bool intel;

static bool vendor_is(const char *v)
{
    struct cpuid_result r = cpuid(0, 0);
    return !memcmp(&r.ebx, v, 4) && !memcmp(&r.edx, v + 4, 4) && !memcmp(&r.ecx, v + 8, 4);
}

void arch_idle_setup(const struct acpi_cstate *c, int n)
{
    struct cpuid_result f = cpuid(1, 0);
    bool mwait = f.ecx >> 3 & 1, hypervisor = f.ecx >> 31 & 1;
    bool arat = cpuid(0, 0).eax >= 6 && (cpuid(6, 0).eax >> 2 & 1);
    bool invariant = cpuid(0x80000000, 0).eax >= 0x80000007 && (cpuid(0x80000007, 0).edx >> 8 & 1);
    intel = vendor_is("GenuineIntel");
    const struct acpi_fadt *fadt = acpi_fadt();
    uint64_t hz = arch_counter_hz();
    struct state s[IDLE_STATES] = { { .type = 1, .by = BY_HLT, .latency_us = 1 } };
    int count = 1;
    bool deep = arat && (invariant || hypervisor), c3 = false;
    const char *why = !arat ? "the APIC timer stops in deep states"
                      : "the time stamp counter may stop in deep states";
    for (int i = 0; i < n; i++) {
        bool by_mwait = c[i].space == 0x7F && c[i].bit_offset == 2 && mwait;
        bool by_io = c[i].space == 1 && c[i].address && c[i].address <= 0xFFFF;
        if (c[i].type == 1) {
            if (by_mwait)
                s[0].by = BY_MWAIT, s[0].hint = (uint32_t)c[i].address;
            continue;
        }
        if (!deep || count == IDLE_STATES || (!by_mwait && !by_io))
            continue;
        if (c[i].type == 3 && (!fadt || !fadt->pm1a_event_block))
            continue;                   /* (no BM_STS to look at) */
        s[count].type = c[i].type;
        s[count].by = by_mwait ? BY_MWAIT : BY_IO;
        s[count].hint = (uint32_t)c[i].address;
        s[count].latency_us = c[i].latency_us;
        c3 |= c[i].type == 3;
        count++;
    }
    for (int i = 0; i < count; i++)
        s[i].target = hz / 1000000 * 2 * s[i].latency_us;
    int safest = 0;
    for (int i = 0; i < count; i++)
        if (s[i].type < 3)
            safest = i;
    if (fadt) {
        pm1_status = (uint16_t)fadt->pm1a_event_block;
        pm_timer = (uint16_t)fadt->pm_timer_block;
        if (c3 && fadt->pm1a_control_block) {
            uint16_t control = (uint16_t)fadt->pm1a_control_block;
            outw(control, inw(control) | PM1_BM_RLD);
        }
    }
    memcpy(states, s, sizeof(states));
    safe = safest;
    __atomic_store_n(&state_count, count, __ATOMIC_RELEASE);
    char text[200];
    size_t at = 0;
    for (int i = 0; i < count; i++) {
        at += (size_t)ksnprintf(text + at, sizeof(text) - at, "%sC%u by %s", i ? ", " : "",
                                s[i].type, by_names[s[i].by]);
        if (s[i].by == BY_MWAIT)
            at += (size_t)ksnprintf(text + at, sizeof(text) - at, " (hint 0x%02X)", s[i].hint);
        else if (s[i].by == BY_IO)
            at += (size_t)ksnprintf(text + at, sizeof(text) - at, " 0x%X", s[i].hint);
        if (i)
            at += (size_t)ksnprintf(text + at, sizeof(text) - at, " (%u us)", s[i].latency_us);
    }
    bool dropped = false;
    for (int i = 0; i < n; i++)
        dropped |= c[i].type > 1;
    kprintf("  %-15s%s%s%s\n", "idle:", text, dropped && !deep ? ": only C1, " : "",
            dropped && !deep ? why : "");
}

/* A device's DMA since the last look (clears the bit). */
static bool bus_master_active(void)
{
    if (!pm1_status)
        return false;
    if (!(inw(pm1_status) & PM1_BM_STS))
        return false;
    outw(pm1_status, PM1_BM_STS);
    return true;
}

/* With interrupts off; returns with them on. */
void arch_idle(void)
{
    int count = __atomic_load_n(&state_count, __ATOMIC_ACQUIRE);
    struct cpu *me = arch_this_cpu();
    int i = count ? count - 1 : 0;
    while (i > 0 && states[i].target > me->idle_predicted)
        i--;
    if (i > 0 && states[i].type == 3 && bus_master_active())
        i = safe;
    const struct state *s = count ? &states[i] : &(struct state){ .by = BY_HLT };
    uint64_t start = arch_counter();
    switch (s->by) {
    case BY_MWAIT:
        __asm__ volatile ("monitor" : : "a"(&me->idle_predicted), "c"(0), "d"(0));
        __asm__ volatile ("sti; mwait" : : "a"(s->hint), "c"(0) : "memory");
        break;
    case BY_IO:
        inb((uint16_t)s->hint);         /* (the chipset stops the core here) */
        if (intel && pm_timer)
            inl(pm_timer);
        __asm__ volatile ("sti" ::: "memory");
        break;
    default:
        __asm__ volatile ("sti; hlt" ::: "memory");
    }
    uint64_t took = arch_counter() - start;
    me->idle_predicted = me->idle_predicted - me->idle_predicted / 8 + took / 8;
    me->idle_entries[i]++;
    me->idle_ticks[i] += took;
}

void arch_idle_print(void)
{
    int count = __atomic_load_n(&state_count, __ATOMIC_ACQUIRE);
    uint64_t entries[IDLE_STATES], ticks[IDLE_STATES], all = 0;
    cpus_idle_totals(entries, ticks);
    for (int i = 0; i < IDLE_STATES; i++)
        all += ticks[i];
    char text[200];
    size_t at = 0;
    for (int i = 0; i < (count ? count : 1); i++)
        at += (size_t)ksnprintf(text + at, sizeof(text) - at, "%sC%u %lu times, %lu%%",
                                i ? "; " : "", count ? states[i].type : 1u, entries[i],
                                all ? ticks[i] * 100 / all : 0);
    kprintf("idle (every core, since the start): %s of the idle time\n", text);
}
