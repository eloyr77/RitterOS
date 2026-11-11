/*
 * USB Ethernet in the CDC-NCM class (step 50): a USB network adapter whose
 * Ethernet frames ride in NCM Transfer Blocks (NTB) on the bulk endpoints.
 * It becomes an ordinary card (eth0, eth1, ...) through the network stack,
 * so IPv4, the firewall and everything else work on it as on the PCI
 * cards. Only NCM (not the older CDC-ECM or RNDIS); since step 50d also one
 * function of a composite device (the PlutoSDR's NCM beside its storage
 * and serial port). (67e) A kernel module, against the driver interface's
 * USB class: offered every device with a CDC-NCM interface (class 02,
 * subclass 0d), it reads the configuration itself (find_ncm).
 *
 * NTB framing: an NCM Transfer
 * Block is an NTH16 header ("NCMH") pointing at an NDP16 table ("NCM0") of
 * (datagram offset, length) pairs, then the Ethernet frames. We send one
 * frame per NTB and read whatever the adapter packs into one (up to its
 * input size: several frames).
 *
 * (64m) Frames to send wait in a queue (TX_QUEUE) for the card's send
 * thread, which makes the transfers: the receive thread, which delivers what
 * came and so sends TCP's acknowledgements, never waits on the OUT endpoint
 * while no read is pending on IN. Every failed transfer is counted, and the
 * first few are logged; `net` shows the counts and the endpoints' own
 * (usb_bulk_stats). Found with the PlutoSDR: under many small request and
 * answer exchanges the link stopped for good, with nothing logged.
 */
#include "driver.h"
#include "driver_net.h"
#include "driver_usb.h"

#define MAX_ADAPTERS    8
#define NTB_BUF         2048            /* one NTB we send; fits in a page */
#define RX_PAGES        4               /* an NTB we receive: up to 16 KiB (several frames) */
#define RX_MIN          2048            /* the smallest input size NCM allows */
#define BULK_MAX_PACKET 0x200           /* must match the model's wMaxPacketSize */
#define RX_RETRY_MS     5               /* gap after a failed read */
#define TX_QUEUE        128             /* (64m) frames waiting for the send thread */
#define ROOM_WAIT_MS    1000            /* how long a sender waits for room in it */
#define LOGGED_ERRORS   10              /* failed transfers logged per card (then only counted) */

/* CDC/NCM control requests (request_type for the class ones is 0x21, out). */
#define SET_INTERFACE               0x0B    /* standard, recipient interface (0x01) */
#define SET_ETHERNET_PACKET_FILTER  0x43
#define GET_NTB_PARAMETERS          0x80
#define SET_NTB_FORMAT              0x84
#define SET_NTB_INPUT_SIZE         0x86
#define NOTIFY_NETWORK_CONNECTION   0x00

struct usbnet {
    struct net_card *dev;               /* (67e) its card (net_card_new): its priv is this */
    struct usb_device *udev;
    uint8_t in_ep, out_ep;
    uint64_t rx_phys, tx_phys;
    uint8_t *rx, *tx;
    uint32_t rx_size;                   /* the input size agreed (SET_NTB_INPUT_SIZE) */
    struct mutex tx_lock;
    volatile bool gone;
    volatile bool link;
    volatile bool rx_running;           /* its receive thread has not ended yet */
    volatile bool tx_running;           /* (64m) its send thread has not ended yet */
    uint16_t tx_seq;
    char model[40];
    char tx_name[16];                   /* the send thread's name ("eth1 send") */
    /* (64m) The send queue (under queue_lock; tx_wait wakes the send thread). */
    struct spinlock queue_lock;
    struct wait_queue tx_wait;
    struct wait_queue tx_room;          /* woken when a frame leaves the queue */
    uint8_t (*queue)[ETH_FRAME_MAX];
    uint8_t taken[ETH_FRAME_MAX];       /* the frame the send thread took out */
    uint16_t queue_length[TX_QUEUE];
    uint64_t queue_at[TX_QUEUE];        /* (65e) when each was queued */
    uint32_t queue_head, queue_count;
    /* (64m) What happened, for net's report. */
    uint64_t ntbs, ntb_bad, frames_in, read_errors, sent, zero_ends, send_errors, queue_full;
    uint64_t skipped;                   /* (64n) datagrams an NTB named but did not hold */
    uint64_t joined;                    /* (64n) NTBs found after another in one read */
    uint32_t offered;                   /* (64n) the adapter's dwNtbInMaxSize */
    uint32_t bad_logged;
    uint32_t last_read;                 /* (64n) the read before, for the log */
    uint32_t last_block, last_ndp;      /* (64s) its first NTB's wBlockLength, wNdpIndex */
    struct usb_bulk_stats last_end;     /* (64s) the bulk IN's counts after it */
    uint32_t logged;
    /* (65e) The receive thread's time (arch_counter counts): delivering
     * what came (no read pending then), and the reads that brought data by
     * how long each was pending (RX_SPANS: under 0.25, 0.5, 1, 2, 5 ms,
     * longer: a link that was idle). */
    uint64_t rx_delivering;
    uint64_t rx_waits[6];
    uint64_t tx_waits[6];               /* (65e) frames sent, by queued-to-sent (same spans) */
};

