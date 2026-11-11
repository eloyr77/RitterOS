/*
 * Realtek RTL8111/8168 (Gigabit) and RTL8125 (2.5 Gigabit) Ethernet
 * ("rtl8168"; the test PC's own port, an RTL8168G, and the laptop's, an
 * RTL8168H). Realtek publishes no manual for these chips: the register
 * facts are the ones other systems' drivers rely on. The tests run on our
 * own model of the chips in QEMU (scripts/qemu/rtl8168.c); only the real
 * chips show whether the facts hold.
 *
 * (67a) A kernel module (kernel/module.h), the first: loaded after the
 * root mount when a chip in its table is on PCI; its start sets each one
 * up (rtl8168_init) and registers it as ethN.
 *
 * Both rings are 16-byte descriptors: opts1 (OWN, the ring's end, first
 * and last fragment, the length), opts2, the buffer's address. There is
 * no head or tail register: the chip takes every descriptor marked OWN
 * and clears OWN when it is done; the ring's last descriptor carries END,
 * so the chip goes back to the first. Sending: fill a descriptor, then
 * TxPoll. A received frame's length includes its 4-byte Ethernet checksum.
 *
 * The family spans many versions (TxConfig bits 30:20). From the RTL8168G
 * on (version 0x4c0, and every RTL8125) the PHY is reached through OCP
 * (GPHY_OCP) instead of PHYAR, the "RXDV gate" (MISC bit 19) must be
 * opened before anything is received, and the chip may still be in its
 * management firmware's hands ("out of band") until the driver takes it.
 * The PHY may come powered down: it is switched on and its negotiation
 * started again. A received frame raises an interrupt (MSI) that wakes the
 * card's thread; without MSI the timer tick polls, and it stays as a
 * safety net.
 */
#include "driver.h"
#include "driver_net.h"

#define R_MAC0          0x00
#define R_MAR0          0x08
#define R_COUNTERS      0x10            /* where the tally counters go; high half at 0x14 */
#define R_TXDESC        0x20            /* low half; the high half at 0x24 */
#define R_CHIPCMD       0x37
#define R_TXPOLL        0x38            /* RTL8168 */
#define R_IMR           0x3C            /* RTL8168: 16 bits; status at 0x3E */
#define R_ISR           0x3E
#define R_IMR_8125      0x38            /* RTL8125: 32 bits; status at 0x3C */
#define R_ISR_8125      0x3C
#define R_TXCONFIG      0x40
#define R_RXCONFIG      0x44
#define R_CFG9346       0x50
#define R_CONFIG3       0x54
#define R_PHYAR         0x60
#define R_PHYSTATUS     0x6C
#define R_ERIDR         0x70            /* the chip's ERI registers: data, then the command */
#define R_ERIAR         0x74
#define R_PMCH          0x6F
#define R_TXPOLL_8125   0x90
#define R_OCPDR         0xB0            /* the chip's own OCP registers */
#define R_GPHY_OCP      0xB8            /* the PHY's, through OCP */
#define R_DLLPR         0xD0
#define R_MCU           0xD3
#define R_RXMAXSIZE     0xDA
#define R_MITIGATE      0xE2
#define R_RXDESC        0xE4            /* low half; the high half at 0xE8 */
#define R_MAXTXPACKET   0xEC
#define R_MISC          0xF0
#define R_MISC_1        0xF2
#define R_MAC0_BKP      0x19E0          /* RTL8125: the MAC address's copy */
#define R_RSS_8125      0x4500
#define R_QUEUES_8125   0x4800

#define COUNTERS_DUMP   0x08            /* in R_COUNTERS: write them now */
#define INT_ROK         0x0001          /* a frame received */
#define INT_RDU         0x0010          /* no receive descriptor left */
#define CMD_RESET       0x10
#define CMD_RX          0x08
#define CMD_TX          0x04
#define TXPOLL_NPQ      0x40
#define TXCFG_EMPTY     (1u << 11)
#define TXCFG_AUTO_FIFO (1u << 7)
#define TXCFG_DMA       (7u << 8)       /* no limit on DMA bursts */
#define TXCFG_IFG       (3u << 24)      /* the normal gap between frames */
#define RX_ACCEPT_ALL   0x01            /* (38b) AcceptAllPhys: every unicast frame */
#define RX_ACCEPT_MINE  0x02
#define RX_ACCEPT_MULTI 0x04
#define RX_ACCEPT_BCAST 0x08
#define RX_DMA          (7u << 8)
#define RX_128_INT      (1u << 15)
#define RX_MULTI        (1u << 14)
#define RX_EARLY_OFF    (1u << 11)
#define RX_FETCH_8125   (8u << 27)
#define CFG_UNLOCK      0xC0
#define CFG_LOCK        0x00
#define MCU_OOB         0x80            /* the management firmware has the chip */
#define MCU_FIFOS_EMPTY 0x30
#define MCU_LIST_READY  0x02
#define MISC_RXDV_GATE  (1u << 19)
#define ERI_FLAG        (1u << 31)      /* write: set until done; read: set when done */
#define ERI_MASK_ALL    (0xFu << 12)    /* bytes written: all four */
#define ERI_MASK_LOW2   (0x3u << 12)
#define ERI_MASK_LOW1   (0x1u << 12)
#define CONFIG3_L23     0x02            /* "ready to L2/L3": off */
#define DLLPR_PFM       0x40
#define DLLPR_TX_10M_PS 0x80
#define MISC_1_PFM_D3   0x40
#define OCP_FLAG        (1u << 31)
#define OCP_PHY_BASE    0xA400          /* the PHY's standard registers in OCP space */
#define PHY_BMCR        0
#define BMCR_RESTART_AN (1u << 9)
#define BMCR_POWER_DOWN (1u << 11)
#define BMCR_AN_ENABLE  (1u << 12)
/* (38d) The standard registers that say what the PHY can do and offers. */
#define PHY_BMSR        1
#define BMSR_ESTATUS    (1u << 8)       /* register 15 says the 1000BASE-T modes */
#define PHY_ANAR        4
#define ANAR_MODES      0x01E0          /* 10 half, 10 full, 100 half, 100 full */
#define PHY_GBCR        9               /* 1000BASE-T control */
#define GBCR_1000_HALF  (1u << 8)
#define GBCR_1000_FULL  (1u << 9)
#define GBCR_MODES      (GBCR_1000_HALF | GBCR_1000_FULL)
#define PHY_ESTATUS     15
#define ESTATUS_1000T_HALF (1u << 12)
#define ESTATUS_1000T_FULL (1u << 13)

