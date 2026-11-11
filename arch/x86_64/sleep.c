/*
 * (37g-1) Sleep, ACPI S3 ("suspend to RAM"), on x86-64: the processor's
 * part (kernel/sleep.c does the firmware's and the devices').
 *
 * In S3 only memory keeps its contents: every core, the interrupt
 * controllers, the timers and the time stamp counter lose theirs. On
 * waking the firmware starts the boot processor alone, in real mode, at
 * the FACS's waking vector; it has to climb back to 64-bit mode as a core
 * the boot starts does, so the vector is smp.c's start-up page, whose
 * entry is set to x86_wake_first.
 *
 * Any core may be the one asked to sleep (a thread on it), but (37g-5d)
 * the boot processor enters S3, as Linux does (the core the firmware
 * wakes): every core keeps its place in kept[] (its registers as
 * x86_context_save takes them, CR3, its FS and kernel GS bases, its FP
 * state), the sleeper in arch_sleep, the others in the handler of
 * VECTOR_SLEEP (an interrupt comes only while a core holds no spinlock);
 * all but the boot processor then halt with interrupts off and their
 * caches written back (Linux's hlt_play_dead), and the boot processor,
 * once they all have, enters S3 from its handler (or from arch_sleep when
 * it is the sleeper). On waking the boot processor
 * makes itself the kernel's again (smp.c's x86_core_registers, its TSS,
 * the system-call registers and XCR0, its Local APIC), then the
 * platform: the 8259s masked, the IO APIC's entries back, the counter
 * carried over (it goes on from its value before plus the time asleep,
 * which the RTC tells), the serial line; then it starts every other core
 * again through the start-up page into x86_core_back, one at a time, each
 * on a stack page of its own (a slow core never shares one with the next),
 * each taking its own place back, and last it takes its own. Each place
 * returns where it was taken: the parked cores from their interrupt, the
 * sleeper from arch_sleep.
 *
 * The RTC (the CMOS clock at ports 0x70/0x71) wakes the machine: its
 * alarm (hour, minute, second; BCD or binary, 12 or 24 hours as register
 * B says; and the day of the month where the FADT names its register, as
 * Linux's rtc-cmos sets it) with the alarm interrupt on, which ACPI's
 * RTC_EN turns into a wake-up (kernel/acpi_events.c). Sleeps are shorter
 * than a day.
 *
 * If the machine does not go to sleep (the firmware refuses S3), the boot
 * processor sees itself still running after a second and starts the
 * halted cores again as after a wake: nothing was lost.
 */
#include "apic.h"
#include "acpi.h"
#include "arch.h"
#include "cpu.h"
#include "cpus.h"
#include "gdt.h"
#include "io.h"
#include "memory.h"
#include "paging.h"
#include "percpu.h"
#include "pmm.h"
#include "string.h"

#define MSR_FS_BASE         0xC0000100
#define MSR_KERNEL_GS_BASE  0xC0000102

#define PM1_SLP_TYP         (7u << 10)
#define PM1_SLP_EN          (1u << 13)

#define CMOS_INDEX          0x70
#define CMOS_DATA           0x71
#define RTC_SECONDS         0x00
#define RTC_ALARM_SECONDS   0x01
#define RTC_MINUTES         0x02
#define RTC_ALARM_MINUTES   0x03
#define RTC_HOURS           0x04
#define RTC_ALARM_HOURS     0x05
#define RTC_DAY             0x07
#define RTC_MONTH           0x08
#define RTC_YEAR            0x09
#define RTC_A               0x0A        /* bit 7: an update in progress */
#define RTC_B               0x0B        /* bit 5 alarm interrupt, 2 binary, 1 24 hours */
#define RTC_C               0x0C        /* the interrupt flags (reading clears them) */
#define RTC_B_ALARM         0x20
#define RTC_B_BINARY        0x04
#define RTC_B_24H           0x02

#define WAKE_STACK          16384

struct x86_context { uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip; };
int x86_context_save(struct x86_context *c);
__attribute__((noreturn)) void x86_context_restore(struct x86_context *c, volatile int *done);

uint64_t x86_startup_page(void);                    /* smp.c */
void x86_startup_point(uint64_t entry, uint64_t stack, uint64_t arg);
bool x86_core_restart(uint32_t id, volatile int *done);
void x86_core_registers(struct cpu *c);
void x86_user_init_core(void);                      /* user.c */

