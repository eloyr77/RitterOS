/*
 * USB keyboards in the HID "boot protocol": every report is 8 bytes, the
 * same on all keyboards, so no HID report descriptor needs to be parsed.
 *
 *   byte 0     modifier keys: bit 0-3 left Ctrl, Shift, Alt, GUI; 4-7 right
 *   byte 1     reserved
 *   bytes 2-7  up to six keys held down, as HID usage IDs (0 = none)
 *
 * A key is pressed when it appears in a report and was not in the previous
 * one, released when it disappears; what keys do is keyboard.c's part.
 */
#ifndef RITTEROS_USBKBD_H
#define RITTEROS_USBKBD_H

#include <stdbool.h>
#include "usb.h"

/* Switch the keyboard interface to the boot protocol and start receiving
 * reports from its interrupt endpoint. The device must be configured. */
bool usbkbd_attach(struct usb_device *dev, const struct usb_interface_descriptor *interface,
                   const struct usb_endpoint_descriptor *endpoint);

/* The device was unplugged (its interrupt endpoint is gone already). */
void usbkbd_detach(struct usb_device *dev);

#endif
