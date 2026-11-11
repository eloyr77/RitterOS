/* Access to x86-64 control registers, model-specific registers and CPUID. */
#ifndef RITTEROS_CPU_H
#define RITTEROS_CPU_H

#include <stdint.h>

#define MSR_EFER        0xC0000080
#define MSR_PAT         0x00000277      /* page attribute table: 8 memory types */
#define EFER_NXE        (1u << 11)      /* enables the no-execute page bit */

static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile ("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v; }

/* Load a new top-level page table; also flushes non-global TLB entries. */
static inline void write_cr3(uint64_t v)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

struct cpuid_result {
    uint32_t eax, ebx, ecx, edx;
};

static inline struct cpuid_result cpuid(uint32_t leaf, uint32_t subleaf)
{
    struct cpuid_result r;
    __asm__ volatile ("cpuid" : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                              : "a"(leaf), "c"(subleaf));
    return r;
}

#endif
