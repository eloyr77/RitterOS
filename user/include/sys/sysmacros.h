/*
 * sys/sysmacros.h (43a): a device number's parts (major, minor) and
 * makedev, in glibc's layout.
 */
#ifndef RITTEROS_SYS_SYSMACROS_H
#define RITTEROS_SYS_SYSMACROS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define major(dev)      ((unsigned)((((uint64_t)(dev) >> 32) & 0xfffff000u) | (((uint64_t)(dev) >> 8) & 0xfffu)))
#define minor(dev)      ((unsigned)((((uint64_t)(dev) >> 12) & 0xffffff00u) | ((uint64_t)(dev) & 0xffu)))
#define makedev(ma, mi) ((((uint64_t)(ma) & 0xfffff000u) << 32) | (((uint64_t)(ma) & 0xfffu) << 8) | \
                         (((uint64_t)(mi) & 0xffffff00u) << 12) | ((uint64_t)(mi) & 0xffu))

#ifdef __cplusplus
}
#endif

#endif