static struct kept_core {
    struct x86_context ctx;
    uint64_t cr3, fs_base, kernel_gs_base;
    volatile int parked, back;
    uint8_t fpu[ARCH_FPU_STATE_SIZE] __attribute__((aligned(64)));
} kept[MAX_CPUS];

static uint8_t wake_stack[WAKE_STACK] __attribute__((aligned(16)));     /* the boot processor's */
static uint64_t core_stack[MAX_CPUS];       /* each other core's wake stack: a page */
static uint64_t counter_before;
static uint32_t rtc_before;                 /* seconds of the day */
static uint32_t slept_s;
static int cores_back;

/* What the boot processor is asked to do, and what came of it. */
static struct {
    uint8_t type_a, type_b;
    uint8_t s0_a, s0_b;                     /* (37g-5d) \_S0's, put back after the wake */
    bool s0;
    uint16_t pm1a, pm1b;
    unsigned seconds;
    volatile int entrant;                   /* the boot processor's index; -1: none */
    volatile int started, done;
    int code;
    struct arch_sleep_result r;
} job = { .entrant = -1 };

static void wbinvd(void)
{
    __asm__ volatile (".byte 0x0f, 0x09" : : : "memory");   /* wbinvd: caches to memory */
}

/* ---- The RTC ---- */

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static void cmos_write(uint8_t reg, uint8_t v)
{
    outb(CMOS_INDEX, reg);
    outb(CMOS_DATA, v);
}

static uint8_t from_rtc(uint8_t v, uint8_t b)
{
    return b & RTC_B_BINARY ? v : (uint8_t)((v >> 4) * 10 + (v & 0xF));
}

static uint8_t to_rtc(uint8_t v, uint8_t b)
{
    return b & RTC_B_BINARY ? v : (uint8_t)((v / 10) << 4 | v % 10);
}

static uint8_t hour_from(uint8_t v, uint8_t b)
{
    if (b & RTC_B_24H)
        return from_rtc(v, b);
    uint8_t h = from_rtc(v & 0x7F, b) % 12;          /* 12 is 0, bit 7 PM */
    return (uint8_t)(h + (v & 0x80 ? 12 : 0));
}

static uint8_t hour_to(uint8_t h, uint8_t b)
{
    if (b & RTC_B_24H)
        return to_rtc(h, b);
    uint8_t twelve = h % 12 ? h % 12 : 12;
    return (uint8_t)(to_rtc(twelve, b) | (h >= 12 ? 0x80 : 0));
}

/* The time of day in seconds: read outside an update, twice the same. */
static uint32_t rtc_seconds(void)
{
    uint8_t b = cmos_read(RTC_B);
    uint32_t last = ~0u;
    for (int tries = 0; tries < 1000; tries++) {
        for (int w = 0; w < 10000 && (cmos_read(RTC_A) & 0x80); w++)
            arch_cpu_relax();
        uint32_t now = hour_from(cmos_read(RTC_HOURS), b) * 3600u +
                       from_rtc(cmos_read(RTC_MINUTES), b) * 60u + from_rtc(cmos_read(RTC_SECONDS), b);
        if (now == last)
            return now;
        last = now;
    }
    return last;
}

static uint8_t days_in(uint8_t month, uint8_t year)
{
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12)
        return 31;
    return (uint8_t)(days[month - 1] + (month == 2 && year % 4 == 0));   /* (2000-2099) */
}

/* The alarm at second `at` of the day (from 86400 on: tomorrow's). */
static void rtc_alarm(uint32_t at)
{
    const struct acpi_fadt *f = acpi_fadt();
    uint8_t b = cmos_read(RTC_B);
    bool tomorrow = at >= 86400;
    at %= 86400;
    cmos_write(RTC_B, b & ~RTC_B_ALARM);
    cmos_write(RTC_ALARM_SECONDS, to_rtc((uint8_t)(at % 60), b));
    cmos_write(RTC_ALARM_MINUTES, to_rtc((uint8_t)(at / 60 % 60), b));
    cmos_write(RTC_ALARM_HOURS, hour_to((uint8_t)(at / 3600), b));
    if (f && (f->day_alarm || f->month_alarm)) {    /* (the alarm's date too, where there are its registers) */
        uint8_t day = from_rtc(cmos_read(RTC_DAY), b), month = from_rtc(cmos_read(RTC_MONTH), b);
        uint8_t year = from_rtc(cmos_read(RTC_YEAR), b);
        if (tomorrow && ++day > days_in(month, year)) {
            day = 1;
            month = month == 12 ? 1 : month + 1;
        }
        if (f->day_alarm)
            cmos_write(f->day_alarm, to_rtc(day, b));
        if (f->month_alarm)
            cmos_write(f->month_alarm, to_rtc(month, b));
    }
    cmos_read(RTC_C);
    cmos_write(RTC_B, b | RTC_B_ALARM);
}

