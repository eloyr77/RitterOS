#include "uefi.h"
#include "efi.h"
#include "arch.h"
#include "kprintf.h"
#include "memory.h"
#include "arch_api.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include <stdbool.h>

static EFI_RUNTIME_SERVICES *runtime;

void uefi_init(uint64_t runtime_services)
{
    runtime = (EFI_RUNTIME_SERVICES *)runtime_services;
}

/* (65a-4) One firmware call at a time: UEFI's runtime services may not be
 * entered by two cores at once. Held with interrupts off, as the calls
 * need anyway (the scheduler must not switch address spaces meanwhile). */
static struct spinlock firmware_lock = SPINLOCK_INIT("firmware");

/* GetTime in the firmware's address space. */
static bool get_time(EFI_TIME *t)
{
    if (!runtime)
        return false;
    uint64_t irq = spin_lock(&firmware_lock);
    uint64_t previous = paging_firmware_enter();
    EFI_STATUS status = runtime->GetTime(t, 0);
    paging_firmware_leave(previous);
    spin_unlock(&firmware_lock, irq);
    return !EFI_ERROR(status);
}

/* --- variables -------------------------------------------------------------- */

/* Around a firmware call made for a program: the firmware's lock (and so
 * interrupts off: no switch of address spaces), the program's FP/SIMD
 * registers saved (the firmware's code may use them; the kernel never does). */
static uint64_t firmware_begin(void)
{
    uint64_t irq = spin_lock(&firmware_lock);
    struct thread *t = thread_current();
    if (t && t->fpu)
        arch_fpu_save(t->fpu);
    return irq;
}

static void firmware_end(uint64_t irq)
{
    struct thread *t = thread_current();
    if (t && t->fpu)
        arch_fpu_restore(t->fpu);
    spin_unlock(&firmware_lock, irq);
}

static bool variable_name(const char *name, CHAR16 *out, size_t room)
{
    size_t n = strlen(name);
    if (n == 0 || n >= room)
        return false;
    for (size_t i = 0; i <= n; i++)
        out[i] = (CHAR16)(uint8_t)name[i];
    return true;
}

static int variable_error(EFI_STATUS status)
{
    return status == EFI_SUCCESS ? 0 : status == EFI_NOT_FOUND ? -ERR_NOENT
         : status == EFI_BUFFER_TOO_SMALL ? -ERR_2BIG : -ERR_IO;
}

int uefi_variable_get(const char *name, const uint8_t *guid, uint32_t *attributes, void *buf,
                      uint64_t *size)
{
    CHAR16 wide[64];
    EFI_GUID vendor;
    if (!runtime)
        return -ERR_NOSYS;
    if (!variable_name(name, wide, 64))
        return -ERR_INVAL;
    memcpy(&vendor, guid, sizeof(vendor));
    UINT32 attr = 0;
    UINTN n = *size;
    uint64_t irq = firmware_begin();
    uint64_t previous = paging_firmware_enter();
    EFI_STATUS status = runtime->GetVariable(wide, &vendor, &attr, &n, buf);
    paging_firmware_leave(previous);
    firmware_end(irq);
    *attributes = attr;
    *size = n;
    return variable_error(status);
}

int uefi_variable_set(const char *name, const uint8_t *guid, uint32_t attributes,
                      const void *data, uint64_t size)
{
    CHAR16 wide[64];
    EFI_GUID vendor;
    if (!runtime)
        return -ERR_NOSYS;
    if (!variable_name(name, wide, 64))
        return -ERR_INVAL;
    memcpy(&vendor, guid, sizeof(vendor));
    uint64_t irq = firmware_begin();
    uint64_t previous = paging_firmware_enter();
    EFI_STATUS status = runtime->SetVariable(wide, &vendor, attributes, size, (void *)data);
    paging_firmware_leave(previous);
    firmware_end(irq);
    return variable_error(status);
}

bool uefi_report_time(void)
{
    EFI_TIME t;
    if (!get_time(&t)) {
        kprintf("  UEFI time:     unavailable\n");
        return false;
    }
    kprintf("  UEFI time:     %04u-%02u-%02u %02u:%02u:%02u", t.Year, t.Month, t.Day,
            t.Hour, t.Minute, t.Second);
    if (t.TimeZone == 0x07FF)
        kprintf(" (time zone unspecified)");
    else
        kprintf(" (UTC offset %d min)", t.TimeZone);
    kprintf(", read through runtime services after ExitBootServices\n");
    return true;
}