static const uint32_t rx_spans_us[5] = { 250, 500, 1000, 2000, 5000 };

static struct usbnet *adapters[MAX_ADAPTERS];
static int adapter_count;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff, p[1] = v >> 8;
}

static uint16_t get16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static uint32_t usbnet_link(struct net_card *d)
{
    struct usbnet *un = d->priv;
    return un->link && !un->gone ? 1000 : 0;
}

/* A link-status notification on the interrupt endpoint (interrupt context:
 * no console, no heap). NETWORK_CONNECTION's wValue is 1 when connected. */
static void on_notify(void *context, const uint8_t *data, uint32_t length)
{
    struct usbnet *un = context;
    if (length >= 4 && data[1] == NOTIFY_NETWORK_CONNECTION)
        un->link = get16(data + 2) != 0;
}

/* (64m) A failed transfer: counted always, logged the first few times. */
static void failed(struct usbnet *un, const char *what, uint8_t endpoint)
{
    if (un->logged >= LOGGED_ERRORS)
        return;
    un->logged++;
    struct usb_bulk_stats st;
    uint32_t code = usb_bulk_stats(un->udev, endpoint, &st) ? st.last_error : 0;
    kprintf("%s: %s failed (USB completion code %u)%s\n", un->dev->name, what, code,
            un->logged == LOGGED_ERRORS ? "; further failures only counted (net)" : "");
}

/* Wrap one Ethernet frame in a one-datagram NTB and send it (the send
 * thread). */
static void send_frame(struct usbnet *un, const uint8_t *frame, size_t length)
{
    struct net_card *d = un->dev;
    mutex_lock(&un->tx_lock);
    uint8_t *b = un->tx;
    uint32_t total = 28 + (uint32_t)length;
    memset(b, 0, 28);
    memcpy(b, "NCMH", 4);
    put16(b + 4, 12);                   /* wHeaderLength */
    put16(b + 6, un->tx_seq++);         /* wSequence */
    put16(b + 8, (uint16_t)total);      /* wBlockLength */
    put16(b + 10, 12);                  /* wNdpIndex */
    memcpy(b + 12, "NCM0", 4);
    put16(b + 16, 16);                  /* wLength (NDP: 8 header + two entries) */
    put16(b + 18, 0);                   /* wNextNdpIndex */
    put16(b + 20, 28);                  /* wDatagramIndex */
    put16(b + 22, (uint16_t)length);    /* wDatagramLength */
    put16(b + 24, 0), put16(b + 26, 0); /* terminating entry */
    memcpy(b + 28, frame, length);
    bool stalled = false;
    int r = usb_bulk_transfer(un->udev, un->out_ep, un->tx_phys, total, &stalled);
    if (r >= 0 && total % BULK_MAX_PACKET == 0) {
        /* a zero-length end: without it the adapter takes the next NTB as
         * part of this transfer (until 64m the controller refused it) */
        r = usb_bulk_transfer(un->udev, un->out_ep, un->tx_phys, 0, &stalled);
        if (r >= 0)
            un->zero_ends++;
    }
    if (stalled)
        usb_bulk_clear_halt(un->udev, un->out_ep);
    if (r >= 0) {
        d->tx_frames++;
        d->tx_bytes += length;
        un->sent++;
    } else {
        d->tx_dropped++;
        un->send_errors++;
        failed(un, "a send", un->out_ep);
    }
    mutex_unlock(&un->tx_lock);
}