static void rtc_alarm_off(void)
{
    cmos_write(RTC_B, cmos_read(RTC_B) & ~RTC_B_ALARM);
    cmos_read(RTC_C);
}

/* ---- Each core's place ---- */

static void keep(struct kept_core *k)
{
    k->cr3 = read_cr3();
    k->fs_base = rdmsr(MSR_FS_BASE);
    k->kernel_gs_base = rdmsr(MSR_KERNEL_GS_BASE);
    arch_fpu_save(k->fpu);
}

/* A core out of the start-up page: the kernel's again, then its own. */
static void take_back(struct cpu *c, struct kept_core *k)
{
    x86_core_registers(c);
    gdt_reload_tss(c->index);
    x86_user_init_core();
    wrmsr(MSR_FS_BASE, k->fs_base);
    wrmsr(MSR_KERNEL_GS_BASE, k->kernel_gs_base);
    arch_fpu_restore(k->fpu);
    apic_init_core();
    write_cr3(k->cr3);
}

static void clflush(const volatile void *p)
{
    __asm__ volatile (".byte 0x0f, 0xae, 0x38" : : "a"(p) : "memory");  /* clflush (%rax) */
}

/* This core's place kept, then halted with interrupts off and its caches
 * written back (Linux's hlt_play_dead): only a start (INIT, then the
 * start-up page) ends that, and then this returns. parked: 1 the place
 * kept, 2 also the caches written back (the line itself flushed after). */
static void park_here(struct kept_core *k)
{
    keep(k);
    if (x86_context_save(&k->ctx))
        return;                                     /* started again */
    __atomic_store_n(&k->parked, 1, __ATOMIC_RELEASE);
    wbinvd();
    __atomic_store_n(&k->parked, 2, __ATOMIC_RELEASE);
    clflush(&k->parked);
    for (;;)
        __asm__ volatile ("hlt" : : : "memory");
}

static void enter(struct cpu *me);
__attribute__((noreturn)) void x86_core_back(struct cpu *c);

/* VECTOR_SLEEP: the boot processor enters S3 from here when another core
 * is the sleeper; every other core parks. */
void x86_sleep_park(void)
{
    struct cpu *c = arch_this_cpu();
    int entrant = __atomic_load_n(&job.entrant, __ATOMIC_ACQUIRE);
    if (entrant < 0)
        return;                                     /* (too late: the sleep was called off) */
    if (c->index == entrant) {
        enter(c);
        return;
    }
    park_here(&kept[c->index]);
}

/* Every parked core but self started again through the start-up page into
 * x86_core_back, each on its own stack page, one at a time: how many came
 * back (each waited for up to a second). */
static int restart_others(struct cpu *self)
{
    int n = cpus_online(), back = 0;
    uint64_t hz = arch_counter_hz();
    for (int i = 0; i < n; i++) {
        struct cpu *o = cpu_online_at(i);
        struct kept_core *k = &kept[o->index];
        if (o == self || !k->parked || !core_stack[o->index])
            continue;
        k->back = 0;
        x86_startup_point((uint64_t)x86_core_back,
                          (uint64_t)phys_to_virt(core_stack[o->index]) + 4096 - 8, (uint64_t)o);
        bool started = x86_core_restart((uint32_t)o->hw_id, &k->back);
        uint64_t start = arch_counter();
        while (!started && !__atomic_load_n(&k->back, __ATOMIC_ACQUIRE) &&
               arch_counter() - start < hz)
            arch_cpu_relax();
        if (__atomic_load_n(&k->back, __ATOMIC_ACQUIRE))
            back++;
    }
    return back;
}

