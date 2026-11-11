/*
 * AMD's GPIO controller (ACPI AMDI0030; 15u): a pin as an interrupt, for
 * devices whose interrupt line is a GPIO pin (the touchpad's on the laptop).
 */
#ifndef RITTEROS_AMDGPIO_H
#define RITTEROS_AMDGPIO_H

#include <stdbool.h>
#include <stdint.h>

struct gpio_pin;

/* Pin `pin` of the GPIO controller named seg (an ACPI name like "GPIO") as
 * a level-triggered, active-low interrupt: fn(ctx) runs in interrupt
 * context when it is asserted, and the pin stays masked until
 * gpio_pin_rearm (the device served, its line released). 0 when there is
 * no such controller or its interrupt cannot be routed. */
struct gpio_pin *gpio_pin_listen(const char *seg, uint16_t pin, void (*fn)(void *), void *ctx);
/* Is the line asserted (low) now? */
bool gpio_pin_asserted(const struct gpio_pin *p);
/* Unmask it again (an interrupt follows at once if still asserted). */
void gpio_pin_rearm(struct gpio_pin *p);

/* (37g-3) After a sleep: the pin's interrupt settings again. */
void gpio_pin_wake(struct gpio_pin *p);

#endif