/* (64m) Queue a frame for the send thread. A full queue makes a sender
 * wait for room (up to ROOM_WAIT_MS), but never a thread delivering what a
 * card received (`net_rx`: this card's receive thread among them, which
 * must go on reading): for it the frame is dropped (counted), as TCP
 * allows. */
static bool usbnet_send(struct net_card *d, const void *frame, size_t length)
{
    struct usbnet *un = d->priv;
    if (un->gone || length < 14 || length > ETH_FRAME_MAX || 28 + length > NTB_BUF)
        return false;
    if (un->queue_count >= TX_QUEUE && !net_receiving())
        wait_event_until(&un->tx_room, un->queue_count < TX_QUEUE || un->gone,
                         timer_ticks() + ROOM_WAIT_MS * TIMER_HZ / 1000);
    uint64_t state = spin_lock(&un->queue_lock);
    bool room = un->queue_count < TX_QUEUE;
    if (room) {
        uint32_t at = (un->queue_head + un->queue_count) % TX_QUEUE;
        memcpy(un->queue[at], frame, length);
        un->queue_length[at] = (uint16_t)length;
        un->queue_at[at] = arch_counter();
        un->queue_count++;
    }
    spin_unlock(&un->queue_lock, state);
    if (!room) {
        un->queue_full++;
        d->tx_dropped++;
        return false;
    }
    wait_queue_wake_all(&un->tx_wait);
    return true;
}

/* (64m) The send thread: frames from the queue, one transfer each, until
 * the adapter is unplugged. */
static void sender(void *context)
{
    struct usbnet *un = context;
    uint8_t *frame = un->taken;
    for (;;) {
        wait_event(&un->tx_wait, un->queue_count > 0 || un->gone);
        if (un->gone) {
            uint64_t state = spin_lock(&un->queue_lock);
            un->queue_count = 0;                            /* nothing goes out any more */
            spin_unlock(&un->queue_lock, state);
            un->tx_running = false;
            return;
        }
        uint64_t state = spin_lock(&un->queue_lock);
        uint32_t at = un->queue_head;
        size_t length = un->queue_length[at];
        uint64_t queued = un->queue_at[at];
        memcpy(frame, un->queue[at], length);
        un->queue_head = (at + 1) % TX_QUEUE;
        un->queue_count--;
        spin_unlock(&un->queue_lock, state);
        wait_queue_wake_all(&un->tx_room);
        send_frame(un, frame, length);
        uint64_t us = arch_counter_hz() / 1000000, took = us ? (arch_counter() - queued) / us : 0;
        int span = 0;
        while (span < 5 && took >= rx_spans_us[span])
            span++;
        un->tx_waits[span]++;
    }
}

/* (64m) net's report: the driver's counts, then each endpoint's. */
/* (38b) A bridge's port: nothing to change, the packet filter set at the
 * attach (0x0F) already takes every frame (promiscuous, all multicast,
 * directed, broadcast). */
static void usbnet_promiscuous(struct net_card *card, bool on)
{
    (void)card;
    (void)on;
}

