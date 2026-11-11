/*
 * USB HID absolute pointing device (a "tablet"): the pointer QEMU, UTM and
 * other hypervisors use so the host and guest cursors stay aligned without
 * capturing the mouse. Unlike the boot-protocol mouse (usbmouse.c), it reports
 * an absolute position, and its report layout is read from the device's HID
 * report descriptor rather than fixed. It feeds the same mouse0 / winput path.
 */
#ifndef RITTEROS_USBTABLET_H
#define RITTEROS_USBTABLET_H

#include <stdbool.h>
#include "usb.h"

/* Set up a HID interface whose report descriptor describes an absolute
 * pointer. Returns false if it is not one (the caller leaves the device
 * unbound). */
bool usbtablet_attach(struct usb_device *dev, const struct usb_interface_descriptor *interface,
                      const struct usb_endpoint_descriptor *endpoint);

/* An unplugged device: end any tablet of it. */
void usbtablet_detach(struct usb_device *dev);

#endif
