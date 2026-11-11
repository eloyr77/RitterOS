/*
 * USB CDC ACM serial ports (see usbserial.h). A port is a ring of received
 * bytes filled by its receive thread (one read always pending on the bulk
 * IN), and writes sent in pieces of a page on the bulk OUT.
 */
#include "usbserial.h"
#include "devname.h"
#include "heap.h"
#include "kprintf.h"
#include "memory.h"
#include "pmm.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "timer.h"

#define MAX_PORTS       8
#define RING_SIZE       16384           /* bytes kept until a program reads them */
#define RX_RETRY_MS     5               /* gap after a failed read */

/* CDC ACM class requests (request_type 0x21: class, interface, out). */
#define SET_LINE_CODING         0x20
#define SET_CONTROL_LINE_STATE  0x22
#define LINE_DTR                0x01
#define LINE_RTS                0x02

struct usbserial {
    struct usb_device *udev;
    uint8_t in_ep, out_ep;
    int number;                         /* usbserial<number>, reserved (devname.h) */
    uint64_t rx_phys, tx_phys;          /* a page each (DMA) */
    uint8_t *rx, *tx;
    struct spinlock lock;               /* the ring */
    uint8_t ring[RING_SIZE];
    uint32_t head, tail;                /* bytes at tail..head (mod RING_SIZE) */
    struct wait_queue readers;
    struct mutex tx_lock;
    volatile bool gone;
    uint64_t dropped;                   /* received while the ring was full */
};

/* The port of each number now (a port plugged in again replaces its old
 * entry; the old one stays allocated: an open file may still hold it). */
static struct usbserial *ports[MAX_PORTS];
static struct mutex ports_lock = MUTEX_INIT("usb serial ports");

static uint32_t ring_used(const struct usbserial *s)
{
    return (s->head - s->tail) % RING_SIZE;
}

/* The receive thread: a read stays pending until the device sends; what
 * comes goes into the ring (what does not fit is dropped and counted). */
static void receiver(void *context)
{
    struct usbserial *s = context;
    while (!s->gone) {
        bool stalled = false;
        int n = usb_bulk_transfer_timeout(s->udev, s->in_ep, s->rx_phys, PMM_PAGE_SIZE,
                                          &stalled, 0);
        if (s->gone)
            break;
        if (stalled) {
            usb_bulk_clear_halt(s->udev, s->in_ep);
            timer_sleep_ms(RX_RETRY_MS);
            continue;
        }
        if (n < 0) {
            timer_sleep_ms(RX_RETRY_MS);
            continue;
        }
        uint64_t state = spin_lock(&s->lock);
        for (int i = 0; i < n; i++) {
            if (ring_used(s) == RING_SIZE - 1) {
                s->dropped += (uint64_t)(n - i);
                break;
            }
            s->ring[s->head] = s->rx[i];
            s->head = (s->head + 1) % RING_SIZE;
        }
        spin_unlock(&s->lock, state);
        if (n > 0)
            wait_queue_wake_all(&s->readers);
    }
    wait_queue_wake_all(&s->readers);   /* readers see the end */
}

int64_t usbserial_read(struct usbserial *s, uint8_t *buf, size_t size)
{
    if (size == 0)
        return 0;
    for (;;) {
        uint64_t state = spin_lock(&s->lock);
        size_t done = 0;
        while (done < size && s->tail != s->head) {
            buf[done++] = s->ring[s->tail];
            s->tail = (s->tail + 1) % RING_SIZE;
        }
        spin_unlock(&s->lock, state);
        if (done > 0)
            return (int64_t)done;
        if (s->gone)
            return 0;                   /* unplugged: the end of the input */
        if (thread_current()->dont_wait)
            return -ERR_AGAIN;
        if (!wait_event_interruptible(&s->readers, s->head != s->tail || s->gone))
            return -ERR_INTR;
    }
}

int64_t usbserial_write(struct usbserial *s, const uint8_t *buf, size_t size)
{
    size_t done = 0;
    mutex_lock(&s->tx_lock);
    while (done < size && !s->gone) {
        uint32_t piece = size - done < PMM_PAGE_SIZE ? (uint32_t)(size - done) : PMM_PAGE_SIZE;
        memcpy(s->tx, buf + done, piece);
        bool stalled = false;
        int r = usb_bulk_transfer(s->udev, s->out_ep, s->tx_phys, piece, &stalled);
        if (stalled)
            usb_bulk_clear_halt(s->udev, s->out_ep);
        if (r < 0)
            break;
        done += (size_t)r;
    }
    mutex_unlock(&s->tx_lock);
    return done > 0 ? (int64_t)done : -ERR_IO;
}

