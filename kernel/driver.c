/*
 * (67b) The driver interface's functions the kernel has inline
 * (drivers/include/driver.h marks them DRIVER_WRAPPED): a callable copy of
 * each, driver_NAME, which the loader's table gives modules for NAME.
 * virt_to_phys also knows the module region: a module's own data lies
 * above the kernel image's base but is not the image (paging_query).
 * (67d) wlan_register takes the card and its Wi-Fi side (the kernel's links
 * them first); firmware_load reads a file of /system/firmware/.
 * (67f) gpu_register gives a module's struct gpu_card a struct gpu of the
 * layer's (gpu.h, unchanged), whose operations hand each call on to the
 * card's; the card's `state` is that struct gpu.
 */
#include "driver.h"
#include "arch.h"
#include "gpu.h"
#include "heap.h"
#include "kprintf.h"
#include "memory.h"
#include "mmio.h"
#include "mounts.h"
#include "paging.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "vfs.h"
#include "vmlayout.h"
#include "wlan.h"

void *driver_phys_to_virt(uint64_t phys);
uint64_t driver_virt_to_phys(const void *virt);
uint8_t driver_mmio_read8(const volatile void *a);
uint16_t driver_mmio_read16(const volatile void *a);
uint32_t driver_mmio_read32(const volatile void *a);
uint64_t driver_mmio_read64(const volatile void *a);
void driver_mmio_write8(volatile void *a, uint8_t v);
void driver_mmio_write16(volatile void *a, uint16_t v);
void driver_mmio_write32(volatile void *a, uint32_t v);
void driver_mmio_write64(volatile void *a, uint64_t v);
void driver_arch_write_barrier(void);
void driver_arch_read_barrier(void);
void driver_arch_cpu_relax(void);
uint64_t driver_spin_lock(struct spinlock *lock);
void driver_spin_unlock(struct spinlock *lock, uint64_t state);
uint64_t driver_arch_irq_save(void);
void driver_arch_irq_restore(uint64_t state);
void driver_wlan_register(struct net_card *card, struct wifi_card *wifi);
void *firmware_load(const char *name, uint64_t *size);
void driver_gpu_register(struct gpu_card *card);
const char *gpu_output_name(struct gpu_card *card, int i);
const struct edid_info *gpu_output_monitor(struct gpu_card *card, int i);
bool thread_stop_requested(void);

void *driver_phys_to_virt(uint64_t phys) { return phys_to_virt(phys); }

uint64_t driver_virt_to_phys(const void *virt)
{
    uint64_t v = (uint64_t)virt, phys, flags;
    if (v >= MODULE_BASE && v < MODULE_BASE + MODULE_SIZE)
        return paging_query(v, &phys, &flags) ? phys : 0;
    return virt_to_phys(virt);
}

uint8_t driver_mmio_read8(const volatile void *a) { return mmio_read8(a); }
uint16_t driver_mmio_read16(const volatile void *a) { return mmio_read16(a); }
uint32_t driver_mmio_read32(const volatile void *a) { return mmio_read32(a); }
uint64_t driver_mmio_read64(const volatile void *a) { return mmio_read64(a); }
void driver_mmio_write8(volatile void *a, uint8_t v) { mmio_write8(a, v); }
void driver_mmio_write16(volatile void *a, uint16_t v) { mmio_write16(a, v); }
void driver_mmio_write32(volatile void *a, uint32_t v) { mmio_write32(a, v); }
void driver_mmio_write64(volatile void *a, uint64_t v) { mmio_write64(a, v); }
void driver_arch_write_barrier(void) { arch_write_barrier(); }
void driver_arch_read_barrier(void) { arch_read_barrier(); }
void driver_arch_cpu_relax(void) { arch_cpu_relax(); }
uint64_t driver_spin_lock(struct spinlock *lock) { return spin_lock(lock); }
void driver_spin_unlock(struct spinlock *lock, uint64_t state) { spin_unlock(lock, state); }
uint64_t driver_arch_irq_save(void) { return arch_irq_save(); }
void driver_arch_irq_restore(uint64_t state) { arch_irq_restore(state); }

void driver_wlan_register(struct net_card *card, struct wifi_card *wifi)
{
    struct netdev *d = (struct netdev *)card;
    d->wlan = (struct wlan *)wifi;
    wlan_register(d);
}

#define FIRMWARE_MAX (16u << 20)