static void details(struct net_card *d)
{
    struct usbnet *un = d->priv;
    kprintf("       usbnet: read %lu NTB(s) (%lu frames, %lu skipped; %lu after another in one read), "
            "%lu read(s) failed, %lu without a header; sent %lu (%lu zero-length end(s)), %lu "
            "failed, %lu dropped (queue full), %u waiting\n", un->ntbs, un->frames_in, un->skipped,
            un->joined, un->read_errors, un->ntb_bad, un->sent, un->zero_ends, un->send_errors,
            un->queue_full, un->queue_count);
    kprintf("       usbnet: NTBs read up to %u bytes (the adapter offers %u)\n", un->rx_size,
            un->offered);
    uint64_t us = arch_counter_hz() / 1000000;
    if (us && un->ntbs) {                   /* (65e) where the receive thread's time goes */
        const uint64_t *w = un->rx_waits;
        kprintf("       usbnet: reads with data, by their wait: under 0.25 ms %lu, 0.5 ms %lu, "
                "1 ms %lu, 2 ms %lu, 5 ms %lu, longer %lu; delivering %lu us an NTB\n",
                w[0], w[1], w[2], w[3], w[4], w[5], un->rx_delivering / us / un->ntbs);
        const uint64_t *t = un->tx_waits;
        kprintf("       usbnet: frames sent, queued to sent: under 0.25 ms %lu, 0.5 ms %lu, "
                "1 ms %lu, 2 ms %lu, 5 ms %lu, longer %lu\n", t[0], t[1], t[2], t[3], t[4], t[5]);
        usb_report(un->udev);          /* (its controller: interrupts, events) */
    }
    const uint8_t eps[2] = { un->in_ep, un->out_ep };
    for (int i = 0; i < 2; i++) {
        struct usb_bulk_stats st;
        if (!usb_bulk_stats(un->udev, eps[i], &st))
            continue;
        kprintf("       usbnet: bulk %s 0x%02x: %lu transfer(s), %lu error(s) (last code %u), %lu "
                "out of time, %lu cancelled, %lu earlier event(s) ignored, %lu reset(s), %lu failed "
                "command(s)\n", i ? "OUT" : "IN", eps[i], st.transfers, st.errors, st.last_error,
                st.timeouts, st.cancels, st.stale, st.resets, st.failed_commands);
    }
}

/* Parse an NTB the adapter sent and deliver each Ethernet frame. */
static void deliver_one(struct usbnet *un, const uint8_t *ntb, uint32_t total)
{
    un->ntbs++;
    uint32_t ndp = get16(ntb + 10);
    for (int guard = 0; ndp && ndp + 8 <= total && guard < 8; guard++) {
        if (memcmp(ntb + ndp, "NCM0", 4) != 0)
            break;
        uint32_t ndp_len = get16(ntb + ndp + 4), next = get16(ntb + ndp + 6);
        for (uint32_t e = ndp + 8; e + 4 <= ndp + ndp_len && e + 4 <= total; e += 4) {
            uint32_t idx = get16(ntb + e), dlen = get16(ntb + e + 2);
            if (!idx && !dlen)
                break;                  /* the terminating null entry */
            if (dlen >= 14 && dlen <= ETH_FRAME_MAX && idx + dlen <= total) {
                un->frames_in++;
                net_receive(un->dev, ntb + idx, dlen);
            } else {
                un->skipped++;          /* (64n) too short, too long, or past the NTB's end */
            }
        }
        ndp = next;
    }
}

/* What one read brought: an NTB, or (64n) several one after another (a
 * transfer the adapter did not end before the next NTB: Linux's gadget
 * leaves the zero-length end out when an NTB is exactly the size it
 * thinks the host reads). A read that does not begin with an NTB's header
 * is the tail of an earlier one: its frames are lost (counted and the
 * first few logged, with their first bytes, and (64s) what the read
 * before held and how the controller ended both). Found with the
 * PlutoSDR: 289 such reads in 245,804, each losing the frames of an NTB;
 * the cause was the AMD controller's (see alloc_rx), and since 64s none. */
