/*
 * Intel 82576 Gigabit Ethernet ("igb"; the test PC's two-port card, and
 * QEMU's igb), after Intel's "82576EB Gigabit Ethernet Controller
 * Datasheet".
 *
 * The same family as the e1000 (kernel/e1000.c): rings of 16-byte
 * descriptors in RAM, a head the card moves on and a tail the driver
 * moves on. The 82576 has 16 queues of each kind, their registers at
 * 0xC000 (receiving) and 0xE000 (sending), each switched on by itself
 * (RXDCTL, TXDCTL); RitterOS uses queue 0. Its descriptors are the
 * "advanced" ones (QEMU's model knows no others): the card writes its
 * report over a receive descriptor, buffer address included, so the
 * driver writes the address again before it gives the buffer back; a
 * send descriptor carries the frame's length twice (buffer and packet).
 * Each port of a two-port card is a PCI function of its own with its own
 * registers and PHY. The PHY (the chip that drives the cable) may come
 * powered down from the firmware: the driver switches it on and restarts
 * the negotiation of speed with the other end, which takes a few seconds
 * on a real cable. A received frame raises an interrupt that wakes the
 * card's thread: MSI-X (vector 0, as Linux uses the chip), else MSI (the
 * simple cause register ICR); without either the card is polled by the
 * timer tick, which also stays as a safety net.
 */
#include "driver.h"
#include "driver_net.h"

#define REG_CTRL        0x0000
#define REG_STATUS      0x0008
#define REG_EECD        0x0010
#define REG_EERD        0x0014
#define REG_CTRL_EXT    0x0018
#define REG_MDIC        0x0020
#define REG_ICR         0x00C0          /* interrupt causes: cleared when read */
#define REG_IMS         0x00D0          /* interrupt mask set */
#define REG_IMC         0x00D8
#define REG_RCTL        0x0100
#define REG_EITR0       0x1680          /* interrupt throttling of vector 0 */
#define REG_IVAR0       0x1700          /* which vector each queue's interrupt goes to */
#define REG_GPIE        0x1514
#define REG_EIMS        0x1524
#define REG_EIAC        0x152C          /* extended causes cleared when their message goes */
#define REG_EICR        0x1580          /* extended causes (MSI-X vectors) */
#define REG_TCTL        0x0400
#define REG_EIMC        0x1528
#define REG_CRCERRS     0x4000          /* statistics: cleared when read */
#define REG_MPC         0x4010
#define REG_GPRC        0x4074
#define REG_GPTC        0x4080
#define REG_RNBC        0x40A0
#define REG_TPR         0x40D0
#define REG_MTA         0x5200
#define REG_RAL0        0x5400
#define REG_RAH0        0x5404
#define REG_RDBAL       0xC000          /* queue 0 */
#define REG_RDBAH       0xC004
#define REG_RDLEN       0xC008
#define REG_SRRCTL      0xC00C
#define REG_RDH         0xC010
#define REG_RDT         0xC018
#define REG_RXDCTL      0xC028
#define REG_RQDPC       0xC030          /* frames the queue dropped: cleared when read */
#define REG_TDBAL       0xE000
#define REG_TDBAH       0xE004
#define REG_TDLEN       0xE008
#define REG_TDH         0xE010
#define REG_TDT         0xE018
#define REG_TXDCTL      0xE028

