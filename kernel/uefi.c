#include "uefi.h"
#include "efi.h"
#include "arch.h"
#include "kprintf.h"
#include "arch_api.h"
#include "paging.h"
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

void uefi_clock_start(void)
{
    uint64_t hz = arch_counter_hz();
    if (!hz)
        return;
    clock_base_ns = uefi_time_ns();
    clock_base_counter = arch_counter();
    clock_hz = hz;
}

uint64_t uefi_clock_ns_running(void)
{
    return clock_hz ? uefi_clock_ns() : 0;
}

uint64_t uefi_clock_ns(void)
{
    if (!clock_hz)
        return uefi_time_ns();
    if (!clock_base_ns)
        return 0;
    uint64_t delta = arch_counter() - clock_base_counter;
    /* Seconds and the rest apart: delta * 10^9 would overflow in seconds. */
    return clock_base_ns + delta / clock_hz * 1000000000ULL +
           delta % clock_hz * 1000000000ULL / clock_hz;
}