static void deliver(struct usbnet *un, const uint8_t *ntb, uint32_t total)
{
    uint32_t at = 0, before = un->last_read, before_block = un->last_block;
    uint32_t before_ndp = un->last_ndp;
    struct usb_bulk_stats then = un->last_end, now;
    if (!usb_bulk_stats(un->udev, un->in_ep, &now))
        memset(&now, 0, sizeof(now));
    un->last_end = now;
    un->last_read = total;
    bool header = total >= 12 && memcmp(ntb, "NCMH", 4) == 0;
    un->last_block = header ? get16(ntb + 8) : 0;
    un->last_ndp = header ? get16(ntb + 10) : 0;
    for (int blocks = 0; blocks < 8 && total - at >= 12; blocks++) {
        const uint8_t *b = ntb + at;
        uint32_t left = total - at;
        if (memcmp(b, "NCMH", 4) != 0) {
            if (blocks)
                break;                  /* (padding after the last one) */
            un->ntb_bad++;
            if (un->bad_logged < 5) {
                un->bad_logged++;
                kprintf("%s: a read of %u bytes (after one of %u) without an NTB header: %02x "
                        "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
                        "%02x\n", un->dev->name, total, before, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8],
                        b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
                /* (64s) What the read before promised and how the
                 * controller ended it: a split NTB, or one cut short. */
                kprintf("%s: the read before: NTB of %u bytes, NDP at %u; ended by TRB %d of %u, "
                        "code %u, %u left; this one by TRB %d of %u, code %u, %u left; %lu earlier "
                        "event(s) ignored since (last: TRB %d of %u, code %u, %u left)\n",
                        un->dev->name, before_block, before_ndp, then.end_trb, then.end_trbs,
                        then.end_code, then.end_residual, now.end_trb, now.end_trbs, now.end_code,
                        now.end_residual, now.stale - then.stale, now.stale_trb, now.stale_trbs,
                        now.stale_code, now.stale_residual);
            }
            return;
        }
        uint32_t block = get16(b + 8);  /* wBlockLength */
        if (block < 12 || block > left)
            block = left;
        if (blocks)
            un->joined++;
        deliver_one(un, b, block);
        at += block;
    }
    if (total < 12) {
        un->ntb_bad++;
    }
}

/* The receive thread: a read stays pending on the bulk IN until the
 * adapter has an NTB (sending and other transfers go on meanwhile); an
 * unplug ends it. */
static void receiver(void *context)
{
    struct usbnet *un = context;
    for (;;) {
        if (un->gone) {
            un->rx_running = false;
            return;
        }
        bool stalled = false;
        uint64_t began = arch_counter();
        int n = usb_bulk_transfer_timeout(un->udev, un->in_ep, un->rx_phys, un->rx_size,
                                          &stalled, 0);
        uint64_t came = arch_counter();
        if (un->gone) {
            un->rx_running = false;
            return;
        }
        if (stalled) {
            un->read_errors++;
            failed(un, "a read (stalled)", un->in_ep);
            usb_bulk_clear_halt(un->udev, un->in_ep);
            timer_sleep_ms(RX_RETRY_MS);
        } else if (n > 0) {
            uint64_t us = arch_counter_hz() / 1000000, waited = us ? (came - began) / us : 0;
            int span = 0;
            while (span < 5 && waited >= rx_spans_us[span])
                span++;
            un->rx_waits[span]++;
            net_batch_begin();              /* (65e) one ACK per connection for the NTB */
            deliver(un, un->rx, (uint32_t)n);
            net_batch_end();
            un->rx_delivering += arch_counter() - came;
        } else if (n < 0) {
            un->read_errors++;
            failed(un, "a read", un->in_ep);
            timer_sleep_ms(RX_RETRY_MS);    /* an error: try again a little later */
        }
    }
}

/* (64s) The receive buffer, aligned to its own size (RX_PAGES is a power
 * of two), so it lies inside one 64 KiB span and every read is a single
 * TRB: with several, the AMD laptop's controller sometimes put the start
 * of the next NTB into the later TRBs of a read a short packet had ended
 * (xhci.c). Seven pages always hold four aligned ones; the rest go back. */
