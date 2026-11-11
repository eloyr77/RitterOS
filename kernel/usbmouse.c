#include "usbmouse.h"
#include "mouse.h"
#include "kprintf.h"
#include "string.h"

#define MAX_MICE            4
#define HID_SET_PROTOCOL    0x0B
#define HID_SET_IDLE        0x0A
#define HID_BOOT_PROTOCOL   0

struct usb_mouse {
    struct mouse *m;                /* name, pointer, counters: mouse.c */
    struct usb_device *dev;
};

static struct usb_mouse usb_mice[MAX_MICE];
static int usb_mouse_count;

/* A report from the mouse's interrupt endpoint. Interrupt context. */
static void on_report(void *context, const uint8_t *data, uint32_t length)
{
    struct usb_mouse *u = context;
    if (length < 3)
        return;                                     /* not a boot-protocol report */
    mouse_count_event(u->m);
    int dx = (int8_t)data[1];
    int dy = (int8_t)data[2];
    int wheel = length >= 4 ? (int8_t)data[3] : 0;
    mouse_event(u->m, dx, dy, wheel, data[0]);
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

    /* Boot protocol: the fixed reports. Idle rate 0: a report only when
     * something changes. Not every mouse supports SET_IDLE, so its failure
     * does not matter. */
    if (usb_control(dev, USB_TYPE_CLASS_INTERFACE, HID_SET_PROTOCOL, HID_BOOT_PROTOCOL,
                    interface->number, 0, 0) < 0) {
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
    kprintf("  %-15sUSB mouse on %s, boot protocol, reports from endpoint 0x%02x\n",
            label, dev->name, endpoint->address);
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
