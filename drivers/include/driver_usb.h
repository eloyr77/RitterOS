/*
 * (67e) The USB device class of the driver interface (driver.h): a driver
 * for a USB function (a network adapter, ...) on a device the kernel's USB
 * core has found. The core gives every device its address, reads its
 * descriptors and keeps its own drivers (hubs, keyboards, mice, storage,
 * serial ports); a module declares, with USB_MODULE, the devices or
 * interfaces it drives, by IDs or by class (USB_ANY: any), and is offered
 * each matching device: the ones found before it was loaded and every one
 * plugged in later. Its attach reads the configuration (usb_configuration:
 * the interfaces, their endpoints and class descriptors, as the device
 * sent them), sets its function up and says whether it took the device;
 * detach comes when the device is unplugged (the device's transfers fail
 * from then on). The device's configuration is selected before attach.
 *
 * A struct usb_device is the core's: a driver keeps the pointer and asks
 * through the functions below. The descriptors are the USB 2.0
 * specification's (chapter 9), as the device sends them.
 */
#ifndef RITTEROS_DRIVER_USB_H
#define RITTEROS_DRIVER_USB_H

#include "driver.h"

/* Setup packet fields. */
#define USB_DIR_IN              0x80    /* request_type: device to host */
#define USB_TYPE_CLASS_INTERFACE 0x21   /* request_type: class request to an interface */
#define USB_RECIPIENT_ENDPOINT  0x02    /* request_type: standard request to an endpoint */
#define USB_REQ_CLEAR_FEATURE   1
#define USB_REQ_GET_DESCRIPTOR  6
#define USB_REQ_SET_CONFIGURATION 9
#define USB_FEATURE_HALT        0       /* CLEAR_FEATURE on an endpoint: un-stall it */
#define USB_DESC_DEVICE         1
#define USB_DESC_CONFIGURATION  2
#define USB_DESC_STRING         3
#define USB_DESC_INTERFACE      4
#define USB_DESC_ENDPOINT       5
#define USB_DESC_SS_COMPANION   0x30    /* SuperSpeed endpoint companion */
#define USB_LANG_EN_US          0x0409

struct usb_device_descriptor {
    uint8_t length;
    uint8_t type;                   /* USB_DESC_DEVICE */
    uint16_t usb_version;           /* BCD: 0x0200 = 2.0 */
    uint8_t device_class, device_subclass, device_protocol;
    uint8_t max_packet0;            /* endpoint 0; for USB 3: 2^value */
    uint16_t vendor_id, product_id;
    uint16_t device_version;
    uint8_t manufacturer, product, serial;     /* string indexes, 0 = none */
    uint8_t configurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t length;
    uint8_t type;                   /* USB_DESC_CONFIGURATION */
    uint16_t total_length;          /* with all interface and endpoint descriptors */
    uint8_t interfaces;
    uint8_t value;                  /* for SET_CONFIGURATION */
    uint8_t name;                   /* string index */
    uint8_t attributes;             /* bit 6: self-powered, bit 5: remote wakeup */
    uint8_t max_power;              /* in 2 mA units (USB 3: 8 mA) */
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t length;
    uint8_t type;                   /* USB_DESC_INTERFACE */
    uint8_t number, alternate;
    uint8_t endpoints;
    uint8_t interface_class, interface_subclass, interface_protocol;
    uint8_t name;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t length;
    uint8_t type;                   /* USB_DESC_ENDPOINT */
    uint8_t address;                /* bit 7: IN; bits 3-0: endpoint number */
    uint8_t attributes;             /* bits 1-0: control, isochronous, bulk, interrupt */
    uint16_t max_packet;
    uint8_t interval;
} __attribute__((packed));

/* What a bulk endpoint did so far, for a driver's report: transfers ended
 * (an error among them, the last one's completion code), out of time
 * (cancelled), events of earlier transfers ignored, endpoint resets after a
 * halting error, controller commands among those that failed; the event
 * that ended the last transfer and the last earlier event ignored (the
 * TRB's place in its transfer, -1: not found, of how many, the completion
 * code and what the TRB had left over). */
struct usb_bulk_stats {
    uint64_t transfers, errors, timeouts, cancels, stale, resets, failed_commands;
    uint32_t last_error;
    int32_t end_trb, stale_trb;
    uint32_t end_trbs, end_code, end_residual, stale_trbs, stale_code, stale_residual;
};

