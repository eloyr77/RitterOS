/*
 * The processor's clock on x86-64 (64t, the user's: /system/config/cpu,
 * read by kernel/cpuclock.c).
 *
 * AMD's Zen processors (family 0x17 on) with CPPC (CPUID 0x80000008, EBX
 * bit 27) are told what to aim for through two registers: CPPC_ENABLE
 * (once on, it stays on until a reset) and CPPC_REQ, which holds the
 * lowest and highest performance level allowed (bits 15:8 and 7:0), a
 * desired level (23:16, 0: the processor chooses) and the energy
 * preference (31:24, 0 = performance .. 255 = power). The levels come from
 * CPPC_CAP1: highest (31:24), nominal (23:16), lowest nonlinear (15:8),
 * lowest (7:0). This is what Linux's amd-pstate driver does in its active
 * mode. Without a request the processor keeps the firmware's P-state: on
 * the user's laptop (Ryzen 7 5800H) a busy loop then ran at 4,073 MHz, but
 * the short bursts of streaming from the PlutoSDR ran at the base 3.2 GHz;
 * with `top` (lowest = highest) at 3,539 MHz.
 *
 * The request is the running processor's own (each core has one); only
 * the boot processor runs. Intel processors (the J1900: no HWP) and QEMU's
 * are left alone.
 */
#include "arch.h"
#include "cpu.h"
#include "kprintf.h"

#define MSR_CPPC_CAP1   0xC00102B0
#define MSR_CPPC_ENABLE 0xC00102B1
#define MSR_CPPC_REQ    0xC00102B3

static bool has_cppc(void)
{
    struct cpuid_result v = cpuid(0, 0), f = cpuid(1, 0);
    if (v.ebx != 0x68747541 || v.edx != 0x69746E65 || v.ecx != 0x444D4163)
        return false;                   /* not "AuthenticAMD" */
    uint32_t family = ((f.eax >> 8) & 0xF) + ((f.eax >> 20) & 0xFF);
    return family >= 0x17 && cpuid(0x80000000, 0).eax >= 0x80000008 &&
           (cpuid(0x80000008, 0).ebx >> 27 & 1);
}

bool arch_clock_set(enum arch_clock mode, char *out, size_t size)
{
    if (!has_cppc())
        return false;
    if (mode == ARCH_CLOCK_FIRMWARE) {
        ksnprintf(out, size, "CPPC left off: the firmware's own settings");
        return true;
    }
    uint64_t cap = rdmsr(MSR_CPPC_CAP1);
    uint64_t highest = cap >> 24 & 0xFF, nonlinear = cap >> 8 & 0xFF;
    uint64_t lowest = mode == ARCH_CLOCK_TOP ? highest : nonlinear;
    uint64_t preference = mode == ARCH_CLOCK_BALANCED ? 128 : mode == ARCH_CLOCK_POWERSAVE ? 255 : 0;
    wrmsr(MSR_CPPC_ENABLE, 1);
    wrmsr(MSR_CPPC_REQ, highest | lowest << 8 | preference << 24);
    if (mode == ARCH_CLOCK_TOP)
        ksnprintf(out, size, "CPPC on: its highest level (%lu) whenever it runs", highest);
    else
        ksnprintf(out, size, "CPPC on: the processor chooses between levels %lu and %lu, "
                  "energy preference %lu", lowest, highest, preference);
    return true;
}
