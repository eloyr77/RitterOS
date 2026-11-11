/*
 * Intel 8254x Gigabit Ethernet ("e1000"; QEMU's default card on x86 is
 * the 82540EM), after Intel's "PCI/PCI-X Family of Gigabit Ethernet
 * Controllers Software Developer's Manual".
 *
 * The card moves frames by DMA through two rings of 16-byte descriptors
 * in RAM. Receiving: each descriptor points to a 2 KiB buffer; the card
 * fills buffers from the head (RDH) up to the tail (RDT) the driver
 * leaves it, and sets "descriptor done" (DD) in each one it filled.
 * Sending: the driver fills descriptors up to the tail (TDT) and moves the
 * tail on; the card sends them and sets DD. QEMU's card has no MSI, so the
 * driver polls: the timer tick looks at the next receive descriptor and
 * wakes the card's thread, which hands the frames on (net_receive).
 */
#include "driver.h"
#include "driver_net.h"

#define REG_CTRL        0x0000
#define REG_STATUS      0x0008
#define REG_EERD        0x0014
#define REG_IMC         0x00D8
#define REG_RCTL        0x0100
#define REG_TCTL        0x0400
#define REG_TIPG        0x0410
#define REG_RDBAL       0x2800
#define REG_RDBAH       0x2804
#define REG_RDLEN       0x2808
#define REG_RDH         0x2810
#define REG_RDT         0x2818
#define REG_TDBAL       0x3800
#define REG_TDBAH       0x3804
#define REG_TDLEN       0x3808
#define REG_TDH         0x3810
#define REG_TDT         0x3818
#define REG_MTA         0x5200
#define REG_RAL0        0x5400
#define REG_RAH0        0x5404

#define CTRL_SLU        (1u << 6)       /* set link up */
#define CTRL_RST        (1u << 26)
#define STATUS_LU       (1u << 1)
#define RCTL_EN         (1u << 1)
#define RCTL_UPE        (1u << 3)       /* (38b) every unicast frame (set_promiscuous) */
#define RCTL_MPE        (1u << 4)       /* every multicast frame (set_multicast's -1) */
#define RCTL_BAM        (1u << 15)      /* broadcasts too */
#define RCTL_SECRC      (1u << 26)      /* strip the Ethernet checksum */
#define TCTL_EN         (1u << 1)
#define TCTL_PSP        (1u << 3)       /* pad short frames */
#define TCTL_CT         (0x10u << 4)    /* collision threshold */
#define TCTL_COLD       (0x40u << 12)   /* collision distance (full duplex) */
#define RAH_AV          (1u << 31)

#define DESC_DD         0x01            /* status: descriptor done */
#define DESC_EOP        0x02            /* receive status: end of packet */
#define CMD_EOP         0x01            /* send: end of packet */
#define CMD_IFCS        0x02            /* insert the checksum */
#define CMD_RS          0x08            /* report status (sets DD) */

#define RING            64              /* descriptors per ring: 1 KiB */
#define BUFFER          2048

struct rx_desc {
    uint64_t address;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t address;
    uint16_t length;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} __attribute__((packed));

struct e1000 {
    struct net_card *dev;               /* (67c) its card (net_card_new): its priv is this */
    volatile uint8_t *regs;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint8_t *rx_buffers[RING], *tx_buffers[RING];
    uint32_t rx_next, tx_next;
    struct spinlock tx_lock;
    struct wait_queue rx_waiters;
    bool promiscuous, all_multicast;    /* (38b) RCTL's UPE and MPE as asked */
};

static uint32_t rd(struct e1000 *c, uint32_t reg)
{
    return mmio_read32(c->regs + reg);
}

static void wr(struct e1000 *c, uint32_t reg, uint32_t value)
{
    mmio_write32(c->regs + reg, value);
}

static uint32_t link_speed(struct net_card *d)
{
    struct e1000 *c = d->priv;
    uint32_t status = rd(c, REG_STATUS);
    if (!(status & STATUS_LU))
        return 0;
    uint32_t speed = status >> 6 & 3;
    return speed == 0 ? 10 : speed == 1 ? 100 : 1000;
}

static bool send(struct net_card *d, const void *frame, size_t length)
{
    struct e1000 *c = d->priv;
    if (length > ETH_FRAME_MAX || length < ETH_HEADER)
        return false;
    uint64_t state = spin_lock(&c->tx_lock);
    struct tx_desc *t = &c->tx[c->tx_next];
    /* A descriptor used before must be done (DD) before it is used again. */
    bool free = t->cmd == 0 || (t->status & DESC_DD);
    if (free) {
        memcpy(c->tx_buffers[c->tx_next], frame, length);
        t->length = (uint16_t)length;
        t->cmd = CMD_EOP | CMD_IFCS | CMD_RS;
        t->status = 0;
        c->tx_next = (c->tx_next + 1) % RING;
        arch_write_barrier();                   /* the descriptor before the tail */
        wr(c, REG_TDT, c->tx_next);
        d->tx_frames++;
        d->tx_bytes += length;
    } else {
        d->tx_dropped++;
    }
    spin_unlock(&c->tx_lock, state);
    return free;
}

