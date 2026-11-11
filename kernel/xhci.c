#include "xhci.h"
#include "sleep.h"
#include "arch.h"
#include "heap.h"
#include "kprintf.h"
#include "memory.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "timer.h"
#include "usb.h"
#include "usbhub.h"
#include "sync.h"

/* Capability registers (xHCI 1.2, section 5.3), at the start of BAR 0. */
#define CAP_LENGTH_VERSION  0x00    /* bits 7-0: length of this block; 31-16: version */
#define CAP_HCSPARAMS1      0x04    /* slots 7-0, interrupters 18-8, ports 31-24 */
#define CAP_HCSPARAMS2      0x08    /* scratchpad buffers: 25-21 (high), 31-27 (low) */
#define CAP_HCCPARAMS1      0x10    /* bit 2: 64-byte contexts; 3: port power
                                       control; 31-16: extended caps */
#define HCCPARAMS1_PPC      (1u << 3)
#define CAP_DBOFF           0x14    /* doorbell array offset */
#define CAP_RTSOFF          0x18    /* runtime registers offset */

/* Operational registers (section 5.4), after the capability registers. */
#define OP_USBCMD           0x00
#define OP_USBSTS           0x04
#define OP_PAGESIZE         0x08
#define OP_CRCR             0x18    /* command ring control (64-bit) */
#define OP_DCBAAP           0x30    /* device context base address array (64-bit) */
#define OP_CONFIG           0x38
#define OP_PORTSC(n)        (0x400 + 0x10 * ((n) - 1))      /* ports count from 1 */

#define USBCMD_RUN          (1u << 0)
#define USBCMD_RESET        (1u << 1)
#define USBCMD_INTE         (1u << 2)   /* interrupter enable */
#define USBSTS_HALTED       (1u << 0)
#define USBSTS_HSE          (1u << 2)   /* host system error */
#define USBSTS_EINT         (1u << 3)   /* event interrupt */
#define USBSTS_CNR          (1u << 11)  /* controller not ready */
#define CRCR_RCS            (1u << 0)   /* ring cycle state */

#define PORTSC_CCS          (1u << 0)   /* a device is connected */
#define PORTSC_PED          (1u << 1)   /* port enabled */
#define PORTSC_PR           (1u << 4)   /* port reset */
#define PORTSC_PP           (1u << 9)   /* port power */
#define PORTSC_SPEED(v)     (((v) >> 10) & 0xF)
#define PORTSC_CSC          (1u << 17)  /* connect status change */
#define PORTSC_PRC          (1u << 21)  /* port reset change */
#define PORTSC_CHANGES      (0x7Fu << 17)   /* change bits 17-23: write 1 to clear */
/* Bits written back unchanged when changing PORTSC; the others either clear
 * something when written as 1 (PED, change bits) or start an action (PR). */
#define PORTSC_KEEP         (PORTSC_CCS | 1u << 3 | 0xFu << 5 | 1u << 9 | 0xFu << 10 | \
                             3u << 14 | 7u << 25 | 1u << 30)

/* Runtime registers (section 5.5): interrupter 0. */
#define RT_IMAN             0x20
#define RT_IMOD             0x24
#define RT_ERSTSZ           0x28
#define RT_ERSTBA           0x30    /* event ring segment table base (64-bit) */
#define RT_ERDP             0x38    /* event ring dequeue pointer (64-bit) */
#define IMAN_IP             (1u << 0)   /* interrupt pending (write 1 to clear) */
#define IMAN_IE             (1u << 1)   /* interrupt enable */
#define ERDP_EHB            (1u << 3)   /* event handler busy (write 1 to clear) */

/* Extended capabilities (section 7). */
#define XCAP_LEGACY         1       /* USB legacy support: firmware/OS ownership */
#define XCAP_PROTOCOL       2       /* supported protocol: which ports are USB 2/3 */
#define LEGACY_BIOS_OWNED   (1u << 16)
#define LEGACY_OS_OWNED     (1u << 24)

/* TRB types (section 6.4.6) and fields. */
#define TRB_NORMAL          1
#define TRB_SETUP_STAGE     2
#define TRB_DATA_STAGE      3
#define TRB_STATUS_STAGE    4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_DISABLE_SLOT    10
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT 13
#define TRB_RESET_ENDPOINT  14
#define TRB_SET_TR_DEQUEUE  16
#define TRB_NOOP_COMMAND    23
#define TRB_TRANSFER_EVENT  32
#define TRB_COMMAND_DONE    33
#define TRB_PORT_CHANGE     34
#define TRB_TYPE(t)         ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)     (((c) >> 10) & 0x3F)
#define TRB_SLOT(s)         ((uint32_t)(s) << 24)
#define TRB_CYCLE           (1u << 0)
#define LINK_TOGGLE_CYCLE   (1u << 1)
#define TRB_ISP             (1u << 2)   /* interrupt (event) on a short packet */
#define TRB_CHAIN           (1u << 4)   /* the next TRB belongs to the same transfer */
#define TRB_IOC             (1u << 5)   /* interrupt (event) on completion */
#define TRB_ENDPOINT(dci)   ((uint32_t)(dci) << 16)
#define TRB_IDT             (1u << 6)   /* immediate data: the parameter is the data */
#define TRB_DIR_IN          (1u << 16)
#define SETUP_NO_DATA       (0u << 16)  /* setup stage: transfer type */
#define SETUP_IN_DATA       (3u << 16)
#define COMPLETION_SUCCESS  1
#define COMPLETION_SHORT    13          /* less data than asked for: fine for reads */
#define COMPLETION_STALL    6           /* the endpoint refused (halted) */

/* Contexts (section 6.2): the controller's view of a device (slot context)
 * and of each of its endpoints. The input context, which commands read, has
 * an input control context first ("add these contexts"). */
#define CTX_INPUT_CONTROL   0           /* index in the input context */
#define CTX_IN_SLOT         1
#define CTX_IN_EP0          2
#define CTX_OUT_SLOT        0           /* index in the output (device) context */
#define EP_TYPE_CONTROL     4
#define EP_TYPE_INTERRUPT_IN 7
#define EP_TYPE_BULK_OUT    2
#define EP_TYPE_BULK_IN     6
#define BULK_TRBS_MAX       64          /* pieces of one bulk transfer: 256 KiB at least */
#define TRB_SPAN            0x10000     /* a TRB's data never crosses a 64 KiB boundary */
#define BULK_TIMEOUT_MS     5000
#define BULK_ENDPOINTS      8           /* per device: composite ones have several functions */
#define TRB_STOP_ENDPOINT   15
#define COMPLETION_STOPPED  26          /* a transfer cut off by Stop Endpoint */
#define COMPLETION_STOPPED_LENGTH 27
#define COMPLETION_CONTEXT_STATE 19     /* a command the endpoint's state does not allow
                                         * (Stop Endpoint on a halted one) */
#define MAX_ENDPOINTS       16          /* interrupt endpoints, all devices together */
#define ENDPOINT_BUFFERS    16          /* transfers kept queued per endpoint */
#define ENDPOINT_BUFFER_MAX 256         /* bytes per transfer (16 x 256 = one page) */
#define DOORBELL_EP0        1           /* doorbell target for the control endpoint */

#define RING_TRBS           (PMM_PAGE_SIZE / 16)    /* one page per ring: 256 TRBs */
#define TIMEOUT_MS          1000
#define MAX_PROTOCOLS       4

struct trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;               /* bit 0: cycle; bits 15-10: type */
};

struct erst_entry {                 /* event ring segment table entry */
    uint64_t base;
    uint32_t size;                  /* in TRBs */
    uint32_t reserved;
};

_Static_assert(sizeof(struct trb) == 16, "TRB size");
_Static_assert(sizeof(struct erst_entry) == 16, "ERST entry size");

struct ring {
    volatile struct trb *trbs;      /* direct-map address */
    uint64_t phys;
    uint32_t index;                 /* producer: enqueue; event ring: dequeue */
    uint32_t cycle;                 /* the cycle bit that marks a valid TRB */
};

struct protocol {                   /* a range of ports and its USB version */
    uint8_t major, minor;
    uint8_t first, count;
};

struct xhci {
    const char *name;               /* e.g. "usb0" */
    struct pci_device *pci;
    volatile uint8_t *cap, *op, *rt, *db;
    uint32_t version;
    uint32_t max_slots, max_ports, max_interrupters, scratchpads, context_size;
    uint64_t *dcbaa;                /* device context base address array */
    struct ring cmd, event;
    struct protocol protocols[MAX_PROTOCOLS];
    int protocol_count;
    bool interrupts;                /* MSI-X on: sleep instead of polling */
    volatile uint64_t interrupt_count;
    uint64_t events, port_events, commands;
    uint64_t tick_events;           /* found by the tick although interrupts are on */
    /* Written by process_events (often in the interrupt handler), read by
     * the code waiting for them. */
    volatile uint64_t done_trb;     /* command TRB of the last completion seen */
    volatile uint32_t done_code;    /* and its completion code */
    volatile uint32_t done_slot;    /* and its slot (Enable Slot) */
    volatile struct {               /* the control transfer in progress */
        uint32_t slot;
        uint64_t data_trb, status_trb;
        uint32_t residual;          /* bytes not transferred (short packet) */
        uint32_t code;
        bool done;
    } xfer;
    struct wait_queue waiters;      /* threads waiting for a command or transfer */
    struct mutex lock;              /* one command or control transfer at a time */
    /* (65a-4) The event ring's one consumer at a time (interrupt handler,
     * timer tick, waits), and what it reads that others change: transfers
     * registered before their doorbell, the endpoint list, the port bits.
     * Until 65a-4 interrupts off did this, which on one core is enough. */
    struct spinlock events_lock;
    struct xhci_device *slots[256]; /* the device in each slot, for bulk events */
    struct endpoint {               /* interrupt IN endpoints in use */
        uint32_t slot, dci;         /* device slot, device context index */
        struct usb_device *owner;   /* (37g-6) its device: slots change over a sleep */
        struct ring ring;
        uint64_t buffers_phys;      /* ENDPOINT_BUFFERS buffers of size bytes */
        uint32_t size;
        usb_data_handler handler;
        void *context;
        uint64_t errors;
    } endpoints[MAX_ENDPOINTS];
    int endpoint_count;
    /* Hot plug: the device on each root port, the ports with a change
     * event not yet looked at (a bit each; set by process_events), and the
     * usb thread that looks at them. */
    struct usb_device *root[256];
    volatile uint64_t changed[4];
    volatile bool hub_work;         /* a hub reported changes (usbhub.c) */
    struct wait_queue hotplug_waiters;
    /* (37g-2) What a start after a sleep needs again; and whether the
     * controller forgot its devices (reset: detaching sends no command). */
    uint64_t dcbaa_phys, erst_phys;
    bool port_power, lost;
    uint64_t sleeps;
    uint64_t kept, let_go;          /* (37g-6) devices kept over sleeps, let go of */
    char let_go_why[96];            /* (37g-8) the last one let go of, and why */
    struct mutex passes;            /* (37g-2) the usb thread's pass; a sleep waits for it */
};

