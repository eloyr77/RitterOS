#include "usbmouse.h"
#include "heap.h"
#include "hid.h"
#include "mouse.h"
#include "kprintf.h"
#include "string.h"

#define MAX_MICE            4
#define HID_SET_PROTOCOL    0x0B
#define HID_SET_IDLE        0x0A
#define HID_BOOT_PROTOCOL   0
#define HID_REPORT_PROTOCOL 1
#define HID_GET_REPORT_DESC 0x06        /* GET_DESCRIPTOR, standard, interface */
#define HID_DESC_REPORT     0x22        /* the HID report descriptor's type */
#define HID_REQ_TYPE_IN     0x81        /* device-to-host, standard, recipient interface */
#define DESC_MAX            1024        /* (gaming mice describe several reports) */
#define FIELDS              160

struct usb_mouse {
    struct mouse *m;                /* name, pointer, counters: mouse.c */
    struct usb_device *dev;
    bool report;                    /* (68m-3) report protocol, read through `layout` */
    struct hid_pointer layout;
};

static struct usb_mouse usb_mice[MAX_MICE];
static int usb_mouse_count;

/* A report from the mouse's interrupt endpoint. Interrupt context. */
static void on_report(void *context, const uint8_t *data, uint32_t length)
{
    struct usb_mouse *u = context;
    if (u->report) {                                /* (68m-3) as its descriptor says */
        int dx, dy, wheel, pan;
        uint8_t buttons;
        if (!hid_pointer_read(&u->layout, data, length, &dx, &dy, &wheel, &pan, &buttons))
            return;                                 /* another report (media keys...) */
        mouse_count_event(u->m);
        mouse_event(u->m, dx, dy, wheel, pan, buttons);
        return;
    }
    if (length < 3)
        return;                                     /* not a boot-protocol report */
    mouse_count_event(u->m);
    int dx = (int8_t)data[1];
    int dy = (int8_t)data[2];
    int wheel = length >= 4 ? (int8_t)data[3] : 0;
    mouse_event(u->m, dx, dy, wheel, 0, data[0]);
}

/* (68m-3) The mouse's report descriptor read: where its pointer's fields
 * lie, or false if it does not say (or cannot be read). */
static bool report_layout(struct usb_device *dev, const struct usb_interface_descriptor *interface,
                          struct hid_pointer *layout)
{
    uint8_t *desc = kmalloc(DESC_MAX);
    struct hid_field *fields = kmalloc(FIELDS * sizeof(*fields));
    bool found = false;
    if (desc && fields) {
        int n = usb_control(dev, HID_REQ_TYPE_IN, HID_GET_REPORT_DESC, HID_DESC_REPORT << 8,
                            interface->number, desc, DESC_MAX);
        int count = n > 0 ? hid_parse(desc, (size_t)n, fields, FIELDS) : -1;
        found = count > 0 && hid_pointer_find(fields, count, layout) &&
                layout->button[0].size && layout->bytes <= 64;
    }
    kfree(desc);
    kfree(fields);
    return found;
}

bool usbmouse_attach(struct usb_device *dev, const struct usb_interface_descriptor *interface,
                     const struct usb_endpoint_descriptor *endpoint)
{
    /* A free slot: unplugged mice leave theirs. */
    struct usb_mouse *u = 0;
    for (int i = 0; i < usb_mouse_count && !u; i++)
        if (!usb_mice[i].dev)
            u = &usb_mice[i];
    if (!u && usb_mouse_count < MAX_MICE)
        u = &usb_mice[usb_mouse_count++];
    if (!u)
        return false;
    const struct devname_id id = { dev->descriptor.vendor_id, dev->descriptor.product_id,
                                   dev->serial, dev->name };
    struct mouse *m = mouse_new("reports", &id);
    if (!m)
        return false;
    memset(u, 0, sizeof(*u));
    u->m = m;
    u->dev = dev;
    char label[16];
    ksnprintf(label, sizeof(label), "%s:", mouse_name(m));

    /* (68m-3) Report protocol where the report descriptor says where X, Y
     * and the buttons lie (boot reports have no horizontal wheel); else, or
     * if the mouse refuses it, the boot protocol: the fixed reports. */
    u->report = report_layout(dev, interface, &u->layout) &&
                usb_control(dev, USB_TYPE_CLASS_INTERFACE, HID_SET_PROTOCOL, HID_REPORT_PROTOCOL,
                            interface->number, 0, 0) >= 0;
    /* Idle rate 0: a report only when something changes. Not every mouse
     * supports SET_IDLE, so its failure does not matter. */
    if (!u->report && usb_control(dev, USB_TYPE_CLASS_INTERFACE, HID_SET_PROTOCOL,
                                  HID_BOOT_PROTOCOL, interface->number, 0, 0) < 0) {
        kprintf("  %-15sSET_PROTOCOL (boot) failed\n", label);
        u->dev = 0;
        mouse_drop(m);
        return false;
    }
    usb_control(dev, USB_TYPE_CLASS_INTERFACE, HID_SET_IDLE, 0, interface->number, 0, 0);
    if (!usb_interrupt_in(dev, endpoint, on_report, u)) {
        u->dev = 0;
        mouse_drop(m);
        kprintf("  %-15sreceiving on endpoint 0x%02x failed\n", label, endpoint->address);
        return false;
    }
    kprintf("  %-15sUSB mouse on %s, %s, reports from endpoint 0x%02x\n", label, dev->name,
            !u->report ? "boot protocol" : u->layout.pan.size ? "report protocol, two wheels"
            : u->layout.wheel.size ? "report protocol, a wheel" : "report protocol",
            endpoint->address);
    mouse_ready(m);
    return true;
}

void usbmouse_detach(struct usb_device *dev)
{
    for (int i = 0; i < usb_mouse_count; i++) {
        struct usb_mouse *u = &usb_mice[i];
        if (u->dev != dev)
            continue;
        char label[16];
        ksnprintf(label, sizeof(label), "%s:", mouse_name(u->m));
        kprintf("  %-15sremoved (its name stays reserved)\n", label);
        mouse_remove(u->m);
        u->dev = 0;
    }
}