#define CTRL_SLU        (1u << 6)       /* set link up */
#define CTRL_FRCSPD     (1u << 11)      /* forced speed and duplex: off, negotiate */
#define CTRL_FRCDPX     (1u << 12)
#define CTRL_RST        (1u << 26)
#define STATUS_LU       (1u << 1)
#define EECD_AUTO_RD    (1u << 9)       /* the reset's reading of the EEPROM is done */
#define EERD_START      (1u << 0)
#define EERD_DONE       (1u << 1)
#define CTRL_EXT_DRV_LOAD (1u << 28)    /* tells a management firmware a driver runs */
#define MDIC_PHY        (1u << 21)      /* the internal PHY is number 1 */
#define MDIC_WRITE      (1u << 26)
#define MDIC_READ       (2u << 26)
#define MDIC_READY      (1u << 28)
#define MDIC_ERROR      (1u << 30)
#define PHY_BMCR        0               /* the PHY's control register */
#define BMCR_RESTART_AN (1u << 9)
#define BMCR_POWER_DOWN (1u << 11)
#define BMCR_AN_ENABLE  (1u << 12)
#define RCTL_EN         (1u << 1)
#define RCTL_UPE        (1u << 3)       /* (38b) every unicast frame (set_promiscuous) */
#define RCTL_MPE        (1u << 4)       /* every multicast frame (set_multicast's -1) */
#define RCTL_BAM        (1u << 15)      /* broadcasts too */
#define RCTL_SECRC      (1u << 26)      /* strip the Ethernet checksum */
#define SRRCTL_2K       2               /* buffer size in KiB */
#define SRRCTL_HEADER   (4u << 8)       /* header buffer 256 bytes (unused with one buffer) */
#define SRRCTL_ADVANCED (1u << 25)      /* advanced descriptors, one buffer */
#define QUEUE_ENABLE    (1u << 25)      /* RXDCTL, TXDCTL */
/* When the chip fetches descriptors and writes them back (Intel's own
 * driver's values): fetch when fewer than PTHRESH are on the chip and at
 * least HTHRESH are in memory; write back after WTHRESH are done. */
#define RXDCTL_THRESH   (8u | 8u << 8 | 1u << 16)
#define TXDCTL_THRESH   (8u | 1u << 8 | 1u << 16)
#define TCTL_EN         (1u << 1)
#define TCTL_PSP        (1u << 3)       /* pad short frames */
#define TCTL_CT         (0x0Fu << 4)    /* collision threshold */
#define TCTL_COLD       (0x3Fu << 12)   /* collision distance (full duplex) */
#define RAH_AV          (1u << 31)
#define ICR_RXDW        (1u << 7)       /* receive descriptors written back */
#define ICR_RXO         (1u << 6)       /* receive overrun */
#define ICR_RXDMT0      (1u << 4)       /* receive descriptors running low */
#define IVAR_VALID      0x80u
#define GPIE_NSICR      (1u << 0)       /* reading ICR clears it in every mode */
#define GPIE_MSIX_MODE  (1u << 4)
#define GPIE_PBA        (1u << 31)      /* MSI-X's pending bits (Linux sets it) */

#define RX_DD           (1u << 0)       /* written back: descriptor done */
#define RX_EOP          (1u << 1)       /* end of packet */
#define RX_ERROR        (1u << 31)      /* RXE: a CRC or symbol error */
#define TX_DATA         (3u << 20)      /* advanced data descriptor */
#define TX_EOP          (1u << 24)
#define TX_IFCS         (1u << 25)      /* insert the checksum */
#define TX_RS           (1u << 27)      /* report status (sets DD) */
#define TX_DEXT         (1u << 29)      /* an advanced descriptor */
#define TX_PAYLEN_SHIFT 14
#define TX_DD           (1u << 0)

#define RING            64              /* descriptors per ring: 1 KiB */
#define BUFFER          2048

/* Given to the card: the buffer's address, then 0. Written back by the
 * card: packet type and hash, then status and errors, length, VLAN. */
struct rx_desc {
    uint64_t address;
    uint32_t status;
    uint16_t length, vlan;
} __attribute__((packed));

/* The card writes its status (DD) over olinfo when it is done. */
struct tx_desc {
    uint64_t address;
    uint32_t cmd_type_len;
    uint32_t olinfo;
} __attribute__((packed));

