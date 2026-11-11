/*
 * Power off and reboot on x86-64, the ACPI way.
 *
 * Power off: enter sleep state S5 ("soft off") by writing its sleep type,
 * together with the SLP_EN bit, into the PM1 control register(s) whose I/O
 * ports the FADT names. The sleep type value itself is only in the DSDT
 * (\_S5_). If the firmware left the machine in legacy mode, ACPI mode is
 * switched on first through the SMI command port.
 *
 * Reboot: the FADT's reset register (on q35: I/O port 0xCF9, value 6);
 * otherwise the old keyboard controller's reset line; as a last resort, a
 * triple fault (an exception with no usable IDT) resets the CPU.
 *
 * (37g-5d) A reboot as Linux's: on the boot processor (asked elsewhere,
 * the boot processor's NMI does it), every other core halted as Linux's
 * stop_this_cpu (smp.c), the IO APIC's inputs masked and its Local APIC
 * off; then the reset register, the keyboard controller ten times
 * (Linux's), the triple fault.
 *
 * (66e-2b) Before either, as a panic does: interrupts off, then the other
 * cores stopped, then kprintf's emergency path. A core is stopped
 * wherever it is, so a lock it held stays held: kprintf's mutex (a line
 * being printed) or sched_lock (taken by this core's next tick) waited
 * for forever, and the laptop once kept its screen and never reset.
 */
#include "arch.h"
#include "acpi.h"
#include "apic.h"
#include "cpus.h"
#include "cpu.h"
#include "io.h"
#include "timer.h"
#include "kprintf.h"
#include "paging.h"
#include "string.h"

#define PM1_SCI_EN      (1u << 0)       /* ACPI mode is on */
#define PM1_SLP_TYP     (7u << 10)
#define PM1_SLP_EN      (1u << 13)

#define KBC_COMMAND     0x64
#define KBC_PULSE_RESET 0xFE

/* Nothing else runs from here on, and printing takes no lock. */
static void stop_everything(void)
{
    arch_interrupts_disable();
    arch_stop_other_cores();                /* (65c) */
    kprintf_emergency();
}

void arch_power_off(void)
{
    const struct acpi_fadt *f = acpi_fadt();
    uint8_t typ_a, typ_b;
    if (!f || !f->pm1a_control_block || !acpi_s5_sleep_type(&typ_a, &typ_b)) {
        kprintf("poweroff: ACPI sleep state S5 not available\n");
        return;
    }
    stop_everything();
    uint16_t pm1a = (uint16_t)f->pm1a_control_block, pm1b = (uint16_t)f->pm1b_control_block;
    if (!(inw(pm1a) & PM1_SCI_EN) && f->smi_command && f->acpi_enable) {
        outb((uint16_t)f->smi_command, f->acpi_enable);
        for (int ms = 0; ms < 1000 && !(inw(pm1a) & PM1_SCI_EN); ms++)
            arch_delay_ms(1);
    }
    kprintf("powering off: ACPI S5, PM1a control port 0x%x, sleep type %u\n", pm1a, typ_a);
    outw(pm1a, (uint16_t)((inw(pm1a) & ~PM1_SLP_TYP) | typ_a << 10 | PM1_SLP_EN));
    if (pm1b)
        outw(pm1b, (uint16_t)((inw(pm1b) & ~PM1_SLP_TYP) | typ_b << 10 | PM1_SLP_EN));
    arch_delay_ms(1000);
    arch_interrupts_enable();
    kprintf("poweroff: the machine is still running\n");
}

static int asked_on;

static void delay_us(uint32_t us)
{
    uint64_t end = arch_counter() + arch_counter_hz() / 1000000 * us;
    while (arch_counter() < end)
        arch_cpu_relax();
}

void arch_reboot(void)
{
    arch_interrupts_disable();
    asked_on = arch_this_cpu()->index;
    kprintf_emergency();
    x86_reboot_cores();
}

void x86_reset_machine(void)
{
    int others = cpus_online() - 1;
    for (int ms = 0; ms < 1000 && x86_cores_stopped() < others; ms++)
        arch_delay_ms(1);
    apic_mask_all();
    apic_shutdown_core();
    kprintf("rebooting: on the boot processor (asked on core %d), %d of %d other cores stopped\n",
            asked_on, x86_cores_stopped(), others);
    const struct acpi_fadt *f = acpi_fadt();
    if (f && f->header.length >= 129 && (f->flags & FADT_RESET_REGISTER)) {
        const struct acpi_gas *r = &f->reset_register;
        if (r->space == ACPI_SPACE_IO) {
            kprintf("rebooting: ACPI reset register, I/O port 0x%lx = 0x%02x\n", r->address,
                    f->reset_value);
            outb((uint16_t)r->address, f->reset_value);
            arch_delay_ms(500);
        } else if (r->space == ACPI_SPACE_MEMORY) {
            kprintf("rebooting: ACPI reset register, memory %p = 0x%02x\n", (void *)r->address,
                    f->reset_value);
            mmio_write8(paging_map_mmio(r->address, 1), f->reset_value);
            arch_delay_ms(500);
        }
    }
    kprintf("rebooting: keyboard controller reset line\n");
    for (int i = 0; i < 10; i++) {          /* (Linux's ten tries, its input buffer free) */
        for (int k = 0; k < 0x10000 && (inb(KBC_COMMAND) & 2); k++)
            delay_us(2);
        delay_us(50);
        outb(KBC_COMMAND, KBC_PULSE_RESET);
        delay_us(50);
    }
    arch_delay_ms(500);

    kprintf("rebooting: triple fault\n");
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) empty = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(empty));
    for (;;)
        __asm__ volatile ("cli; hlt");
}

