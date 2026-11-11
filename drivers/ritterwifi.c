/*
 * The driver for our own test Wi-Fi card (step 24d, scripts/qemu/ritterwifi.c;
 * PCI 0xF055:0x11AC). Not a real chip: the simplest card that drives the
 * wireless layer (wlan.c). It carries whole 802.11 frames through two rings
 * of 16-byte descriptors, like the e1000, and has a channel register. QEMU's
 * card raises no interrupt we use: the timer tick looks at the next receive
 * descriptor and wakes the card's thread (polled, as for the e1000).
 * (67d) A kernel module, against the driver interface's Wi-Fi class.
 */
#include "driver.h"
#include "driver_net.h"
#include "driver_wifi.h"

#define R_ID            0x00
#define R_MAC_LO        0x04
#define R_MAC_HI        0x08
#define R_CHANNEL       0x0C
#define R_ISR           0x10
#define R_IMR           0x14
#define R_RX_BASE_LO    0x18
#define R_RX_BASE_HI    0x1C
#define R_RX_LEN        0x20
#define R_TX_BASE_LO    0x28
#define R_TX_BASE_HI    0x2C
#define R_TX_LEN        0x30
#define R_TX_TAIL       0x34
#define R_CTRL          0x38

#define ID_MAGIC        0x52464931u
#define CTRL_ENABLE     0x1
#define DESC_DONE       0x1
#define RING            16
#define BUFFER          4096            /* one frame per page (a frame is < 2400) */

struct desc {
    uint64_t address;
    uint16_t length;
    uint8_t flags;
    int8_t signal;
    uint8_t channel;
    uint8_t reserved[3];
};

struct ritterwifi {
    volatile uint8_t *regs;
    struct desc *rx, *tx;
    uint8_t *rx_buffers[RING], *tx_buffers[RING];
    uint32_t rx_next, tx_next;
    struct wait_queue rx_waiters;
    struct wifi_card wlan;
    struct net_card *dev;               /* (67d) its card (net_card_new): its priv is this */
};

static uint32_t rd(struct ritterwifi *c, uint32_t reg)
{
    return mmio_read32(c->regs + reg);
}

static void wr(struct ritterwifi *c, uint32_t reg, uint32_t value)
{
    mmio_write32(c->regs + reg, value);
}

static bool rx_ready(struct ritterwifi *c)
{
    return c->rx[c->rx_next].flags & DESC_DONE;
}

static void tick(void *context)
{
    struct ritterwifi *c = context;
    if (rx_ready(c))
        wait_queue_wake_all(&c->rx_waiters);
}

static void receiver(void *context)
{
    struct ritterwifi *c = context;
    for (;;) {
        wait_event(&c->rx_waiters, rx_ready(c));
        arch_read_barrier();
        net_batch_begin();                      /* (65e) one ACK per connection for a burst */
        while (rx_ready(c)) {
            struct desc *r = &c->rx[c->rx_next];
            if (r->length <= WLAN_FRAME_MAX)
                wlan_receive(c->dev, c->rx_buffers[c->rx_next], r->length, r->channel,
                             r->signal);
            r->length = 0;
            arch_write_barrier();
            r->flags = 0;                       /* the buffer is the card's again */
            c->rx_next = (c->rx_next + 1) % RING;
        }
        net_batch_end();
    }
}

/* A Block Ack agreement: the model card has no acknowledgements of its own
 * to set up (the test access point sends bursts as separate frames), so it
 * agrees; the wireless layer's reorder buffer is what the test exercises. */
static bool block_ack(struct net_card *d, int tid, bool sending, bool on, uint16_t ssn,
                      uint16_t window)
{
    (void)d, (void)tid, (void)sending, (void)on, (void)ssn, (void)window;
    return true;
}

static bool transmit(struct net_card *d, const void *frame, size_t length)
{
    struct ritterwifi *c = d->priv;
    if (length > BUFFER)
        return false;
    struct desc *t = &c->tx[c->tx_next];
    memcpy(c->tx_buffers[c->tx_next], frame, length);
    t->length = (uint16_t)length;
    t->flags = 0;
    arch_write_barrier();
    c->tx_next = (c->tx_next + 1) % RING;
    wr(c, R_TX_TAIL, c->tx_next);               /* the model sends synchronously */
    return true;
}

static bool set_channel(struct net_card *d, int channel)
{
    struct ritterwifi *c = d->priv;
    wr(c, R_CHANNEL, (uint32_t)channel);
    return true;
}

