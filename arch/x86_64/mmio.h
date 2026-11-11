/* Device register access for x86-64 (see kernel/arch_api.h); also used by the bootloader. */
#ifndef RITTEROS_MMIO_H
#define RITTEROS_MMIO_H

#include <stdint.h>

/* Device registers: plain volatile accesses (x86 has no access forms that
 * a hypervisor could not emulate). */
static inline uint8_t mmio_read8(const volatile void *a) { return *(const volatile uint8_t *)a; }
static inline uint16_t mmio_read16(const volatile void *a) { return *(const volatile uint16_t *)a; }
static inline uint32_t mmio_read32(const volatile void *a) { return *(const volatile uint32_t *)a; }
static inline uint64_t mmio_read64(const volatile void *a) { return *(const volatile uint64_t *)a; }
static inline void mmio_write8(volatile void *a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline void mmio_write16(volatile void *a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void mmio_write32(volatile void *a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void mmio_write64(volatile void *a, uint64_t v) { *(volatile uint64_t *)a = v; }

#endif
