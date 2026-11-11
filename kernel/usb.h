/*
 * USB devices, independent of the host controller (USB 2.0 specification,
 * chapter 9).
 *
 * Every USB device answers "control transfers" on its endpoint 0: an 8-byte
 * setup packet (a request), optionally followed by data. With them the host
 * reads the device's descriptors: what it is (vendor, product, class), how
 * it can be configured, and its interfaces (functions such as "keyboard")
 * with their endpoints (data channels).
 */
#ifndef RITTEROS_USB_H
#define RITTEROS_USB_H

#include <stdbool.h>
#include <stdint.h>

#include "driver_usb.h"    /* (67e) the request fields, descriptors and bulk stats: the drivers' too */

/* Speeds, numbered as in the xHCI port status register. */
enum { USB_SPEED_FULL = 1, USB_SPEED_LOW = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 4,
       USB_SPEED_SUPER_PLUS = 5 };

_Static_assert(sizeof(struct usb_device_descriptor) == 18, "USB device descriptor size");
_Static_assert(sizeof(struct usb_config_descriptor) == 9, "USB configuration descriptor size");
_Static_assert(sizeof(struct usb_interface_descriptor) == 9, "USB interface descriptor size");
_Static_assert(sizeof(struct usb_endpoint_descriptor) == 7, "USB endpoint descriptor size");

struct xhci;

struct usb_device {
    struct xhci *hc;                /* the controller it hangs on */
    char name[32];                  /* e.g. "usb0 port 5", behind hubs "usb0 port 5.1.3" */
    uint8_t port, speed, slot, address;
    /* Where it hangs: on a root port of the controller (parent 0), or on a
     * hub's port. The xHCI controller needs the route string (the hub port
     * on each level, 4 bits each), the root port at the top, and for a
     * low or full speed device behind a high speed hub that hub's
     * transaction translator (TT) and port. */
    struct usb_device *parent;
    uint8_t depth;                  /* hubs above it */
    uint8_t root_port;
    uint32_t route;
    uint8_t tt_slot, tt_port;
    bool tt_multi;
    /* If it is a hub: */
    uint8_t hub_ports;
    bool hub_multi_tt;              /* a transaction translator per port */
    uint16_t max_packet0;           /* endpoint 0 packet size in bytes */
    struct usb_device_descriptor descriptor;
    char manufacturer[64], product[64], serial[64];    /* UTF-8, "" if none */
    char function[40];              /* what its first interface is, e.g. "HID keyboard" */
    uint8_t config;                 /* (67e) its configuration's value (SET_CONFIGURATION) */
    bool configured;                /* (67e) selected: by a driver of the kernel's or for a module */
    void *hc_data;                  /* the controller driver's per-device state */
};

/* A control transfer on endpoint 0 (implemented by the host controller
 * driver); data must hold length bytes (at most 4096). Returns the number
 * of bytes transferred, or -1 on an error. */
int usb_control(struct usb_device *dev, uint8_t request_type, uint8_t request, uint16_t value,
                uint16_t index, void *data, uint16_t length);

/* Bulk endpoints (implemented by the host controller driver): set up an
 * IN and an OUT endpoint (burst: from the SuperSpeed endpoint companion
 * descriptor, else 0), then transfer length bytes from or to the physically
 * contiguous memory at phys. Returns the bytes transferred (fewer on a
 * short packet), or -1; *stalled: the endpoint refused, recover with
 * usb_bulk_clear_halt. */
bool usb_bulk_configure(struct usb_device *dev, const struct usb_endpoint_descriptor *in,
                        const struct usb_endpoint_descriptor *out, unsigned in_burst,
                        unsigned out_burst);
int usb_bulk_transfer(struct usb_device *dev, uint8_t endpoint, uint64_t phys, uint32_t length,
                      bool *stalled);
/* The same with a time limit of timeout_ms (usb_bulk_transfer's is 5 s); 0
 * waits until the device sends (a network adapter's read, which it keeps
 * pending while it has nothing) or is unplugged. A transfer that runs out
 * of time is cancelled on the controller. Transfers on other endpoints and
 * control transfers go on meanwhile. */
int usb_bulk_transfer_timeout(struct usb_device *dev, uint8_t endpoint, uint64_t phys,
                              uint32_t length, bool *stalled, uint32_t timeout_ms);
bool usb_bulk_clear_halt(struct usb_device *dev, uint8_t endpoint);

bool usb_bulk_stats(struct usb_device *dev, uint8_t endpoint, struct usb_bulk_stats *out);


/* Start receiving on an interrupt IN endpoint (implemented by the host
 * controller driver): transfers stay queued, each completed one is passed to
 * handler. The device must be configured (SET_CONFIGURATION). */
bool usb_interrupt_in(struct usb_device *dev, const struct usb_endpoint_descriptor *endpoint,
                      usb_data_handler handler, void *context);

/* Implemented by the host controller driver, for the hub driver
 * (usbhub.c): a device appeared on port `port` of hub (the port is reset
 * and enabled); give it a slot and an address and read its descriptors
 * (usb_device_added). Logs the line that the caller has begun. */
struct usb_device *usb_attach(struct usb_device *hub, unsigned port, unsigned speed);

/* Tell the controller that dev is a hub with this many ports (and, at high
 * speed, its TT think time and whether it has one TT per port). */
bool usb_hub_set(struct usb_device *dev, unsigned ports, unsigned think_time, bool multi_tt);

/* "  usb0 port 5.1:  ": a device's name as the start of a log line,
 * padded to the log's second column. */
void usb_log_label(const char *name);

/* Called by the controller driver when a device has its address: read its
 * descriptors, log them, add it to the device list and start a driver for
 * it if there is one (keyboards: usbkbd.c). */
void usb_device_added(struct usb_device *dev);

/* Called by the controller driver for an unplugged device, after the
 * controller stopped serving it: its drivers end, it leaves the list. */
void usb_device_removed(struct usb_device *dev);

/* Implemented by the host controller driver, for the hub driver: wake the
 * thread that handles plugging in and out (interrupt context is fine; it
 * then calls usbhub_work), and, from that thread, release a device behind
 * a hub that is gone (and whatever hangs behind it). */
void usb_hotplug_wake(struct usb_device *dev);
void usb_detach(struct usb_device *dev);

int usb_device_count(void);
struct usb_device *usb_device_at(int index);

/* One line per device, as the terminal's usb command shows them. */
void usb_print_devices(void);

const char *usb_speed_name(unsigned speed);

/* (67e) For modules (driver_usb.h): the device's name, product string and
 * descriptor, its configuration whole, a string, its controller's report.
 * usb_select selects its configuration if no driver has (true when it is). */
const char *usb_device_name(const struct usb_device *dev);
const char *usb_device_product(const struct usb_device *dev);
const struct usb_device_descriptor *usb_device_descriptor(const struct usb_device *dev);
int usb_configuration(struct usb_device *dev, uint8_t *buf, uint16_t size);
void usb_string(struct usb_device *dev, uint8_t index, char *out, size_t size);
void usb_report(struct usb_device *dev);
bool usb_select(struct usb_device *dev);

#endif
