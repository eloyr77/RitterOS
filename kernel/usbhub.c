#include "usbhub.h"
#include "arch.h"
#include "kprintf.h"
#include "sync.h"
#include "timer.h"

#define MAX_DEPTH           5               /* hubs in a chain (USB allows 5) */
#define MAX_PORTS           15
#define MAX_HUBS            8

/* Requests to a hub: class requests to the hub itself or to one of its ports. */
#define TO_HUB              0x20            /* class, device, host to device */
#define FROM_HUB            0xA0            /* class, device, device to host */
#define TO_PORT             0x23            /* class, "other" (a port) */
#define FROM_PORT           0xA3
#define REQ_GET_STATUS      0
#define REQ_CLEAR_FEATURE   1
#define REQ_SET_FEATURE     3
#define REQ_GET_DESCRIPTOR  6
#define REQ_SET_HUB_DEPTH   12              /* USB 3 hubs */
#define DESC_HUB            0x29
#define DESC_SS_HUB         0x2A

/* Port features (wValue) and the bits of a port's status. */
#define PORT_RESET          4
#define PORT_POWER          8
#define C_PORT_CONNECTION   16
#define C_PORT_ENABLE       17
#define C_PORT_OVER_CURRENT 19
#define C_PORT_RESET        20
#define STATUS_CONNECTION   (1u << 0)
#define STATUS_ENABLE       (1u << 1)
#define STATUS_LOW_SPEED    (1u << 9)       /* USB 2 hubs */
#define STATUS_HIGH_SPEED   (1u << 10)
#define CHANGE_CONNECTION   (1u << 0)
#define CHANGE_ENABLE       (1u << 1)
#define CHANGE_OVER_CURRENT (1u << 3)
#define CHANGE_RESET        (1u << 4)

/* Hubs listening on their status change endpoint: a bit per port that
 * reported a change (bit 0: the hub itself), set in interrupt context,
 * taken by the usb thread (usbhub_work). */
static struct hub {
    struct usb_device *dev;         /* 0: free */
    volatile uint32_t changed;
} hubs[MAX_HUBS];

/* (65a-4) The hubs' slots and their changed bits: the status change
 * handler (inside the controller's event lock) sets them, the usb thread
 * takes them. Until 65a-4 interrupts off did this. */
static struct spinlock hubs_lock = SPINLOCK_INIT("usb hubs");

static int port_request(struct usb_device *hub, uint8_t request, uint16_t feature, unsigned port)
{
    return usb_control(hub, TO_PORT, request, feature, (uint16_t)port, 0, 0);
}

/* wPortStatus and wPortChange, or false. */
static bool port_status(struct usb_device *hub, unsigned port, uint16_t *status, uint16_t *change)
{
    uint8_t buf[4];
    if (usb_control(hub, FROM_PORT, REQ_GET_STATUS, 0, (uint16_t)port, buf, 4) != 4)
        return false;
    *status = (uint16_t)(buf[0] | buf[1] << 8);
    *change = (uint16_t)(buf[2] | buf[3] << 8);
    return true;
}

/* Reset a USB 2 hub's port and wait until it is done (C_PORT_RESET). */
static bool reset_port(struct usb_device *hub, unsigned port)
{
    uint16_t status, change;
    if (port_request(hub, REQ_SET_FEATURE, PORT_RESET, port) < 0)
        return false;
    for (int ms = 0; ms < 500; ms += 10) {
        timer_sleep_ms(10);
        if (!port_status(hub, port, &status, &change))
            return false;
        if (change & CHANGE_RESET)
            break;
    }
    port_request(hub, REQ_CLEAR_FEATURE, C_PORT_RESET, port);
    timer_sleep_ms(10);                     /* reset recovery (USB 2.0, 7.1.7.5) */
    return port_status(hub, port, &status, &change) && (status & STATUS_ENABLE);
}

/* A device is connected to hub port `port`: reset the port (USB 2), learn
 * the speed and attach the device. */