#define DESC_OWN        (1u << 31)
#define DESC_END        (1u << 30)
#define DESC_FIRST      (1u << 29)
#define DESC_LAST       (1u << 28)
#define RX_ERROR        (1u << 21)      /* RES: a CRC, runt or too long frame */
#define RX_LENGTH       0x3FFF

#define RING            256             /* (65e) as Linux's r8169; 64 before: the laptop's chip
                                         * missed frames at 27 MB/s, its ring full */
#define BUFFER          2048
#define TX_TIMEOUT      (5 * TIMER_HZ)  /* (64p) ticks without a frame sent, then a reset */
#define ETH_MIN         60              /* shorter frames are padded */

struct desc {
    uint32_t opts1, opts2;
    uint64_t address;
} __attribute__((packed));

/* The chip's tally counters, as it writes them (since its reset). */
struct tally {
    uint64_t tx_ok, rx_ok, tx_errors;
    uint32_t rx_errors;
    uint16_t rx_missed, align_errors;
    uint32_t tx_one_collision, tx_collisions;
    uint64_t rx_mine, rx_broadcast;
    uint32_t rx_multicast;
    uint16_t tx_aborted, tx_underrun;
} __attribute__((packed));

#define CARDS 8

struct rtl {
    struct net_card *dev;               /* (67b) its card (net_card_new): its priv is this */
    volatile uint8_t *regs;
    uint64_t regs_size;
    bool is8125, newer;
    uint32_t version;                   /* TxConfig bits 30:20 */
    struct desc *rx, *tx;
    uint8_t *rx_buffers[RING], *tx_buffers[RING];
    uint64_t rx_phys[RING], tx_phys[RING];
    uint32_t rx_next, tx_next;
    uint64_t rx_ring_phys, tx_ring_phys;
    /* (64p) The send watchdog: descriptors given to the chip and not yet
     * done (tx_pending, from tx_clean on), ticks since one was last done
     * while some wait with the link up, and the reset it asks for. */
    uint32_t tx_clean, tx_pending, tx_quiet;
    bool tx_stuck, restarting;
    uint64_t restarts, lost_in_reset;
    uint64_t sleeps;                    /* (37g-3) */
    int aspm_was, aspm_port_was;        /* (65e) the link's ASPM before we switched it off */
    uint32_t mar[2];                    /* the multicast filter, for a reset */
    bool promiscuous;                   /* (38b) every frame: AcceptAllPhys, the hash all ones */
    struct spinlock tx_lock;
    struct wait_queue rx_waiters;
    bool msi;                           /* else polled */
    volatile uint64_t interrupts, found_by_tick;
    struct mutex tally_lock;
    uint64_t tally_phys;
};

static struct rtl *cards[CARDS];          /* (37g-3) for sleep: the cards by device */
static int ncards;

static uint8_t rd8(struct rtl *c, uint32_t reg) { return mmio_read8(c->regs + reg); }
static uint16_t rd16(struct rtl *c, uint32_t reg) { return mmio_read16(c->regs + reg); }
static uint32_t rd32(struct rtl *c, uint32_t reg) { return mmio_read32(c->regs + reg); }
static void wr8(struct rtl *c, uint32_t reg, uint8_t v) { mmio_write8(c->regs + reg, v); }
static void wr16(struct rtl *c, uint32_t reg, uint16_t v) { mmio_write16(c->regs + reg, v); }
static void wr32(struct rtl *c, uint32_t reg, uint32_t v) { mmio_write32(c->regs + reg, v); }

/* Wait up to ms milliseconds for (register & mask) == want. */
static bool wait8(struct rtl *c, uint32_t reg, uint8_t mask, uint8_t want, int ms)
{
    for (int i = 0; i <= ms; i++) {
        if ((rd8(c, reg) & mask) == want)
            return true;
        timer_sleep_ms(1);
    }
    return false;
}

static bool wait16(struct rtl *c, uint32_t reg, uint16_t mask, uint16_t want, int ms)
{
    for (int i = 0; i <= ms; i++) {
        if ((rd16(c, reg) & mask) == want)
            return true;
        timer_sleep_ms(1);
    }
    return false;
}

static bool wait32(struct rtl *c, uint32_t reg, uint32_t mask, uint32_t want, int ms)
{
    for (int i = 0; i <= ms; i++) {
        if ((rd32(c, reg) & mask) == want)
            return true;
        timer_sleep_ms(1);
    }
    return false;
}

/* The chip's own registers in OCP space (newer chips). */
static uint16_t mac_ocp_read(struct rtl *c, uint32_t reg)
{
    wr32(c, R_OCPDR, reg << 15);
    return (uint16_t)rd32(c, R_OCPDR);
}

static void mac_ocp_write(struct rtl *c, uint32_t reg, uint16_t value)
{
    wr32(c, R_OCPDR, OCP_FLAG | reg << 15 | value);
}

/* The chip's ERI registers (FIFO sizes, pause thresholds, filters; the
 * RTL8168G and later), as Linux's r8169 reaches them. */
