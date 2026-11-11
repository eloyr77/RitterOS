#include "usb.h"
#include "heap.h"
#include "kprintf.h"
#include "module.h"
#include "string.h"
#include "sync.h"
#include "usbhub.h"
#include "usbkbd.h"
#include "usbmouse.h"
#include "usbserial.h"
#include "usbstorage.h"
#include "usbtablet.h"
#include "xhci.h"

#define MAX_DEVICES 16

/* The devices plugged in now. The usb thread changes the list (hot plug)
 * while others read it (the usb command). */
static struct usb_device *devices[MAX_DEVICES];
static int device_count;
static struct mutex devices_lock = MUTEX_INIT("usb devices");

int usb_device_count(void)
{
    return device_count;
}

struct usb_device *usb_device_at(int index)
{
    return index >= 0 && index < device_count ? devices[index] : 0;
}

const char *usb_speed_name(unsigned speed)
{
    switch (speed) {
    case USB_SPEED_FULL:       return "full speed (12 Mbit/s)";
    case USB_SPEED_LOW:        return "low speed (1.5 Mbit/s)";
    case USB_SPEED_HIGH:       return "high speed (480 Mbit/s)";
    case USB_SPEED_SUPER:      return "SuperSpeed (5 Gbit/s)";
    case USB_SPEED_SUPER_PLUS: return "SuperSpeed+ (10 Gbit/s)";
    default:                   return "unknown speed";
    }
}

static int get_descriptor(struct usb_device *dev, uint8_t type, uint8_t index, uint16_t language,
                          void *buf, uint16_t length)
{
    return usb_control(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, (uint16_t)(type << 8 | index),
                       language, buf, length);
}