bool arch_port_access(uint16_t port, int width, uint32_t *value, bool write)
{
    switch (width) {
    case 8:
        if (write)
            outb(port, (uint8_t)*value);
        else
            *value = inb(port);
        return true;
    case 16:
        if (write)
            outw(port, (uint16_t)*value);
        else
            *value = inw(port);
        return true;
    case 32:
        if (write)
            outl(port, *value);
        else
            *value = inl(port);
        return true;
    }
    return false;
}

/* ---- (37d) The processor's power features ---- */

#define MSR_AMD_RAPL_UNIT       0xC0010299
#define MSR_AMD_PACKAGE_ENERGY  0xC001029B
#define MSR_RAPL_UNIT           0x606
#define MSR_PACKAGE_ENERGY      0x611

static bool vendor_is(const char *v)
{
    struct cpuid_result r = cpuid(0, 0);
    char name[13];
    memcpy(name, &r.ebx, 4);
    memcpy(name + 4, &r.edx, 4);
    memcpy(name + 8, &r.ecx, 4);
    name[12] = 0;
    return !strcmp(name, v);
}

/* The package's energy counter (32 bits) and its unit in picojoules, where
 * the processor says it has one: AMD from family 17h with CPUID's RAPL bit
 * (Linux's amd_energy); Intel's Silvermont family (the J1900), whose unit
 * is 2^ESU microjoules (Linux's rapl_check_unit_atom). Other Intel models
 * have one too, but no CPUID bit says so and reading a missing MSR faults:
 * they are added when a machine has one. */
static bool energy_counter(uint32_t *msr, uint64_t *unit_pj)
{
    struct cpuid_result f = cpuid(1, 0);
    uint32_t family = ((f.eax >> 8) & 0xF) + ((f.eax >> 20) & 0xFF);
    uint32_t model = ((f.eax >> 4) & 0xF) | ((f.eax >> 12) & 0xF0);
    if (vendor_is("AuthenticAMD") && family >= 0x17 && cpuid(0x80000000, 0).eax >= 0x80000007 &&
        (cpuid(0x80000007, 0).edx >> 14 & 1)) {
        uint32_t esu = (uint32_t)(rdmsr(MSR_AMD_RAPL_UNIT) >> 8 & 0x1F);
        *msr = MSR_AMD_PACKAGE_ENERGY;
        *unit_pj = 1000000000000ull >> esu;
        return true;
    }
    if (vendor_is("GenuineIntel") && family == 6 &&
        (model == 0x37 || model == 0x4A || model == 0x4D || model == 0x5A || model == 0x5D)) {
        uint32_t esu = (uint32_t)(rdmsr(MSR_RAPL_UNIT) >> 8 & 0x1F);
        *msr = MSR_PACKAGE_ENERGY;
        *unit_pj = 1000000ull << esu;
        return true;
    }
    return false;
}

void arch_processor_power_print(void)
{
    struct cpuid_result f = cpuid(1, 0);
    bool mwait = f.ecx >> 3 & 1, interrupt_break = false, arat = false;
    if (mwait && cpuid(0, 0).eax >= 5)
        interrupt_break = cpuid(5, 0).ecx >> 1 & 1;
    if (cpuid(0, 0).eax >= 6)
        arat = cpuid(6, 0).eax >> 2 & 1;
    char energy[96] = "no energy counter";
    uint32_t msr;
    uint64_t unit_pj;
    if (energy_counter(&msr, &unit_pj)) {
        uint32_t before = (uint32_t)rdmsr(msr);
        uint64_t start = arch_counter();
        timer_sleep_ms(1000);
        uint32_t after = (uint32_t)rdmsr(msr);
        uint64_t ticks = arch_counter() - start, hz = arch_counter_hz();
        uint64_t uj = (uint64_t)(after - before) * unit_pj / 1000000;   /* (wraps once at most) */
        uint64_t mw = hz && ticks ? uj * hz / ticks / 1000 : 0;
        ksnprintf(energy, sizeof(energy), "the package used %lu.%02lu W over 1 s (its energy "
                  "counter)", mw / 1000, mw % 1000 / 10);
    }
    kprintf("processor: %s%s; the APIC timer %s in deep states%s; %s\n",
            mwait ? "MWAIT" : "no MWAIT",
            mwait ? (interrupt_break ? " (interrupts break it)" : " (no interrupt break)") : "",
            arat ? "keeps running" : "stops", arat ? " (ARAT)" : "", energy);
    arch_idle_print();
}