/* A device as the controller sees it; the USB part comes first, so a
 * struct usb_device pointer is also one to this. */
struct xhci_device {
    struct usb_device usb;
    uint8_t *input, *output;        /* input context, output (device) context */
    uint64_t input_phys, output_phys;
    struct ring ep0;                /* transfer ring of the control endpoint */
    uint8_t *buffer;                /* one page for control transfer data */
    uint64_t buffer_phys;
    /* Bulk endpoints in use, each with its own transfer: a transfer waits
     * without the controller's lock, so a read the device leaves pending
     * (a network adapter with nothing to say) holds up nothing else. */
    struct bulk_endpoint {
        uint8_t address;            /* 0: unused */
        uint32_t dci;
        uint16_t max_packet;
        struct ring ring;
        struct mutex lock;          /* one transfer at a time on this endpoint */
        /* (65e) Its waiter, woken when its transfer ends (or the device
         * goes): not the controller's queue, which every event woke, so a
         * read left pending (a serial port) woke at each of another
         * device's thousands of events a second. */
        struct wait_queue waiters;
        /* The transfer in progress, written by process_events: its TRBs,
         * their data offsets and lengths, then the result. */
        volatile bool pending, done;
        volatile uint32_t count, length, code;
        uint64_t trbs[BULK_TRBS_MAX];
        uint32_t offsets[BULK_TRBS_MAX], lengths[BULK_TRBS_MAX];
        uint64_t before[BULK_TRBS_MAX];     /* (64s) the transfer before's TRBs */
        uint32_t before_count;
        struct usb_bulk_stats stats;    /* (64m) for the drivers' reports (net) */
    } bulk[BULK_ENDPOINTS];
    volatile bool gone;             /* unplugged: waiting transfers end */
};

static uint32_t op_read(struct xhci *c, uint32_t off)
{
    return mmio_read32(c->op + off);
}

static void op_write(struct xhci *c, uint32_t off, uint32_t v)
{
    mmio_write32(c->op + off, v);
}

/* 64-bit registers as two 32-bit writes, low half first: QEMU acts when the
 * high half arrives (CRCR, DCBAAP, ERSTBA). */
static void write64(volatile uint8_t *reg, uint64_t v)
{
    mmio_write32(reg, (uint32_t)v);
    mmio_write32(reg + 4, (uint32_t)(v >> 32));
}

static void *alloc_page(uint64_t *phys)
{
    *phys = pmm_alloc_page();
    if (!*phys)
        return 0;
    void *p = phys_to_virt(*phys);
    memset(p, 0, PMM_PAGE_SIZE);
    return p;
}

static bool wait_status(struct xhci *c, uint32_t mask, uint32_t want)
{
    for (uint32_t ms = 0; ms <= TIMEOUT_MS; ms++) {
        if ((op_read(c, OP_USBSTS) & mask) == want)
            return true;
        arch_delay_ms(1);
    }
    return false;
}


/* A ring the driver produces (the command ring): its last TRB links back to
 * the start and tells the controller to flip its cycle state there. */