static void eri_write(struct rtl *c, uint32_t reg, uint32_t mask, uint32_t value)
{
    wr32(c, R_ERIDR, value);
    wr32(c, R_ERIAR, ERI_FLAG | mask | reg);
    wait32(c, R_ERIAR, ERI_FLAG, 0, 2);
}

static uint32_t eri_read(struct rtl *c, uint32_t reg)
{
    wr32(c, R_ERIAR, ERI_MASK_ALL | reg);
    return wait32(c, R_ERIAR, ERI_FLAG, ERI_FLAG, 2) ? rd32(c, R_ERIDR) : 0xFFFFFFFF;
}

static void eri_change(struct rtl *c, uint32_t reg, uint32_t set, uint32_t clear)
{
    eri_write(c, reg, ERI_MASK_ALL, (eri_read(c, reg) & ~clear) | set);
}

/* The settings Linux's driver makes on the RTL8168G and H before they
 * receive and send (rtl_hw_start_8168g, _8168h_1): the FIFOs' sizes (the
 * laptop's RTL8168H received but never sent without them; the J1900's
 * firmware had probably set its RTL8168G's), the pause thresholds, the
 * packet filter reset, power saving that gets in the way off; the G's and
 * H's own settings only for them (the EP and the others get the common
 * part, as in Linux). The configuration registers are unlocked. */
static void start_8168g(struct rtl *c)
{
    bool h = (c->version & 0x7C0) == 0x540;                 /* RTL8168H, RTL8117 */
    bool g = (c->version & 0x7C0) == 0x4C0 || c->version == 0x509 || c->version == 0x5C8;
    eri_write(c, 0xC8, ERI_MASK_ALL, 0x08 << 16 | 0x10);    /* receive FIFO */
    eri_write(c, 0xE8, ERI_MASK_ALL, 0x02 << 16 | 0x06);    /* send FIFO */
    eri_write(c, 0xCC, ERI_MASK_LOW1, 0x38);                /* pause thresholds */
    eri_write(c, 0xD0, ERI_MASK_LOW1, 0x48);
    eri_change(c, 0xDC, 0, 1);                              /* the packet filter reset */
    eri_change(c, 0xDC, 1, 0);
    if (h) {
        eri_change(c, 0xDC, 0x1C, 0);
        eri_write(c, 0x5F0, ERI_MASK_LOW2, 0x4F87);
    } else if (g) {
        eri_write(c, 0x2F8, ERI_MASK_LOW2, 0x1D8F);
    }
    eri_write(c, 0xC0, ERI_MASK_LOW2, 0);
    eri_write(c, 0xB8, ERI_MASK_LOW2, 0);
    if (h) {
        wr8(c, R_DLLPR, rd8(c, R_DLLPR) & ~(DLLPR_PFM | DLLPR_TX_10M_PS));
        wr8(c, R_MISC_1, rd8(c, R_MISC_1) & ~MISC_1_PFM_D3);
    } else if (g) {
        eri_change(c, 0x2FC, 0x01, 0x06);
    }
    eri_change(c, 0x1B0, 0, 1u << 12);
    wr8(c, R_CONFIG3, rd8(c, R_CONFIG3) & ~CONFIG3_L23);
}

/* A PHY register: through OCP on newer chips, PHYAR on older ones. */
static bool phy_read(struct rtl *c, uint32_t reg, uint16_t *value)
{
    uint32_t at = c->newer ? R_GPHY_OCP : R_PHYAR;
    wr32(c, at, c->newer ? (OCP_PHY_BASE + reg * 2) << 15 : reg << 16);
    if (!wait32(c, at, OCP_FLAG, OCP_FLAG, 10))
        return false;
    *value = (uint16_t)rd32(c, at);
    return true;
}

static bool phy_write(struct rtl *c, uint32_t reg, uint16_t value)
{
    uint32_t at = c->newer ? R_GPHY_OCP : R_PHYAR;
    wr32(c, at, OCP_FLAG | (c->newer ? (OCP_PHY_BASE + reg * 2) << 15 : reg << 16) | value);
    return wait32(c, at, OCP_FLAG, 0, 10);
}

static uint32_t link_speed(struct net_card *d)
{
    struct rtl *c = d->priv;
    uint16_t status = c->is8125 ? rd16(c, R_PHYSTATUS) : rd8(c, R_PHYSTATUS);
    if (!(status & 0x02))
        return 0;
    if (c->is8125 && (status & 0x400))
        return 2500;
    return status & 0x10 ? 1000 : status & 0x08 ? 100 : 10;
}

static void tx_poll(struct rtl *c)
{
    if (c->is8125)
        wr8(c, R_TXPOLL_8125, 0x01);
    else
        wr8(c, R_TXPOLL, TXPOLL_NPQ);
}

static bool send(struct net_card *d, const void *frame, size_t length)
{
    struct rtl *c = d->priv;
    if (length > ETH_FRAME_MAX || length < ETH_HEADER)
        return false;
    uint64_t state = spin_lock(&c->tx_lock);
    uint32_t i = c->tx_next;
    struct desc *t = &c->tx[i];
    bool free = !(t->opts1 & DESC_OWN) && !c->restarting;   /* the chip is done with it */
    if (free) {
        size_t padded = length < ETH_MIN ? ETH_MIN : length;
        memcpy(c->tx_buffers[i], frame, length);
        memset(c->tx_buffers[i] + length, 0, padded - length);
        t->address = c->tx_phys[i];
        t->opts2 = 0;
        arch_write_barrier();                   /* the rest before OWN */
        t->opts1 = DESC_OWN | DESC_FIRST | DESC_LAST | (i == RING - 1 ? DESC_END : 0) |
                   (uint32_t)padded;
        c->tx_next = (i + 1) % RING;
        c->tx_pending++;
        arch_write_barrier();                   /* the descriptor before the poll */
        tx_poll(c);
        d->tx_frames++;
        d->tx_bytes += length;
    } else {
        d->tx_dropped++;
    }
    spin_unlock(&c->tx_lock, state);
    return free;
}