static bool rx_ready(struct e1000 *c)
{
    return c->rx[c->rx_next].status & DESC_DD;
}

/* The timer tick (interrupt context): frames waiting? Wake the thread. */
static void tick(void *context)
{
    struct e1000 *c = context;
    if (rx_ready(c))
        wait_queue_wake_all(&c->rx_waiters);
}

/* The card's thread: hand received frames on, give the buffers back. */
static void receiver(void *context)
{
    struct e1000 *c = context;
    for (;;) {
        wait_event(&c->rx_waiters, rx_ready(c));
        arch_read_barrier();                    /* DD before the frame's bytes */
        net_batch_begin();                      /* (65e) one ACK per connection for a burst */
        while (rx_ready(c)) {
            struct rx_desc *r = &c->rx[c->rx_next];
            if ((r->status & DESC_EOP) && !r->errors && r->length <= ETH_FRAME_MAX)
                net_receive(c->dev, c->rx_buffers[c->rx_next], r->length);
            r->status = 0;
            uint32_t done = c->rx_next;
            c->rx_next = (c->rx_next + 1) % RING;
            arch_write_barrier();
            wr(c, REG_RDT, done);               /* the buffer is the card's again */
        }
        net_batch_end();
    }
}

/* A word of the EEPROM (the MAC address, if the registers lack it). */
static uint16_t eeprom(struct e1000 *c, uint8_t address)
{
    wr(c, REG_EERD, 1u | (uint32_t)address << 8);
    for (int i = 0; i < 10000; i++) {
        uint32_t v = rd(c, REG_EERD);
        if (v & (1u << 4))
            return (uint16_t)(v >> 16);
        arch_cpu_relax();
    }
    return 0;
}

static const struct {
    uint16_t device;
    const char *model;
} models[] = {
    { 0x100E, "Intel 82540EM" },        /* QEMU's e1000 */
    { 0x100F, "Intel 82545EM" },
};

static const char *e1000_model(const struct pci_device *pci)
{
    if (pci->vendor_id != 0x8086)
        return 0;
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++)
        if (models[i].device == pci->device_id)
            return models[i].model;
    return 0;
}

/* The groups the protocols want (netdev.set_multicast, 64a): the
 * multicast table's 4096 bits, one per value of a destination's bits
 * 36-47 (RCTL.MO 0: the last byte and the high half of the one before);
 * every multicast frame (RCTL.MPE) for -1. */
static void set_multicast(struct net_card *d, const uint8_t (*macs)[6], int count)
{
    struct e1000 *c = d->priv;
    uint32_t table[128];
    memset(table, 0, sizeof(table));
    for (int i = 0; i < count; i++) {
        unsigned bit = ((unsigned)macs[i][5] << 4 | macs[i][4] >> 4) & 0xFFF;
        table[bit >> 5] |= 1u << (bit & 31);
    }
    for (int i = 0; i < 128; i++)
        wr(c, REG_MTA + 4 * i, table[i]);
    c->all_multicast = count < 0;
    uint32_t rctl = rd(c, REG_RCTL);
    wr(c, REG_RCTL, count < 0 || c->promiscuous ? rctl | RCTL_MPE : rctl & ~RCTL_MPE);
}

/* (38b) Every frame (a bridge's port: RCTL.UPE and MPE), or again only
 * the card's own and the groups asked for. */
static void set_promiscuous(struct net_card *d, bool on)
{
    struct e1000 *c = d->priv;
    c->promiscuous = on;
    uint32_t rctl = rd(c, REG_RCTL) & ~(RCTL_UPE | RCTL_MPE);
    wr(c, REG_RCTL, rctl | (on ? RCTL_UPE : 0) | (on || c->all_multicast ? RCTL_MPE : 0));
}

static void *fail(const char *where, const char *why)
{
    kprintf("  e1000:         %s: %s\n", where, why);
    return 0;
}

static struct net_card *e1000_init(struct pci_device *pci, const char *model)
{
    char where[16];
    ksnprintf(where, sizeof(where), "%02x:%02x.%u", pci->bus, pci->device, pci->function);
    struct pci_bar *bar = &pci->bars[0];
    if (!bar->size || bar->is_io)
        return fail(where, "no register BAR");
    struct e1000 *c = kzalloc(sizeof(*c));
    if (!c || !(c->dev = net_card_new()))
        return fail(where, "out of memory");
    pci_enable_dma(pci);
    pci_write16(pci, PCI_COMMAND, pci_read16(pci, PCI_COMMAND) | PCI_COMMAND_INTX_OFF);
    c->regs = paging_map_mmio(bar->address, bar->size);