struct igb {
    struct net_card *dev;               /* (67c) its card (net_card_new): its priv is this */
    volatile uint8_t *regs;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint8_t *rx_buffers[RING], *tx_buffers[RING];
    uint64_t rx_phys[RING], tx_phys[RING];
    bool tx_used[RING];
    uint32_t rx_next, tx_next;
    struct spinlock tx_lock;
    struct wait_queue rx_waiters;
    bool msi;                           /* MSI or MSI-X; else polled */
    bool msix;
    volatile uint32_t icr_seen, eicr_seen;  /* what the tick found pending (missed interrupts) */
    volatile uint64_t interrupts, found_by_tick;
    struct mutex stats_lock;
    uint64_t crc_errors, missed, good_rx, good_tx, no_buffer, all_rx, queue_dropped;
    bool promiscuous, all_multicast;    /* (38b) RCTL's UPE and MPE as asked */
};

static uint32_t rd(struct igb *c, uint32_t reg)
{
    return mmio_read32(c->regs + reg);
}

static void wr(struct igb *c, uint32_t reg, uint32_t value)
{
    mmio_write32(c->regs + reg, value);
}

static uint32_t link_speed(struct net_card *d)
{
    struct igb *c = d->priv;
    uint32_t status = rd(c, REG_STATUS);
    if (!(status & STATUS_LU))
        return 0;
    uint32_t speed = status >> 6 & 3;
    return speed == 0 ? 10 : speed == 1 ? 100 : 1000;
}