static bool rx_ready(struct rtl *c)
{
    return !(c->rx[c->rx_next].opts1 & DESC_OWN);
}

/* Give receive descriptor i (back) to the chip. */
static void rx_give(struct rtl *c, uint32_t i)
{
    c->rx[i].address = c->rx_phys[i];
    c->rx[i].opts2 = 0;
    arch_write_barrier();
    c->rx[i].opts1 = DESC_OWN | (i == RING - 1 ? DESC_END : 0) | BUFFER;
}

/* The chip's interrupt (MSI): acknowledge its causes, wake the thread. */
static void interrupt(void *context)
{
    struct rtl *c = context;
    if (c->is8125)
        wr32(c, R_ISR_8125, rd32(c, R_ISR_8125));
    else
        wr16(c, R_ISR, rd16(c, R_ISR));
    c->interrupts++;
    wait_queue_wake_all(&c->rx_waiters);
}

/* The timer tick (interrupt context): frames waiting? Wake the thread.
 * With MSI only a safety net: what it finds, an interrupt missed. */
static void tick(void *context)
{
    struct rtl *c = context;
    if (rx_ready(c)) {
        if (c->msi)
            c->found_by_tick++;
        wait_queue_wake_all(&c->rx_waiters);
    }
    /* (64p) The send watchdog, as Linux's for these chips (its r8169 resets
     * a chip whose queue stopped for 6 s): frames the chip has had for
     * TX_TIMEOUT with the link up and none sent meanwhile. */
    uint64_t state = spin_lock(&c->tx_lock);
    while (!c->restarting && c->tx_pending && !(c->tx[c->tx_clean].opts1 & DESC_OWN)) {
        c->tx_clean = (c->tx_clean + 1) % RING;
        c->tx_pending--;
        c->tx_quiet = 0;
    }
    if (!c->tx_pending || c->restarting || !link_speed(c->dev)) {
        c->tx_quiet = 0;
    } else if (++c->tx_quiet >= TX_TIMEOUT && !c->tx_stuck) {
        c->tx_stuck = true;
        wait_queue_wake_all(&c->rx_waiters);
    }
    spin_unlock(&c->tx_lock, state);
}

static void program(struct rtl *c);
static void interrupts_on(struct rtl *c);
static void rings_go(struct rtl *c);

/* (64p) Reset a chip that stopped sending and start it again with empty
 * rings (the frames it held are lost: TCP sends them again), as at the
 * first start; the address filters as they were. On the card's thread,
 * while send() refuses frames. */
static void restart(struct rtl *c)
{
    uint64_t state = spin_lock(&c->tx_lock);
    c->restarting = true;
    uint32_t waiting = c->tx_pending;
    spin_unlock(&c->tx_lock, state);
    kprintf("%s: the chip sent nothing for %lu s with %u frame(s) waiting: reset\n", c->dev->name,
            (uint64_t)(TX_TIMEOUT / TIMER_HZ), waiting);
    if (c->is8125)
        wr32(c, R_IMR_8125, 0);
    else
        wr16(c, R_IMR, 0);
    if (c->newer) {
        wr32(c, R_MISC, rd32(c, R_MISC) | MISC_RXDV_GATE);
        wait32(c, R_TXCONFIG, TXCFG_EMPTY, TXCFG_EMPTY, 10);
    }
    wr8(c, R_CHIPCMD, CMD_RESET);
    if (!wait8(c, R_CHIPCMD, CMD_RESET, 0, 100))
        kprintf("%s: the reset does not end\n", c->dev->name);
    rings_go(c);
    c->restarts++;
}

/* A reset chip running again: empty rings, programmed, the address
 * filters as they were, its interrupts; then send() takes frames again
 * (those it held are lost: TCP sends them again). */
static void rings_go(struct rtl *c)
{
    memset(c->tx, 0, RING * sizeof(struct desc));
    c->tx[RING - 1].opts1 = DESC_END;
    for (int i = 0; i < RING; i++)
        rx_give(c, i);
    c->rx_next = 0;
    program(c);
    wr32(c, R_MAR0 + 4, c->promiscuous ? 0xFFFFFFFF : c->mar[1]);
    wr32(c, R_MAR0, c->promiscuous ? 0xFFFFFFFF : c->mar[0]);
    if (c->msi)
        interrupts_on(c);
    uint64_t state = spin_lock(&c->tx_lock);
    c->tx_next = c->tx_clean = 0;
    c->lost_in_reset += c->tx_pending;
    c->tx_pending = c->tx_quiet = 0;
    c->tx_stuck = c->restarting = false;
    spin_unlock(&c->tx_lock, state);
}

/* The card's thread: hand received frames on, give the buffers back. */
static void receiver(void *context)
{
    struct rtl *c = context;
    for (;;) {
        wait_event(&c->rx_waiters, rx_ready(c) || c->tx_stuck);
        if (c->tx_stuck) {
            restart(c);
            continue;
        }
        arch_read_barrier();                    /* OWN before the frame's bytes */
        net_batch_begin();                      /* (65e) one ACK per connection for a burst */
        while (rx_ready(c)) {
            uint32_t i = c->rx_next;
            uint32_t status = c->rx[i].opts1;
            uint32_t length = status & RX_LENGTH;
            bool whole = (status & DESC_FIRST) && (status & DESC_LAST);
            if (whole && !(status & RX_ERROR) && length >= ETH_HEADER + 4 &&
                length - 4 <= ETH_FRAME_MAX)
                net_receive(c->dev, c->rx_buffers[i], length - 4);
            c->rx_next = (i + 1) % RING;
            rx_give(c, i);
        }
        net_batch_end();
    }
}

