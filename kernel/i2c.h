/*
 * I2C controllers (15t): the Synopsys DesignWare controller, which AMD's
 * (ACPI AMDI0010) and Intel's chipsets build in, as an I2C master named
 * i2c0, i2c1, ... in the order set up. A transfer writes some bytes to a
 * device and/or reads some back, polled, with a limit on every wait.
 */
#ifndef RITTEROS_I2C_H
#define RITTEROS_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct i2c_bus;

/* The controller whose registers are at phys (once per address: the same
 * bus again for a second device), its input clock clock_khz, run at
 * speed_hz (100 or 400 kHz); 0 if it does not answer as a DesignWare
 * controller. `what` names it on the boot line (its ACPI path and ID). */
struct i2c_bus *i2c_bus_at(uint64_t phys, uint32_t clock_khz, uint32_t speed_hz, const char *what);
const char *i2c_name(const struct i2c_bus *b);

/* Write wn bytes to the device at address, then (with a repeated start)
 * read rn bytes; either may be 0. 0, or a negative ERR_*: ERR_NODEV when
 * nobody answered at that address, ERR_IO on another abort, ERR_TIMEDOUT. */
int i2c_transfer(struct i2c_bus *b, uint16_t address, const uint8_t *w, size_t wn, uint8_t *r,
                 size_t rn);

/* (37g-3) Sleep: the bus held (no transfer by anyone else) and let go;
 * after a sleep the controller set up again (false: it does not switch
 * off to take it). */
void i2c_bus_hold(struct i2c_bus *b);
void i2c_bus_let_go(struct i2c_bus *b);
bool i2c_bus_wake(struct i2c_bus *b);

#endif