void *firmware_load(const char *name, uint64_t *size)
{
    char path[128];
    ksnprintf(path, sizeof(path), "/system/firmware/%s", name);
    struct vfs *fs = mounts_root();
    uint64_t id;
    struct rfs_inode inode;
    if (!fs || !vfs_lookup(fs, path, &id, &inode) || inode.size > FIRMWARE_MAX)
        return 0;
    void *data = kmalloc(inode.size ? inode.size : 1);
    if (!data)
        return 0;
    if (vfs_read(fs, id, &inode, 0, data, inode.size) != (int64_t)inode.size) {
        kfree(data);
        return 0;
    }
    *size = inode.size;
    return data;
}

bool thread_stop_requested(void)
{
    return thread_current()->stop_requested;
}

/* --- (67f) a module's graphics card --- */

static struct gpu_card *card_of(struct gpu *g)
{
    return g->priv;
}

static bool card_read_edid(struct gpu *g, int i, uint32_t offset, uint8_t *buf, uint32_t len)
{
    return card_of(g)->read_edid(card_of(g), i, offset, buf, len);
}

static bool card_showing(struct gpu *g, int i)
{
    return card_of(g)->showing(card_of(g), i);
}

static void card_report(struct gpu *g)
{
    card_of(g)->report(card_of(g));
}

static bool card_set_mode(struct gpu *g, int i, const struct edid_detailed *t, struct framebuffer *fb,
                          char *why, size_t why_size)
{
    struct gpu_screen screen;
    memset(&screen, 0, sizeof(screen));
    if (!card_of(g)->set_mode(card_of(g), i, t, &screen, why, why_size))
        return false;
    *fb = (struct framebuffer){ screen.base, screen.width, screen.height, screen.stride,
                                screen.red_shift, screen.green_shift, screen.blue_shift };
    return true;
}

static bool card_wait_vblank(struct gpu *g, int i, uint64_t *frame, uint32_t timeout_ms)
{
    return card_of(g)->wait_vblank(card_of(g), i, frame, timeout_ms);
}

static bool card_flip(struct gpu *g, int i, int buffer, uint64_t *frame)
{
    return card_of(g)->flip(card_of(g), i, buffer, frame);
}

static bool card_buffers(struct gpu *g, int i, uint64_t *phys, uint64_t *slot, int *count,
                         uint32_t *stride)
{
    return card_of(g)->buffers(card_of(g), i, phys, slot, count, stride);
}

static bool card_backlight(struct gpu *g, int i, bool on)
{
    return card_of(g)->backlight(card_of(g), i, on);
}

static bool card_brightness(struct gpu *g, int i, uint32_t level)
{
    return card_of(g)->brightness(card_of(g), i, level);
}

static bool card_brightness_now(struct gpu *g, int i, uint32_t *level)
{
    return card_of(g)->brightness_now(card_of(g), i, level);
}

static void card_report_sleep(struct gpu *g)
{
    card_of(g)->report_sleep(card_of(g));
}

void driver_gpu_register(struct gpu_card *card)
{
    struct gpu *g = kzalloc(sizeof(*g));
    if (!g) {
        kprintf("  gpu:           no memory for %s\n", card->model);
        return;
    }
    g->model = card->model;
    g->pci = card->pci;
    g->output_count = card->output_count < GPU_OUTPUTS ? card->output_count : GPU_OUTPUTS;
    for (int i = 0; i < g->output_count; i++) {
        g->outputs[i].kind = card->outputs[i].kind;
        g->outputs[i].where = card->outputs[i].where;
    }
    g->priv = card;
    g->read_edid = card->read_edid ? card_read_edid : 0;
    g->showing = card->showing ? card_showing : 0;
    g->report = card->report ? card_report : 0;
    g->set_mode = card->set_mode ? card_set_mode : 0;
    g->wait_vblank = card->wait_vblank ? card_wait_vblank : 0;
    g->flip = card->flip ? card_flip : 0;
    g->buffers = card->buffers ? card_buffers : 0;
    g->backlight = card->backlight ? card_backlight : 0;
    g->brightness = card->brightness ? card_brightness : 0;
    g->brightness_now = card->brightness_now ? card_brightness_now : 0;
    g->report_sleep = card->report_sleep ? card_report_sleep : 0;
    card->state = g;
    gpu_register(g);
}

const char *gpu_output_name(struct gpu_card *card, int i)
{
    struct gpu *g = card->state;
    return g && i >= 0 && i < g->output_count ? g->outputs[i].name : "";
}

const struct edid_info *gpu_output_monitor(struct gpu_card *card, int i)
{
    struct gpu *g = card->state;
    return g && i >= 0 && i < g->output_count && g->outputs[i].connected ? &g->outputs[i].info : 0;
}