/* net's lines for the chip: its tally counters (it writes them into
 * memory when asked), its state, and how many send descriptors it still
 * has (marked OWN, not sent yet). */
static void details(struct net_card *d)
{
    struct rtl *c = d->priv;
    mutex_lock(&c->tally_lock);
    struct tally *t = phys_to_virt(c->tally_phys);
    memset(t, 0, sizeof(*t));
    wr32(c, R_COUNTERS + 4, (uint32_t)(c->tally_phys >> 32));
    wr32(c, R_COUNTERS, (uint32_t)c->tally_phys);
    wr32(c, R_COUNTERS, (uint32_t)c->tally_phys | COUNTERS_DUMP);
    if (wait32(c, R_COUNTERS, COUNTERS_DUMP, 0, 10)) {
        arch_read_barrier();
        kprintf("       rtl8168: chip sent %lu (errors %lu, aborted %u, underruns %u), received %lu "
                "(errors %u, missed %u, to us %lu, broadcasts %lu)\n", t->tx_ok, t->tx_errors,
                t->tx_aborted, t->tx_underrun, t->rx_ok, t->rx_errors, t->rx_missed, t->rx_mine,
                t->rx_broadcast);
    } else {
        kprintf("       rtl8168: the chip does not write its counters\n");
    }
    /* (38d) The PHY's own registers: what it can do (BMSR, ESTATUS), what it
     * advertises (ANAR, GBCR) and what the other end offered (ANLPAR, GBSR):
     * why a link came up at its speed. */
    uint16_t bmsr = 0, anar = 0, anlpar = 0, gbcr = 0, gbsr = 0, estatus = 0;
    if (!c->newer)
        phy_write(c, 0x1F, 0);                  /* its standard page */
    if (phy_read(c, 1, &bmsr) && phy_read(c, 4, &anar) && phy_read(c, 5, &anlpar) &&
        phy_read(c, 9, &gbcr) && phy_read(c, 10, &gbsr) && phy_read(c, 15, &estatus))
        kprintf("       rtl8168: PHY BMSR 0x%04x, ESTATUS 0x%04x; advertised 0x%04x, 1000BASE-T "
                "0x%04x; partner 0x%04x, 1000BASE-T 0x%04x\n", bmsr, estatus, anar, gbcr, anlpar,
                gbsr);
    else
        kprintf("       rtl8168: PHY registers not read\n");
    mutex_unlock(&c->tally_lock);
    int waiting = 0;
    for (int i = 0; i < RING; i++)
        waiting += (c->tx[i].opts1 & DESC_OWN) != 0;
    if (c->msi)
        kprintf("       rtl8168: interrupts: MSI, %lu, frames found by the tick instead %lu\n",
                c->interrupts, c->found_by_tick);
    else
        kprintf("       rtl8168: interrupts: none (no MSI), polled by the tick\n");
    static const char *const aspm[4] = { "off", "L0s", "L1", "L0s and L1" };
    if (c->aspm_was < 0)
        kprintf("       rtl8168: no PCIe link power saving (no PCIe capability)\n");
    else
        kprintf("       rtl8168: PCIe power saving (ASPM) off; it was %s, at the port above %s\n",
                aspm[c->aspm_was & 3], c->aspm_port_was < 0 ? "none" : aspm[c->aspm_port_was & 3]);
    kprintf("       rtl8168: reset after a send timeout: %lu time(s), %lu frame(s) lost then "
            "(and at %lu sleep(s))\n", c->restarts, c->lost_in_reset, c->sleeps);
    kprintf("       rtl8168: ChipCmd 0x%02x, PHYstatus 0x%04x, MISC 0x%08x, TxConfig 0x%08x, "
            "RxConfig 0x%08x; %d to send\n", rd8(c, R_CHIPCMD), rd16(c, R_PHYSTATUS),
            rd32(c, R_MISC), rd32(c, R_TXCONFIG), rd32(c, R_RXCONFIG), waiting);
    int next = -1;
    for (int i = 0; i < RING && next < 0; i++)
        if (c->tx[i].opts1 & DESC_OWN)
            next = i;
    if (c->newer && !c->is8125) {
        mutex_lock(&c->tally_lock);
        uint32_t tx_fifo = eri_read(c, 0xE8), rx_fifo = eri_read(c, 0xC8);
        mutex_unlock(&c->tally_lock);
        kprintf("       rtl8168: ERI send FIFO 0x%08x, receive FIFO 0x%08x; DLLPR 0x%02x, "
                "Config3 0x%02x, MaxTxPacket 0x%02x; send ring at 0x%08x%08x, %s 0x%08x\n",
                tx_fifo, rx_fifo, rd8(c, R_DLLPR), rd8(c, R_CONFIG3), rd8(c, R_MAXTXPACKET),
                rd32(c, R_TXDESC + 4), rd32(c, R_TXDESC), next >= 0 ? "first waiting" : "none waiting",
                next >= 0 ? c->tx[next].opts1 : 0);
    }
}

static const struct {
    uint16_t version;
    uint16_t mask;
    const char *name;
} versions[] = {
    { 0x54a, 0x7fe, "RTL8117" },
    { 0x509, 0x7ff, "RTL8168GU" },
    { 0x5c8, 0x7ff, "RTL8411B" },
    { 0x540, 0x7c0, "RTL8168H" },
    { 0x500, 0x7c0, "RTL8168EP" },
    { 0x4c0, 0x7c0, "RTL8168G" },
    { 0x480, 0x7c0, "RTL8168F" },
    { 0x2c0, 0x7c0, "RTL8168E" },
    { 0x280, 0x7c0, "RTL8168D" },
    { 0x3c0, 0x7c0, "RTL8168C" },
    { 0x380, 0x7c0, "RTL8168B" },
    { 0x300, 0x7c0, "RTL8168B" },
    { 0x640, 0x7c0, "RTL8125B" },
    { 0x600, 0x7c0, "RTL8125A" },
};

