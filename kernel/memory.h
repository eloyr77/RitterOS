/*
 * Converting between physical and virtual addresses (see common/vmlayout.h).
 * All physical memory is reachable through the direct map at HHDM_BASE; the
 * kernel image is additionally mapped at KERNEL_VIRT_BASE.
 */
#ifndef RITTEROS_MEMORY_H
#define RITTEROS_MEMORY_H

#include <stdint.h>
#include "vmlayout.h"

/* Physical address of the kernel image (from the boot info). */
extern uint64_t kernel_phys_base;

/* Pointer to physical memory, through the direct map. */
static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(phys + HHDM_BASE);
}

/* Physical address of a kernel-image or direct-map pointer. */
static inline uint64_t virt_to_phys(const void *virt)
{
    uint64_t v = (uint64_t)virt;
    if (v >= KERNEL_VIRT_BASE)
        return v - KERNEL_VIRT_BASE + kernel_phys_base;
    return v - HHDM_BASE;
}

#endif