bool uefi_time_text(char *buf, size_t size)
{
    EFI_TIME t;
    if (!get_time(&t))
        return false;
    ksnprintf(buf, size, "%04u-%02u-%02u %02u:%02u:%02u", t.Year, t.Month, t.Day, t.Hour,
              t.Minute, t.Second);
    return true;
}

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint64_t uefi_time_ns(void)
{
    EFI_TIME t;
    if (!get_time(&t))
        return 0;
    int64_t days = days_from_civil(t.Year, t.Month, t.Day);
    int64_t secs = days * 86400 + t.Hour * 3600 + t.Minute * 60 + t.Second;
    /* With a zone, take TimeZone as the offset of local time from UTC (the
     * sign convention differs between UEFI spec versions; OVMF reports the
     * zone as unspecified, i.e. the clock runs on UTC). */
    if (t.TimeZone != 0x07FF)
        secs -= t.TimeZone * 60;
    return (uint64_t)secs * 1000000000ULL + t.Nanosecond;
}

/* --- the clock --------------------------------------------------------------- */

/* The firmware clock read once, then carried on with the CPU's counter:
 * programs and RitterFS commits ask for the time from system calls, where
 * a firmware call could use the program's FP registers. */
static uint64_t clock_base_ns, clock_base_counter, clock_hz;
/* (68u) Odd while clock_base_counter and clock_hz change together (a
 * better rate, uefi_clock_rate): since_base reads both inside it. */
static uint32_t clock_seq;

/* (68p) The clock page programs read (struct sys_clock), at clock_page
 * physical; changed under clock_page_lock, `sequence` odd meanwhile. */
static uint64_t clock_page;
static struct spinlock clock_page_lock = SPINLOCK_INIT("clock page");

/* The page made as the clock is now; clock_page_lock held. */
static void publish_locked(void)
{
    struct sys_clock *c = phys_to_virt(clock_page);
    __atomic_store_n(&c->sequence, c->sequence + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);        /* (odd before the fields change) */
    uint64_t offset;
    c->counter = clock_hz ? arch_counter_user(&offset) : 0;
    c->offset = c->counter ? offset : 0;
    c->hz = clock_hz;
    c->base_counter = clock_base_counter;
    c->base_ns = __atomic_load_n(&clock_base_ns, __ATOMIC_ACQUIRE);
    __atomic_store_n(&c->sequence, c->sequence + 1, __ATOMIC_RELEASE);
}

void uefi_clock_publish(void)
{
    if (!clock_page)
        return;
    uint64_t state = spin_lock(&clock_page_lock);
    publish_locked();
    spin_unlock(&clock_page_lock, state);
}

/* Nanoseconds from delta counts at hz: seconds and the rest apart (delta *
 * 10^9 would overflow in seconds). */
static uint64_t counts_ns(uint64_t delta, uint64_t hz)
{
    return delta / hz * 1000000000ULL + delta % hz * 1000000000ULL / hz;
}

/* (68u) The counter's rate measured better (x86: against the ACPI PM
 * timer, after the boot's 50 ms against the PIT): the clock goes on at
 * the new rate from now without a jump. The base moves so that now is
 * the same time under the new rate (a count earlier: rounding never takes
 * it back), so the page's layout and the programs' reading stay as they
 * are; the kernel's readers and the page change in one hold. */