/* The family ("RTL8111/8168", "RTL8125") if pci is a chip this driver
 * knows (by its IDs), else 0. */
static const char *rtl8168_model(const struct pci_device *pci)
{
    if (pci->vendor_id != 0x10EC)
        return 0;
    switch (pci->device_id) {
    case 0x8168:
    case 0x8161:
    case 0x2502:                        /* Killer E2500 (an RTL8168) */
    case 0x2600:                        /* Killer E2600 (an RTL8168H: the laptop's) */
        return "RTL8111/8168";
    case 0x8125:
    case 0x3000:                        /* Killer E3000, E3100 */
        return "RTL8125";
    }
    return 0;
}

static uint32_t byte_reversed(uint32_t v)
{
    return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24;
}

/* The groups the protocols want (netdev.set_multicast, 64a): the 64-bit
 * hash at MAR, one bit per value of the top 6 bits of the destination's
 * CRC-32 (Ethernet's, most significant bit first). Linux's r8169 (facts
 * only): every chip after the RTL8169SC takes the two words swapped and
 * byte-reversed, MAR0 + 4 written first; the RTL8168F (version 0x480)
 * must take every multicast frame. -1: every one, the hash all ones. */
static void set_multicast(struct net_card *d, const uint8_t (*macs)[6], int count)
{
    struct rtl *c = d->priv;
    uint32_t hash[2] = { 0, 0 };
    if (count < 0 || (c->version & 0x7cf) == 0x480) {
        hash[0] = hash[1] = 0xFFFFFFFF;
    } else {
        for (int i = 0; i < count; i++) {
            uint32_t crc = 0xFFFFFFFF;
            for (int k = 0; k < 6; k++)
                for (int b = 0; b < 8; b++) {
                    bool carry = ((crc >> 31) ^ (macs[i][k] >> b)) & 1;
                    crc = crc << 1 ^ (carry ? 0x04C11DB7 : 0);
                }
            unsigned bit = crc >> 26;
            hash[bit >> 5] |= 1u << (bit & 31);
        }
        uint32_t first = hash[0];
        hash[0] = byte_reversed(hash[1]);
        hash[1] = byte_reversed(first);
    }
    c->mar[0] = hash[0];
    c->mar[1] = hash[1];
    if (c->promiscuous)
        return;                                 /* (38b) the hash stays all ones meanwhile */
    wr32(c, R_MAR0 + 4, hash[1]);
    wr32(c, R_MAR0, hash[0]);
}

/* (38b) Every frame (a bridge's port), as Linux's r8169 does it:
 * AcceptAllPhys and every multicast hash bit; off, the filters as asked. */
static void set_promiscuous(struct net_card *d, bool on)
{
    struct rtl *c = d->priv;
    c->promiscuous = on;
    uint32_t rx = rd32(c, R_RXCONFIG);
    wr32(c, R_RXCONFIG, on ? rx | RX_ACCEPT_ALL : rx & ~(uint32_t)RX_ACCEPT_ALL);
    wr32(c, R_MAR0 + 4, on ? 0xFFFFFFFF : c->mar[1]);
    wr32(c, R_MAR0, on ? 0xFFFFFFFF : c->mar[0]);
}

static void *fail(const char *where, const char *why)
{
    kprintf("  rtl8168:       %s: %s\n", where, why);
    return 0;
}

/* Take the chip from its management firmware (newer chips): the RXDV gate
 * closed, the FIFOs empty, receiving and sending off, "out of band" off. */
static void take_over(struct rtl *c)
{
    wr32(c, R_MISC, rd32(c, R_MISC) | MISC_RXDV_GATE);
    if (c->is8125) {
        wait16(c, R_MITIGATE, 0x0103, 0x0103, 10);  /* the FIFOs empty */
    } else {
        wait32(c, R_TXCONFIG, TXCFG_EMPTY, TXCFG_EMPTY, 10);
        wait8(c, R_MCU, MCU_FIFOS_EMPTY, MCU_FIFOS_EMPTY, 10);
    }
    wr8(c, R_CHIPCMD, rd8(c, R_CHIPCMD) & ~(CMD_RX | CMD_TX));
    timer_sleep_ms(1);
    wr8(c, R_MCU, rd8(c, R_MCU) & ~MCU_OOB);
    mac_ocp_write(c, 0xE8DE, mac_ocp_read(c, 0xE8DE) & ~(1u << 14));
    wait8(c, R_MCU, MCU_LIST_READY, MCU_LIST_READY, 10);
    if (c->is8125) {
        mac_ocp_write(c, 0xC0AA, 0x07D0);
        mac_ocp_write(c, 0xC0A6, 0x0150);
        mac_ocp_write(c, 0xC01E, 0x5555);
    } else {
        mac_ocp_write(c, 0xE8DE, mac_ocp_read(c, 0xE8DE) | 1u << 15);
    }
    wait8(c, R_MCU, MCU_LIST_READY, MCU_LIST_READY, 10);
}

/* Start a reset chip (at the first start and after a send timeout): the
 * configuration registers unlocked, the rings' addresses
 * (high halves first: the low half's write takes them), the gate open,
 * then receiving and sending on and configured. */