static bool producer_ring_init(struct ring *r)
{
    r->trbs = alloc_page(&r->phys);
    if (!r->trbs)
        return false;
    r->trbs[RING_TRBS - 1].parameter = r->phys;
    r->trbs[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | LINK_TOGGLE_CYCLE;
    r->index = 0;
    r->cycle = 1;
    return true;
}

/* Put a TRB on a producer ring; returns its physical address. The control
 * word, with the cycle bit, is written last: it hands the TRB over. */
static uint64_t ring_push(struct ring *r, uint64_t parameter, uint32_t status, uint32_t control)
{
    volatile struct trb *t = &r->trbs[r->index];
    uint64_t address = r->phys + r->index * sizeof(struct trb);
    t->parameter = parameter;
    t->status = status;
    arch_write_barrier();
    t->control = control | r->cycle;
    if (++r->index == RING_TRBS - 1) {
        /* At the link TRB: hand it over too, and continue at the start
         * with the other cycle bit. */
        volatile struct trb *link = &r->trbs[r->index];
        arch_write_barrier();
        link->control = (link->control & ~TRB_CYCLE) | r->cycle;
        r->index = 0;
        r->cycle ^= 1;
    }
    return address;
}

/* The next event, or 0 if the controller has not written one yet. */
static volatile struct trb *next_event(struct xhci *c)
{
    volatile struct trb *e = &c->event.trbs[c->event.index];
    if ((e->control & TRB_CYCLE) != c->event.cycle)
        return 0;
    arch_read_barrier();                    /* the TRB's other fields after its cycle bit */
    return e;
}

/* A transfer on an interrupt endpoint finished: hand the data to the
 * device's driver and put the buffer back on the ring. */
static bool endpoint_event(struct xhci *c, volatile struct trb *e)
{
    uint32_t slot = e->control >> 24, dci = (e->control >> 16) & 0x1F;
    for (int i = 0; i < c->endpoint_count; i++) {
        struct endpoint *ep = &c->endpoints[i];
        if (ep->slot != slot || ep->dci != dci)
            continue;
        uint32_t code = e->status >> 24;
        if (code != COMPLETION_SUCCESS && code != COMPLETION_SHORT) {
            ep->errors++;                   /* the endpoint stops: no more transfers */
            return true;
        }
        /* The event names the TRB; the TRB names the buffer. */
        uint64_t index = (e->parameter - ep->ring.phys) / sizeof(struct trb);
        if (index >= RING_TRBS)
            return true;
        uint64_t buffer = ep->ring.trbs[index].parameter;
        uint32_t residual = e->status & 0xFFFFFF;
        uint32_t length = residual < ep->size ? ep->size - residual : 0;
        ep->handler(ep->context, phys_to_virt(buffer), length);
        ring_push(&ep->ring, buffer, ep->size, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
        arch_write_barrier();
        mmio_write32(c->db + 4 * slot, dci);
        return true;
    }
    return false;
}

/*
 * Tell the controller how far we got and clear "event handler busy" (EHB):
 * while it is set, the controller sends no interrupt. It sets EHB when it
 * raises an interrupt, which on real hardware can happen just after code
 * waiting with interrupts disabled has already handled the event: the
 * interrupt handler then finds nothing new and must still clear EHB, or the
 * interrupts stop (a J1900 PC lost most of them this way).
 */
static void acknowledge_events(struct xhci *c)
{
    uint64_t erdp = c->event.phys + c->event.index * sizeof(struct trb);
    /* High half first: QEMU acts on the low half. */
    mmio_write32(c->rt + RT_ERDP + 4, (uint32_t)(erdp >> 32));
    mmio_write32(c->rt + RT_ERDP, (uint32_t)erdp | ERDP_EHB);
}

/* Clear EHB only if it is set: a needless write made the J1900's controller
 * send a second, empty interrupt for every event. */
static void acknowledge_if_busy(struct xhci *c)
{
    if (mmio_read32(c->rt + RT_ERDP) & ERDP_EHB)
        acknowledge_events(c);
}

/* A transfer event of a bulk endpoint: a transfer ends with its last TRB,
 * or early with a short packet (that TRB's data minus what was left over),
 * a stall or an error. */
static bool bulk_event(struct xhci *c, volatile struct trb *e)
{
    uint32_t slot = e->control >> 24, dci = (e->control >> 16) & 0x1F;
    struct xhci_device *x = c->slots[slot & 0xFF];
    if (!x || dci == DOORBELL_EP0)
        return false;
    for (int i = 0; i < BULK_ENDPOINTS; i++) {
        struct bulk_endpoint *b = &x->bulk[i];
        if (!b->address || b->dci != dci)
            continue;
        if (!b->pending || b->done)
            return true;                    /* late news of a transfer given up */
        uint32_t code = e->status >> 24, residual = e->status & 0xFFFFFF;
        uint32_t k = 0;
        while (k < b->count && b->trbs[k] != e->parameter)
            k++;
        if (k == b->count && (code == COMPLETION_SUCCESS || code == COMPLETION_SHORT ||
                              code == COMPLETION_STOPPED || code == COMPLETION_STOPPED_LENGTH)) {
            /* (64m) An event for a TRB of an earlier transfer (a controller's
             * second event after a short packet, or one cut off): not this
             * transfer's. Until 64m any such event but a plain success was
             * taken as the current transfer's end, which left its TRBs on the
             * ring: every transfer after it was then matched with the one
             * before's event, and an NCM card received nothing again. */
            b->stats.stale++;
            uint32_t j = 0;
            while (j < b->before_count && b->before[j] != e->parameter)
                j++;
            b->stats.stale_trb = j < b->before_count ? (int32_t)j : -1;
            b->stats.stale_trbs = b->before_count;
            b->stats.stale_code = code;
            b->stats.stale_residual = residual;
            return true;
        }
        if (k < b->count && (code == COMPLETION_SHORT || code == COMPLETION_SUCCESS)) {
            uint32_t got = b->lengths[k] > residual ? b->lengths[k] - residual : 0;
            if (code == COMPLETION_SHORT || k == b->count - 1) {
                b->length = b->offsets[k] + got;
                b->code = COMPLETION_SUCCESS;
                b->done = true;
            }
        } else if (k < b->count || code != COMPLETION_SUCCESS) {
            b->code = code;
            b->done = true;
        }
        if (b->done) {
            b->stats.end_trb = k < b->count ? (int32_t)k : -1;
            b->stats.end_trbs = b->count;
            b->stats.end_code = code;
            b->stats.end_residual = residual;
            wait_queue_wake_all(&b->waiters);       /* (65e) its waiter alone */
        }
        return true;
    }
    return false;
}

/* Handle all pending events, then tell the controller how far we got
 * (which also clears "event handler busy", allowing the next interrupt).
 * The event ring has one consumer at a time: called with events_lock held. */
static void process_events(struct xhci *c)
{
    volatile struct trb *e;
    bool any = false;
    while ((e = next_event(c))) {
        uint32_t type = TRB_GET_TYPE(e->control);
        c->events++;
        if (type == TRB_TRANSFER_EVENT && endpoint_event(c, e)) {
            /* handled */
        } else if (type == TRB_COMMAND_DONE) {
            c->done_trb = e->parameter;
            c->done_code = e->status >> 24;
            c->done_slot = e->control >> 24;
        } else if (type == TRB_TRANSFER_EVENT && bulk_event(c, e)) {
            /* handled */
        } else if (type == TRB_TRANSFER_EVENT && (e->control >> 24) == c->xfer.slot &&
                   ((e->control >> 16) & 0x1F) == DOORBELL_EP0) {
            uint32_t code = e->status >> 24;
            if (e->parameter == c->xfer.data_trb && code == COMPLETION_SHORT) {
                c->xfer.residual = e->status & 0xFFFFFF;
            } else if (e->parameter == c->xfer.status_trb ||
                       (code != COMPLETION_SUCCESS && code != COMPLETION_SHORT)) {
                c->xfer.code = code;        /* finished, or failed on the way */
                c->xfer.done = true;
            }
        } else if (type == TRB_PORT_CHANGE) {
            c->port_events++;
            uint32_t port = (uint32_t)(e->parameter >> 24) & 0xFF;
            c->changed[port / 64] |= 1ULL << (port % 64);
            wait_queue_wake_all(&c->hotplug_waiters);
        }
        if (++c->event.index == RING_TRBS) {
            c->event.index = 0;
            c->event.cycle ^= 1;
        }
        any = true;
    }
    if (any)
        acknowledge_events(c);
}

static bool command_done(struct xhci *c, uint64_t trb)
{
    return c->done_trb == trb;
}

static bool transfer_done(struct xhci *c, uint64_t unused)
{
    (void)unused;
    return c->xfer.done;
}

/* Interrupt context: no console, no heap. */
static void xhci_interrupt(void *context)
{
    struct xhci *c = context;
    c->interrupt_count++;
    mmio_write32(c->rt + RT_IMAN, IMAN_IE | IMAN_IP);
    mmio_write32(c->op + OP_USBSTS, USBSTS_EINT);
    uint64_t state = spin_lock(&c->events_lock);
    process_events(c);
    acknowledge_if_busy(c);                 /* also when there was nothing new */
    spin_unlock(&c->events_lock, state);
    wait_queue_wake_all(&c->waiters);
}

/* Every timer tick: without interrupts, this is how events are found;
 * with them, a safety net (a lost interrupt then costs a tick, not a
 * timeout), counted so that lost interrupts show in the report. */
static void xhci_poll(void *context)
{
    struct xhci *c = context;
    uint64_t state = spin_lock(&c->events_lock);
    uint64_t before = c->events;
    process_events(c);
    if (c->interrupts) {
        c->tick_events += c->events - before;
        acknowledge_if_busy(c);
    }
    spin_unlock(&c->events_lock, state);
    wait_queue_wake_all(&c->waiters);
}

/* Wait until finished(c, arg), sleeping until the controller's interrupt;
 * other threads run meanwhile. False after a timeout. Without interrupts,
 * the event ring is checked continuously (a transfer would otherwise take
 * at least a timer tick, 10 ms), giving other threads their turn. */
/* The events so far, then whether finished(c, arg). */
static bool events_then(struct xhci *c, bool (*finished)(struct xhci *, uint64_t), uint64_t arg)
{
    uint64_t state = spin_lock(&c->events_lock);
    process_events(c);
    bool done = finished(c, arg);
    spin_unlock(&c->events_lock, state);
    return done;
}

static bool wait_ms(struct xhci *c, bool (*finished)(struct xhci *, uint64_t), uint64_t arg,
                    uint32_t timeout_ms)
{
    uint64_t deadline = timer_ticks() + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 2;
    if (c->interrupts)
        return wait_event_until(&c->waiters, events_then(c, finished, arg), deadline);
    for (;;) {
        bool done = events_then(c, finished, arg);
        if (done)
            return true;
        if (timer_ticks() >= deadline)
            return false;
        thread_yield();
    }
}

/* (65e) A bulk transfer's wait: on its endpoint's own queue. */
static bool bulk_done(struct xhci *c, uint64_t endpoint);
static bool wait_bulk(struct xhci *c, struct bulk_endpoint *b, uint32_t timeout_ms)
{
    if (!c->interrupts)
        return wait_ms(c, bulk_done, (uint64_t)(uintptr_t)b, timeout_ms);
    uint64_t deadline = timer_ticks() + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 2;
    return wait_event_until(&b->waiters, events_then(c, bulk_done, (uint64_t)(uintptr_t)b),
                            deadline);
}

static bool wait_for(struct xhci *c, bool (*finished)(struct xhci *, uint64_t), uint64_t arg)
{
    return wait_ms(c, finished, arg, TIMEOUT_MS);
}

static bool bulk_done(struct xhci *c, uint64_t endpoint)
{
    (void)c;
    struct bulk_endpoint *b = (struct bulk_endpoint *)(uintptr_t)endpoint;
    return b->done;
}

/* Run one command and wait for its completion event. Returns the completion
 * code (1 = success), or 0 on a timeout. */
static uint32_t command(struct xhci *c, uint64_t parameter, uint32_t status, uint32_t control)
{
    mutex_lock(&c->lock);
    uint64_t trb = ring_push(&c->cmd, parameter, status, control);
    arch_write_barrier();                   /* the TRB in RAM before the doorbell */
    mmio_write32(c->db, 0);                 /* doorbell 0, target 0: the command ring */
    c->commands++;
    uint32_t code = wait_for(c, command_done, trb) ? c->done_code : 0;
    mutex_unlock(&c->lock);
    return code;
}

/* --- devices ------------------------------------------------------------------ */

static volatile uint32_t *context(uint8_t *base, unsigned index, uint32_t size)
{
    return (volatile uint32_t *)(base + index * size);
}

static int control_locked(struct usb_device *dev, uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index, void *data, uint16_t length);

int usb_control(struct usb_device *dev, uint8_t request_type, uint8_t request, uint16_t value,
                uint16_t index, void *data, uint16_t length)
{
    struct xhci *c = dev->hc;
    mutex_lock(&c->lock);
    int n = control_locked(dev, request_type, request, value, index, data, length);
    mutex_unlock(&c->lock);
    return n;
}

static int control_locked(struct usb_device *dev, uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index, void *data, uint16_t length)
{
    struct xhci_device *x = (struct xhci_device *)dev;
    struct xhci *c = dev->hc;
    bool in = request_type & USB_DIR_IN;
    if (length > PMM_PAGE_SIZE)
        return -1;
    if (!in && length)
        memcpy(x->buffer, data, length);

    /* Three stages: the 8-byte setup packet (carried in the TRB itself),
     * the data, and a status handshake in the opposite direction. */
    c->xfer.slot = dev->slot;
    c->xfer.residual = 0;
    c->xfer.code = 0;
    c->xfer.done = false;
    uint64_t setup = request_type | (uint64_t)request << 8 | (uint64_t)value << 16 |
                     (uint64_t)index << 32 | (uint64_t)length << 48;
    ring_push(&x->ep0, setup, 8, TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT |
              (length ? (in ? SETUP_IN_DATA : 2u << 16) : SETUP_NO_DATA));
    c->xfer.data_trb = length ? ring_push(&x->ep0, x->buffer_phys, length,
                                          TRB_TYPE(TRB_DATA_STAGE) | TRB_ISP |
                                          (in ? TRB_DIR_IN : 0))
                              : 0;
    c->xfer.status_trb = ring_push(&x->ep0, 0, 0, TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC |
                                                  (in && length ? 0 : TRB_DIR_IN));
    arch_write_barrier();
    mmio_write32(c->db + 4 * dev->slot, DOORBELL_EP0);

    bool finished = wait_for(c, transfer_done, 0);
    c->xfer.slot = 0;
    if (!finished || c->xfer.code != COMPLETION_SUCCESS)
        return -1;
    int done = length - (int)c->xfer.residual;
    if (in && done > 0)
        memcpy(data, x->buffer, (size_t)done);
    return done;
}

/* The endpoint context's interval: 2^value x 125 us. Descriptors give it
 * in frames (1 ms) at full and low speed, as an exponent from high speed on. */
static uint32_t endpoint_interval(unsigned speed, uint8_t interval)
{
    if (speed == USB_SPEED_FULL || speed == USB_SPEED_LOW) {
        uint32_t e = 3;                     /* 8 x 125 us = 1 ms */
        while (e < 10 && (1u << (e + 1)) <= interval * 8u)
            e++;
        return e;
    }
    return interval >= 1 && interval <= 16 ? interval - 1u : 3;
}

static bool interrupt_in_locked(struct usb_device *dev, const struct usb_endpoint_descriptor *desc,
                                usb_data_handler handler, void *handler_context);

bool usb_interrupt_in(struct usb_device *dev, const struct usb_endpoint_descriptor *desc,
                      usb_data_handler handler, void *handler_context)
{
    struct xhci *c = dev->hc;
    mutex_lock(&c->lock);
    bool ok = interrupt_in_locked(dev, desc, handler, handler_context);
    mutex_unlock(&c->lock);
    return ok;
}

static bool interrupt_in_locked(struct usb_device *dev, const struct usb_endpoint_descriptor *desc,
                                usb_data_handler handler, void *handler_context)
{
    struct xhci_device *x = (struct xhci_device *)dev;
    struct xhci *c = dev->hc;
    uint32_t size = desc->max_packet & 0x7FF;
    uint32_t dci = (desc->address & 0xF) * 2 + 1;           /* IN endpoints are odd */
    if (c->endpoint_count == MAX_ENDPOINTS || !(desc->address & 0x80) || !size ||
        size > ENDPOINT_BUFFER_MAX)
        return false;
    struct endpoint *ep = &c->endpoints[c->endpoint_count];
    uint64_t buffers_phys;
    if (!producer_ring_init(&ep->ring) || !alloc_page(&buffers_phys))
        return false;

    /* Configure Endpoint: the slot context (copied from the controller's,
     * with more context entries) and the new endpoint's context. */
    uint32_t cs = c->context_size;
    volatile uint32_t *control = context(x->input, CTX_INPUT_CONTROL, cs);
    control[0] = 0;
    control[1] = 1u << 0 | 1u << dci;
    volatile uint32_t *slot_in = context(x->input, CTX_IN_SLOT, cs);
    volatile uint32_t *slot_out = context(x->output, CTX_OUT_SLOT, cs);
    for (int i = 0; i < 4; i++)
        slot_in[i] = slot_out[i];
    if ((slot_in[0] >> 27) < dci)
        slot_in[0] = (slot_in[0] & 0x07FFFFFF) | dci << 27;
    slot_in[3] = 0;                                         /* address and state: output only */
    volatile uint32_t *ctx = context(x->input, dci + 1, cs);
    ctx[0] = endpoint_interval(dev->speed, desc->interval) << 16;
    ctx[1] = 3u << 1 | EP_TYPE_INTERRUPT_IN << 3 | size << 16;     /* 3 retries */
    ctx[2] = (uint32_t)ep->ring.phys | 1;
    ctx[3] = (uint32_t)(ep->ring.phys >> 32);
    ctx[4] = size | size << 16;             /* average TRB length, max payload per interval */
    arch_write_barrier();
    uint32_t code = command(c, x->input_phys, 0, TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
                                                 TRB_SLOT(dev->slot));
    if (code != COMPLETION_SUCCESS) {
        kprintf("                 Configure Endpoint failed (completion code %u)\n", code);
        return false;
    }

    ep->slot = dev->slot;
    ep->owner = dev;
    ep->dci = dci;
    ep->buffers_phys = buffers_phys;
    ep->size = size;
    ep->handler = handler;
    ep->context = handler_context;
    /* Keep transfers queued: the controller polls the device at the
     * endpoint's interval and completes one per report. Registered before
     * the doorbell, under the event lock, so no event can come too early. */
    uint64_t state = spin_lock(&c->events_lock);
    c->endpoint_count++;
    for (uint32_t i = 0; i < ENDPOINT_BUFFERS; i++)
        ring_push(&ep->ring, buffers_phys + i * ENDPOINT_BUFFER_MAX, size,
                  TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    arch_write_barrier();
    mmio_write32(c->db + 4 * dev->slot, dci);
    spin_unlock(&c->events_lock, state);
    return true;
}

/* --- bulk endpoints ------------------------------------------------------------ */

bool usb_bulk_configure(struct usb_device *dev, const struct usb_endpoint_descriptor *in,
                        const struct usb_endpoint_descriptor *out, unsigned in_burst,
                        unsigned out_burst)
{
    struct xhci_device *x = (struct xhci_device *)dev;
    struct xhci *c = dev->hc;
    const struct usb_endpoint_descriptor *desc[2] = { in, out };
    unsigned burst[2] = { in_burst, out_burst };
    uint32_t cs = c->context_size;
    mutex_lock(&c->lock);
    volatile uint32_t *control = context(x->input, CTX_INPUT_CONTROL, cs);
    control[0] = 0;
    control[1] = 1u << 0;
    volatile uint32_t *slot_in = context(x->input, CTX_IN_SLOT, cs);
    volatile uint32_t *slot_out = context(x->output, CTX_OUT_SLOT, cs);
    for (int i = 0; i < 4; i++)
        slot_in[i] = slot_out[i];
    slot_in[3] = 0;
    bool ok = true;
    struct bulk_endpoint *taken[2] = { 0, 0 };
    for (int i = 0; i < 2 && ok; i++) {
        /* a free entry (a composite device's functions each take two) */
        struct bulk_endpoint *b = 0;
        for (int k = 0; k < BULK_ENDPOINTS && !b; k++)
            if (!x->bulk[k].address)
                b = &x->bulk[k];
        if (!b) {
            kprintf("                 more than %u bulk endpoints\n", BULK_ENDPOINTS);
            ok = false;
            break;
        }
        taken[i] = b;
        bool is_in = desc[i]->address & 0x80;
        b->address = desc[i]->address;
        b->lock = (struct mutex)MUTEX_INIT("usb bulk");
        b->pending = b->done = false;
        b->dci = (desc[i]->address & 0xF) * 2 + (is_in ? 1 : 0);
        b->max_packet = desc[i]->max_packet & 0x7FF;
        ok = producer_ring_init(&b->ring);
        if (!ok)
            break;
        control[1] |= 1u << b->dci;
        if ((slot_in[0] >> 27) < b->dci)
            slot_in[0] = (slot_in[0] & 0x07FFFFFF) | b->dci << 27;
        volatile uint32_t *ctx = context(x->input, b->dci + 1, cs);
        ctx[0] = 0;
        ctx[1] = 3u << 1 | (is_in ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT) << 3 |
                 (burst[i] & 0xFF) << 8 | (uint32_t)b->max_packet << 16;   /* 3 retries */
        ctx[2] = (uint32_t)b->ring.phys | 1;
        ctx[3] = (uint32_t)(b->ring.phys >> 32);
        ctx[4] = 3072;                      /* average TRB length (xHCI's advice for bulk) */
    }
    uint32_t code = 0;
    if (ok) {
        arch_write_barrier();
        code = command(c, x->input_phys, 0, TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
                                            TRB_SLOT(dev->slot));
    }
    if (!ok || code != COMPLETION_SUCCESS)
        for (int i = 0; i < 2; i++)
            if (taken[i])
                taken[i]->address = 0;      /* the entry is free again (its ring page stays) */
    mutex_unlock(&c->lock);
    if (ok && code != COMPLETION_SUCCESS)
        kprintf("                 Configure Endpoint (bulk) failed (completion code %u)\n", code);
    return ok && code == COMPLETION_SUCCESS;
}

static struct bulk_endpoint *bulk_endpoint(struct xhci_device *x, uint8_t address)
{
    for (int i = 0; i < BULK_ENDPOINTS; i++)
        if (x->bulk[i].address == address)
            return &x->bulk[i];
    return 0;
}

/* Give up a transfer the device never finished: stop the endpoint, then
 * move its dequeue pointer past the abandoned TRBs (to where the next
 * transfer goes), so they can never complete into a later one. */
static void cancel_transfer(struct xhci *c, struct usb_device *dev, struct bulk_endpoint *b)
{
    mutex_lock(&c->lock);
    uint32_t code = command(c, 0, 0, TRB_TYPE(TRB_STOP_ENDPOINT) | TRB_SLOT(dev->slot) |
                                     TRB_ENDPOINT(b->dci));
    if (code == COMPLETION_CONTEXT_STATE)       /* (64m) it had halted: reset it first */
        code = command(c, 0, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | TRB_SLOT(dev->slot) |
                                TRB_ENDPOINT(b->dci));
    uint64_t dequeue = (b->ring.phys + b->ring.index * sizeof(struct trb)) | b->ring.cycle;
    if (code == COMPLETION_SUCCESS)
        code = command(c, dequeue, 0, TRB_TYPE(TRB_SET_TR_DEQUEUE) | TRB_SLOT(dev->slot) |
                                      TRB_ENDPOINT(b->dci));
    b->stats.cancels++;
    if (code != COMPLETION_SUCCESS)
        b->stats.failed_commands++;
    b->pending = false;
    mutex_unlock(&c->lock);
}

/* (64m) After an error that halts the endpoint (a transaction error after
 * its retries, babble, ...: anything but a stall, which the driver clears
 * with the device too): the controller's side only, Reset Endpoint, then
 * the dequeue pointer to where the next transfer goes. Until 64m only a
 * stall was reset, and a halted endpoint never ran another transfer (an
 * NCM read waits for ever, without a message). */
static void reset_halted(struct xhci *c, struct usb_device *dev, struct bulk_endpoint *b)
{
    mutex_lock(&c->lock);
    uint32_t code = command(c, 0, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | TRB_SLOT(dev->slot) |
                                     TRB_ENDPOINT(b->dci));
    uint64_t dequeue = (b->ring.phys + b->ring.index * sizeof(struct trb)) | b->ring.cycle;
    if (code == COMPLETION_SUCCESS)
        code = command(c, dequeue, 0, TRB_TYPE(TRB_SET_TR_DEQUEUE) | TRB_SLOT(dev->slot) |
                                      TRB_ENDPOINT(b->dci));
    b->stats.resets++;
    if (code != COMPLETION_SUCCESS)
        b->stats.failed_commands++;
    mutex_unlock(&c->lock);
}

int usb_bulk_transfer_timeout(struct usb_device *dev, uint8_t address, uint64_t phys,
                              uint32_t length, bool *stalled, uint32_t timeout_ms)
{
    struct xhci_device *x = (struct xhci_device *)dev;
    struct xhci *c = dev->hc;
    struct bulk_endpoint *b = bulk_endpoint(x, address);
    *stalled = false;
    if (!b || x->gone)
        return -1;
    mutex_lock(&b->lock);
    /* The TRBs go on the ring under the controller's lock (the doorbell and
     * the ring's state are shared with commands); the wait is without it. */
    mutex_lock(&c->lock);
    memcpy(b->before, b->trbs, b->count * sizeof(b->trbs[0]));
    b->before_count = b->count;
    b->count = 0;
    b->length = 0;
    b->done = false;
    /* The buffer is physically contiguous: one TRB per piece up to the next
     * 64 KiB boundary, which a TRB's data must not cross (as Linux). (64s)
     * Until 64s each page had a TRB of its own, and the AMD laptop's
     * controller, after a short packet ended a transfer in one of the
     * first TRBs, sometimes wrote the next transfer's start into a later
     * TRB of the one it had finished: an NCM read then began inside an
     * NTB whose header had gone into the read before's buffer (1 read in
     * 700 when downloading from the PlutoSDR). TD Size: the packets still
     * to come after it. */
    uint32_t done = 0;
    uint32_t count = 0;
    bool fits = (length + PAGE_SIZE - 1) / PAGE_SIZE + 1 <= BULK_TRBS_MAX;
    while (fits && done < length) {
        uint64_t at = phys + done;
        uint32_t piece = TRB_SPAN - (uint32_t)(at % TRB_SPAN);
        if (piece > length - done)
            piece = length - done;
        bool last = done + piece == length;
        uint32_t packets_left = (length - done - piece + b->max_packet - 1) / b->max_packet;
        b->offsets[count] = done;
        b->lengths[count] = piece;
        b->trbs[count] = ring_push(&b->ring, at,
                                   piece | (packets_left < 31 ? packets_left : 31) << 17,
                                   TRB_TYPE(TRB_NORMAL) | TRB_ISP | (last ? TRB_IOC : TRB_CHAIN));
        count++;
        done += piece;
    }
    if (length == 0) {                      /* (64m) a zero-length packet: one empty TRB */
        b->offsets[0] = 0;
        b->lengths[0] = 0;
        b->trbs[0] = ring_push(&b->ring, phys, 0, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
        count = 1;
    }
    if (!fits) {
        mutex_unlock(&c->lock);
        mutex_unlock(&b->lock);
        return -1;                          /* longer than BULK_TRBS_MAX pages */
    }
    /* Complete before the doorbell, under the event lock: no event can come
     * for TRBs the event handler does not know yet. */
    uint64_t irq = spin_lock(&c->events_lock);
    b->count = count;
    b->pending = true;
    arch_write_barrier();
    mmio_write32(c->db + 4 * dev->slot, b->dci);
    spin_unlock(&c->events_lock, irq);
    mutex_unlock(&c->lock);

    bool finished;
    if (timeout_ms) {
        finished = wait_bulk(c, b, timeout_ms);
    } else {
        /* No limit: data comes when the device has some (a network
         * adapter's read); an unplug ends the wait. */
        while (!(finished = wait_bulk(c, b, 1000)) && !x->gone)
            ;
    }
    if (!finished) {
        if (!x->gone)
            cancel_transfer(c, dev, b);
        b->pending = false;
        b->stats.timeouts++;
        mutex_unlock(&b->lock);
        return -1;
    }
    b->pending = false;
    int result = b->code == COMPLETION_SUCCESS ? (int)b->length : -1;
    *stalled = b->code == COMPLETION_STALL;
    b->stats.transfers++;
    if (b->code != COMPLETION_SUCCESS) {
        b->stats.errors++;
        b->stats.last_error = b->code;
        if (b->code != COMPLETION_STALL && !x->gone)
            reset_halted(c, dev, b);        /* (a stall: the driver clears it, both sides) */
    }
    mutex_unlock(&b->lock);
    return result;
}

int usb_bulk_transfer(struct usb_device *dev, uint8_t address, uint64_t phys, uint32_t length,
                      bool *stalled)
{
    return usb_bulk_transfer_timeout(dev, address, phys, length, stalled, BULK_TIMEOUT_MS);
}

bool usb_bulk_stats(struct usb_device *dev, uint8_t address, struct usb_bulk_stats *out)
{
    struct bulk_endpoint *b = bulk_endpoint((struct xhci_device *)dev, address);
    if (!b)
        return false;
    *out = b->stats;
    return true;
}

bool usb_bulk_clear_halt(struct usb_device *dev, uint8_t address)
{
    /* The controller's side (Reset Endpoint, then continue the ring where
     * it is), then the device's (CLEAR_FEATURE ENDPOINT_HALT). */
    struct xhci_device *x = (struct xhci_device *)dev;
    struct xhci *c = dev->hc;
    struct bulk_endpoint *b = bulk_endpoint(x, address);
    if (!b)
        return false;
    mutex_lock(&c->lock);
    uint32_t code = command(c, 0, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | TRB_SLOT(dev->slot) |
                                     TRB_ENDPOINT(b->dci));
    uint64_t dequeue = (b->ring.phys + b->ring.index * sizeof(struct trb)) | b->ring.cycle;
    if (code == COMPLETION_SUCCESS)
        code = command(c, dequeue, 0, TRB_TYPE(TRB_SET_TR_DEQUEUE) | TRB_SLOT(dev->slot) |
                                      TRB_ENDPOINT(b->dci));
    mutex_unlock(&c->lock);
    return code == COMPLETION_SUCCESS &&
           usb_control(dev, USB_RECIPIENT_ENDPOINT, USB_REQ_CLEAR_FEATURE, USB_FEATURE_HALT,
                       address, 0, 0) >= 0;
}

/* The packet size of endpoint 0 until the device descriptor says otherwise. */
static uint16_t default_max_packet0(unsigned speed)
{
    switch (speed) {
    case USB_SPEED_HIGH:       return 64;
    case USB_SPEED_SUPER:
    case USB_SPEED_SUPER_PLUS: return 512;
    default:                   return 8;
    }
}

/* USB 2 ports need a reset before the device answers; it also tells the
 * device's speed. USB 3 ports have already trained their link. */
static bool reset_port(struct xhci *c, uint32_t port)
{
    uint32_t sc = op_read(c, OP_PORTSC(port));
    op_write(c, OP_PORTSC(port), (sc & PORTSC_KEEP) | PORTSC_PR);
    for (int ms = 0; ms < 500; ms++) {
        sc = op_read(c, OP_PORTSC(port));
        if (sc & PORTSC_PRC)
            break;
        arch_delay_ms(1);
    }
    op_write(c, OP_PORTSC(port), (sc & PORTSC_KEEP) | PORTSC_CHANGES);
    timer_sleep_ms(10);                     /* reset recovery time (USB 2.0, 7.1.7.5) */
    return op_read(c, OP_PORTSC(port)) & PORTSC_PED;
}

static void *device_fail(struct xhci_device *x, const char *what, uint32_t code)
{
    kprintf("                 %s failed (completion code %u)\n", what, code);
    (void)x;                                /* its pages stay allocated: rare, small */
    return 0;
}

/* Bring a device to the "addressed" state and read what it is: on root
 * port `port` (parent 0) or on port `port` of hub parent. */
static struct usb_device *attach(struct xhci *c, struct usb_device *parent, uint32_t port,
                                 uint32_t speed)
{
    /* A device slot: the controller's record of one device. */
    uint32_t code = command(c, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    if (code != COMPLETION_SUCCESS)
        return device_fail(0, "Enable Slot", code);
    uint32_t slot = c->done_slot;

    struct xhci_device *x = kzalloc(sizeof(*x));
    if (!x || !(x->input = alloc_page(&x->input_phys)) ||
        !(x->output = alloc_page(&x->output_phys)) || !producer_ring_init(&x->ep0) ||
        !(x->buffer = alloc_page(&x->buffer_phys))) {
        kprintf("                 out of memory\n");
        return 0;
    }
    struct usb_device *dev = &x->usb;
    c->slots[slot & 0xFF] = x;
    dev->hc = c;
    dev->port = (uint8_t)port;
    dev->speed = (uint8_t)speed;
    dev->slot = (uint8_t)slot;
    dev->max_packet0 = default_max_packet0(speed);
    dev->parent = parent;
    if (parent) {
        /* The route string: this hub port on the next level (4 bits; ports
         * above 15 cannot be named and use 15). */
        dev->depth = parent->depth + 1;
        dev->root_port = parent->root_port;
        dev->route = parent->route | (port < 15 ? port : 15) << (4 * parent->depth);
        ksnprintf(dev->name, sizeof(dev->name), "%s.%u", parent->name, port);
        /* Low and full speed behind a high speed hub: its TT converts. */
        bool slow = speed == USB_SPEED_LOW || speed == USB_SPEED_FULL;
        if (slow && parent->speed == USB_SPEED_HIGH) {
            dev->tt_slot = parent->slot;
            dev->tt_port = (uint8_t)port;
            dev->tt_multi = parent->hub_multi_tt;
        } else {
            dev->tt_slot = parent->tt_slot;
            dev->tt_port = parent->tt_port;
            dev->tt_multi = parent->tt_multi;
        }
    } else {
        dev->root_port = (uint8_t)port;
        ksnprintf(dev->name, sizeof(dev->name), "%s port %u", c->name, port);
    }

    /* Input context: add the slot context and endpoint 0. */
    uint32_t cs = c->context_size;
    context(x->input, CTX_INPUT_CONTROL, cs)[1] = 1u << 0 | 1u << 1;
    volatile uint32_t *slot_ctx = context(x->input, CTX_IN_SLOT, cs);
    slot_ctx[0] = dev->route | speed << 20 | (dev->tt_multi ? 1u << 25 : 0) |
                  1u << 27;                 /* one context entry (endpoint 0) */
    slot_ctx[1] = (uint32_t)dev->root_port << 16;
    slot_ctx[2] = dev->tt_slot | (uint32_t)dev->tt_port << 8;
    volatile uint32_t *ep0 = context(x->input, CTX_IN_EP0, cs);
    ep0[1] = 3u << 1 | EP_TYPE_CONTROL << 3 | (uint32_t)dev->max_packet0 << 16;   /* 3 retries */
    ep0[2] = (uint32_t)x->ep0.phys | 1;     /* dequeue pointer, cycle state 1 */
    ep0[3] = (uint32_t)(x->ep0.phys >> 32);
    ep0[4] = 8;                             /* average TRB length: setup packets */
    c->dcbaa[slot] = x->output_phys;

    /* Address Device: the controller sends SET_ADDRESS to the device. */
    arch_write_barrier();
    code = command(c, x->input_phys, 0, TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(slot));
    if (code != COMPLETION_SUCCESS)
        return device_fail(x, "Address Device", code);
    dev->address = (uint8_t)context(x->output, CTX_OUT_SLOT, cs)[3];

    /* At full speed, endpoint 0 may take 8, 16, 32 or 64 bytes per packet:
     * the first 8 bytes of the device descriptor say which. */
    if (speed == USB_SPEED_FULL) {
        uint8_t head[8];
        if (usb_control(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, head,
                        8) == 8 && head[7] != dev->max_packet0 && head[7] >= 8) {
            dev->max_packet0 = head[7];
            context(x->input, CTX_INPUT_CONTROL, cs)[1] = 1u << 1;
            ep0[1] = (ep0[1] & 0xFFFF) | (uint32_t)dev->max_packet0 << 16;
            arch_write_barrier();
            code = command(c, x->input_phys, 0, TRB_TYPE(TRB_EVALUATE_CONTEXT) | TRB_SLOT(slot));
            if (code != COMPLETION_SUCCESS)
                return device_fail(x, "Evaluate Context", code);
        }
    }
    kprintf("                 slot %u, address %u, endpoint 0: %u-byte packets%s\n", slot,
            dev->address, dev->max_packet0, dev->tt_slot ? ", through a hub's TT" : "");
    usb_device_added(dev);
    return dev;
}

/* A device on a root port: USB 2 ports need a reset first. */
static struct usb_device *attach_port(struct xhci *c, uint32_t port, uint32_t usb_major)
{
    if (usb_major < 3 && !reset_port(c, port)) {
        kprintf(", port reset failed\n");
        return 0;
    }
    uint32_t speed = PORTSC_SPEED(op_read(c, OP_PORTSC(port)));
    kprintf("%s, %s\n", usb_major < 3 ? ", port reset" : "", usb_speed_name(speed));
    struct usb_device *dev = attach(c, 0, port, speed);
    c->root[port] = dev;
    return dev;
}

/* --- hot plug -------------------------------------------------------------------- */

static const struct protocol *port_protocol(struct xhci *c, uint32_t port);

/*
 * A device is gone (unplugged, or its hub is): first whatever hangs behind
 * it, then its interrupt endpoints leave the list (the interrupt handler
 * must not call its driver any more), its drivers end, and the controller
 * frees its slot. Its memory stays: other threads may still hold pointers
 * (a disk's I/O ending with an error); a few pages per unplug.
 */
static void detach(struct xhci *c, struct usb_device *dev)
{
    struct usb_device *children[16];
    int n = 0;
    for (int i = 0; i < usb_device_count() && n < 16; i++)
        if (usb_device_at(i)->parent == dev)
            children[n++] = usb_device_at(i);
    for (int i = 0; i < n; i++)
        detach(c, children[i]);

    uint64_t irq = spin_lock(&c->events_lock);
    int kept = 0;
    for (int i = 0; i < c->endpoint_count; i++)
        if (c->endpoints[i].owner != dev)
            c->endpoints[kept++] = c->endpoints[i];
    c->endpoint_count = kept;
    ((struct xhci_device *)dev)->gone = true;   /* bulk transfers waiting end */
    spin_unlock(&c->events_lock, irq);
    wait_queue_wake_all(&c->waiters);
    for (int i = 0; i < BULK_ENDPOINTS; i++)    /* (65e) and those on their own queues */
        wait_queue_wake_all(&((struct xhci_device *)dev)->bulk[i].waiters);

    usb_log_label(dev->name);
    kprintf("device disconnected (%s)\n", dev->product[0] ? dev->product : dev->function);
    usb_device_removed(dev);
    uint32_t code = c->lost ? COMPLETION_SUCCESS :
                    command(c, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(dev->slot));
    if (code != COMPLETION_SUCCESS)
        kprintf("                 Disable Slot %u failed (completion code %u)\n", dev->slot, code);
    if (c->slots[dev->slot] == (struct xhci_device *)dev) {    /* (37g-6: a slot number */
        c->dcbaa[dev->slot] = 0;                            /* from before a sleep may */
        c->slots[dev->slot] = 0;                            /* be another's now) */
    }
    if (!dev->parent)
        c->root[dev->port] = 0;
}

/* A root port reported a change: compare what is connected with what we
 * have, and attach or detach. A connection change while a device is there
 * means it was replaced faster than we looked. */
static void port_changed(struct xhci *c, uint32_t port)
{
    uint32_t sc = op_read(c, OP_PORTSC(port));
    op_write(c, OP_PORTSC(port), (sc & PORTSC_KEEP) | (sc & PORTSC_CHANGES));
    struct usb_device *dev = c->root[port];
    bool connected = sc & PORTSC_CCS;
    if (dev && (!connected || (sc & PORTSC_CSC)))
        detach(c, dev);
    if (!connected || c->root[port])
        return;
    /* Contacts bounce while a plug goes in: wait for the connection to
     * settle (USB 2.0, 7.1.7.3: 100 ms). */
    timer_sleep_ms(100);
    sc = op_read(c, OP_PORTSC(port));
    op_write(c, OP_PORTSC(port), (sc & PORTSC_KEEP) | (sc & PORTSC_CHANGES));
    if (!(sc & PORTSC_CCS))
        return;
    const struct protocol *p = port_protocol(c, port);
    uint32_t major = p ? p->major : 2;
    char label[32];
    ksnprintf(label, sizeof(label), "%s port %u", c->name, port);
    usb_log_label(label);
    kprintf("USB %x device connected", major);
    attach_port(c, port, major);            /* continues the line */
}

static bool any_changed(struct xhci *c)
{
    return c->changed[0] | c->changed[1] | c->changed[2] | c->changed[3] || c->hub_work;
}

void usb_hotplug_wake(struct usb_device *dev)
{
    struct xhci *c = dev->hc;
    c->hub_work = true;
    wait_queue_wake_all(&c->hotplug_waiters);
}

void usb_detach(struct usb_device *dev)
{
    detach(dev->hc, dev);
}

/* The usb thread: plugging in and unplugging, outside interrupt context
 * (setting a device up waits for many transfers). */
static void hotplug(void *arg)
{
    struct xhci *c = arg;
    for (;;) {
        wait_event(&c->hotplug_waiters, any_changed(c));
        uint64_t changed[4];
        uint64_t irq = spin_lock(&c->events_lock);
        for (int i = 0; i < 4; i++) {
            changed[i] = c->changed[i];
            c->changed[i] = 0;
        }
        bool hubs = c->hub_work;
        c->hub_work = false;
        spin_unlock(&c->events_lock, irq);
        /* (37g-2) A pass holds `passes`: a sleep starts between passes, so
         * none goes on after it with what it read of the devices before. */
        mutex_lock(&c->passes);
        for (uint32_t port = 1; port <= c->max_ports; port++)
            if (changed[port / 64] & (1ULL << (port % 64)))
                port_changed(c, port);
        if (hubs)
            usbhub_work(c);
        mutex_unlock(&c->passes);
    }
}

int xhci_attach_new(struct xhci *c)
{
    int attached = 0;
    for (uint32_t port = 1; port <= c->max_ports; port++) {
        if (!(op_read(c, OP_PORTSC(port)) & PORTSC_CCS) || c->root[port])
            continue;
        port_changed(c, port);
        attached += c->root[port] != 0;
    }
    return attached;
}

bool xhci_start_hotplug(struct xhci *c)
{
    /* Look at every port once: something may have changed during the
     * boot, after the ports were scanned. */
    uint64_t irq = spin_lock(&c->events_lock);
    for (uint32_t port = 1; port <= c->max_ports; port++)
        c->changed[port / 64] |= 1ULL << (port % 64);
    spin_unlock(&c->events_lock, irq);
    return thread_create("usb", hotplug, c) != 0;
}

struct usb_device *usb_attach(struct usb_device *hub, unsigned port, unsigned speed)
{
    return attach(hub->hc, hub, port, speed);
}

bool usb_hub_set(struct usb_device *dev, unsigned ports, unsigned think_time, bool multi_tt)
{
    /* Configure Endpoint with only the slot context: Hub, Number of Ports,
     * TT Think Time and MTT are evaluated there (xHCI 1.2, 6.2.2). */
    struct xhci_device *x = (struct xhci_device *)dev;
    struct xhci *c = dev->hc;
    uint32_t cs = c->context_size;
    mutex_lock(&c->lock);
    volatile uint32_t *control = context(x->input, CTX_INPUT_CONTROL, cs);
    control[0] = 0;
    control[1] = 1u << 0;
    volatile uint32_t *slot_in = context(x->input, CTX_IN_SLOT, cs);
    volatile uint32_t *slot_out = context(x->output, CTX_OUT_SLOT, cs);
    for (int i = 0; i < 4; i++)
        slot_in[i] = slot_out[i];
    slot_in[0] |= 1u << 26 | (multi_tt ? 1u << 25 : 0);            /* a hub */
    slot_in[1] = (slot_in[1] & 0x00FFFFFF) | ports << 24;
    slot_in[2] = (slot_in[2] & ~(3u << 16)) | (think_time & 3) << 16;
    slot_in[3] = 0;
    arch_write_barrier();
    uint32_t code = command(c, x->input_phys, 0, TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
                                                 TRB_SLOT(dev->slot));
    mutex_unlock(&c->lock);
    if (code != COMPLETION_SUCCESS)
        kprintf("                 Configure Endpoint (hub) failed (completion code %u)\n", code);
    return code == COMPLETION_SUCCESS;
}

/* Walk the extended capabilities: take the controller over from the
 * firmware, and learn which ports speak which USB version. */
static void read_extended_caps(struct xhci *c, uint32_t hccparams1)
{
    uint32_t offset = (hccparams1 >> 16) * 4;
    for (int n = 0; offset && n < 64; n++) {
        volatile uint8_t *cap = c->cap + offset;
        uint32_t head = mmio_read32(cap);
        uint8_t id = head & 0xFF;
        if (id == XCAP_LEGACY && (head & LEGACY_BIOS_OWNED)) {
            mmio_write32(cap, head | LEGACY_OS_OWNED);
            for (int ms = 0; ms < TIMEOUT_MS && (mmio_read32(cap) & LEGACY_BIOS_OWNED); ms++)
                arch_delay_ms(1);
            kprintf("                 taken over from the firmware: %s\n",
                    mmio_read32(cap) & LEGACY_BIOS_OWNED ? "no answer" : "ok");
        } else if (id == XCAP_PROTOCOL && c->protocol_count < MAX_PROTOCOLS) {
            uint32_t ports = mmio_read32(cap + 8);
            struct protocol *p = &c->protocols[c->protocol_count++];
            p->major = (uint8_t)(head >> 24);
            p->minor = (uint8_t)(head >> 16);
            p->first = (uint8_t)ports;
            p->count = (uint8_t)(ports >> 8);
        }
        uint32_t next = (head >> 8) & 0xFF;
        offset = next ? offset + next * 4 : 0;
    }
}

/*
 * Intel chipsets of the EHCI-to-xHCI transition (7/8/9 series, Bay Trail)
 * wire each USB socket to the EHCI or the xHCI controller. Firmware may
 * leave them on EHCI (a real J1900 PC did: the stick and the keyboard were
 * invisible here). The xHCI controller's PCI configuration space has the
 * switches and, set by the firmware, the masks of the sockets it allows to
 * switch. Returns true if sockets moved.
 */
#define PCI_XUSB2PR         0xD0    /* USB 2 sockets routed to xHCI */
#define PCI_XUSB2PRM        0xD4    /* ... which may be */
#define PCI_USB3_PSSEN      0xD8    /* USB 3 (SuperSpeed) enabled on sockets */
#define PCI_USB3PRM         0xDC    /* ... which may be */

static bool intel_switchable(const struct pci_device *pci)
{
    static const uint16_t ids[] = {
        0x1E31,                     /* 7 series (Panther Point) */
        0x8C31, 0x9C31,             /* 8 series (Lynx Point, -LP) */
        0x8CB1, 0x9CB1,             /* 9 series (Wildcat Point, -LP) */
        0x0F35,                     /* Atom E3800 / Celeron J1900 (Bay Trail) */
    };
    if (pci->vendor_id != 0x8086)
        return false;
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        if (pci->device_id == ids[i])
            return true;
    return false;
}

static bool switch_intel_ports(struct xhci *c)
{
    struct pci_device *pci = c->pci;
    if (!intel_switchable(pci))
        return false;
    uint32_t usb2_old = pci_read32(pci, PCI_XUSB2PR), usb3_old = pci_read32(pci, PCI_USB3_PSSEN);
    uint32_t usb2_mask = pci_read32(pci, PCI_XUSB2PRM), usb3_mask = pci_read32(pci, PCI_USB3PRM);
    pci_write32(pci, PCI_USB3_PSSEN, usb3_old | usb3_mask);
    pci_write32(pci, PCI_XUSB2PR, usb2_old | usb2_mask);
    uint32_t usb2 = pci_read32(pci, PCI_XUSB2PR), usb3 = pci_read32(pci, PCI_USB3_PSSEN);
    kprintf("                 Intel socket routing: USB 2 0x%x -> 0x%x, USB 3 0x%x -> 0x%x "
            "(allowed 0x%x, 0x%x)%s\n", usb2_old, usb2, usb3_old, usb3, usb2_mask, usb3_mask,
            usb2 == usb2_old && usb3 == usb3_old
                ? (usb2_mask | usb3_mask ? "" : ": the firmware allows none (its xHCI setting)")
                : ": moved from EHCI to xHCI");
    return usb2 != usb2_old || usb3 != usb3_old;
}

static const struct protocol *port_protocol(struct xhci *c, uint32_t port)
{
    for (int i = 0; i < c->protocol_count; i++) {
        const struct protocol *p = &c->protocols[i];
        if (port >= p->first && port < (uint32_t)p->first + p->count)
            return p;
    }
    return 0;
}

/* The controller stopped (the firmware may have left it running), reset
 * (all its state, rings included, starts from scratch), given its context
 * array, command ring and event ring (cleared: the boot's, and after a
 * sleep the same memory again), its interrupter, then running; ports the
 * controller lets software power (PPC) are off after the reset: switched
 * on (*powered). 0, or why not. */
static const char *run(struct xhci *c, bool *powered)
{
    op_write(c, OP_USBCMD, op_read(c, OP_USBCMD) & ~USBCMD_RUN);
    if (!wait_status(c, USBSTS_HALTED, USBSTS_HALTED))
        return "does not stop";
    op_write(c, OP_USBCMD, USBCMD_RESET);
    for (int ms = 0; ms < TIMEOUT_MS && (op_read(c, OP_USBCMD) & USBCMD_RESET); ms++)
        arch_delay_ms(1);
    if ((op_read(c, OP_USBCMD) & USBCMD_RESET) || !wait_status(c, USBSTS_CNR, 0))
        return "reset does not finish";

    for (uint32_t i = 1; i < PMM_PAGE_SIZE / 8; i++)
        c->dcbaa[i] = 0;                            /* (entry 0: the scratchpads) */
    memset((void *)c->cmd.trbs, 0, (RING_TRBS - 1) * sizeof(struct trb));
    c->cmd.index = 0;
    c->cmd.cycle = 1;
    uint64_t irq = spin_lock(&c->events_lock);      /* (the tick's look waits) */
    memset((void *)c->event.trbs, 0, PMM_PAGE_SIZE);
    c->event.index = 0;
    c->event.cycle = 1;                             /* the controller starts with 1 */
    spin_unlock(&c->events_lock, irq);
    op_write(c, OP_CONFIG, c->max_slots);
    write64(c->op + OP_DCBAAP, c->dcbaa_phys);
    write64(c->op + OP_CRCR, c->cmd.phys | CRCR_RCS);
    mmio_write32(c->rt + RT_ERSTSZ, 1);
    mmio_write32(c->rt + RT_ERDP + 4, (uint32_t)(c->event.phys >> 32));
    mmio_write32(c->rt + RT_ERDP, (uint32_t)c->event.phys);
    write64(c->rt + RT_ERSTBA, c->erst_phys);       /* last: this enables the event ring */

    mmio_write32(c->rt + RT_IMOD, 0);               /* no moderation: an interrupt per event */
    mmio_write32(c->rt + RT_IMAN, IMAN_IE | IMAN_IP);
    op_write(c, OP_USBCMD, USBCMD_RUN | USBCMD_INTE);
    if (!wait_status(c, USBSTS_HALTED, 0))
        return "does not start";

    *powered = false;
    for (uint32_t port = 1; c->port_power && port <= c->max_ports; port++) {
        uint32_t sc = op_read(c, OP_PORTSC(port));
        if (!(sc & PORTSC_PP)) {
            op_write(c, OP_PORTSC(port), (sc & PORTSC_KEEP) | PORTSC_PP);
            *powered = true;
        }
    }
    return 0;
}

/* (37g-2) Sleep: the controller stopped (no command or transfer from
 * here: the lock kept until resume). In S3 it loses everything, as Linux's
 * xhci_resume finds when its state is gone: started again from the
 * reset, every device it had let go of as unplugged (no command: the
 * controller forgot them), every port looked at again by the usb thread,
 * which sets up what is plugged in as at the boot. */
static bool xhci_suspend(void *ctx)
{
    struct xhci *c = ctx;
    mutex_lock(&c->passes);                         /* (the usb thread's pass ends first) */
    mutex_lock(&c->lock);
    op_write(c, OP_USBCMD, op_read(c, OP_USBCMD) & ~USBCMD_RUN);
    wait_status(c, USBSTS_HALTED, USBSTS_HALTED);
    return true;
}

/* (37g-6) A producer ring emptied (its page kept) for a start after a sleep. */
static void ring_reset(struct ring *r)
{
    memset((void *)r->trbs, 0, (RING_TRBS - 1) * sizeof(struct trb));
    r->trbs[RING_TRBS - 1].parameter = r->phys;
    r->trbs[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | LINK_TOGGLE_CYCLE;
    r->index = 0;
    r->cycle = 1;
}

/*
 * (37g-6) A device on a root port kept over a sleep, as Linux's USB
 * persist (usb_reset_and_verify_device): its port reset (USB 2) or its
 * link up (USB 3), a new slot and its address again, its device
 * descriptor and serial number compared with what it was, its
 * configuration selected again, and every endpoint the controller had for
 * it (saved: the device context before the sleep) made again with its
 * ring emptied; interrupt endpoints get their transfers queued again, a
 * bulk transfer the sleep cut short ends with an error (its driver tries
 * again). Its drivers go on as before: the disk the system runs from
 * stays mounted. 0, or why not (the caller lets the device go).
 */
static const char *revive(struct xhci *c, struct usb_device *dev, const uint8_t *saved)
{
    struct xhci_device *x = (struct xhci_device *)dev;
    uint32_t port = dev->port, cs = c->context_size;
    if (!(op_read(c, OP_PORTSC(port)) & PORTSC_CCS))
        return "nothing on its port";
    const struct protocol *p = port_protocol(c, port);
    if ((p ? p->major : 2) < 3) {
        if (!reset_port(c, port))
            return "its port reset failed";
    } else {
        for (int ms = 0; ms < 500 && !(op_read(c, OP_PORTSC(port)) & PORTSC_PED); ms++)
            arch_delay_ms(1);
    }
    uint32_t sc = op_read(c, OP_PORTSC(port));
    op_write(c, OP_PORTSC(port), (sc & PORTSC_KEEP) | (sc & PORTSC_CHANGES));
    if (!(sc & PORTSC_PED) || PORTSC_SPEED(sc) != dev->speed)
        return "its port is not enabled at its speed";
    /* (37g-8) Its recovery time before the first command, as Linux's
     * hub_port_reset (10 + 40 ms): with the controller's interrupts back
     * after a sleep, the VivoBook's stick was addressed at once and let go
     * of (before, each command waited for a tick, by chance long enough). */
    timer_sleep_ms(50);
    uint32_t code = command(c, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    if (code != COMPLETION_SUCCESS)
        return "no slot (Enable Slot)";
    dev->slot = (uint8_t)c->done_slot;
    c->slots[dev->slot] = x;
    memset(x->output, 0, PMM_PAGE_SIZE);
    c->dcbaa[dev->slot] = x->output_phys;
    ring_reset(&x->ep0);
    memset(x->input, 0, PMM_PAGE_SIZE);
    context(x->input, CTX_INPUT_CONTROL, cs)[1] = 1u << 0 | 1u << 1;
    volatile uint32_t *slot_ctx = context(x->input, CTX_IN_SLOT, cs);
    slot_ctx[0] = dev->route | (uint32_t)dev->speed << 20 | (dev->tt_multi ? 1u << 25 : 0) |
                  1u << 27;
    slot_ctx[1] = (uint32_t)dev->root_port << 16;
    slot_ctx[2] = dev->tt_slot | (uint32_t)dev->tt_port << 8;
    volatile uint32_t *ep0 = context(x->input, CTX_IN_EP0, cs);
    ep0[1] = 3u << 1 | EP_TYPE_CONTROL << 3 | (uint32_t)dev->max_packet0 << 16;
    ep0[2] = (uint32_t)x->ep0.phys | 1;
    ep0[3] = (uint32_t)(x->ep0.phys >> 32);
    ep0[4] = 8;
    arch_write_barrier();
    code = command(c, x->input_phys, 0, TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(dev->slot));
    if (code != COMPLETION_SUCCESS)
        return "no address (Address Device)";
    dev->address = (uint8_t)context(x->output, CTX_OUT_SLOT, cs)[3];
    struct usb_device_descriptor now;
    if (usb_control(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, &now,
                    sizeof(now)) != (int)sizeof(now) ||
        memcmp(&now, &dev->descriptor, sizeof(now)))
        return "another device (its descriptor differs)";
    if (now.serial) {
        char serial[64];
        usb_string(dev, now.serial, serial, sizeof(serial));
        if (strcmp(serial, dev->serial))
            return "another device (its serial number differs)";
    }
    if (dev->configured &&
        usb_control(dev, 0, USB_REQ_SET_CONFIGURATION, dev->config, 0, 0, 0) < 0)
        return "its configuration not selected again";
    /* The endpoints, from the device context the controller had. */
    volatile uint32_t *control = context(x->input, CTX_INPUT_CONTROL, cs);
    control[0] = 0;
    control[1] = 1u << 0;
    volatile uint32_t *slot_out = context(x->output, CTX_OUT_SLOT, cs);
    for (int i = 0; i < 4; i++)
        slot_ctx[i] = slot_out[i];
    slot_ctx[0] = (slot_ctx[0] & 0x07FFFFFF) | (((const uint32_t *)saved)[0] & 0xF8000000u);
    slot_ctx[3] = 0;
    for (uint32_t dci = 2; dci < 32; dci++) {
        const uint32_t *was = (const uint32_t *)(saved + dci * cs);
        if (!(was[0] & 7))
            continue;                               /* (state: disabled) */
        struct ring *ring = 0;
        for (int i = 0; i < c->endpoint_count && !ring; i++)
            if (c->endpoints[i].owner == dev && c->endpoints[i].dci == dci)
                ring = &c->endpoints[i].ring;
        for (int k = 0; k < BULK_ENDPOINTS && !ring; k++)
            if (x->bulk[k].address && x->bulk[k].dci == dci)
                ring = &x->bulk[k].ring;
        if (!ring)
            continue;                               /* (not ours to make again) */
        ring_reset(ring);
        volatile uint32_t *ctx = context(x->input, dci + 1, cs);
        ctx[0] = was[0] & ~7u;
        ctx[1] = was[1];
        ctx[2] = (uint32_t)ring->phys | 1;
        ctx[3] = (uint32_t)(ring->phys >> 32);
        ctx[4] = was[4];
        control[1] |= 1u << dci;
    }
    if (control[1] != 1u << 0) {
        arch_write_barrier();
        code = command(c, x->input_phys, 0, TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
                                            TRB_SLOT(dev->slot));
        if (code != COMPLETION_SUCCESS)
            return "its endpoints not made again (Configure Endpoint)";
    }
    uint64_t irq = spin_lock(&c->events_lock);
    for (int i = 0; i < c->endpoint_count; i++) {
        struct endpoint *ep = &c->endpoints[i];
        if (ep->owner != dev)
            continue;
        ep->slot = dev->slot;
        for (uint32_t k = 0; k < ENDPOINT_BUFFERS; k++)
            ring_push(&ep->ring, ep->buffers_phys + k * ENDPOINT_BUFFER_MAX, ep->size,
                      TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
        arch_write_barrier();
        mmio_write32(c->db + 4 * dev->slot, ep->dci);
    }
    for (int k = 0; k < BULK_ENDPOINTS; k++) {
        struct bulk_endpoint *b = &x->bulk[k];
        if (b->address && b->pending && !b->done) {
            b->code = COMPLETION_STALL;             /* (cut short: an error, tried again) */
            b->done = true;
        }
    }
    spin_unlock(&c->events_lock, irq);
    for (int k = 0; k < BULK_ENDPOINTS; k++)
        wait_queue_wake_all(&x->bulk[k].waiters);
    return 0;
}

static bool xhci_resume(void *ctx)
{
    struct xhci *c = ctx;
    /* Hubs (and what hangs behind them) let go of first, the controller
     * still stopped: no port event yet for the usb thread to detach them
     * beside this. The devices on root ports keep their state: each
     * one's device context copied (the controller forgets its slots). */
    c->lost = true;
    for (uint32_t port = 1; port <= c->max_ports; port++)
        if (c->root[port] && c->root[port]->hub_ports)
            detach(c, c->root[port]);
    c->lost = false;
    uint8_t *saved[256];
    for (uint32_t port = 0; port <= c->max_ports && port < 256; port++) {
        struct usb_device *dev = c->root[port];
        saved[port] = dev ? kmalloc(PMM_PAGE_SIZE) : 0;
        if (saved[port])
            memcpy(saved[port], ((struct xhci_device *)dev)->output, PMM_PAGE_SIZE);
        else if (dev) {
            c->lost = true;                         /* (no memory: as unplugged) */
            detach(c, dev);
            c->lost = false;
        }
    }
    for (int i = 0; i < 256; i++)
        c->slots[i] = 0;
    bool powered;
    const char *why = run(c, &powered);
    uint32_t kept = 0, let_go = 0;
    for (uint32_t port = 1; port <= c->max_ports && port < 256; port++) {
        struct usb_device *dev = c->root[port];
        if (!dev)
            continue;
        const char *not_kept = why ? why : revive(c, dev, saved[port]);
        usb_log_label(dev->name);
        if (!not_kept) {
            kprintf("the same device after the sleep: kept (%s)\n",
                    dev->product[0] ? dev->product : dev->function);
            kept++;
            continue;
        }
        kprintf("after the sleep: %s: let go of as unplugged\n", not_kept);
        ksnprintf(c->let_go_why, sizeof(c->let_go_why), "%s: %s", dev->name, not_kept);
        let_go++;
        uint8_t slot = dev->slot;
        c->lost = !c->slots[slot] || c->slots[slot] != (struct xhci_device *)dev;
        detach(c, dev);                             /* (Disable Slot if it has its new one) */
        c->lost = false;
    }
    for (uint32_t port = 0; port <= c->max_ports && port < 256; port++)
        kfree(saved[port]);
    mutex_unlock(&c->lock);
    mutex_unlock(&c->passes);
    if (why) {
        kprintf("  %s: after the sleep: %s\n", c->name, why);
        return false;
    }
    c->sleeps++;
    c->kept += kept;
    c->let_go += let_go;
    /* Every port without a device kept looked at again by the usb thread. */
    uint64_t irq = spin_lock(&c->events_lock);
    for (uint32_t port = 1; port <= c->max_ports; port++)
        if (port >= 256 || !c->root[port])
            c->changed[port / 64] |= 1ULL << (port % 64);
    spin_unlock(&c->events_lock, irq);
    wait_queue_wake_all(&c->hotplug_waiters);
    return true;
}

static struct xhci *fail(struct xhci *c, const char *why)
{
    kprintf("  xHCI:          %s: %s\n", c ? c->name : "?", why);
    return 0;
}

struct xhci *xhci_init(struct pci_device *pci, const char *name, bool scan)
{
    struct pci_bar *bar = &pci->bars[0];
    if (!bar->size || bar->is_io)
        return fail(0, "controller has no register BAR");
    struct xhci *c = kzalloc(sizeof(*c));
    if (!c)
        return fail(0, "out of memory");
    c->name = name;
    c->pci = pci;
    c->lock = (struct mutex)MUTEX_INIT("xhci");
    c->passes = (struct mutex)MUTEX_INIT("xhci passes");
    c->events_lock = (struct spinlock)SPINLOCK_INIT("xhci events");

    pci_write16(pci, PCI_COMMAND, pci_read16(pci, PCI_COMMAND) | PCI_COMMAND_MEMORY |
                                  PCI_COMMAND_BUS_MASTER | PCI_COMMAND_INTX_OFF);
    c->cap = paging_map_mmio(bar->address, bar->size);

    uint32_t head = mmio_read32(c->cap + CAP_LENGTH_VERSION);
    uint32_t params1 = mmio_read32(c->cap + CAP_HCSPARAMS1);
    uint32_t params2 = mmio_read32(c->cap + CAP_HCSPARAMS2);
    uint32_t hccparams1 = mmio_read32(c->cap + CAP_HCCPARAMS1);
    c->version = head >> 16;
    c->op = c->cap + (head & 0xFF);
    c->rt = c->cap + (mmio_read32(c->cap + CAP_RTSOFF) & ~0x1Fu);
    c->db = c->cap + (mmio_read32(c->cap + CAP_DBOFF) & ~0x3u);
    c->max_slots = params1 & 0xFF;
    c->max_interrupters = (params1 >> 8) & 0x7FF;
    c->max_ports = params1 >> 24;
    c->scratchpads = ((params2 >> 21) & 0x1F) << 5 | params2 >> 27;
    c->context_size = hccparams1 & (1u << 2) ? 64 : 32;

    kprintf("  xHCI:          %02x:%02x.%u, version %x.%02x, registers %p, %s\n", pci->bus,
            pci->device, pci->function, c->version >> 8, c->version & 0xFF, (void *)c->cap,
            name);
    read_extended_caps(c, hccparams1);
    if (!(op_read(c, OP_PAGESIZE) & 1))
        return fail(c, "does not support 4 KiB pages");
    bool switched = switch_intel_ports(c);

    /* Device context base address array: one pointer per device slot; entry
     * 0 points to the scratchpad buffers the controller may ask for. The
     * command ring; the event ring, one segment of 256 TRBs described by a
     * one-entry table. */
    c->dcbaa = alloc_page(&c->dcbaa_phys);
    if (!c->dcbaa || !producer_ring_init(&c->cmd))
        return fail(c, "out of memory");
    if (c->scratchpads) {
        uint64_t array_phys;
        uint64_t *array = alloc_page(&array_phys);
        if (!array || c->scratchpads > PMM_PAGE_SIZE / 8)
            return fail(c, "cannot provide the scratchpad buffers");
        for (uint32_t i = 0; i < c->scratchpads; i++) {
            if (!alloc_page(&array[i]))
                return fail(c, "out of memory for scratchpad buffers");
        }
        c->dcbaa[0] = array_phys;
    }
    struct erst_entry *erst = alloc_page(&c->erst_phys);
    c->event.trbs = alloc_page(&c->event.phys);
    if (!erst || !c->event.trbs)
        return fail(c, "out of memory");
    erst->base = c->event.phys;
    erst->size = RING_TRBS;

    /* Interrupts: MSI-X entry 0 belongs to interrupter 0; without MSI-X,
     * the single MSI message does. */
    uint64_t msi_address = 0;
    uint32_t msi_data = 0;
    uint16_t msi_entries = 0;
    c->interrupts = pci_msix_enable(pci, xhci_interrupt, c, &msi_address, &msi_data,
                                    &msi_entries);
    bool msi = !c->interrupts &&
               pci_msi_enable(pci, xhci_interrupt, c, &msi_address, &msi_data);
    c->interrupts = c->interrupts || msi;
    c->port_power = hccparams1 & HCCPARAMS1_PPC;
    bool powered = false;
    const char *why = run(c, &powered);
    if (why)
        return fail(c, why);

    kprintf("                 %u device slots, %u ports, %u interrupters, %u-byte contexts, "
            "%u scratchpad pages\n", c->max_slots, c->max_ports, c->max_interrupters,
            c->context_size, c->scratchpads);
    for (int i = 0; i < c->protocol_count; i++) {
        const struct protocol *p = &c->protocols[i];
        kprintf("                 USB %x.%x: ports %u-%u\n", p->major, p->minor >> 4, p->first,
                p->first + p->count - 1);
    }
    if (msi)
        kprintf("                 interrupts: MSI (no MSI-X), message 0x%x to %p\n", msi_data,
                (void *)msi_address);
    else if (c->interrupts)
        kprintf("                 interrupts: MSI-X entry 0 of %u, message 0x%x to %p\n",
                msi_entries, msi_data, (void *)msi_address);
    else
        kprintf("                 interrupts: none (no MSI or MSI-X): events polled while "
                "waiting and every tick\n");
    if (!timer_add_tick_handler(xhci_poll, c) && !c->interrupts)
        return fail(c, "no interrupts and no timer tick to poll with");

    /* A command that does nothing proves that the command ring, the doorbell,
     * the event ring and the interrupt all work. */
    uint64_t interrupts_before = c->interrupt_count;
    uint32_t code = command(c, 0, 0, TRB_TYPE(TRB_NOOP_COMMAND));
    if (code != COMPLETION_SUCCESS)
        return fail(c, code ? "No-Op command failed" : "No-Op command: no answer");
    kprintf("                 No-Op command: completed, %lu interrupt(s)\n",
            c->interrupt_count - interrupts_before);
    if (op_read(c, OP_USBSTS) & USBSTS_HSE)
        return fail(c, "host system error");

    /* Devices need time to show up after their ports were switched on
     * (run), and after sockets moved from EHCI (connect debounce, 100 ms in
     * USB 2.0 section 7.1.7.3, plus their own start): wait half a second. */
    if (powered)
        kprintf("                 ports switched on (the controller has port power control)\n");
    if (switched || powered)
        arch_delay_ms(500);

    /* What is plugged in: each device gets a slot and an address, then its
     * descriptors are read (hubs set up the devices behind them). Without
     * scan ("usblate" on the command line), nothing is: as if every link
     * came up late (xhci_attach_new finds them). */
    for (uint32_t port = 1; port <= c->max_ports && scan; port++) {
        if (!(op_read(c, OP_PORTSC(port)) & PORTSC_CCS))
            continue;
        const struct protocol *p = port_protocol(c, port);
        uint32_t major = p ? p->major : 2;
        char label[32];
        ksnprintf(label, sizeof(label), "%s port %u", name, port);
        usb_log_label(label);
        kprintf("USB %x device connected", major);
        attach_port(c, port, major);        /* continues the line */
    }
    /* What changed until now is handled: from here on, a connection change
     * is news for the usb thread (xhci_start_hotplug). */
    for (uint32_t port = 1; port <= c->max_ports; port++) {
        uint32_t sc = op_read(c, OP_PORTSC(port));
        op_write(c, OP_PORTSC(port), (sc & PORTSC_KEEP) | (sc & PORTSC_CHANGES));
    }
    sleep_register(name, xhci_suspend, xhci_resume, c);     /* (37g-2) */
    return c;
}

void xhci_report(struct xhci *c)
{
    uint64_t state = spin_lock(&c->events_lock);
    process_events(c);
    spin_unlock(&c->events_lock, state);
    char lost[48] = "";
    if (c->interrupts)
        ksnprintf(lost, sizeof(lost), ", %lu events found by the tick instead", c->tick_events);
    kprintf("  %s:          %lu commands, %lu events (%lu port changes), %lu interrupts%s\n",
            c->name, c->commands, c->events, c->port_events, c->interrupt_count, lost);
    if (c->sleeps) {                        /* (37g-8) */
        kprintf("  %s:          over %lu sleep(s): %lu device(s) kept, %lu let go of\n",
                c->name, c->sleeps, c->kept, c->let_go);
        if (c->let_go_why[0])
            kprintf("  %s:          (the last let go of: %s)\n", c->name, c->let_go_why);
    }
}
