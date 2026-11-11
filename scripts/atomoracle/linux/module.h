/* atomhost's stand-in (see ../README): what atom.c takes from the kernel. */
#ifndef ATOMORACLE_MODULE_H
#define ATOMORACLE_MODULE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "linux/types.h"
#define pr_info(...) ((void)0)
#define printk(...) ((void)0)
#define KERN_DEBUG ""
#define DRM_ERROR(...) ((void)0)
#define GFP_KERNEL 0
#define kzalloc(n, f) calloc(1, (n))
#define kcalloc(n, m, f) calloc((n), (m))
#define kfree(p) free(p)
#define mutex_lock(m) ((void)(m))
#define mutex_unlock(m) ((void)(m))
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define le16_to_cpu(x) (x)
#define lower_32_bits(x) ((uint32_t)(x))
#define upper_32_bits(x) ((uint32_t)((uint64_t)(x) >> 32))
#define do_div(n, base) ((n) = (n) / (base))
/* Time: atomhost's clock, which moves a jiffy (1 ms) at every look; ours
 * reads the same clock where atom.c reads jiffies. */
unsigned long atomhost_tick(void);
#define jiffies atomhost_tick()
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define jiffies_to_msecs(j) ((unsigned int)(j))
void atomhost_delay_us(unsigned long us);
#define udelay(us) atomhost_delay_us(us)
#define mdelay(ms) atomhost_delay_us((ms) * 1000ul)
#define msleep(ms) atomhost_delay_us((ms) * 1000ul)
#define drm_can_sleep() 1
static inline const char *str_yes_no(int v) { return v ? "yes" : "no"; }
static inline uint32_t get_unaligned_le32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
#endif