int usbserial_poll(struct usbserial *s)
{
    poll_watch(&s->readers);                /* (65e) before the state is read */
    uint64_t state = spin_lock(&s->lock);
    bool waiting = s->head != s->tail;
    spin_unlock(&s->lock, state);
    return (waiting || s->gone ? POLLIN : 0) | (s->gone ? POLLHUP : POLLOUT);
}

struct usbserial *usbserial_find(int number)
{
    if (number < 0 || number >= MAX_PORTS)
        return 0;
    mutex_lock(&ports_lock);
    struct usbserial *s = ports[number];
    mutex_unlock(&ports_lock);
    return s && !s->gone ? s : 0;
}

bool usbserial_present(int number)
{
    return usbserial_find(number) != 0;
}

bool usbserial_attach(struct usb_device *dev, uint8_t control_interface,
                      const struct usb_endpoint_descriptor *in,
                      const struct usb_endpoint_descriptor *out, unsigned in_burst,
                      unsigned out_burst)
{
    const struct devname_id id = { dev->descriptor.vendor_id, dev->descriptor.product_id,
                                   dev->serial, dev->name };
    bool again;
    int number = devname_attach("usbserial", &id, &again);
    if (number < 0 || number >= MAX_PORTS) {
        if (number >= 0)
            devname_detach("usbserial", number);
        kprintf("                 serial port: no room for another one\n");
        return false;
    }
    struct usbserial *s = kzalloc(sizeof(*s));
    if (!s || !(s->rx_phys = pmm_alloc_page()) || !(s->tx_phys = pmm_alloc_page())) {
        if (s && s->rx_phys)
            pmm_free_page(s->rx_phys);
        kfree(s);
        devname_detach("usbserial", number);
        kprintf("                 serial port: out of memory\n");
        return false;
    }
    s->udev = dev;
    s->in_ep = in->address;
    s->out_ep = out->address;
    s->number = number;
    s->rx = phys_to_virt(s->rx_phys);
    s->tx = phys_to_virt(s->tx_phys);
    s->lock = (struct spinlock)SPINLOCK_INIT("usb serial");
    s->readers.pollable = true;
    s->tx_lock = (struct mutex)MUTEX_INIT("usb serial tx");
    if (!usb_bulk_configure(dev, in, out, in_burst, out_burst)) {
        kprintf("                 serial port: bulk endpoints cannot be set up\n");
        goto fail;
    }
    /* 115200 baud, 1 stop bit, no parity, 8 data bits; then "a terminal is
     * there" (DTR) and "ready to receive" (RTS). Some ports refuse the
     * line settings: that is no reason to give up. */
    uint8_t line[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 };
    usb_control(dev, USB_TYPE_CLASS_INTERFACE, SET_LINE_CODING, 0, control_interface, line,
                sizeof(line));
    usb_control(dev, USB_TYPE_CLASS_INTERFACE, SET_CONTROL_LINE_STATE, LINE_DTR | LINE_RTS,
                control_interface, 0, 0);

    char name[16];
    ksnprintf(name, sizeof(name), "usbserial%d", number);
    mutex_lock(&ports_lock);
    ports[number] = s;
    mutex_unlock(&ports_lock);
    if (!thread_create(name, receiver, s))
        kprintf("  %-15sno receive thread\n", "usbserial:");
    usb_log_label(dev->name);
    kprintf("USB serial port on %s as %s%s\n", dev->name, name, again ? " again" : "");
    return true;

fail:
    pmm_free_page(s->rx_phys);
    pmm_free_page(s->tx_phys);
    kfree(s);
    devname_detach("usbserial", number);
    return false;
}

void usbserial_detach(struct usb_device *dev)
{
    mutex_lock(&ports_lock);
    for (int i = 0; i < MAX_PORTS; i++) {
        struct usbserial *s = ports[i];
        if (!s || s->udev != dev || s->gone)
            continue;
        s->gone = true;                 /* readers end once the ring is empty */
        wait_queue_wake_all(&s->readers);
        devname_detach("usbserial", s->number);
        usb_log_label(dev->name);
        kprintf("USB serial port usbserial%d removed\n", s->number);
    }
    mutex_unlock(&ports_lock);
}
