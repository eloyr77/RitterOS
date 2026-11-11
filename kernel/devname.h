/*
 * Stable device names: kbd0, usbdisk1, ... A name stays reserved for its
 * device until the reboot, so a device that is unplugged and plugged in
 * again gets its name back, and a name never moves to another device.
 *
 * "The same device": the same kind, vendor and product ID and serial
 * number, in any socket. Without a serial number (many keyboards), an
 * unplugged device with the same IDs is taken to be it; if several such
 * are unplugged, the one last seen in the same socket (path), else the one
 * with the lowest number. A new device gets the next number never used.
 * The boot assigns names in the order the devices are found.
 */
#ifndef RITTEROS_DEVNAME_H
#define RITTEROS_DEVNAME_H

#include <stdbool.h>
#include <stdint.h>

struct devname_id {
    uint16_t vendor, product;       /* 0, 0 for built-in devices (PS/2) */
    const char *serial;             /* "" if none */
    const char *path;               /* where it hangs, e.g. "usb0 port 5.1" */
};

/* The number for a device of this kind ("kbd", "usbdisk") that has just
 * appeared; *again is set if it is a known device coming back. -1 if the
 * table is full. */
int devname_attach(const char *kind, const struct devname_id *id, bool *again);

/* The device with this kind and number is gone; its name stays reserved. */
void devname_detach(const char *kind, int number);

#endif
