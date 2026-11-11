/*
 * Power off and reboot on AArch64: PSCI (Power State Coordination
 * Interface), a standard set of calls into the firmware or hypervisor. A
 * call puts its function number into X0 and executes SMC (to the secure
 * firmware) or HVC (to the hypervisor), as the FADT's ARM boot flags say;
 * under QEMU with HVF it is HVC. ARM's ACPI is "hardware-reduced": there
 * are no PM1 registers to write.
 */
#include "arch.h"
#include "acpi.h"
#include "kprintf.h"
#include "psci.h"
#include "cpus.h"
#include "percpu.h"

#define PSCI_VERSION        0x84000000u
#define PSCI_SYSTEM_OFF     0x84000008u
#define PSCI_SYSTEM_RESET   0x84000009u

static bool use_hvc;

/* (66e-2b) As a panic does: interrupts off, then the other cores stopped
 * (wherever they are: a lock one held stays held, so kprintf's mutex or
 * sched_lock would be waited for forever), then kprintf's emergency path. */
static void stop_everything(void)
{
    arch_interrupts_disable();
    arch_stop_other_cores();                /* (65c) */
    kprintf_emergency();
}

int64_t psci_call3(uint32_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    if (use_hvc)
        __asm__ volatile ("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
    else
        __asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
    return (int64_t)x0;
}

static uint64_t psci_call(uint32_t function)
{
    return (uint64_t)psci_call3(function, 0, 0, 0);
}

bool psci_ready(void)
{
    const struct acpi_fadt *f = acpi_fadt();
    if (!f || f->header.length < 131 || !(f->arm_boot_arch & FADT_ARM_PSCI))
        return false;
    use_hvc = f->arm_boot_arch & FADT_ARM_PSCI_HVC;
    return true;
}

/* Is PSCI there? Logs the version and the conduit. */
static bool psci_available(const char *what)
{
    if (!psci_ready()) {
        kprintf("%s: the firmware has no PSCI\n", what);
        return false;
    }
    uint64_t version = psci_call(PSCI_VERSION);
    kprintf("%s: PSCI %lu.%lu via %s\n", what, (version >> 16) & 0xFFFF, version & 0xFFFF,
            use_hvc ? "hvc" : "smc");
    return true;
}

void arch_power_off(void)
{
    if (!psci_available("powering off"))
        return;
    stop_everything();
    psci_call(PSCI_SYSTEM_OFF);
    arch_interrupts_enable();
    kprintf("poweroff: PSCI SYSTEM_OFF returned\n");
}

void arch_reboot(void)
{
    if (!psci_available("rebooting"))
        return;
    stop_everything();
    psci_call(PSCI_SYSTEM_RESET);
    arch_interrupts_enable();
    kprintf("reboot: PSCI SYSTEM_RESET returned\n");
}

/* (37b-4) ARM has no I/O ports. */
bool arch_port_access(uint16_t port, int width, uint32_t *value, bool write)
{
    (void)port, (void)width, (void)value, (void)write;
    return false;
}

/* (37d) ARM idles by WFI; no energy counter is read. */
void arch_processor_power_print(void)
{
    kprintf("processor: WFI; no energy counter\n");
    arch_idle_print();
}

/* (37d-2) ARM's idle is WFI alone (C1; PSCI's deeper states, the
 * firmware's _LPI, are not used), counted as x86's states are. */
void arch_idle_setup(const struct acpi_cstate *states, int count)
{
    (void)states, (void)count;
    kprintf("  %-15sC1 by WFI\n", "idle:");
}

void arch_idle(void)
{
    struct cpu *me = arch_this_cpu();
    uint64_t start = arch_counter();
    arch_wait_for_interrupt();
    me->idle_entries[0]++;
    me->idle_ticks[0] += arch_counter() - start;
}

void arch_idle_print(void)
{
    uint64_t entries[IDLE_STATES], ticks[IDLE_STATES];
    cpus_idle_totals(entries, ticks);
    kprintf("idle (every core, since the start): C1 %lu times, 100%% of the idle time\n",
            entries[0]);
}

/* (37g-1) No S3 on this architecture (PSCI's SYSTEM_SUSPEND is not done). */
int arch_sleep(uint8_t type_a, uint8_t type_b, unsigned seconds, struct arch_sleep_result *r)
{
    (void)type_a, (void)type_b, (void)seconds;
    *r = (struct arch_sleep_result){ "not on this architecture", 0, 0, 0, 0, 0 };
    return -1;
}