/* Append a code point to out as UTF-8 (the console's encoding). */
static size_t put_utf8(char *out, size_t pos, size_t size, uint32_t cp)
{
    uint8_t bytes[3];
    size_t n;
    if (cp < 0x80) {
        bytes[0] = (uint8_t)cp;
        n = 1;
    } else if (cp < 0x800) {
        bytes[0] = (uint8_t)(0xC0 | cp >> 6);
        bytes[1] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 2;
    } else {
        bytes[0] = (uint8_t)(0xE0 | cp >> 12);
        bytes[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
        bytes[2] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 3;
    }
    if (pos + n >= size)
        return pos;
    memcpy(out + pos, bytes, n);
    return pos + n;
}

/* String descriptors hold UTF-16 text (without a terminating zero) in the
 * device's languages; string 0 lists the languages. English (US) if the
 * device has it, else its first language. */
static void get_string(struct usb_device *dev, uint8_t index, char *out, size_t size)
{
    uint8_t buf[256];
    out[0] = 0;
    if (!index)
        return;
    int n = get_descriptor(dev, USB_DESC_STRING, 0, 0, buf, sizeof(buf));
    if (n < 4 || buf[1] != USB_DESC_STRING)
        return;
    uint16_t language = (uint16_t)(buf[2] | buf[3] << 8);
    for (int i = 2; i + 1 < n && i + 1 < buf[0]; i += 2) {
        if ((buf[i] | buf[i + 1] << 8) == USB_LANG_EN_US)
            language = USB_LANG_EN_US;
    }
    n = get_descriptor(dev, USB_DESC_STRING, index, language, buf, sizeof(buf));
    if (n < 2 || buf[1] != USB_DESC_STRING)
        return;
    if (n > buf[0])
        n = buf[0];
    size_t pos = 0;
    for (int i = 2; i + 1 < n; i += 2) {
        uint32_t cp = (uint32_t)(buf[i] | buf[i + 1] << 8);
        if (cp >= 0xD800 && cp <= 0xDFFF)
            cp = '?';                       /* outside the BMP: not in our font anyway */
        else if (cp < 0x20)
            continue;
        pos = put_utf8(out, pos, size, cp);
    }
    out[pos] = 0;
}

static const char *class_name(uint8_t c)
{
    switch (c) {
    case 0x00: return "see interfaces";
    case 0x01: return "audio";
    case 0x02: return "communications";
    case 0x03: return "HID";
    case 0x06: return "image";
    case 0x07: return "printer";
    case 0x08: return "mass storage";
    case 0x09: return "hub";
    case 0x0A: return "CDC data";
    case 0x0B: return "smart card";
    case 0x0E: return "video";
    case 0xE0: return "wireless";
    case 0xEF: return "miscellaneous";
    case 0xFF: return "vendor-specific";
    default:   return "other";
    }
}

/* Interface class, subclass and protocol in words, e.g. "HID keyboard". */
static void describe_interface(const struct usb_interface_descriptor *i, char *out, size_t size)
{
    if (i->interface_class == 0x03) {
        const char *what = i->interface_protocol == 1 ? "keyboard"
                         : i->interface_protocol == 2 ? "mouse" : "device";
        ksnprintf(out, size, "HID %s%s", what,
                  i->interface_subclass == 1 ? " (boot interface)" : "");
    } else if (i->interface_class == 0x08) {
        ksnprintf(out, size, "mass storage%s",
                  i->interface_protocol == 0x50 ? " (bulk-only)" : "");
    } else {
        ksnprintf(out, size, "%s", class_name(i->interface_class));
    }
}

/* How often the host polls an interrupt endpoint: in frames of 1 ms at full
 * and low speed, in 2^(interval-1) microframes of 125 us from high speed on. */
static uint32_t interval_us(const struct usb_device *dev, uint8_t interval)
{
    if (dev->speed == USB_SPEED_FULL || dev->speed == USB_SPEED_LOW)
        return interval * 1000u;
    if (interval < 1 || interval > 16)
        return 0;
    return 125u << (interval - 1);
}

static const char *const transfer_types[4] = { "control", "isochronous", "bulk", "interrupt" };

/* The parts of a configuration a driver needs: so far, a HID boot keyboard
 * interface and its interrupt IN endpoint. */
struct found {
    struct usb_interface_descriptor keyboard;
    struct usb_endpoint_descriptor keyboard_in;
    bool has_keyboard, in_keyboard;
    struct usb_interface_descriptor mouse;      /* a HID boot mouse interface */
    struct usb_endpoint_descriptor mouse_in;
    bool has_mouse, in_mouse;
    struct usb_interface_descriptor tablet;     /* a generic HID interface (an absolute pointer?) */
    struct usb_endpoint_descriptor tablet_in;
    bool has_tablet, in_tablet;
    bool hub;                       /* a hub interface (class 09) */
    bool in_hub, has_hub_in;        /* and its status change endpoint */
    struct usb_endpoint_descriptor hub_in;
    /* A mass storage interface: SCSI commands (subclass 06), bulk-only
     * transport (protocol 0x50), with a bulk IN and a bulk OUT endpoint. */
    struct usb_interface_descriptor storage;
    struct usb_endpoint_descriptor storage_in, storage_out;
    unsigned storage_in_burst, storage_out_burst;
    bool in_storage, has_storage_in, has_storage_out;
    /* A CDC ACM serial port (step 50e): a Communications interface of
     * subclass 02 and the CDC Data interface its Union names, with a bulk
     * IN and OUT. */
    bool in_acm, in_acm_data, has_acm, has_acm_in, has_acm_out;
    uint8_t acm_control_iface, acm_data_iface;
    struct usb_endpoint_descriptor acm_in, acm_out;
    unsigned acm_in_burst, acm_out_burst;
    struct usb_endpoint_descriptor *last_endpoint;  /* for a following companion */
    unsigned *last_burst;
};

static void read_configuration(struct usb_device *dev, struct found *found, uint8_t *config)
{
    struct usb_config_descriptor head;
    if (get_descriptor(dev, USB_DESC_CONFIGURATION, 0, 0, &head, sizeof(head)) < (int)sizeof(head) ||
        head.type != USB_DESC_CONFIGURATION || head.total_length < sizeof(head)) {
        kprintf("                 configuration descriptor: unreadable\n");
        return;
    }
    uint16_t total = head.total_length > 4096 ? 4096 : head.total_length;
    uint8_t *buf = kmalloc(total);
    int n = buf ? get_descriptor(dev, USB_DESC_CONFIGURATION, 0, 0, buf, total) : -1;
    if (n < (int)sizeof(head)) {
        kprintf("                 configuration descriptor: unreadable\n");
        kfree(buf);
        return;
    }
    char name[64];
    get_string(dev, head.name, name, sizeof(name));
    *config = head.value;
    unsigned power_unit = dev->speed >= USB_SPEED_SUPER ? 8 : 2;
    kprintf("                 configuration %u of %u%s%s%s: %u interface(s), %s %u mA%s\n",
            head.value, dev->descriptor.configurations, name[0] ? " '" : "", name,
            name[0] ? "'" : "", head.interfaces,
            head.attributes & 0x40 ? "self-powered," : "bus-powered,",
            head.max_power * power_unit, head.attributes & 0x20 ? ", remote wakeup" : "");

    /* The descriptors follow each other, each starting with its length and
     * type: interfaces, each followed by its endpoints (and class-specific
     * descriptors such as HID's, which we skip here). */
    for (int pos = head.length; pos + 2 <= n && buf[pos] >= 2; pos += buf[pos]) {
        if (pos + buf[pos] > n)
            break;
        if (buf[pos + 1] == USB_DESC_INTERFACE && buf[pos] >= sizeof(struct usb_interface_descriptor)) {
            struct usb_interface_descriptor i;
            memcpy(&i, buf + pos, sizeof(i));
            char what[40];
            describe_interface(&i, what, sizeof(what));
            if (!dev->function[0])
                memcpy(dev->function, what, sizeof(dev->function));
            found->in_keyboard = !found->has_keyboard && i.interface_class == 0x03 &&
                                 i.interface_subclass == 1 && i.interface_protocol == 1;
            if (found->in_keyboard)
                found->keyboard = i;
            found->in_mouse = !found->has_mouse && i.interface_class == 0x03 &&
                              i.interface_subclass == 1 && i.interface_protocol == 2;
            if (found->in_mouse)
                found->mouse = i;
            /* A generic HID interface (not a boot keyboard or mouse): maybe an
             * absolute pointer (a tablet). usbtablet_attach reads its report
             * descriptor to be sure. */
            found->in_tablet = !found->has_tablet && i.interface_class == 0x03 &&
                               i.interface_subclass == 0 && i.interface_protocol == 0;
            if (found->in_tablet)
                found->tablet = i;
            found->hub |= i.interface_class == 0x09;
            found->in_hub = i.interface_class == 0x09;
            found->in_storage = !found->has_storage_in && i.interface_class == 0x08 &&
                                i.interface_subclass == 0x06 && i.interface_protocol == 0x50;
            if (found->in_storage)
                found->storage = i;
            /* (67e) A CDC-NCM network adapter is the usbnet module's: it
             * reads the configuration itself (usb_configuration). A serial
             * port's data interface is the one its control interface's
             * Union descriptor names (until it is read: the next one). */
            found->in_acm = !found->has_acm && i.interface_class == 0x02 &&
                            i.interface_subclass == 0x02;
            if (found->in_acm) {
                found->has_acm = true;
                found->acm_control_iface = i.number;
                found->acm_data_iface = i.number + 1;
            }
            found->in_acm_data = found->has_acm && i.interface_class == 0x0a &&
                                 i.number == found->acm_data_iface;
            kprintf("                 interface %u: %s, class %02x.%02x.%02x, %u endpoint(s)\n",
                    i.number, what, i.interface_class, i.interface_subclass,
                    i.interface_protocol, i.endpoints);
        } else if (buf[pos + 1] == USB_DESC_SS_COMPANION && buf[pos] >= 3) {
            /* SuperSpeed: how many packets the endpoint sends in a burst. */
            if (found->last_burst)
                *found->last_burst = buf[pos + 2];
            kprintf("                     SuperSpeed companion: bursts of %u packets\n",
                    buf[pos + 2] + 1u);
        } else if (buf[pos + 1] == USB_DESC_ENDPOINT &&
                   buf[pos] >= sizeof(struct usb_endpoint_descriptor)) {
            struct usb_endpoint_descriptor e;
            memcpy(&e, buf + pos, sizeof(e));
            unsigned type = e.attributes & 3;
            if (found->in_keyboard && type == 3 && (e.address & 0x80) && !found->has_keyboard) {
                found->keyboard_in = e;
                found->has_keyboard = true;
            }
            if (found->in_mouse && type == 3 && (e.address & 0x80) && !found->has_mouse) {
                found->mouse_in = e;
                found->has_mouse = true;
            }
            if (found->in_tablet && type == 3 && (e.address & 0x80) && !found->has_tablet) {
                found->tablet_in = e;
                found->has_tablet = true;
            }
            if (found->in_hub && type == 3 && (e.address & 0x80) && !found->has_hub_in) {
                found->hub_in = e;
                found->has_hub_in = true;
            }
            found->last_burst = 0;
            if (found->in_storage && type == 2) {           /* bulk */
                bool in = e.address & 0x80;
                if (in && !found->has_storage_in) {
                    found->storage_in = e;
                    found->has_storage_in = true;
                    found->last_burst = &found->storage_in_burst;
                } else if (!in && !found->has_storage_out) {
                    found->storage_out = e;
                    found->has_storage_out = true;
                    found->last_burst = &found->storage_out_burst;
                }
            }
            if (found->in_acm_data && type == 2) {          /* the serial port's bulk */
                bool in = e.address & 0x80;
                if (in && !found->has_acm_in) {
                    found->acm_in = e;
                    found->has_acm_in = true;
                    found->last_burst = &found->acm_in_burst;
                } else if (!in && !found->has_acm_out) {
                    found->acm_out = e;
                    found->has_acm_out = true;
                    found->last_burst = &found->acm_out_burst;
                }
            }
            kprintf("                   endpoint 0x%02x: %s %s, max packet %u bytes", e.address,
                    transfer_types[type], e.address & 0x80 ? "IN" : "OUT", e.max_packet & 0x7FF);
            if (type == 3) {
                uint32_t us = interval_us(dev, e.interval);
                if (us % 1000 == 0)
                    kprintf(", every %u ms", us / 1000);
                else
                    kprintf(", every %u us", us);
            }
            kprintf("\n");
        } else if (buf[pos + 1] == 0x24 && buf[pos] >= 5 && found->in_acm && buf[pos + 2] == 0x06) {
            found->acm_data_iface = buf[pos + 4];
        }
    }
    kfree(buf);
}

void usb_device_added(struct usb_device *dev)
{
    struct usb_device_descriptor *d = &dev->descriptor;
    if (get_descriptor(dev, USB_DESC_DEVICE, 0, 0, d, sizeof(*d)) < (int)sizeof(*d) ||
        d->type != USB_DESC_DEVICE) {
        kprintf("                 device descriptor: unreadable\n");
        return;
    }
    get_string(dev, d->manufacturer, dev->manufacturer, sizeof(dev->manufacturer));
    get_string(dev, d->product, dev->product, sizeof(dev->product));
    get_string(dev, d->serial, dev->serial, sizeof(dev->serial));
    kprintf("                 device %04x:%04x, USB %x.%02x, class %02x (%s), version %x.%02x\n",
            d->vendor_id, d->product_id, d->usb_version >> 8, d->usb_version & 0xFF,
            d->device_class, class_name(d->device_class), d->device_version >> 8,
            d->device_version & 0xFF);
    kprintf("                 '%s' '%s', serial '%s'\n", dev->manufacturer, dev->product,
            dev->serial);
    struct found found = {0};
    uint8_t config = 0;
    read_configuration(dev, &found, &config);
    dev->config = config;
    mutex_lock(&devices_lock);
    if (device_count < MAX_DEVICES)
        devices[device_count++] = dev;
    mutex_unlock(&devices_lock);

    /* Drivers: a device is used once a configuration is selected. */
    if (found.hub || d->device_class == 0x09) {
        if (!usb_select(dev))
            kprintf("                 SET_CONFIGURATION %u failed\n", config);
        else
            usbhub_attach(dev, found.has_hub_in ? &found.hub_in : 0);
    } else if ((found.has_storage_in && found.has_storage_out) ||
               (found.has_acm && found.has_acm_in && found.has_acm_out)) {
        /* Storage, a serial port, or both: a composite device (a PlutoSDR
         * has storage, a serial port, NCM and its own interface) gets a
         * driver for each function we know (NCM: the usbnet module's). */
        if (!usb_select(dev)) {
            kprintf("                 SET_CONFIGURATION %u failed\n", config);
        } else {
            if (found.has_storage_in && found.has_storage_out)
                usbstorage_attach(dev, &found.storage, &found.storage_in, &found.storage_out,
                                  found.storage_in_burst, found.storage_out_burst);
            if (found.has_acm && found.has_acm_in && found.has_acm_out)
                usbserial_attach(dev, found.acm_control_iface, &found.acm_in, &found.acm_out,
                                 found.acm_in_burst, found.acm_out_burst);
        }
    } else if (found.has_keyboard) {
        if (!usb_select(dev))
            kprintf("                 SET_CONFIGURATION %u failed\n", config);
        else
            usbkbd_attach(dev, &found.keyboard, &found.keyboard_in);
    } else if (found.has_mouse) {
        if (!usb_select(dev))
            kprintf("                 SET_CONFIGURATION %u failed\n", config);
        else
            usbmouse_attach(dev, &found.mouse, &found.mouse_in);
    } else if (found.has_tablet) {
        if (!usb_select(dev))
            kprintf("                 SET_CONFIGURATION %u failed\n", config);
        else
            usbtablet_attach(dev, &found.tablet, &found.tablet_in);
    }
    modules_usb_offer(dev);                 /* (67e) the USB modules' turn */
}

void usb_log_label(const char *name)
{
    char label[40];
    int n = ksnprintf(label, sizeof(label), "%s:", name);
    kprintf("  %-15s%s", label, n >= 15 ? " " : "");
}

void usb_device_removed(struct usb_device *dev)
{
    modules_usb_removed(dev);               /* (67e) */
    usbkbd_detach(dev);
    usbmouse_detach(dev);
    usbtablet_detach(dev);
    usbstorage_detach(dev);
    usbserial_detach(dev);
    usbhub_detach(dev);
    mutex_lock(&devices_lock);
    for (int i = 0; i < device_count; i++) {
        if (devices[i] == dev) {
            for (int j = i; j + 1 < device_count; j++)
                devices[j] = devices[j + 1];        /* keep the order of plugging in */
            device_count--;
            break;
        }
    }
    mutex_unlock(&devices_lock);
}

void usb_print_devices(void)
{
    mutex_lock(&devices_lock);
    kprintf("%d USB device(s):\n", device_count);
    for (int i = 0; i < device_count; i++) {
        const struct usb_device *dev = devices[i];
        kprintf("    %-16s %04x:%04x  %-24s %s, '%s' '%s'\n", dev->name,
                dev->descriptor.vendor_id, dev->descriptor.product_id,
                usb_speed_name(dev->speed), dev->function[0] ? dev->function : "-",
                dev->manufacturer, dev->product);
    }
    /* (37g-8) Each controller's counters, as at the boot: interrupts lost
     * after a sleep show as events found by the tick. */
    for (int i = 0; i < device_count; i++) {
        bool first = true;
        for (int j = 0; j < i && first; j++)
            first = devices[j]->hc != devices[i]->hc;
        if (first)
            xhci_report(devices[i]->hc);
    }
    mutex_unlock(&devices_lock);
}

/* --- (67e) for modules (driver_usb.h) --- */

const char *usb_device_name(const struct usb_device *dev)
{
    return dev->name;
}

const char *usb_device_product(const struct usb_device *dev)
{
    return dev->product;
}

const struct usb_device_descriptor *usb_device_descriptor(const struct usb_device *dev)
{
    return &dev->descriptor;
}

int usb_configuration(struct usb_device *dev, uint8_t *buf, uint16_t size)
{
    struct usb_config_descriptor head;
    if (get_descriptor(dev, USB_DESC_CONFIGURATION, 0, 0, &head, sizeof(head)) < (int)sizeof(head) ||
        head.type != USB_DESC_CONFIGURATION)
        return -1;
    return get_descriptor(dev, USB_DESC_CONFIGURATION, 0, 0, buf,
                          head.total_length < size ? head.total_length : size);
}

void usb_string(struct usb_device *dev, uint8_t index, char *out, size_t size)
{
    get_string(dev, index, out, size);
}

void usb_report(struct usb_device *dev)
{
    xhci_report(dev->hc);
}

bool usb_select(struct usb_device *dev)
{
    if (dev->configured)
        return true;
    if (usb_control(dev, 0, USB_REQ_SET_CONFIGURATION, dev->config, 0, 0, 0) < 0)
        return false;
    dev->configured = true;
    return true;
}