static void program(struct rtl *c)
{
    wr8(c, R_CFG9346, CFG_UNLOCK);
    if (c->newer && !c->is8125)
        start_8168g(c);
    wr16(c, R_MITIGATE, 0);
    if (!c->is8125)
        wr8(c, R_MAXTXPACKET, c->newer ? 0x27 : 0x3F);
    if (c->is8125 && c->regs_size >= R_QUEUES_8125 + 2) {
        wr8(c, R_RSS_8125, 0);                  /* one queue, no spreading */
        wr16(c, R_QUEUES_8125, 0);
    }
    wr16(c, R_RXMAXSIZE, BUFFER);
    wr32(c, R_TXDESC + 4, (uint32_t)(c->tx_ring_phys >> 32));
    wr32(c, R_TXDESC, (uint32_t)c->tx_ring_phys);
    wr32(c, R_RXDESC + 4, (uint32_t)(c->rx_ring_phys >> 32));
    wr32(c, R_RXDESC, (uint32_t)c->rx_ring_phys);
    if (c->newer)
        wr32(c, R_MISC, rd32(c, R_MISC) & ~MISC_RXDV_GATE);
    wr8(c, R_CFG9346, CFG_LOCK);
    wr8(c, R_CHIPCMD, CMD_RX | CMD_TX);
    uint32_t rxconfig = c->is8125 ? RX_FETCH_8125 | RX_DMA
                      : c->newer ? RX_128_INT | RX_MULTI | RX_DMA | RX_EARLY_OFF
                      : RX_128_INT | RX_DMA;
    wr32(c, R_RXCONFIG, rxconfig | RX_ACCEPT_MULTI | RX_ACCEPT_BCAST | RX_ACCEPT_MINE |
                        (c->promiscuous ? RX_ACCEPT_ALL : 0));
    wr32(c, R_MAR0 + 4, 0);                     /* no multicast until set_multicast */
    wr32(c, R_MAR0, 0);
    wr32(c, R_TXCONFIG, TXCFG_DMA | TXCFG_IFG | (c->newer ? TXCFG_AUTO_FIFO : 0));
}

/* The chip's interrupts (MSI): received frames and a full receive ring. */
static void interrupts_on(struct rtl *c)
{
    if (c->is8125) {
        wr32(c, R_ISR_8125, 0xFFFFFFFF);
        wr32(c, R_IMR_8125, INT_ROK | INT_RDU);
    } else {
        wr16(c, R_ISR, 0xFFFF);
        wr16(c, R_IMR, INT_ROK | INT_RDU);
    }
}

/* The PHY: powered on, advertising what it says it can do, the negotiation
 * started again. (38d) Advertised as Linux's PHY layer does (genphy's
 * config_aneg): 10 and 100 Mbit/s from BMSR into ANAR, 1000BASE-T from the
 * extended status into its control register. The firmware may leave them
 * short: the J1900's RTL8168G had 1000BASE-T 0x0000 (ESTATUS 0x2000: it can),
 * so it linked at 100 with a gigabit partner. */
static bool phy_on(struct rtl *c)
{
    uint16_t bmcr, bmsr, anar, estatus, gbcr;
    if (!c->newer)
        phy_write(c, 0x1F, 0);                  /* its standard page */
    if (phy_read(c, PHY_BMSR, &bmsr) && phy_read(c, PHY_ANAR, &anar)) {
        uint16_t can = (uint16_t)((bmsr >> 11 & 0xF) << 5);    /* 10H 10F 100H 100F */
        phy_write(c, PHY_ANAR, (uint16_t)((anar & ~ANAR_MODES) | can));
        if ((bmsr & BMSR_ESTATUS) && phy_read(c, PHY_ESTATUS, &estatus) &&
            phy_read(c, PHY_GBCR, &gbcr))
            phy_write(c, PHY_GBCR, (uint16_t)((gbcr & ~GBCR_MODES) |
                                              (estatus & ESTATUS_1000T_FULL ? GBCR_1000_FULL : 0) |
                                              (estatus & ESTATUS_1000T_HALF ? GBCR_1000_HALF : 0)));
    }
    return phy_read(c, PHY_BMCR, &bmcr) &&
           phy_write(c, PHY_BMCR, (uint16_t)((bmcr & ~BMCR_POWER_DOWN) | BMCR_AN_ENABLE |
                                             BMCR_RESTART_AN));
}

/* No interrupts, from the management firmware, the PLL on, reset (newer
 * chips: with the gate closed and the FIFOs empty): the boot's steps, and
 * (37g-3) again after a sleep. */
static bool chip_reset(struct rtl *c)
{
    if (c->is8125) {
        wr32(c, R_IMR_8125, 0);
        wr32(c, R_ISR_8125, 0xFFFFFFFF);
    } else {
        wr16(c, R_IMR, 0);
        wr16(c, R_ISR, 0xFFFF);
    }
    if (c->newer)
        take_over(c);
    wr8(c, R_PMCH, rd8(c, R_PMCH) | (c->newer ? 0xC0 : 0x80));     /* the PLL on */
    if (c->newer) {
        wr32(c, R_MISC, rd32(c, R_MISC) | MISC_RXDV_GATE);
        wait32(c, R_TXCONFIG, TXCFG_EMPTY, TXCFG_EMPTY, 10);
    }
    wr8(c, R_CHIPCMD, CMD_RESET);
    return wait8(c, R_CHIPCMD, CMD_RESET, 0, 100);
}

/* Set the chip up (take it from the management firmware, reset, PHY, MAC
 * address, rings) and register it as ethN; 0 if that failed (logged). Its
 * model names the version the chip reports. */