static uint64_t alloc_rx(void)
{
    const uint64_t size = RX_PAGES * PAGE_SIZE, run_pages = 2 * RX_PAGES - 1;
    uint64_t run = pmm_alloc_pages(run_pages);
    if (!run)
        return 0;
    uint64_t start = (run + size - 1) & ~(size - 1);
    for (uint64_t a = run; a < run + run_pages * PAGE_SIZE; a += PAGE_SIZE)
        if (a < start || a >= start + size)
            pmm_free_page(a);
    return start;
}

/* Set up the NCM function of a configured device: the control interface and
 * its notification endpoint, the data interface (switched to its alternate
 * setting 1) and its bulk endpoints, and the MAC address from the device's
 * Ethernet descriptor. */
static bool setup(struct usb_device *dev, uint8_t control_interface, uint8_t data_interface,
                   const struct usb_endpoint_descriptor *notify,
                   const struct usb_endpoint_descriptor *in,
                   const struct usb_endpoint_descriptor *out, unsigned in_burst,
                   unsigned out_burst, const uint8_t mac[6])
{
    /* An adapter plugged in again (the same MAC) gets its card back: the
     * same name and settings, so DHCP asks again when the link comes. */
    struct usbnet *un = 0;
    for (int i = 0; i < adapter_count && !un; i++)
        if (adapters[i]->gone && memcmp(adapters[i]->dev->mac, mac, 6) == 0)
            un = adapters[i];
    bool again = un != 0;
    if (again) {
        wait_queue_wake_all(&un->tx_wait);  /* (its old send thread looks and ends) */
        for (int wait = 0; (un->rx_running || un->tx_running) && wait < 200; wait++)
            timer_sleep_ms(10);         /* its old receive thread ends at its next look */
        if (un->rx_running || un->tx_running) {
            kprintf("  %-15s%s: its old threads have not ended\n", "usbnet:", un->dev->name);
            return false;
        }
    } else {
        if (adapter_count >= MAX_ADAPTERS) {
            kprintf("  %-15sno room for another USB network adapter\n", "usbnet:");
            return false;
        }
        un = kzalloc(sizeof(*un));
        if (!un || !(un->dev = net_card_new())) {
            kfree(un);
            return false;
        }
        un->rx_phys = alloc_rx();
        un->tx_phys = pmm_alloc_page();
        un->queue = kmalloc(sizeof(*un->queue) * TX_QUEUE);
        if (!un->rx_phys || !un->tx_phys || !un->queue) {
            if (un->rx_phys)
                pmm_free_pages(un->rx_phys, RX_PAGES);
            if (un->tx_phys)
                pmm_free_page(un->tx_phys);
            kfree(un->queue);
            kfree(un);
            return false;
        }
        un->queue_lock = (struct spinlock)SPINLOCK_INIT("usbnet queue");
        un->rx = phys_to_virt(un->rx_phys);
        un->tx = phys_to_virt(un->tx_phys);
        un->tx_lock = (struct mutex)MUTEX_INIT("usbnet tx");
    }
    un->udev = dev;
    un->in_ep = in->address;
    un->out_ep = out->address;

    /* NCM setup while the data interface is still off (alternate setting
     * 0), as the class asks: the NTB-16 format and the input size, the
     * largest NTB the adapter takes (GET_NTB_PARAMETERS: dwNtbInMaxSize)
     * up to our buffer, so it can pack several frames into one. */
    uint8_t params[28];
    un->rx_size = RX_MIN;
    if (usb_control(dev, USB_DIR_IN | USB_TYPE_CLASS_INTERFACE, GET_NTB_PARAMETERS, 0,
                    control_interface, params, sizeof(params)) >= 8) {
        uint32_t most = get16(params + 4) | (uint32_t)get16(params + 6) << 16;
        uint32_t room = RX_PAGES * PAGE_SIZE;
        un->rx_size = most > room ? room : most < RX_MIN ? RX_MIN : most;
        un->offered = most;
    }
    uint8_t size[4] = { un->rx_size & 0xff, (un->rx_size >> 8) & 0xff,
                        (un->rx_size >> 16) & 0xff, 0 };
    usb_control(dev, USB_TYPE_CLASS_INTERFACE, SET_NTB_FORMAT, 0, control_interface, 0, 0);
    usb_control(dev, USB_TYPE_CLASS_INTERFACE, SET_NTB_INPUT_SIZE, 0, control_interface, size, 4);

    /* The data interface carries its bulk endpoints only in alternate
     * setting 1; switch to it, then set up the endpoints. */
    if (usb_control(dev, 0x01, SET_INTERFACE, 1, data_interface, 0, 0) < 0) {
        kprintf("  %-15sSET_INTERFACE(%u, alt 1) failed\n", "usbnet:", data_interface);
        goto fail;
    }
    if (!usb_bulk_configure(dev, in, out, in_burst, out_burst)) {
        kprintf("  %-15sbulk endpoints cannot be set up\n", "usbnet:");
        goto fail;
    }
    usb_control(dev, USB_TYPE_CLASS_INTERFACE, SET_ETHERNET_PACKET_FILTER, 0x0F, control_interface,
                0, 0);

    ksnprintf(un->dev->where, sizeof(un->dev->where), "%s", usb_device_name(dev));
    un->link = true;                    /* plugged in; the notification refines it */
    if (again) {
        un->gone = false;               /* sending goes to the new device from here */
        un->dev->gone = false;
        net_rename(un->dev);           /* its name from the names file, as a new card's */
    } else {
        memcpy(un->dev->mac, mac, 6);
        un->dev->driver = "usbnet";
        const char *product = usb_device_product(dev);
        ksnprintf(un->model, sizeof(un->model), "%s", product[0] ? product : "USB NCM");
        un->dev->model[0] = 0;
        memcpy(un->dev->model, un->model, sizeof(un->dev->model) < sizeof(un->model) ?
               sizeof(un->dev->model) : sizeof(un->model));
        un->dev->model[sizeof(un->dev->model) - 1] = 0;
        un->dev->link_speed = usbnet_link;
        un->dev->send = usbnet_send;
        un->dev->details = details;
        un->dev->set_promiscuous = usbnet_promiscuous;
        un->dev->priv = un;
        adapters[adapter_count++] = un;
        net_register(un->dev);
    }

    if (notify)
        usb_interrupt_in(dev, notify, on_notify, un);   /* link status */
    un->rx_running = true;
    if (!thread_create(un->dev->name, receiver, un)) {
        un->rx_running = false;
        kprintf("  %-15s%s: no receive thread\n", "usbnet:", un->dev->name);
    }
    un->tx_running = true;
    ksnprintf(un->tx_name, sizeof(un->tx_name), "%s send", un->dev->name);
    if (!thread_create(un->tx_name, sender, un)) {
        un->tx_running = false;
        kprintf("  %-15s%s: no send thread\n", "usbnet:", un->dev->name);
    }
    usb_log_label(usb_device_name(dev));
    kprintf("USB NCM Ethernet on %s as %s%s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", usb_device_name(dev),
            un->dev->name, again ? " again" : "", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;

fail:
    if (again)
        return false;                   /* the card stays, gone */
    pmm_free_pages(un->rx_phys, RX_PAGES);
    pmm_free_page(un->tx_phys);
    kfree(un);
    return false;
}

/* The device was unplugged: its card reports no link and sends nothing. */
static void detach(struct usb_device *dev)
{
    for (int i = 0; i < adapter_count; i++) {
        struct usbnet *un = adapters[i];
        if (un && un->udev == dev && !un->gone) {
            un->gone = true;
            un->dev->gone = true;        /* "unplugged" in net's report; its name may go */
            un->link = false;           /* the card stays listed, with no link */
            wait_queue_wake_all(&un->tx_wait);      /* (64m) its send thread ends */
            wait_queue_wake_all(&un->tx_room);      /* (and nobody waits for room) */
            usb_log_label(usb_device_name(dev));
            kprintf("USB NCM Ethernet %s removed (no link now)\n", un->dev->name);
        }
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* (67e) The NCM function in the device's configuration: a Communications
 * interface (class 02, subclass 0d) with its notification endpoint
 * (interrupt IN), the CDC Data interface its Union descriptor names (the
 * next one until it is read; the bulk endpoints are in its alternate
 * setting 1), with the SuperSpeed bursts of their companions, and the MAC
 * from the Ethernet functional descriptor (a string of 12 hex digits). */
struct ncm {
    uint8_t control, data, mac[6];
    struct usb_endpoint_descriptor notify, in, out;
    unsigned in_burst, out_burst;
    bool has_notify, has_in, has_out, has_mac;
};

static bool find_ncm(struct usb_device *dev, struct ncm *f)
{
    uint8_t *buf = kmalloc(4096);
    int n = buf ? usb_configuration(dev, buf, 4096) : -1;
    bool found = false, in_control = false, in_data = false;
    unsigned *last_burst = 0;
    for (int pos = n > 0 ? buf[0] : 0; pos + 2 <= n && buf[pos] >= 2 && pos + buf[pos] <= n;
         pos += buf[pos]) {
        uint8_t length = buf[pos], type = buf[pos + 1];
        if (type == USB_DESC_INTERFACE && length >= sizeof(struct usb_interface_descriptor)) {
            uint8_t number = buf[pos + 2], cls = buf[pos + 5], sub = buf[pos + 6];
            in_control = !found && cls == 0x02 && sub == 0x0d;
            if (in_control) {
                found = true;
                f->control = number;
                f->data = number + 1;
            }
            in_data = found && cls == 0x0a && number == f->data;
            last_burst = 0;
        } else if (type == USB_DESC_ENDPOINT && length >= sizeof(struct usb_endpoint_descriptor)) {
            struct usb_endpoint_descriptor e;
            memcpy(&e, buf + pos, sizeof(e));
            unsigned kind = e.attributes & 3;
            last_burst = 0;
            if (in_control && kind == 3 && (e.address & 0x80) && !f->has_notify) {
                f->notify = e;
                f->has_notify = true;
            }
            if (in_data && kind == 2) {
                if ((e.address & 0x80) && !f->has_in) {
                    f->in = e;
                    f->has_in = true;
                    last_burst = &f->in_burst;
                } else if (!(e.address & 0x80) && !f->has_out) {
                    f->out = e;
                    f->has_out = true;
                    last_burst = &f->out_burst;
                }
            }
        } else if (type == USB_DESC_SS_COMPANION && length >= 3) {
            if (last_burst)
                *last_burst = buf[pos + 2];
        } else if (type == 0x24 && length >= 5 && in_control && buf[pos + 2] == 0x06) {
            f->data = buf[pos + 4];                 /* CDC Union: the first subordinate */
        } else if (type == 0x24 && length >= 4 && in_control && buf[pos + 2] == 0x0f &&
                   !f->has_mac) {
            char text[32];
            usb_string(dev, buf[pos + 3], text, sizeof(text));
            bool ok = true;
            for (int k = 0; k < 6 && ok; k++) {
                int hi = hex_digit(text[k * 2]), lo = hex_digit(text[k * 2 + 1]);
                ok = hi >= 0 && lo >= 0;
                f->mac[k] = (uint8_t)(hi << 4 | lo);
            }
            f->has_mac = ok;
        }
    }
    kfree(buf);
    return found && f->has_notify && f->has_in && f->has_out && f->has_mac;
}

static bool attach(struct usb_device *dev)
{
    struct ncm f;
    memset(&f, 0, sizeof(f));
    if (!find_ncm(dev, &f)) {
        usb_log_label(usb_device_name(dev));
        kprintf("usbnet: its NCM function lacks an endpoint or its MAC address\n");
        return false;
    }
    return setup(dev, f.control, f.data, &f.notify, &f.in, &f.out, f.in_burst, f.out_burst, f.mac);
}

/* Any device with a CDC-NCM interface. */
static const struct module_usb ids[] = {
    { USB_ANY, USB_ANY, 0x02, 0x0D, USB_ANY },
};

USB_MODULE(usbnet, attach, detach, ids)
