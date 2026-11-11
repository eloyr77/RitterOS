#include "usbtablet.h"
#include "mouse.h"
#include "kprintf.h"
#include "string.h"

#define MAX_TABLETS         4
#define HID_SET_IDLE        0x0A
#define HID_GET_REPORT_DESC 0x06        /* GET_DESCRIPTOR, standard, interface */
#define HID_DESC_REPORT     0x22        /* the HID report descriptor's type */
#define HID_REQ_TYPE_IN     0x81        /* device-to-host, standard, recipient interface */

/* Where the fields of this device's input report lie, from its HID report
 * descriptor. Bit offsets are from the start of the report (no report ID is
 * supported: `bad` is set if one is declared). */
struct layout {
    int bad;
    int have_x, x_bit, x_size, x_max;
    int have_y, y_bit, y_size, y_max;
    int have_wheel, wheel_bit, wheel_size;
    int btn_bit, btn_count;
    int total_bits;
};

struct usb_tablet {
    struct mouse *m;
    struct usb_device *dev;
    struct layout layout;
};

static struct usb_tablet tablets[MAX_TABLETS];
static int tablet_count;

/* Extract `size` bits (<= 32) at bit offset `bit`, little-endian. */
static uint32_t get_bits(const uint8_t *data, int bit, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        int b = bit + i;
        if (data[b >> 3] & (1u << (b & 7)))
            v |= 1u << i;
    }
    return v;
}

static int sign_extend(uint32_t v, int bits)
{
    if (bits < 32 && (v & (1u << (bits - 1))))
        return (int)(v | ~((1u << bits) - 1));
    return (int)v;
}

/* Walk a HID report descriptor and record where an absolute X/Y pointer, its
 * buttons and wheel lie. Returns true if it is an absolute pointer we can read.
 * Only the items we need are handled; a report ID makes it unsupported. */
static bool parse_report_descriptor(const uint8_t *d, int len, struct layout *p)
{
    memset(p, 0, sizeof(*p));
    int usage_page = 0, logical_max = 0, report_size = 0, report_count = 0;
    int usages[16], usage_count = 0, usage_min = -1, usage_max = -1;
    int cur_bit = 0;

    for (int i = 0; i < len;) {
        uint8_t b = d[i++];
        if (b == 0xFE) {                        /* long item: skip it */
            if (i < len)
                i += 2 + d[i];
            continue;
        }
        int bsize = b & 0x03;
        if (bsize == 3)
            bsize = 4;
        int btype = (b >> 2) & 0x03, btag = (b >> 4) & 0x0F;
        uint32_t data = 0;
        for (int k = 0; k < bsize && i < len; k++)
            data |= (uint32_t)d[i++] << (8 * k);

        if (btype == 1) {                       /* global */
            switch (btag) {
            case 0x0: usage_page = (int)data; break;
            case 0x2: logical_max = (int)data; break;
            case 0x7: report_size = (int)data; break;
            case 0x8: if (data != 0) p->bad = 1; break;     /* report ID: unsupported */
            case 0x9: report_count = (int)data; break;
            default: break;
            }
        } else if (btype == 2) {                /* local */
            switch (btag) {
            case 0x0: if (usage_count < 16) usages[usage_count++] = (int)data; break;
            case 0x1: usage_min = (int)data; break;
            case 0x2: usage_max = (int)data; break;
            default: break;
            }
        } else if (btype == 0) {                /* main */
            if (btag == 0x8 && report_size > 0) {           /* Input */
                int is_const = (int)data & 0x01, is_rel = (int)data & 0x04;
                for (int f = 0; f < report_count; f++) {
                    int usage;
                    if (usage_min >= 0 && usage_max >= usage_min)
                        usage = usage_min + f <= usage_max ? usage_min + f : usage_max;
                    else if (f < usage_count)
                        usage = usages[f];
                    else
                        usage = usage_count > 0 ? usages[usage_count - 1] : 0;
                    int bit = cur_bit + f * report_size;
                    if (usage_page == 0x01 && !is_rel) {    /* Generic Desktop, absolute */
                        if (usage == 0x30) {
                            p->have_x = 1; p->x_bit = bit; p->x_size = report_size;
                            p->x_max = logical_max;
                        } else if (usage == 0x31) {
                            p->have_y = 1; p->y_bit = bit; p->y_size = report_size;
                            p->y_max = logical_max;
                        }
                    }
                    if (usage_page == 0x01 && usage == 0x38) {      /* wheel (relative) */
                        p->have_wheel = 1; p->wheel_bit = bit; p->wheel_size = report_size;
                    }
                    if (usage_page == 0x09 && !is_const && f == 0) {    /* buttons */
                        p->btn_bit = bit; p->btn_count = report_count;
                    }
                }
                cur_bit += report_size * report_count;
            }
            usage_count = 0;                     /* any main item clears the locals */
            usage_min = usage_max = -1;
        }
    }
    p->total_bits = cur_bit;
    return !p->bad && p->have_x && p->have_y && p->x_max > 0 && p->y_max > 0 &&
           p->x_size <= 16 && p->y_size <= 16;
}