/* Called with the data of each completed transfer on an interrupt
 * endpoint, in interrupt context (no console, no heap, no sleeping). */
typedef void (*usb_data_handler)(void *context, const uint8_t *data, uint32_t length);

struct usb_device;

/* A USB module's declaration, at its end (its `.module_usb` section): the
 * devices it drives, each entry matching a device's IDs and any one of
 * its interfaces' class, subclass and protocol (USB_ANY: any value). */
#define MODULE_USB_MAGIC 0x31535553U    /* "SUS1": this layout of module_usb_info */
#define USB_ANY 0xFFFF

struct module_usb {
    uint16_t vendor, product;
    uint16_t interface_class, interface_subclass, interface_protocol;
};

struct module_usb_info {
    uint32_t magic;
    uint16_t api_major, api_minor;      /* the driver interface it was built for */
    char name[16];
    bool (*attach)(struct usb_device *device);
    void (*detach)(struct usb_device *device);
    const struct module_usb *usb;
    uint32_t usb_count;
};

#define USB_MODULE(name, attach, detach, usb_table)                                       \
    __attribute__((section(".module_usb"), used)) static const struct module_usb_info     \
        module_usb_info_ = { MODULE_USB_MAGIC, DRIVER_API_MAJOR, DRIVER_API_MINOR, #name,  \
                             attach, detach, usb_table,                                   \
                             sizeof(usb_table) / sizeof((usb_table)[0]) };

#ifndef RITTEROS_KERNEL

/* The device: its name ("usb0 port 5.1", the start of its log lines with
 * usb_log_label), its product string ("" if none), its device descriptor. */
const char *usb_device_name(const struct usb_device *dev);
const char *usb_device_product(const struct usb_device *dev);
const struct usb_device_descriptor *usb_device_descriptor(const struct usb_device *dev);

/* The selected configuration, whole (its descriptor, then every interface,
 * endpoint and class descriptor), into buf: its length, or -1. */
int usb_configuration(struct usb_device *dev, uint8_t *buf, uint16_t size);

/* String descriptor index as UTF-8 (out has size bytes); "" if none. */
void usb_string(struct usb_device *dev, uint8_t index, char *out, size_t size);

/* A control transfer on endpoint 0; data holds length bytes (at most
 * 4096). The bytes transferred, or -1 on an error. */
int usb_control(struct usb_device *dev, uint8_t request_type, uint8_t request, uint16_t value,
                uint16_t index, void *data, uint16_t length);

/* Bulk endpoints: set up an IN and an OUT endpoint (burst: from the
 * SuperSpeed companion descriptor, else 0), then transfer length bytes
 * from or to the physically contiguous memory at phys: the bytes
 * transferred (fewer on a short packet), or -1; *stalled: the endpoint
 * refused (usb_bulk_clear_halt). usb_bulk_transfer waits at most 5 s; the
 * _timeout form timeout_ms (0: until the device sends or is unplugged, a
 * network adapter's pending read). A transfer out of time is cancelled. */
bool usb_bulk_configure(struct usb_device *dev, const struct usb_endpoint_descriptor *in,
                        const struct usb_endpoint_descriptor *out, unsigned in_burst,
                        unsigned out_burst);
int usb_bulk_transfer(struct usb_device *dev, uint8_t endpoint, uint64_t phys, uint32_t length,
                      bool *stalled);
int usb_bulk_transfer_timeout(struct usb_device *dev, uint8_t endpoint, uint64_t phys,
                              uint32_t length, bool *stalled, uint32_t timeout_ms);
bool usb_bulk_clear_halt(struct usb_device *dev, uint8_t endpoint);
bool usb_bulk_stats(struct usb_device *dev, uint8_t endpoint, struct usb_bulk_stats *out);

/* Keep receiving on an interrupt IN endpoint: each completed transfer is
 * passed to handler (interrupt context). */
bool usb_interrupt_in(struct usb_device *dev, const struct usb_endpoint_descriptor *endpoint,
                      usb_data_handler handler, void *context);

/* "  usb0 port 5.1:  ": a device's name as the start of a log line. */
void usb_log_label(const char *name);

/* The device's host controller's report lines (its interrupts and events),
 * for a driver's own report. */
void usb_report(struct usb_device *dev);

#endif /* RITTEROS_KERNEL */
#endif