static struct net_card *rtl8168_init(struct pci_device *pci, const char *model)
{
    char where[16];
    ksnprintf(where, sizeof(where), "%02x:%02x.%u", pci->bus, pci->device, pci->function);
    struct pci_bar *bar = 0;
    for (int i = 0; i < 6 && !bar; i++)
        if (pci->bars[i].size >= 256 && !pci->bars[i].is_io)
            bar = &pci->bars[i];                /* the registers: the first memory BAR */
    if (!bar)
        return fail(where, "no register BAR");
    struct rtl *c = kzalloc(sizeof(*c));
    if (!c || !(c->dev = net_card_new()))
        return fail(where, "out of memory");
    pci_enable_dma(pci);
    /* (65e) The link's power saving (ASPM) off, as Linux's r8169 does once
     * the chip stopped sending (rtl_task: "ASPM disabled on Tx timeout"):
     * on the laptop's RTL8168H, uploads stalled the chip six times in two
     * minutes (the watchdog's resets, frames lost, TCP's connections
     * timing out). */
    pci_aspm_off(pci, &c->aspm_was, &c->aspm_port_was);
    pci_write16(pci, PCI_COMMAND, pci_read16(pci, PCI_COMMAND) | PCI_COMMAND_INTX_OFF);
    c->regs = paging_map_mmio(bar->address, bar->size);
    c->regs_size = bar->size;
    uint32_t txconfig = rd32(c, R_TXCONFIG);
    if (txconfig == 0xFFFFFFFF)
        return fail(where, "the chip does not answer");
    c->version = txconfig >> 20 & 0x7cf;
    /* The family by the version the chip reports, as Linux decides it, not
     * by the PCI ID: Killer's IDs say nothing about it (0x2600 was taken
     * for an RTL8125 until the laptop's E2600, an RTL8168H, showed it;
     * driven with the RTL8125's registers it received but never sent). */
    c->is8125 = c->version >= 0x600;
    c->newer = c->is8125 || c->version >= 0x4c0;
    const char *name = model;
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++)
        if ((c->version & versions[i].mask) == versions[i].version) {
            name = versions[i].name;
            break;
        }
    ksnprintf(c->dev->model, sizeof(c->dev->model), "Realtek %s, version 0x%03x", name, c->version);

    if (!chip_reset(c))
        return fail(where, "the reset does not end");

    /* The MAC address, as the chip loaded it at power-on. */
    for (int i = 0; i < 6; i++)
        c->dev->mac[i] = rd8(c, R_MAC0 + i);
    bool none = true;
    for (int i = 0; i < 6; i++)
        none = none && (c->dev->mac[i] == 0 || c->dev->mac[i] == 0xFF);
    if (none && c->is8125 && c->regs_size >= R_MAC0_BKP + 6)
        for (int i = 0; i < 6; i++)
            c->dev->mac[i] = rd8(c, R_MAC0_BKP + i);

    if (!phy_on(c))
        kprintf("  rtl8168:       %s: the PHY does not answer\n", where);

    /* The rings: descriptors in one page each, a 2 KiB buffer for each;
     * every receive buffer is the chip's from the start. */
    uint64_t rx_phys = pmm_alloc_page(), tx_phys = pmm_alloc_page();
    c->tally_phys = pmm_alloc_page();
    if (!rx_phys || !tx_phys || !c->tally_phys)
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
        }
    }
    for (int i = 0; i < RING; i++)
        rx_give(c, i);
    c->tx[RING - 1].opts1 = DESC_END;

    c->rx_ring_phys = rx_phys;
    c->tx_ring_phys = tx_phys;
    program(c);

    c->tx_lock = (struct spinlock)SPINLOCK_INIT("rtl8168 tx");
    c->tally_lock = (struct mutex)MUTEX_INIT("rtl8168 tally");
    c->dev->driver = "rtl8168";
    ksnprintf(c->dev->where, sizeof(c->dev->where), "%s", where);
    c->dev->link_speed = link_speed;
    c->dev->send = send;
    c->dev->pci = pci;
    c->dev->details = details;
    c->dev->priv = c;
    c->dev->set_multicast = set_multicast;
    c->dev->set_promiscuous = set_promiscuous;
    net_register(c->dev);
    if (ncards < CARDS)
        cards[ncards++] = c;                    /* (37g-3) */
    if (!thread_create(c->dev->name, receiver, c) || !timer_add_tick_handler(tick, c))
        return fail(where, "no thread for receiving");
    uint64_t address;
    uint32_t data;
    c->msi = pci_msi_enable(pci, interrupt, c, &address, &data);
    if (c->msi)
        interrupts_on(c);
    return c->dev;
}

static bool start(struct pci_device *pci)
{
    const char *model = rtl8168_model(pci);
    return model && rtl8168_init(pci, model);
}

static struct rtl *card_of(const struct pci_device *pci)
{
    for (int i = 0; i < ncards; i++)
        if (cards[i]->dev->pci == pci)
            return cards[i];
    return 0;
}

/* (37g-3) Sleep: send() refuses frames (restarting, as the watchdog's
 * reset), interrupts off, receiving and sending off; awake (the chip lost
 * everything but its MAC address, which it loads again at power-on), as
 * at the boot: reset, the PHY on (the link negotiated again), the rings
 * empty and running. Frames in flight are lost, as at a reset. */
static bool suspend(struct pci_device *pci)
{
    struct rtl *c = card_of(pci);
    if (!c)
        return true;
    uint64_t state = spin_lock(&c->tx_lock);
    c->restarting = true;
    spin_unlock(&c->tx_lock, state);
    if (c->is8125)
        wr32(c, R_IMR_8125, 0);
    else
        wr16(c, R_IMR, 0);
    wr8(c, R_CHIPCMD, rd8(c, R_CHIPCMD) & ~(CMD_RX | CMD_TX));
    return true;
}

static bool resume(struct pci_device *pci)
{
    struct rtl *c = card_of(pci);
    if (!c)
        return true;
    bool ok = chip_reset(c);
    if (!ok)
        kprintf("%s: after the sleep the reset does not end\n", c->dev->name);
    if (!phy_on(c))
        kprintf("%s: after the sleep the PHY does not answer\n", c->dev->name);
    rings_go(c);
    c->sleeps++;
    return ok;
}

/* The chips rtl8168_model knows, by the same IDs. */
static const struct module_pci ids[] = {
    { 0x10EC, 0x8168 }, { 0x10EC, 0x8161 }, { 0x10EC, 0x2502 }, { 0x10EC, 0x2600 },
    { 0x10EC, 0x8125 }, { 0x10EC, 0x3000 },
};

MODULE(rtl8168, start, suspend, resume, ids)