/* A report from the interrupt endpoint. Interrupt context. */
static void on_report(void *context, const uint8_t *data, uint32_t length)
{
    struct usb_tablet *u = context;
    struct layout *p = &u->layout;
    if ((int)(length * 8) < p->total_bits)
        return;                                 /* short report */
    mouse_count_event(u->m);
    int x = (int)get_bits(data, p->x_bit, p->x_size);
    int y = (int)get_bits(data, p->y_bit, p->y_size);
    uint8_t buttons = (uint8_t)get_bits(data, p->btn_bit, p->btn_count);
    int wheel = p->have_wheel ? sign_extend(get_bits(data, p->wheel_bit, p->wheel_size),
                                            p->wheel_size) : 0;
    mouse_absolute(u->m, x, y, p->x_max, p->y_max, wheel, buttons);
}

bool usbtablet_attach(struct usb_device *dev, const struct usb_interface_descriptor *interface,
                      const struct usb_endpoint_descriptor *endpoint)
{
    uint8_t desc[256];
    int n = usb_control(dev, HID_REQ_TYPE_IN, HID_GET_REPORT_DESC, HID_DESC_REPORT << 8,
                        interface->number, desc, sizeof(desc));
    struct layout layout;
    if (n <= 0 || !parse_report_descriptor(desc, n, &layout))
        return false;                           /* not an absolute pointer: leave it */

    struct usb_tablet *u = 0;
    for (int i = 0; i < tablet_count && !u; i++)
        if (!tablets[i].dev)
            u = &tablets[i];
    if (!u && tablet_count < MAX_TABLETS)
        u = &tablets[tablet_count++];
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
    u->layout = layout;
    char label[16];
    ksnprintf(label, sizeof(label), "%s:", mouse_name(m));

    usb_control(dev, USB_TYPE_CLASS_INTERFACE, HID_SET_IDLE, 0, interface->number, 0, 0);
    if (!usb_interrupt_in(dev, endpoint, on_report, u)) {
        u->dev = 0;
        mouse_drop(m);
        kprintf("  %-15sreceiving on endpoint 0x%02x failed\n", label, endpoint->address);
        return false;
    }
    kprintf("  %-15sUSB tablet on %s, absolute pointer, reports from endpoint 0x%02x\n",
            label, dev->name, endpoint->address);
    mouse_ready(m);
    return true;
}

void usbtablet_detach(struct usb_device *dev)
{
    for (int i = 0; i < tablet_count; i++) {
        struct usb_tablet *u = &tablets[i];
        if (u->dev != dev)
            continue;
        char label[16];
        ksnprintf(label, sizeof(label), "%s:", mouse_name(u->m));
        kprintf("  %-15sremoved (its name stays reserved)\n", label);
        mouse_remove(u->m);
        u->dev = 0;
    }
}
