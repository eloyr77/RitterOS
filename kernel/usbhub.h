/*
 * USB hubs (USB 2.0 specification, chapter 11; USB 3 hubs, 3.2 chapter 10).
 *
 * A hub is a USB device with downstream ports. The host powers them,
 * resets a port where something is connected (which tells the device's
 * speed) and then enumerates that device like one on a root port; it may
 * be a hub itself. Everything goes through class-specific control requests
 * to the hub. Devices are found at the boot; plugging in later is not
 * supported yet.
 */
#ifndef RITTEROS_USBHUB_H
#define RITTEROS_USBHUB_H

#include <stdbool.h>
#include "usb.h"

/* Set up a configured hub and enumerate the devices on its ports; then
 * listen on its status change endpoint (in: 0 if it has none) for devices
 * plugged in or out later. */
bool usbhub_attach(struct usb_device *hub, const struct usb_endpoint_descriptor *in);

/* For a controller's usb thread: look at the ports of its hubs (those on
 * controller hc) that reported a change, and attach or detach their
 * devices. */
void usbhub_work(void *hc);

/* dev is gone: if it is a hub, stop handling it. */
void usbhub_detach(struct usb_device *dev);

#endif