static void attach_port(struct usb_device *hub, unsigned port)
{
    bool super = hub->speed >= USB_SPEED_SUPER;
    uint16_t status, change;
    char name[sizeof(hub->name)];
    ksnprintf(name, sizeof(name), "%s.%u", hub->name, port);
    usb_log_label(name);
    kprintf("device connected (hub port %u)", port);
    /* USB 3 ports connect by themselves; USB 2 ports need a reset, which
     * also tells the speed. */
    if (!super && !reset_port(hub, port)) {
        kprintf(", port reset failed\n");
        return;
    }
    port_status(hub, port, &status, &change);
    unsigned speed = super ? USB_SPEED_SUPER
                   : status & STATUS_LOW_SPEED ? USB_SPEED_LOW
                   : status & STATUS_HIGH_SPEED ? USB_SPEED_HIGH : USB_SPEED_FULL;
    kprintf("%s, %s\n", super ? "" : ", port reset", usb_speed_name(speed));
    usb_attach(hub, port, speed);
}

/* The hub's status change endpoint: which ports changed. Interrupt context:
 * note them and wake the usb thread. */
static void on_change(void *context, const uint8_t *data, uint32_t length)
{
    struct hub *h = context;
    uint32_t bits = 0;
    for (uint32_t i = 0; i < length && i < 4; i++)
        bits |= (uint32_t)data[i] << (8 * i);
    uint64_t state = spin_lock(&hubs_lock);
    struct usb_device *dev = h->dev;
    if (bits && dev)
        h->changed |= bits;
    spin_unlock(&hubs_lock, state);
    if (bits && dev)
        usb_hotplug_wake(dev);
}

bool usbhub_attach(struct usb_device *hub, const struct usb_endpoint_descriptor *in)
{
    bool super = hub->speed >= USB_SPEED_SUPER;
    if (hub->depth >= MAX_DEPTH) {
        kprintf("                 hub: too deep (more than %u hubs in a chain), not used\n",
                MAX_DEPTH);
        return false;
    }
    /* A USB 3 hub needs to know its level for the route string. */
    if (super && usb_control(hub, TO_HUB, REQ_SET_HUB_DEPTH, hub->depth, 0, 0, 0) < 0) {
        kprintf("                 hub: SET_HUB_DEPTH failed\n");
        return false;
    }
    /* The hub descriptor: ports, power switching, time to power good, and
     * at high speed the TT's think time (bits 6-5 of the characteristics). */
    uint8_t desc[16];
    int n = usb_control(hub, FROM_HUB, REQ_GET_DESCRIPTOR, (super ? DESC_SS_HUB : DESC_HUB) << 8,
                        0, desc, sizeof(desc));
    if (n < 7) {
        kprintf("                 hub: descriptor unreadable\n");
        return false;
    }
    unsigned ports = desc[2] < MAX_PORTS ? desc[2] : MAX_PORTS;
    uint16_t characteristics = (uint16_t)(desc[3] | desc[4] << 8);
    unsigned power_good_ms = desc[5] * 2u;
    unsigned think_time = (characteristics >> 5) & 3;
    bool multi_tt = hub->speed == USB_SPEED_HIGH && hub->descriptor.device_protocol == 2;
    static const char *const switching[4] = { "ganged", "per-port", "no", "no" };
    kprintf("                 hub: %u ports, %s power switching, power good after %u ms%s\n",
            desc[2], switching[characteristics & 3], power_good_ms,
            hub->speed == USB_SPEED_HIGH ? (multi_tt ? ", a TT per port" : ", one TT") : "");
    hub->hub_ports = (uint8_t)ports;
    hub->hub_multi_tt = multi_tt;
    if (!usb_hub_set(hub, ports, think_time, multi_tt))
        return false;

    /* Power every port, then give the devices time to come up and their
     * connection to settle (USB 2.0, 7.1.7.3: 100 ms). */
    for (unsigned port = 1; port <= ports; port++)
        port_request(hub, REQ_SET_FEATURE, PORT_POWER, port);
    timer_sleep_ms(power_good_ms + 100);

    for (unsigned port = 1; port <= ports; port++) {
        uint16_t status, change;
        if (!port_status(hub, port, &status, &change) || !(status & STATUS_CONNECTION))
            continue;
        port_request(hub, REQ_CLEAR_FEATURE, C_PORT_CONNECTION, port);
        attach_port(hub, port);
    }

    /* From now on, the hub reports changes on its interrupt endpoint. */
    struct hub *h = 0;
    uint64_t state = spin_lock(&hubs_lock);
    for (int i = 0; i < MAX_HUBS && !h; i++)
        if (!hubs[i].dev)
            h = &hubs[i];
    if (in && h) {
        h->changed = 0;
        h->dev = hub;
    }
    spin_unlock(&hubs_lock, state);
    if (!in || !h) {
        kprintf("                 hub: no status change endpoint: devices plugged in later "
                "are not seen\n");
        return true;
    }
    if (!usb_interrupt_in(hub, in, on_change, h)) {
        state = spin_lock(&hubs_lock);
        h->dev = 0;
        spin_unlock(&hubs_lock, state);
        kprintf("                 hub: status change endpoint 0x%02x failed\n", in->address);
    }
    return true;
}