static void *fail(const char *where, const char *why)
{
    kprintf("  ritterwifi:    %s: %s\n", where, why);
    return 0;
}

static const char *ritterwifi_model(const struct pci_device *pci)
{
    if (pci->vendor_id == 0xF055 && pci->device_id == 0x11AC)
        return "RitterOS test Wi-Fi";
    return 0;
}

static struct net_card *ritterwifi_init(struct pci_device *pci, const char *model)
{
    char where[16];
    ksnprintf(where, sizeof(where), "%02x:%02x.%u", pci->bus, pci->device, pci->function);
    struct pci_bar *bar = &pci->bars[0];
    if (!bar->size || bar->is_io)
        return fail(where, "no register BAR");
    struct ritterwifi *c = kzalloc(sizeof(*c));
    if (!c || !(c->dev = net_card_new()))
        return fail(where, "out of memory");
    pci_enable_dma(pci);
    pci_write16(pci, PCI_COMMAND, pci_read16(pci, PCI_COMMAND) | PCI_COMMAND_INTX_OFF);
    c->regs = paging_map_mmio(bar->address, bar->size);
    if (rd(c, R_ID) != ID_MAGIC)
        return fail(where, "not the expected card");

    uint32_t lo = rd(c, R_MAC_LO), hi = rd(c, R_MAC_HI);
    for (int i = 0; i < 4; i++)
        c->dev->mac[i] = (uint8_t)(lo >> (8 * i));
    c->dev->mac[4] = (uint8_t)hi;
    c->dev->mac[5] = (uint8_t)(hi >> 8);

    uint64_t rx_phys = pmm_alloc_page(), tx_phys = pmm_alloc_page();
    if (!rx_phys || !tx_phys)
        return fail(where, "out of memory");
    c->rx = phys_to_virt(rx_phys);
    c->tx = phys_to_virt(tx_phys);
    memset(c->rx, 0, PAGE_SIZE);
    memset(c->tx, 0, PAGE_SIZE);
    for (int i = 0; i < RING; i++) {
        uint64_t rp = pmm_alloc_page(), tp = pmm_alloc_page();
        if (!rp || !tp)
            return fail(where, "out of memory");
        c->rx_buffers[i] = phys_to_virt(rp);
        c->tx_buffers[i] = phys_to_virt(tp);
        c->rx[i].address = rp;
        c->tx[i].address = tp;
    }
    wr(c, R_IMR, 0);                            /* polled */
    wr(c, R_RX_BASE_LO, (uint32_t)rx_phys);
    wr(c, R_RX_BASE_HI, (uint32_t)(rx_phys >> 32));
    wr(c, R_RX_LEN, RING);
    wr(c, R_TX_BASE_LO, (uint32_t)tx_phys);
    wr(c, R_TX_BASE_HI, (uint32_t)(tx_phys >> 32));
    wr(c, R_TX_LEN, RING);
    wr(c, R_CTRL, CTRL_ENABLE);

    /* The channels this card offers: 2.4 GHz 1-13 and four 5 GHz. */
    int n = 0;
    for (int ch = 1; ch <= 13; ch++)
        c->wlan.channels[n++] = (uint8_t)ch;
    for (int ch = 36; ch <= 48; ch += 4)
        c->wlan.channels[n++] = (uint8_t)ch;
    c->wlan.nchannels = n;
    c->wlan.transmit = transmit;
    c->wlan.block_ack = block_ack;
    c->wlan.set_channel = set_channel;
    c->dev->driver = "ritterwifi";
    ksnprintf(c->dev->model, sizeof(c->dev->model), "%s", model);
    ksnprintf(c->dev->where, sizeof(c->dev->where), "%s", where);
    c->dev->pci = pci;
    c->dev->priv = c;
    wr(c, R_CHANNEL, 1);
    wlan_register(c->dev, &c->wlan);
    if (!thread_create(c->dev->name, receiver, c) || !timer_add_tick_handler(tick, c))
        return fail(where, "no thread for receiving");
    return c->dev;
}

static bool start(struct pci_device *pci)
{
    const char *model = ritterwifi_model(pci);
    return model && ritterwifi_init(pci, model);
}

/* The card's IDs (our QEMU model's). */
static const struct module_pci ids[] = {
    { 0xF055, 0x11AC },
};

MODULE(ritterwifi, start, 0, 0, ids)