/* Another core, started again after the sleep. */
__attribute__((noreturn)) void x86_core_back(struct cpu *c);
void x86_core_back(struct cpu *c)
{
    struct kept_core *k = &kept[c->index];
    take_back(c, k);
    k->parked = 0;
    x86_context_restore(&k->ctx, &k->back);
}

/* The boot processor, from the firmware's waking vector. */
__attribute__((noreturn)) void x86_wake_first(struct cpu *c);
void x86_wake_first(struct cpu *c)
{
    struct kept_core *k = &kept[c->index];
    take_back(c, k);
    apic_wake();
    uint32_t now = rtc_seconds();
    slept_s = (now + 86400 - rtc_before) % 86400;
    x86_counter_continue(counter_before + (uint64_t)slept_s * arch_counter_hz());
    x86_serial_wake();
    cores_back = 1 + restart_others(c);
    x86_context_restore(&k->ctx, &k->back);
}

/* The boot processor's part (from arch_sleep, or from VECTOR_SLEEP's
 * handler): once every other core has halted, into S3 and back, or the
 * reason not; the outcome in job. */
static void enter(struct cpu *me)
{
    struct acpi_facs *facs = acpi_facs();
    int n = cpus_online(), parked = 0;
    __atomic_store_n(&job.started, 1, __ATOMIC_RELEASE);
    uint64_t hz = arch_counter_hz(), start = arch_counter();
    while (parked < n - 1 && arch_counter() - start < 2 * hz) {     /* two seconds at most */
        parked = 0;
        for (int i = 0; i < n; i++) {
            struct cpu *o = cpu_online_at(i);
            parked += o != me && __atomic_load_n(&kept[o->index].parked, __ATOMIC_ACQUIRE) == 2;
        }
        arch_cpu_relax();
    }
    job.r.cores = n;
    if (parked < n - 1) {
        job.r.cores_back = 1 + restart_others(me);
        job.r.why = "a core did not stop";
        job.code = -1;
        __atomic_store_n(&job.done, 1, __ATOMIC_RELEASE);
        return;
    }
    struct kept_core *k = &kept[me->index];
    keep(k);
    facs->waking_vector = (uint32_t)x86_startup_page();
    if (facs->length >= 32)
        facs->x_waking_vector = 0;                  /* (real mode, as the 32-bit vector says) */
    x86_startup_point((uint64_t)x86_wake_first, (uint64_t)(wake_stack + WAKE_STACK) - 8,
                      (uint64_t)me);
    rtc_before = rtc_seconds();
    if (job.seconds)                                /* (37g-7: 0, no alarm) */
        rtc_alarm(rtc_before + job.seconds);
    counter_before = arch_counter();
    if (!x86_context_save(&k->ctx)) {
        wbinvd();
        /* SLP_TYP first, then SLP_EN with it (ACPI 6.5 4.8.3.2.1) */
        uint16_t pm1a = job.pm1a, pm1b = job.pm1b;
        outw(pm1a, (uint16_t)((inw(pm1a) & ~(PM1_SLP_TYP | PM1_SLP_EN)) | job.type_a << 10));
        if (pm1b)
            outw(pm1b, (uint16_t)((inw(pm1b) & ~(PM1_SLP_TYP | PM1_SLP_EN)) | job.type_b << 10));
        outw(pm1a, (uint16_t)(inw(pm1a) | PM1_SLP_EN));
        if (pm1b)
            outw(pm1b, (uint16_t)(inw(pm1b) | PM1_SLP_EN));
        arch_delay_ms(1000);
        /* Still here: the machine did not sleep; the others started again. */
        rtc_alarm_off();
        facs->waking_vector = 0;
        job.r.cores_back = 1 + restart_others(me);
        job.r.why = "the machine did not go to sleep";
        job.code = -1;
        __atomic_store_n(&job.done, 1, __ATOMIC_RELEASE);
        return;
    }
    /* Awake: x86_wake_first started every other core again; each has its
     * place back (it waited for each one). (37g-5d) As ACPICA's
     * AcpiHwLegacyWakePrep, first: SLP_TYP back to S0's and SLP_EN clear
     * ("required by some machines"): left at S3, the Acer's next warm reset
     * never came back. */
    if (job.s0) {
        uint16_t pm1a = job.pm1a, pm1b = job.pm1b;
        outw(pm1a, (uint16_t)((inw(pm1a) & ~(PM1_SLP_TYP | PM1_SLP_EN)) | job.s0_a << 10));
        if (pm1b)
            outw(pm1b, (uint16_t)((inw(pm1b) & ~(PM1_SLP_TYP | PM1_SLP_EN)) | job.s0_b << 10));
    }
    rtc_alarm_off();
    facs->waking_vector = 0;
    job.r.slept_s = slept_s;
    job.r.cores_back = cores_back;
    job.r.resume_ms = (arch_counter() - (counter_before + (uint64_t)slept_s * hz)) * 1000 / hz;
    job.code = 0;
    __atomic_store_n(&job.done, 1, __ATOMIC_RELEASE);
}