void uefi_clock_rate(uint64_t hz)
{
    if (!clock_hz || !hz)
        return;
    uint64_t state = spin_lock(&clock_page_lock);
    __atomic_store_n(&clock_seq, clock_seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint64_t now = arch_counter();
    uint64_t since = counts_ns(now - clock_base_counter, clock_hz);
    uint64_t back = since / 1000000000ULL * hz + since % 1000000000ULL * hz / 1000000000ULL + 1;
    if (back <= now) {                      /* (the counter ran before the clock started) */
        __atomic_store_n(&clock_base_counter, now - back, __ATOMIC_RELAXED);
        __atomic_store_n(&clock_hz, hz, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&clock_seq, clock_seq + 1, __ATOMIC_RELEASE);
    if (clock_page)
        publish_locked();
    spin_unlock(&clock_page_lock, state);
}

uint64_t uefi_clock_page(void)
{
    return clock_page;
}

void uefi_clock_start(void)
{
    uint64_t hz = arch_counter_hz();
    if (hz) {
        clock_base_ns = uefi_time_ns();
        clock_base_counter = arch_counter();
        clock_hz = hz;
    }
    if ((clock_page = pmm_alloc_page())) {
        memset(phys_to_virt(clock_page), 0, PAGE_SIZE);
        paging_kept_page = clock_page;              /* (no address space frees it) */
        uefi_clock_publish();
    }
}

uint64_t uefi_clock_ns_running(void)
{
    return clock_hz ? uefi_clock_ns() : 0;
}

/* Nanoseconds the counter has gone since clock_base_counter (base and
 * rate read together: 68u's clock_seq). */
static uint64_t since_base(void)
{
    for (;;) {
        uint32_t seq = __atomic_load_n(&clock_seq, __ATOMIC_ACQUIRE);
        if (seq & 1) {
            arch_cpu_relax();
            continue;
        }
        uint64_t base = __atomic_load_n(&clock_base_counter, __ATOMIC_RELAXED);
        uint64_t hz = __atomic_load_n(&clock_hz, __ATOMIC_RELAXED);
        uint64_t delta = arch_counter() - base;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&clock_seq, __ATOMIC_RELAXED) == seq)
            return counts_ns(delta, hz);
    }
}

/* (68p) Nanoseconds since the clock started (CLOCK_MONOTONIC: date -s does
 * not move it); without a counter, the date's clock. */
uint64_t uefi_clock_monotonic_ns(void)
{
    return clock_hz ? since_base() : uefi_clock_ns();
}

uint64_t uefi_clock_ns(void)
{
    if (!clock_hz)
        return uefi_time_ns();
    /* (68l) the base is one word, moved at once by uefi_clock_set */
    uint64_t base = __atomic_load_n(&clock_base_ns, __ATOMIC_ACQUIRE);
    if (!base)
        return 0;
    return base + since_base();
}

/* A civil date for days since 1970-01-01 (Howard Hinnant's algorithm). */
static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    *m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

/* EFI_TIME's fields for secs since 1970 (the zone's offset already added). */
static void efi_fields(int64_t secs, EFI_TIME *t)
{
    int64_t days = secs / 86400, rest = secs % 86400, y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    t->Year = (UINT16)y;
    t->Month = (UINT8)m;
    t->Day = (UINT8)d;
    t->Hour = (UINT8)(rest / 3600);
    t->Minute = (UINT8)(rest / 60 % 60);
    t->Second = (UINT8)(rest % 60);
}

void uefi_ns_text(uint64_t ns, char *buf, size_t size)
{
    EFI_TIME t;
    efi_fields((int64_t)(ns / 1000000000ULL), &t);
    ksnprintf(buf, size, "%04u-%02u-%02u %02u:%02u:%02u", t.Year, t.Month, t.Day, t.Hour, t.Minute,
              t.Second);
}

bool uefi_clock_set(uint64_t ns)
{
    if (clock_hz) {                         /* the running clock: its base moved, the counter kept */
        __atomic_store_n(&clock_base_ns, ns - since_base(), __ATOMIC_RELEASE);
        uefi_clock_publish();               /* (68p) and the programs' page */
    }
    EFI_TIME now;
    if (!runtime || !runtime->SetTime || !get_time(&now))
        return false;
    /* In the firmware's own zone (uefi_time_ns's reading of it), with its
     * daylight flags; a clock on UTC (the zone unspecified) gets UTC. */
    int64_t secs = (int64_t)(ns / 1000000000ULL);
    if (now.TimeZone != 0x07FF)
        secs += now.TimeZone * 60;
    EFI_TIME t = now;
    efi_fields(secs, &t);
    t.Nanosecond = 0;
    uint64_t irq = firmware_begin();
    uint64_t previous = paging_firmware_enter();
    EFI_STATUS status = runtime->SetTime(&t);
    paging_firmware_leave(previous);
    firmware_end(irq);
    if (EFI_ERROR(status))
        return false;
    uint64_t back = uefi_time_ns(), want = ns - ns % 1000000000ULL;
    return back + 3000000000ULL >= want && back <= want + 3000000000ULL;
}