static bool send(struct net_card *d, const void *frame, size_t length)
{
    struct igb *c = d->priv;
    if (length > ETH_FRAME_MAX || length < ETH_HEADER)
        return false;
    uint64_t state = spin_lock(&c->tx_lock);
    uint32_t i = c->tx_next;
    struct tx_desc *t = &c->tx[i];
    /* A descriptor used before must be done (DD) before it is used again. */
    bool free = !c->tx_used[i] || (t->olinfo & TX_DD);
    if (free) {
        memcpy(c->tx_buffers[i], frame, length);
        t->address = c->tx_phys[i];
        t->cmd_type_len = (uint32_t)length | TX_DATA | TX_EOP | TX_IFCS | TX_RS | TX_DEXT;
        t->olinfo = (uint32_t)length << TX_PAYLEN_SHIFT;
        c->tx_used[i] = true;
        c->tx_next = (i + 1) % RING;
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

static bool rx_ready(struct igb *c)
{
    return c->rx[c->rx_next].status & RX_DD;
}

/* The card's interrupt (MSI): acknowledge its causes, wake the thread. */
static void interrupt(void *context)
{
    struct igb *c = context;
    wr(c, REG_ICR, rd(c, REG_ICR));
    if (c->msix)
        wr(c, REG_EICR, rd(c, REG_EICR));           /* (EIAC cleared it already) */
    c->interrupts++;
    wait_queue_wake_all(&c->rx_waiters);
}

/* The timer tick (interrupt context): frames waiting? Wake the thread.
 * With MSI only a safety net: what it finds, an interrupt missed. */
static void tick(void *context)
{
    struct igb *c = context;
    if (rx_ready(c)) {
        if (c->msi) {
            c->found_by_tick++;
            uint32_t icr = rd(c, REG_ICR), eicr = rd(c, REG_EICR);
            if (icr)
                c->icr_seen = icr;
            if (eicr)
                c->eicr_seen = eicr;
        }
        wait_queue_wake_all(&c->rx_waiters);
    }
}

/* The card's thread: hand received frames on, give the buffers back. */
static void receiver(void *context)
{
    struct igb *c = context;
    for (;;) {
        wait_event(&c->rx_waiters, rx_ready(c));
        arch_read_barrier();                    /* DD before the frame's bytes */
        net_batch_begin();                      /* (65e) one ACK per connection for a burst */
        while (rx_ready(c)) {
            uint32_t i = c->rx_next;
            struct rx_desc *r = &c->rx[i];
            if ((r->status & RX_EOP) && !(r->status & RX_ERROR) && r->length <= ETH_FRAME_MAX)
                net_receive(c->dev, c->rx_buffers[i], r->length);
            r->address = c->rx_phys[i];         /* the card wrote its report over it */
            r->status = 0;
            r->length = 0;
            r->vlan = 0;
            c->rx_next = (i + 1) % RING;
            arch_write_barrier();
            wr(c, REG_RDT, i);                  /* the buffer is the card's again */
        }
        net_batch_end();
    }
}

/* net's line for the card: what the chip itself counted (its statistics
 * registers clear when read, so they are added up here) and where its
 * rings stand (RDH and TDH move as the chip uses descriptors). */
static void details(struct net_card *d)
{
    struct igb *c = d->priv;
    mutex_lock(&c->stats_lock);
    c->crc_errors += rd(c, REG_CRCERRS);
    c->missed += rd(c, REG_MPC);
    c->good_rx += rd(c, REG_GPRC);
    c->good_tx += rd(c, REG_GPTC);
    c->no_buffer += rd(c, REG_RNBC);
    c->all_rx += rd(c, REG_TPR);
    c->queue_dropped += rd(c, REG_RQDPC);
    kprintf("       igb: chip received %lu (good %lu, missed %lu, no buffer %lu, CRC errors %lu, "
            "queue dropped %lu), sent %lu\n", c->all_rx, c->good_rx, c->missed, c->no_buffer,
            c->crc_errors, c->queue_dropped, c->good_tx);
    mutex_unlock(&c->stats_lock);
    kprintf("       igb: receive ring head %u tail %u, send ring head %u tail %u; STATUS 0x%08x, "
            "RCTL 0x%08x, RXDCTL 0x%08x, TXDCTL 0x%08x\n", rd(c, REG_RDH), rd(c, REG_RDT),
            rd(c, REG_TDH), rd(c, REG_TDT), rd(c, REG_STATUS), rd(c, REG_RCTL), rd(c, REG_RXDCTL),
            rd(c, REG_TXDCTL));
    if (c->msi)
        kprintf("       igb: interrupts: %s, %lu, frames found by the tick instead %lu\n",
                c->msix ? "MSI-X" : "MSI", c->interrupts, c->found_by_tick);
    else
        kprintf("       igb: interrupts: none (no MSI), polled by the tick\n");
    uint8_t cap = pci_find_capability(c->dev->pci, PCI_CAP_MSI);
    uint8_t xcap = pci_find_capability(c->dev->pci, PCI_CAP_MSIX);
    uint16_t control = cap ? pci_read16(c->dev->pci, cap + 2) : 0;
    kprintf("       igb: IMS 0x%08x, IVAR0 0x%08x, EITR0 0x%08x, GPIE 0x%08x, EIMS 0x%08x; "
            "MSI control 0x%04x, address 0x%08x, data 0x%04x; MSI-X control 0x%04x; "
            "the tick saw ICR 0x%08x, EICR 0x%08x\n", rd(c, REG_IMS), rd(c, REG_IVAR0),
            rd(c, REG_EITR0), rd(c, REG_GPIE), rd(c, REG_EIMS), control,
            cap ? pci_read32(c->dev->pci, cap + 4) : 0,
            cap ? pci_read16(c->dev->pci, cap + (control & 0x80 ? 12 : 8)) : 0,
            xcap ? pci_read16(c->dev->pci, xcap + 2) : 0, c->icr_seen, c->eicr_seen);
    kprintf("       igb: rings at 0x%lx (receive, %u bytes, SRRCTL 0x%08x, next 0x%08x) and 0x%lx "
            "(send), CTRL 0x%08x, CTRL_EXT 0x%08x\n",
            (uint64_t)rd(c, REG_RDBAH) << 32 | rd(c, REG_RDBAL), rd(c, REG_RDLEN),
            rd(c, REG_SRRCTL), c->rx[c->rx_next].status, (uint64_t)rd(c, REG_TDBAH) << 32 |
            rd(c, REG_TDBAL), rd(c, REG_CTRL), rd(c, REG_CTRL_EXT));
}

/* A word of the EEPROM (the MAC address, if the registers lack it). */
static uint16_t eeprom(struct igb *c, uint16_t address)
{
    wr(c, REG_EERD, EERD_START | (uint32_t)address << 2);
    for (int i = 0; i < 10000; i++) {
        uint32_t v = rd(c, REG_EERD);
        if (v & EERD_DONE)
            return (uint16_t)(v >> 16);
        arch_cpu_relax();
    }
    return 0;
}

/* A PHY register through MDIC; false if the PHY did not answer. */
static bool phy_access(struct igb *c, uint32_t command, uint16_t *value)
{
    wr(c, REG_MDIC, command | MDIC_PHY);
    for (int i = 0; i < 100; i++) {
        uint32_t v = rd(c, REG_MDIC);
        if (v & MDIC_READY) {
            *value = (uint16_t)v;
            return !(v & MDIC_ERROR);
        }
        timer_sleep_ms(1);
    }
    return false;
}

static bool phy_read(struct igb *c, uint32_t reg, uint16_t *value)
{
    return phy_access(c, MDIC_READ | reg << 16, value);
}

static bool phy_write(struct igb *c, uint32_t reg, uint16_t value)
{
    uint16_t ignored;
    return phy_access(c, MDIC_WRITE | reg << 16 | value, &ignored);
}

static const char *igb_model(const struct pci_device *pci)
{
    if (pci->vendor_id == 0x8086 && pci->device_id == 0x10C9)
        return "Intel 82576";           /* copper; QEMU's igb */
    return 0;
}

/* The groups the protocols want (netdev.set_multicast, 64a): the
 * multicast table's 4096 bits, one per value of a destination's bits
 * 36-47 (RCTL.MO 0: the last byte and the high half of the one before);
 * every multicast frame (RCTL.MPE) for -1. */
static void set_multicast(struct net_card *d, const uint8_t (*macs)[6], int count)
{
    struct igb *c = d->priv;
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
    struct igb *c = d->priv;
    c->promiscuous = on;
    uint32_t rctl = rd(c, REG_RCTL) & ~(RCTL_UPE | RCTL_MPE);
    wr(c, REG_RCTL, rctl | (on ? RCTL_UPE : 0) | (on || c->all_multicast ? RCTL_MPE : 0));
}

static void *fail(const char *where, const char *why)
{
    kprintf("  igb:           %s: %s\n", where, why);
    return 0;
}

static struct net_card *igb_init(struct pci_device *pci, const char *model)
{
    char where[16];
    ksnprintf(where, sizeof(where), "%02x:%02x.%u", pci->bus, pci->device, pci->function);
    struct pci_bar *bar = &pci->bars[0];
    if (!bar->size || bar->is_io)
        return fail(where, "no register BAR");
    struct igb *c = kzalloc(sizeof(*c));
    if (!c || !(c->dev = net_card_new()))
        return fail(where, "out of memory");
    pci_enable_dma(pci);
    pci_write16(pci, PCI_COMMAND, pci_read16(pci, PCI_COMMAND) | PCI_COMMAND_INTX_OFF);
    c->regs = paging_map_mmio(bar->address, bar->size);

    /* Stop what the firmware may have left running, give DMA in flight
     * time to end, reset the port; no interrupts (polled). */
    wr(c, REG_IMC, 0xFFFFFFFF);
    wr(c, REG_EIMC, 0xFFFFFFFF);
    wr(c, REG_RCTL, 0);
    wr(c, REG_TCTL, TCTL_PSP);
    timer_sleep_ms(10);
    wr(c, REG_CTRL, rd(c, REG_CTRL) | CTRL_RST);
    timer_sleep_ms(1);
    for (int i = 0; i < 1000 && (rd(c, REG_CTRL) & CTRL_RST); i++)
        timer_sleep_ms(1);
    for (int i = 0; i < 10 && !(rd(c, REG_EECD) & EECD_AUTO_RD); i++)
        timer_sleep_ms(1);
    wr(c, REG_IMC, 0xFFFFFFFF);
    wr(c, REG_EIMC, 0xFFFFFFFF);
    wr(c, REG_CTRL_EXT, rd(c, REG_CTRL_EXT) | CTRL_EXT_DRV_LOAD);
    wr(c, REG_CTRL, (rd(c, REG_CTRL) | CTRL_SLU) & ~(CTRL_FRCSPD | CTRL_FRCDPX));

    /* The PHY: powered on, the negotiation started again. */
    uint16_t bmcr;
    if (!phy_read(c, PHY_BMCR, &bmcr) ||
        !phy_write(c, PHY_BMCR, (uint16_t)((bmcr & ~BMCR_POWER_DOWN) | BMCR_AN_ENABLE |
                                           BMCR_RESTART_AN)))
        kprintf("  igb:           %s: the PHY does not answer\n", where);

    /* The MAC address: from the receive address registers (loaded from the
     * EEPROM at the reset), else from the EEPROM itself (the second port's
     * is the first's with the last bit flipped). */
    uint32_t ral = rd(c, REG_RAL0), rah = rd(c, REG_RAH0);
    if (!(rah & RAH_AV)) {
        uint16_t w[3] = { eeprom(c, 0), eeprom(c, 1), eeprom(c, 2) };
        ral = w[0] | (uint32_t)w[1] << 16;
        rah = w[2] | RAH_AV;
        if (pci->function & 1)
            rah ^= 1u << 8;
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
            c->rx_phys[i + k] = rp + (uint64_t)k * BUFFER;
            c->tx_phys[i + k] = tp + (uint64_t)k * BUFFER;
            c->rx[i + k].address = c->rx_phys[i + k];
        }
    }

    /* The queues come out of the reset switched on (queue 0): off first, as
     * the ring registers may only change then (the card says when it is). */
    wr(c, REG_RXDCTL, 0);
    wr(c, REG_TXDCTL, 0);
    for (int i = 0; i < 100 && ((rd(c, REG_RXDCTL) | rd(c, REG_TXDCTL)) & QUEUE_ENABLE); i++)
        timer_sleep_ms(1);

    /* Receiving: the ring, then the queue on (it says so when it is),
     * then receiving on, and only then the buffers to the card: a tail
     * written while RCTL.EN is off does not reach the receive unit
     * (FreeBSD's driver: "needs to be after enable"). The J1900's 82576
     * counted good frames with RDT 63 but its head stayed at 0 and it
     * reported "no buffer"; QEMU's model does not care. */
    wr(c, REG_RDBAL, (uint32_t)rx_phys);
    wr(c, REG_RDBAH, (uint32_t)(rx_phys >> 32));
    wr(c, REG_RDLEN, RING * sizeof(struct rx_desc));
    wr(c, REG_SRRCTL, SRRCTL_2K | SRRCTL_HEADER | SRRCTL_ADVANCED);
    wr(c, REG_RDH, 0);
    wr(c, REG_RDT, 0);
    wr(c, REG_RXDCTL, (rd(c, REG_RXDCTL) & ~0x001F1F1Fu) | RXDCTL_THRESH | QUEUE_ENABLE);
    for (int i = 0; i < 100 && !(rd(c, REG_RXDCTL) & QUEUE_ENABLE); i++)
        timer_sleep_ms(1);
    wr(c, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    wr(c, REG_RDT, RING - 1);                   /* all buffers but one are the card's */

    /* Sending. */
    wr(c, REG_TDBAL, (uint32_t)tx_phys);
    wr(c, REG_TDBAH, (uint32_t)(tx_phys >> 32));
    wr(c, REG_TDLEN, RING * sizeof(struct tx_desc));
    wr(c, REG_TDH, 0);
    wr(c, REG_TDT, 0);
    wr(c, REG_TXDCTL, (rd(c, REG_TXDCTL) & ~0x001F1F1Fu) | TXDCTL_THRESH | QUEUE_ENABLE);
    for (int i = 0; i < 100 && !(rd(c, REG_TXDCTL) & QUEUE_ENABLE); i++)
        timer_sleep_ms(1);
    wr(c, REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    if (!(rd(c, REG_RXDCTL) & QUEUE_ENABLE) || !(rd(c, REG_TXDCTL) & QUEUE_ENABLE))
        return fail(where, "its queues do not start");

    c->tx_lock = (struct spinlock)SPINLOCK_INIT("igb tx");
    c->stats_lock = (struct mutex)MUTEX_INIT("igb stats");
    for (uint32_t reg = REG_CRCERRS; reg <= REG_TPR; reg += 4)
        rd(c, reg);                             /* the statistics from 0 */
    c->dev->driver = "igb";
    ksnprintf(c->dev->model, sizeof(c->dev->model), "%s", model);
    ksnprintf(c->dev->where, sizeof(c->dev->where), "%s", where);
    c->dev->link_speed = link_speed;
    c->dev->send = send;
    c->dev->pci = pci;
    c->dev->details = details;
    c->dev->priv = c;
    c->dev->set_multicast = set_multicast;
    c->dev->set_promiscuous = set_promiscuous;
    net_register(c->dev);
    if (!thread_create(c->dev->name, receiver, c) || !timer_add_tick_handler(tick, c))
        return fail(where, "no thread for receiving");
    uint64_t address;
    uint32_t data;
    uint16_t entries;
    c->msix = pci_msix_enable(pci, interrupt, c, &address, &data, &entries);
    if (c->msix) {
        /* MSI-X as Linux sets it up (igb_configure_msix): the capability
         * on first, then the mode ("or our settings won't stick"), queue
         * 0's receive cause to vector 0 (IVAR), cleared by itself when its
         * message goes out (EIAC), and that vector unmasked (EIMS). On the
         * J1900, plain MSI raised nothing (44g: "MSI, 0" with IVAR set). */
        wr(c, REG_GPIE, GPIE_MSIX_MODE | GPIE_PBA | GPIE_NSICR);
        wr(c, REG_IVAR0, (rd(c, REG_IVAR0) & 0xFFFF0000u) | IVAR_VALID);
        wr(c, REG_EIAC, 1);
        rd(c, REG_ICR);
        wr(c, REG_EICR, 0xFFFFFFFF);                /* old causes away */
        wr(c, REG_EIMS, 1);
        c->msi = true;
    } else if ((c->msi = pci_msi_enable(pci, interrupt, c, &address, &data))) {
        /* Queue 0's receive and send causes to vector 0, as Linux does in
         * MSI mode too (igb_assign_vector): the J1900's 82576 raised no
         * interrupt without it (MSI, 0; all frames found by the tick),
         * QEMU's model raises one regardless. */
        wr(c, REG_IVAR0, (rd(c, REG_IVAR0) & 0xFFFF0000u) | IVAR_VALID | IVAR_VALID << 8);
        rd(c, REG_ICR);                             /* old causes away */
        wr(c, REG_IMS, ICR_RXDW | ICR_RXO | ICR_RXDMT0);
    }
    return c->dev;
}

static bool start(struct pci_device *pci)
{
    const char *model = igb_model(pci);
    return model && igb_init(pci, model);
}

/* The chip igb_model knows (QEMU's igb, the J1900's two-port card). */
static const struct module_pci ids[] = {
    { 0x8086, 0x10C9 },
};

MODULE(igb, start, 0, 0, ids)
