/*
 * USB mice. (68m-3) In report protocol where their HID report descriptor
 * says where X, Y and the buttons lie (hid.c's hid_pointer: also the wheel
 * and the horizontal wheel, AC Pan); else in the HID "boot protocol",
 * where every report is at least 3 bytes, the same on all mice:
 *
 *   byte 0   buttons: bit 0 left, bit 1 right, bit 2 middle
 *   byte 1   X movement since the last report, signed
 *   byte 2   Y movement since the last report, signed (down is positive)
 *   byte 3   wheel movement, signed (optional, present on most mice)
 *
 * What the movement and buttons mean is mouse.c's part.
 */
#ifndef RITTEROS_USBMOUSE_H
#define RITTEROS_USBMOUSE_H

#include <stdbool.h>
#include "usb.h"

/* Switch the mouse interface to report (or boot) protocol and start receiving
 * reports from its interrupt endpoint. The device must be configured. */
bool usbmouse_attach(struct usb_device *dev, const struct usb_interface_descriptor *interface,
                     const struct usb_endpoint_descriptor *endpoint);

/* The device was unplugged (its interrupt endpoint is gone already). */
void usbmouse_detach(struct usb_device *dev);

#endif