int arch_sleep(uint8_t type_a, uint8_t type_b, unsigned seconds, struct arch_sleep_result *r)
{
    const struct acpi_fadt *f = acpi_fadt();
    memset(r, 0, sizeof(*r));
    if (!f || !f->pm1a_control_block) {
        r->why = "no PM1 control register";
        return -1;
    }
    if (!acpi_facs()) {
        r->why = "no FACS (the waking vector's place)";
        return -1;
    }
    if (!x86_startup_page()) {
        r->why = "no start-up page for the waking vector";
        return -1;
    }
    job.s0 = acpi_sleep_type(0, &job.s0_a, &job.s0_b);
    struct cpu *me = arch_this_cpu(), *boot = cpu_online_at(0);
    int n = cpus_online();
    /* each other core's wake stack: a page (pages, never a run of them) */
    bool stacks = true;
    for (int i = 0; i < n; i++) {
        struct cpu *o = cpu_online_at(i);
        core_stack[o->index] = o == boot ? 0 : pmm_alloc_page();
        stacks = stacks && (o == boot || core_stack[o->index]);
    }
    if (!stacks) {
        for (int i = 0; i < n; i++)
            if (core_stack[cpu_online_at(i)->index])
                pmm_free_page(core_stack[cpu_online_at(i)->index]);
        r->why = "no memory for the cores' wake stacks";
        return -1;
    }
    memset(&job.r, 0, sizeof(job.r));
    job.type_a = type_a, job.type_b = type_b, job.seconds = seconds;
    job.pm1a = (uint16_t)f->pm1a_control_block, job.pm1b = (uint16_t)f->pm1b_control_block;
    job.started = job.done = 0;
    job.code = -1;
    for (int i = 0; i < n; i++) {
        struct cpu *o = cpu_online_at(i);
        kept[o->index].parked = kept[o->index].back = 0;
    }
    apic_sleep_save();
    uint64_t irq = arch_irq_save();
    __atomic_store_n(&job.entrant, boot->index, __ATOMIC_RELEASE);
    if (me != boot) {
        /* the boot processor first: it must answer before anything stops */
        apic_send_ipi((uint32_t)boot->hw_id, VECTOR_SLEEP);
        uint64_t hz = arch_counter_hz(), start = arch_counter();
        while (!__atomic_load_n(&job.started, __ATOMIC_ACQUIRE) && arch_counter() - start < hz)
            arch_cpu_relax();
        if (!__atomic_load_n(&job.started, __ATOMIC_ACQUIRE)) {
            job.entrant = -1;
            arch_irq_restore(irq);
            r->why = "the boot processor did not answer";
            return -1;
        }
    }
    for (int i = 0; i < n; i++) {
        struct cpu *o = cpu_online_at(i);
        if (o != me && o != boot)
            apic_send_ipi((uint32_t)o->hw_id, VECTOR_SLEEP);
    }
    if (me == boot)
        enter(me);
    else
        park_here(&kept[me->index]);                /* started again after it, or when none */
    uint64_t hz = arch_counter_hz(), start = arch_counter();
    while (!__atomic_load_n(&job.done, __ATOMIC_ACQUIRE) && arch_counter() - start < hz)
        arch_cpu_relax();                           /* (the boot processor finishing) */
    job.entrant = -1;
    for (int i = 0; i < n; i++) {
        struct cpu *o = cpu_online_at(i);
        /* (back is stored once its stack is the kernel's again; a core that
         * never parked never used its page; one parked and never back keeps it) */
        if (core_stack[o->index] && (kept[o->index].back || !kept[o->index].parked))
            pmm_free_page(core_stack[o->index]);
        core_stack[o->index] = 0;
    }
    *r = job.r;
    r->asked_on = me->index;
    arch_irq_restore(irq);
    return job.code;
}