/* A hub port reported a change: compare what is connected with the device
 * we have there, and attach or detach (a connection change while one is
 * there: it was replaced faster than we looked). */
static void check_port(struct usb_device *hub, unsigned port)
{
    uint16_t status, change;
    if (!port_status(hub, port, &status, &change))
        return;
    /* Clear the change bits, or the hub keeps reporting the port. */
    static const uint16_t bits[] = { CHANGE_CONNECTION, CHANGE_ENABLE, CHANGE_OVER_CURRENT,
                                     CHANGE_RESET };
    static const uint16_t features[] = { C_PORT_CONNECTION, C_PORT_ENABLE, C_PORT_OVER_CURRENT,
                                         C_PORT_RESET };
    for (int i = 0; i < 4; i++)
        if (change & bits[i])
            port_request(hub, REQ_CLEAR_FEATURE, features[i], port);

    struct usb_device *child = 0;
    for (int i = 0; i < usb_device_count() && !child; i++) {
        struct usb_device *d = usb_device_at(i);
        if (d->parent == hub && d->port == port)
            child = d;
    }
    bool connected = status & STATUS_CONNECTION;
    if (child && (!connected || (change & CHANGE_CONNECTION))) {
        usb_detach(child);
        child = 0;
    }
    if (!connected || child)
        return;
    timer_sleep_ms(100);                    /* the contacts settle (USB 2.0, 7.1.7.3) */
    if (!port_status(hub, port, &status, &change) || !(status & STATUS_CONNECTION))
        return;
    port_request(hub, REQ_CLEAR_FEATURE, C_PORT_CONNECTION, port);
    attach_port(hub, port);
}

void usbhub_work(void *hc)
{
    for (int i = 0; i < MAX_HUBS; i++) {
        struct hub *h = &hubs[i];
        uint64_t irq = spin_lock(&hubs_lock);
        struct usb_device *hub = h->dev;
        bool mine = hub && (void *)hub->hc == hc;   /* each controller's thread its own */
        uint32_t changed = mine ? h->changed : 0;
        if (mine)
            h->changed = 0;
        spin_unlock(&hubs_lock, irq);
        if (!hub || !changed)
            continue;
        for (unsigned port = 1; port <= hub->hub_ports; port++)
            if (changed & (1u << port) && h->dev == hub)    /* the hub may go meanwhile */
                check_port(hub, port);
    }
}

void usbhub_detach(struct usb_device *dev)
{
    uint64_t state = spin_lock(&hubs_lock);
    for (int i = 0; i < MAX_HUBS; i++)
        if (hubs[i].dev == dev)
            hubs[i].dev = 0;
    spin_unlock(&hubs_lock, state);
}