    /* Reset; no interrupts (polled). */
    wr(c, REG_IMC, 0xFFFFFFFF);
    wr(c, REG_CTRL, rd(c, REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000 && (rd(c, REG_CTRL) & CTRL_RST); i++)
        timer_sleep_ms(1);
    wr(c, REG_IMC, 0xFFFFFFFF);
    wr(c, REG_CTRL, rd(c, REG_CTRL) | CTRL_SLU);

    /* The MAC address: from the receive address registers (loaded from the
     * EEPROM at the reset), else from the EEPROM itself. */
    uint32_t ral = rd(c, REG_RAL0), rah = rd(c, REG_RAH0);
    if (!(rah & RAH_AV)) {
        uint16_t w[3] = { eeprom(c, 0), eeprom(c, 1), eeprom(c, 2) };
        ral = w[0] | (uint32_t)w[1] << 16;
        rah = w[2] | RAH_AV;
        wr(c, REG_RAL0, ral);
        wr(c, REG_RAH0, rah);
    }
    for (int i = 0; i < 4; i++)
        c->dev->mac[i] = (uint8_t)(ral >> (8 * i));
    c->dev->mac[4] = (uint8_t)rah;
    c->dev->mac[5] = (uint8_t)(rah >> 8);
    for (int i = 0; i < 128; i++)
        wr(c, REG_MTA + 4 * i, 0);              /* no multicast until set_multicast */

    /* The rings: descriptors in one page each, a 2 KiB buffer for each. */
    uint64_t rx_phys = pmm_alloc_page(), tx_phys = pmm_alloc_page();
    if (!rx_phys || !tx_phys)
        return fail(where, "out of memory");
    c->rx = phys_to_virt(rx_phys);
    c->tx = phys_to_virt(tx_phys);
    memset(c->rx, 0, PAGE_SIZE);
    memset(c->tx, 0, PAGE_SIZE);
    for (int i = 0; i < RING; i += 2) {
        uint64_t rp = pmm_alloc_page(), tp = pmm_alloc_page();
        if (!rp || !tp)
            return fail(where, "out of memory");
        for (int k = 0; k < 2; k++) {
            c->rx_buffers[i + k] = (uint8_t *)phys_to_virt(rp) + k * BUFFER;
            c->tx_buffers[i + k] = (uint8_t *)phys_to_virt(tp) + k * BUFFER;
            c->rx[i + k].address = rp + (uint64_t)k * BUFFER;
            c->tx[i + k].address = tp + (uint64_t)k * BUFFER;
        }
    }
    wr(c, REG_RDBAL, (uint32_t)rx_phys);
    wr(c, REG_RDBAH, (uint32_t)(rx_phys >> 32));
    wr(c, REG_RDLEN, RING * sizeof(struct rx_desc));
    wr(c, REG_RDH, 0);
    wr(c, REG_RDT, RING - 1);                   /* all buffers but one are the card's */
    wr(c, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    wr(c, REG_TDBAL, (uint32_t)tx_phys);
    wr(c, REG_TDBAH, (uint32_t)(tx_phys >> 32));
    wr(c, REG_TDLEN, RING * sizeof(struct tx_desc));
    wr(c, REG_TDH, 0);
    wr(c, REG_TDT, 0);
    wr(c, REG_TIPG, 0x0060200A);                /* the manual's values for copper */
    wr(c, REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    c->tx_lock = (struct spinlock)SPINLOCK_INIT("e1000 tx");
    c->dev->driver = "e1000";
    ksnprintf(c->dev->model, sizeof(c->dev->model), "%s", model);
    ksnprintf(c->dev->where, sizeof(c->dev->where), "%s", where);
    c->dev->link_speed = link_speed;
    c->dev->send = send;
    c->dev->pci = pci;
    c->dev->priv = c;
    c->dev->set_multicast = set_multicast;
    c->dev->set_promiscuous = set_promiscuous;
    net_register(c->dev);
    if (!thread_create(c->dev->name, receiver, c) || !timer_add_tick_handler(tick, c))
        return fail(where, "no thread for receiving");
    return c->dev;
}

static bool start(struct pci_device *pci)
{
    const char *model = e1000_model(pci);
    return model && e1000_init(pci, model);
}

/* The chips e1000_model knows, by the same IDs. */
static const struct module_pci ids[] = {
    { 0x8086, 0x100E }, { 0x8086, 0x100F },
};

MODULE(e1000, start, 0, 0, ids)
