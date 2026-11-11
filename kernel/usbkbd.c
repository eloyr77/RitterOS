#include "usbkbd.h"
#include "keyboard.h"
#include "kprintf.h"
#include "string.h"

#define MAX_KEYBOARDS       4
#define HID_SET_IDLE        0x0A
#define HID_SET_PROTOCOL    0x0B
#define HID_BOOT_PROTOCOL   0

#define KEY_ERROR_ROLLOVER  0x01        /* more keys held than a report can list */
#define KEY_A               0x04        /* the first real key */

struct usb_keyboard {
    struct keyboard *k;             /* name, layout, repeat: keyboard.c */
    struct usb_device *dev;
    uint8_t previous[8];            /* the last report */
};

static struct usb_keyboard usb_keyboards[MAX_KEYBOARDS];
static int usb_keyboard_count;

static bool held(const uint8_t *report, uint8_t key)
{
    for (int i = 2; i < 8; i++)
        if (report[i] == key)
            return true;
    return false;
}

/* A report from the keyboard's interrupt endpoint: keys that appear are
 * pressed, keys that disappear released. Interrupt context. */
static void on_report(void *context, const uint8_t *data, uint32_t length)
{
    struct usb_keyboard *u = context;
    uint8_t report[8] = {0};
    if (length < 3)
        return;
    memcpy(report, data, length < 8 ? length : 8);
    keyboard_count_event(u->k);
    if (report[2] == KEY_ERROR_ROLLOVER)
        return;                                     /* says nothing about single keys */
    keyboard_set_modifiers(u->k, report[0]);
    for (int i = 2; i < 8; i++) {
        if (u->previous[i] >= KEY_A && !held(report, u->previous[i]))
            keyboard_key(u->k, u->previous[i], false);
    }
    for (int i = 2; i < 8; i++) {
        if (report[i] >= KEY_A && !held(u->previous, report[i]))
            keyboard_key(u->k, report[i], true);
    }
    memcpy(u->previous, report, sizeof(report));
}

bool usbkbd_attach(struct usb_device *dev, const struct usb_interface_descriptor *interface,
                   const struct usb_endpoint_descriptor *endpoint)
{
    /* A free slot: unplugged keyboards leave theirs. */
    struct usb_keyboard *u = 0;
    for (int i = 0; i < usb_keyboard_count && !u; i++)
        if (!usb_keyboards[i].dev)
            u = &usb_keyboards[i];
    if (!u && usb_keyboard_count < MAX_KEYBOARDS)
        u = &usb_keyboards[usb_keyboard_count++];
    if (!u)
        return false;
    const struct devname_id id = { dev->descriptor.vendor_id, dev->descriptor.product_id,
                                   dev->serial, dev->name };
    struct keyboard *k = keyboard_new("reports", &id);
    if (!k)
        return false;
    memset(u, 0, sizeof(*u));
    u->k = k;
    u->dev = dev;
    char label[16];
    ksnprintf(label, sizeof(label), "%s:", keyboard_name(k));

    /* Boot protocol: the fixed 8-byte reports. Idle rate 0: a report only
     * when something changes (we repeat keys ourselves). Not every keyboard
     * supports SET_IDLE, so its failure does not matter. */
    if (usb_control(dev, USB_TYPE_CLASS_INTERFACE, HID_SET_PROTOCOL, HID_BOOT_PROTOCOL,
                    interface->number, 0, 0) < 0) {
        kprintf("  %-15sSET_PROTOCOL (boot) failed\n", label);
        u->dev = 0;
        keyboard_drop(k);
        return false;
    }
    usb_control(dev, USB_TYPE_CLASS_INTERFACE, HID_SET_IDLE, 0, interface->number, 0, 0);
    if (!usb_interrupt_in(dev, endpoint, on_report, u)) {
        u->dev = 0;
        keyboard_drop(k);
        kprintf("  %-15sreceiving on endpoint 0x%02x failed\n", label, endpoint->address);
        return false;
    }
    kprintf("  %-15sUSB keyboard on %s, boot protocol, reports from endpoint 0x%02x, US layout\n",
            label, dev->name, endpoint->address);
    keyboard_ready(k);
    return true;
}

void usbkbd_detach(struct usb_device *dev)
{
    for (int i = 0; i < usb_keyboard_count; i++) {
        struct usb_keyboard *u = &usb_keyboards[i];
        if (u->dev != dev)
            continue;
        char label[16];
        ksnprintf(label, sizeof(label), "%s:", keyboard_name(u->k));
        kprintf("  %-15sremoved (its name stays reserved)\n", label);
        keyboard_remove(u->k);
        u->dev = 0;
    }
}
