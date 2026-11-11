/*
 * The AMD FCH GPIO controller (register facts as Linux's pinctrl-amd uses
 * them; the code is our own): a 32-bit register per pin at base + 4 * pin,
 * one interrupt line for all pins (ACPI: the controller's Extended
 * Interrupt). Pin register bits: 8 level-triggered, 9-10 active level (1:
 * low), 11 interrupt enable, 12 interrupt unmasked, 16 the pin's level, 28
 * interrupt status (write 1 to clear), 29 wake status (likewise). After an
 * interrupt the controller wants an end-of-interrupt: bit 29 of its master
 * register (0xFC).
 *
 * A level-triggered line stays asserted until the device is served, so the
 * handler masks the pin and the device's thread unmasks it when done
 * (gpio_pin_rearm); asserted still, it interrupts again at once.
 */
#include "amdgpio.h"
#include "acpi.h"
#include "arch.h"
#include "heap.h"
#include "kprintf.h"
#include "mmio.h"
#include "paging.h"
#include "sync.h"

#define PIN_LEVEL_TRIG      (1u << 8)
#define PIN_ACTIVE_LOW      (1u << 9)
#define PIN_ACTIVE_MASK     (3u << 9)
#define PIN_INT_ENABLE      (1u << 11)
#define PIN_INT_UNMASK      (1u << 12)
#define PIN_LEVEL           (1u << 16)
#define PIN_INT_STATUS      (1u << 28)
#define PIN_WAKE_STATUS     (1u << 29)
#define MASTER_REG          0xFC
#define MASTER_EOI          (1u << 29)
#define MAX_PINS            4

struct gpio_pin {
    volatile uint8_t *regs;             /* the controller's */
    uint16_t pin;
    void (*fn)(void *);
    void *ctx;
    uint32_t config;                    /* (37g-3) as set: again after a sleep */
};

static struct gpio_pin *pins[MAX_PINS];
static int pin_count;
/* (65a-4) The pin list and each pin register's read-modify-write: the
 * interrupt handler and a driver's rearm may run on two cores at once.
 * Until 65a-4 interrupts off did this. */
static struct spinlock pins_lock = SPINLOCK_INIT("gpio pins");
static volatile uint8_t *controller;    /* one controller (AMD's has one) */

static uint32_t pin_read(const struct gpio_pin *p)
{
    return mmio_read32(p->regs + 4u * p->pin);
}

/* Write a pin's register without clearing its status bits by accident. */
static void pin_write(const struct gpio_pin *p, uint32_t v)
{
    mmio_write32(p->regs + 4u * p->pin, v & ~(PIN_INT_STATUS | PIN_WAKE_STATUS));
}

/* Interrupt context: every listened pin whose status is set is masked, its
 * status cleared, its device told. */
static void handler(void *unused)
{
    (void)unused;
    uint64_t state = spin_lock(&pins_lock);
    for (int i = 0; i < pin_count; i++) {
        struct gpio_pin *p = pins[i];
        uint32_t v = pin_read(p);
        if (!(v & PIN_INT_STATUS))
            continue;
        pin_write(p, v & ~PIN_INT_UNMASK);
        mmio_write32(p->regs + 4u * p->pin, (v & ~PIN_INT_UNMASK & ~PIN_WAKE_STATUS) |
                                                PIN_INT_STATUS);  /* (1 clears) */
        spin_unlock(&pins_lock, state);
        p->fn(p->ctx);                          /* (not under the lock: it may rearm) */
        state = spin_lock(&pins_lock);
    }
    spin_unlock(&pins_lock, state);
    mmio_write32(controller + MASTER_REG, mmio_read32(controller + MASTER_REG) | MASTER_EOI);
}

struct gpio_pin *gpio_pin_listen(const char *seg, uint16_t pin, void (*fn)(void *), void *ctx)
{
    struct acpi_device all[2], d;
    int n = acpi_devices_with("AMDI0030", all, 2);
    bool found = false;
    for (int i = 0; i < n && !found; i++)
        found = acpi_device_named(seg, &d) && d.start == all[i].start;
    uint64_t base;
    uint32_t length, gsi;
    bool level, active_low;
    if (!found || pin_count == MAX_PINS || !acpi_device_memory(&d, &base, &length) ||
        length < 4u * (pin + 1u) || !acpi_device_interrupt(&d, &gsi, &level, &active_low))
        return 0;
    volatile uint8_t *regs = paging_map_mmio(base, length);
    struct gpio_pin *p = regs ? kzalloc(sizeof(*p)) : 0;
    if (!p)
        return 0;
    p->regs = regs, p->pin = pin, p->fn = fn, p->ctx = ctx;
    if (!controller) {
        unsigned vector;
        controller = regs;
        if (!arch_route_gsi(gsi, level, active_low, handler, 0, &vector)) {
            controller = 0;
            kfree(p);
            return 0;
        }
        kprintf("  gpio:          AMD GPIO controller at 0x%lx (%s), GSI %u (%s, active %s), "
                "vector 0x%02x\n", base, seg, gsi, level ? "level" : "edge",
                active_low ? "low" : "high", vector);
    }
    uint32_t v = pin_read(p);
    v = (v & ~PIN_ACTIVE_MASK) | PIN_LEVEL_TRIG | PIN_ACTIVE_LOW | PIN_INT_ENABLE | PIN_INT_UNMASK;
    uint64_t state = spin_lock(&pins_lock);
    pins[pin_count++] = p;
    p->config = v;
    pin_write(p, v);
    spin_unlock(&pins_lock, state);
    return p;
}

void gpio_pin_wake(struct gpio_pin *p)
{
    uint64_t state = spin_lock(&pins_lock);
    pin_write(p, p->config);
    spin_unlock(&pins_lock, state);
}

bool gpio_pin_asserted(const struct gpio_pin *p)
{
    return !(pin_read(p) & PIN_LEVEL);          /* (active low) */
}

void gpio_pin_rearm(struct gpio_pin *p)
{
    uint64_t state = spin_lock(&pins_lock);
    pin_write(p, pin_read(p) | PIN_INT_UNMASK);
    spin_unlock(&pins_lock, state);
}
