/* AArch64 system registers and barriers. */
#ifndef RITTEROS_CPU_H
#define RITTEROS_CPU_H

#include <stdint.h>

#define read_sysreg(reg) ({ uint64_t v_; __asm__ volatile ("mrs %0, " #reg : "=r"(v_)); v_; })
#define write_sysreg(reg, value) \
    __asm__ volatile ("msr " #reg ", %0" : : "r"((uint64_t)(value)) : "memory")

static inline void isb(void) { __asm__ volatile ("isb" ::: "memory"); }
static inline void dsb_ish(void) { __asm__ volatile ("dsb ish" ::: "memory"); }
static inline void dsb_ishst(void) { __asm__ volatile ("dsb ishst" ::: "memory"); }

/* The exception level we run at: 1 (kernel) or 2 (hypervisor). */
static inline unsigned current_el(void)
{
    return (unsigned)(read_sysreg(CurrentEL) >> 2) & 3;
}

/* Translation table descriptors (4 KiB granule). */
#define DESC_TABLE      3ULL                /* levels 0-2: next table */
#define DESC_BLOCK      1ULL                /* levels 1-2: 1 GiB / 2 MiB block */
#define DESC_PAGE       3ULL                /* level 3: 4 KiB page */
#define DESC_ATTR(i)    ((uint64_t)(i) << 2)  /* index into MAIR_EL1 */
#define DESC_AP_EL0     (1ULL << 6)         /* accessible at EL0 (user mode) */
#define DESC_AP_RO      (2ULL << 6)         /* read-only (EL1) */
#define DESC_NG         (1ULL << 11)        /* not global: belongs to one address space */
#define DESC_SH_INNER   (3ULL << 8)         /* inner shareable */
#define DESC_AF         (1ULL << 10)        /* access flag: set, or every access faults */
#define DESC_PXN        (1ULL << 53)        /* not executable at EL1 */
#define DESC_UXN        (1ULL << 54)        /* not executable at EL0 */
#define DESC_ADDRESS    0x0000FFFFFFFFF000ULL

/* MAIR_EL1 attribute encodings. */
#define MAIR_NORMAL_WB  0xFF                /* normal memory, write-back cacheable */
#define MAIR_DEVICE_NGNRNE 0x00             /* device memory, strictly ordered */
#define MAIR_DEVICE_NGNRE  0x04             /* device memory, early write ack */

#endif
