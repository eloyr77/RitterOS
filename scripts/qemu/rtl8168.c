/*
 * A model of Realtek's RTL8111/8168 and RTL8125 Ethernet chips for
 * RitterOS's tests (step 42b). QEMU has no Realtek chip newer than the
 * RTL8139; scripts/setup-toolchain.sh copies this file into QEMU's
 * hw/net and builds it in. Written for RitterOS, from what is known of
 * the chips' registers; it models what a driver relies on, not the
 * whole chip:
 *
 *   -device rtl8168[,xid=0x541][,phy-off=on|off]          an RTL8111/8168
 *   -device rtl8125[,device-id=0x2600][,xid=0x609][,...]  an RTL8125
 *
 * Registers in BAR2 (4 KiB, 64-bit memory; BAR0 is the same as I/O
 * ports). Descriptor rings of 16-byte descriptors (opts1 with OWN, the
 * ring's end, first and last fragment and the length; opts2; the
 * buffer's address): the chip takes descriptors the driver marked OWN,
 * clears OWN when done. Received frames carry their 4-byte Ethernet
 * checksum in the length, as on the real chips.
 *
 * Things the real chips do that a driver must handle, kept here so the
 * tests notice a driver that does not: the PHY may start powered down
 * (phy-off, on by default: some firmware leaves it so), and chips from
 * the RTL8168G on (xid 0x4c0 and later, every RTL8125) start with the
 * "RXDV gate" closed (MISC bit 19), which keeps every frame out until the
 * driver opens it. Such chips reach their PHY through GPHY_OCP (0xB8),
 * older ones through PHYAR (0x60). The RTL8168G and later (not the
 * RTL8125) also have ERI registers (ERIDR 0x70, ERIAR 0x74, step 47a):
 * they start at 0 and keep what the driver writes (it reads its FIFO
 * settings back for net). xid is the version the chip reports
 * in TxConfig bits 30:20 (0x541 an RTL8168H, 0x609 an RTL8125A).
 * Interrupts follow IntrMask & IntrStatus: INTx, or MSI (one vector, a
 * message when the two first have a bit in common). The
 * tally counters (written to memory on request, R_COUNTERS bit 3) count
 * frames sent and received, missed ones, and those to us or broadcast.
 *
 * (64p) `qom-set /machine/peripheral/ID stuck true` makes the chip stop
 * sending (frames marked OWN stay so) until the driver resets it
 * (ChipCmd's reset bit), as real chips are known to after a send timeout:
 * Linux's r8169 resets them then.
 */
#include "qemu/osdep.h"
#include <zlib.h> /* crc32 */
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/core/qdev-properties.h"
#include "net/net.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_RTL8168 "rtl8168"
#define TYPE_RTL8125 "rtl8125"
#define TYPE_RTL81XX "rtl81xx-base"   /* what both share (abstract) */

/* Registers */
#define R_MAC0          0x00
#define R_MAR0          0x08
#define R_COUNTERS      0x10    /* where to write the tally counters (64-bit) */
#define R_TXDESC        0x20
#define R_ERIDR         0x70
#define R_ERIAR         0x74
#define ERI_FLAG        (1u << 31)
#define ERI_SIZE        0x1000
#define R_CHIPCMD       0x37
#define R_TXPOLL        0x38    /* RTL8168 */
#define R_IMR           0x3C    /* RTL8168: 16-bit mask, 0x3E status */
#define R_ISR           0x3E
#define R_IMR_8125      0x38    /* RTL8125: 32-bit mask, 0x3C status */
#define R_ISR_8125      0x3C
#define R_TXCONFIG      0x40
#define R_RXCONFIG      0x44
#define R_CFG9346       0x50
#define R_PHYAR         0x60
#define R_PHYSTATUS     0x6C
#define R_TXPOLL_8125   0x90
#define R_OCPDR         0xB0    /* the MAC's own OCP registers */
#define R_GPHY_OCP      0xB8    /* the PHY through OCP */
#define R_MCU           0xD3
#define R_MITIGATE      0xE2
#define R_RXDESC        0xE4
#define R_MISC          0xF0

#define COUNTERS_DUMP   0x08
#define CMD_RESET       0x10
#define CMD_RX          0x08
#define CMD_TX          0x04
#define TXPOLL_NPQ      0x40
#define ISR_ROK         0x0001
#define ISR_TOK         0x0004
#define ISR_RDU         0x0010
#define ISR_LINKCHG     0x0020
#define RX_ACCEPT_ALL   0x01
#define RX_ACCEPT_MINE  0x02
#define RX_ACCEPT_MULTI 0x04
#define RX_ACCEPT_BCAST 0x08
#define TXCFG_EMPTY     (1u << 11)
#define MISC_RXDV_GATE  (1u << 19)
#define MCU_READY       0x32    /* link list ready, receive and send FIFOs empty */
#define OCP_FLAG        (1u << 31)
#define OCP_PHY_BASE    0xA400  /* the PHY's standard registers in OCP space */

#define DESC_OWN        (1u << 31)
#define DESC_END        (1u << 30)
#define DESC_FIRST      (1u << 29)
#define DESC_LAST       (1u << 28)

#define BMCR_RESET      0x8000
#define BMCR_POWER_DOWN 0x0800
#define BMCR_RESTART_AN 0x0200

#define REGS_SIZE       4096
#define FRAME_MAX       16384

OBJECT_DECLARE_SIMPLE_TYPE(RTL8168State, RTL81XX)

struct RTL8168State {
    PCIDevice parent_obj;
    NICState *nic;
    NICConf conf;
    MemoryRegion mmio, io;
    uint16_t device_id, xid;
    bool phy_off;
    bool stuck;                         /* (64p) sends nothing until a reset */
    bool is8125, newer;
    bool irq_level;                     /* (IntrStatus & IntrMask) != 0 */
    uint8_t regs[REGS_SIZE];            /* what the driver wrote, by offset */
    uint8_t eri[ERI_SIZE];              /* the ERI registers (newer RTL8168s) */
    uint16_t phy[32];
    uint32_t tx_index, rx_index;
    uint64_t tx_ok, rx_ok, rx_mine, rx_broadcast;   /* tally counters */
    uint16_t rx_missed;
    uint8_t frame[FRAME_MAX];
};

static uint32_t get32(RTL8168State *s, uint32_t reg)
{
    return ldl_le_p(s->regs + reg);
}

static void put32(RTL8168State *s, uint32_t reg, uint32_t v)
{
    stl_le_p(s->regs + reg, v);
}

static uint64_t base(RTL8168State *s, uint32_t reg)
{
    return ldq_le_p(s->regs + reg);
}

static uint32_t isr_reg(RTL8168State *s)
{
    return s->is8125 ? R_ISR_8125 : R_ISR;
}

static uint32_t isr(RTL8168State *s)
{
    return s->is8125 ? get32(s, R_ISR_8125) : lduw_le_p(s->regs + R_ISR);
}

static uint32_t imr(RTL8168State *s)
{
    return s->is8125 ? get32(s, R_IMR_8125) : lduw_le_p(s->regs + R_IMR);
}

/* INTx follows the level; MSI sends a message when it rises. */
static void update_irq(RTL8168State *s)
{
    bool level = (isr(s) & imr(s)) != 0;
    if (msi_enabled(PCI_DEVICE(s))) {
        if (level && !s->irq_level) {
            msi_notify(PCI_DEVICE(s), 0);
        }
    } else {
        pci_set_irq(PCI_DEVICE(s), level);
    }
    s->irq_level = level;
}

static void set_status(RTL8168State *s, uint32_t bits)
{
    if (s->is8125) {
        put32(s, R_ISR_8125, get32(s, R_ISR_8125) | bits);
    } else {
        stw_le_p(s->regs + R_ISR, lduw_le_p(s->regs + R_ISR) | bits);
    }
    update_irq(s);
}

static bool link_up(RTL8168State *s)
{
    return !qemu_get_queue(s->nic)->link_down && !(s->phy[0] & BMCR_POWER_DOWN);
}

static uint32_t phy_status(RTL8168State *s)
{
    if (!link_up(s)) {
        return 0;
    }
    /* Full duplex, link, 1000 Mbit/s (bit 4) or 2500 (bit 10, RTL8125). */
    return 0x01 | 0x02 | (s->is8125 ? 0x400 : 0x10);
}

static void phy_reset(RTL8168State *s)
{
    memset(s->phy, 0, sizeof(s->phy));
    s->phy[0] = 0x1140 | (s->phy_off ? BMCR_POWER_DOWN : 0);  /* autoneg, full, 1000 */
    s->phy[1] = 0x796d;
    s->phy[2] = 0x001c;                                     /* Realtek */
    s->phy[3] = 0xc800;
}

static void phy_write(RTL8168State *s, unsigned reg, uint16_t value)
{
    bool was = link_up(s);
    if (reg == 0) {
        if (value & BMCR_RESET) {
            phy_reset(s);
            value &= ~BMCR_RESET;
        }
        value &= ~BMCR_RESTART_AN;      /* the negotiation is done at once */
    }
    if (reg < 32 && reg != 1) {
        s->phy[reg] = value;
    }
    if (link_up(s) != was) {
        set_status(s, ISR_LINKCHG);
        if (link_up(s)) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
    }
}

static uint16_t phy_read(RTL8168State *s, unsigned reg)
{
    if (reg == 1) {
        return (s->phy[1] & ~0x0004) | (link_up(s) ? 0x0004 : 0);
    }
    return reg < 32 ? s->phy[reg] : 0;
}

/* The chip's state at power-on (the machine's reset). */
static void power_on(RTL8168State *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->eri, 0, sizeof(s->eri));
    memcpy(s->regs + R_MAC0, s->conf.macaddr.a, 6);
    if (s->newer) {
        put32(s, R_MISC, MISC_RXDV_GATE);
    }
    s->tx_index = s->rx_index = 0;
    s->tx_ok = s->rx_ok = s->rx_mine = s->rx_broadcast = 0;
    s->rx_missed = 0;
    phy_reset(s);
}

/* ChipCmd's reset: receiving and sending stop, the rings start again at
 * their first descriptor; the MAC address, MISC and the PHY stay. */
static void soft_reset(RTL8168State *s)
{
    s->regs[R_CHIPCMD] = 0;
    put32(s, R_TXCONFIG, 0);
    put32(s, R_RXCONFIG, 0);
    put32(s, R_IMR_8125, 0);
    put32(s, R_ISR_8125, 0);    /* RTL8168's IMR and ISR are inside these */
    s->tx_index = s->rx_index = 0;
    s->stuck = false;
    update_irq(s);
}

static bool can_receive(NetClientState *nc)
{
    RTL8168State *s = qemu_get_nic_opaque(nc);
    return (s->regs[R_CHIPCMD] & CMD_RX) && !(get32(s, R_MISC) & MISC_RXDV_GATE) &&
           link_up(s);
}

static bool accepted(RTL8168State *s, const uint8_t *frame)
{
    uint32_t mode = get32(s, R_RXCONFIG);
    static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    if (mode & RX_ACCEPT_ALL) {
        return true;
    }
    if (!memcmp(frame, broadcast, 6)) {
        return mode & RX_ACCEPT_BCAST;
    }
    if (frame[0] & 1) {
        /* The hash's bit for the destination: the RTL8168s and RTL8125s
         * hold it byte-reversed against the RTL8139's order (MAR7 bits 0-7
         * first), as Linux's r8169 writes it for every chip after the
         * RTL8169SC. */
        unsigned bit = net_crc32(frame, 6) >> 26;
        return (mode & RX_ACCEPT_MULTI) && (s->regs[R_MAR0 + 7 - bit / 8] & (1 << (bit % 8)));
    }
    return (mode & RX_ACCEPT_MINE) && !memcmp(frame, s->regs + R_MAC0, 6);
}

static ssize_t receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    RTL8168State *s = qemu_get_nic_opaque(nc);
    PCIDevice *d = PCI_DEVICE(s);
    if (!can_receive(nc) || size < 14 || size + 4 > FRAME_MAX) {
        return size;
    }
    if (!accepted(s, buf)) {
        return size;
    }
    /* The frame as the wire has it: at least 60 bytes, then its CRC. */
    size_t length = MAX(size, 60);
    memset(s->frame, 0, length);
    memcpy(s->frame, buf, size);
    uint32_t crc = crc32(0, s->frame, length);
    stl_le_p(s->frame + length, crc);
    length += 4;

    /* Into as many descriptors as it needs; dropped if the driver has
     * not given enough (as the real chip does: "descriptor unavailable"). */
    uint64_t ring = base(s, R_RXDESC);
    uint32_t index = s->rx_index, done = 0;
    bool first = true;
    while (done < length) {
        uint64_t at = ring + (uint64_t)index * 16;
        uint32_t opts1;
        uint64_t address;
        pci_dma_read(d, at, &opts1, 4);
        pci_dma_read(d, at + 8, &address, 8);
        opts1 = le32_to_cpu(opts1);
        address = le64_to_cpu(address);
        if (!(opts1 & DESC_OWN)) {
            s->rx_missed++;
            set_status(s, ISR_RDU);
            return size;
        }
        uint32_t room = opts1 & 0x3fff, part = MIN(room, length - done);
        if (!room) {
            set_status(s, ISR_RDU);
            return size;
        }
        pci_dma_write(d, address, s->frame + done, part);
        done += part;
        uint32_t status = (opts1 & DESC_END) | (first ? DESC_FIRST : 0) |
                          (done == length ? DESC_LAST : 0) | (uint32_t)length;
        uint32_t zero = 0;
        pci_dma_write(d, at + 4, &zero, 4);
        status = cpu_to_le32(status);
        pci_dma_write(d, at, &status, 4);   /* OWN cleared last */
        index = (opts1 & DESC_END) ? 0 : index + 1;
        first = false;
    }
    s->rx_index = index;
    s->rx_ok++;
    if (!memcmp(buf, s->regs + R_MAC0, 6)) {
        s->rx_mine++;
    } else if (buf[0] == 0xff) {
        s->rx_broadcast++;
    }
    set_status(s, ISR_ROK);
    return size;
}

/* TxPoll: send what the driver marked OWN, from where the chip stopped. */
static void transmit(RTL8168State *s)
{
    PCIDevice *d = PCI_DEVICE(s);
    if (!(s->regs[R_CHIPCMD] & CMD_TX) || s->stuck) {
        return;
    }
    uint64_t ring = base(s, R_TXDESC);
    size_t length = 0;
    bool sent = false;
    for (int n = 0; n < 1024; n++) {
        uint64_t at = ring + (uint64_t)s->tx_index * 16;
        uint32_t opts1;
        uint64_t address;
        pci_dma_read(d, at, &opts1, 4);
        pci_dma_read(d, at + 8, &address, 8);
        opts1 = le32_to_cpu(opts1);
        address = le64_to_cpu(address);
        if (!(opts1 & DESC_OWN)) {
            break;
        }
        uint32_t part = opts1 & 0xffff;
        if (opts1 & DESC_FIRST) {
            length = 0;
        }
        if (length + part <= FRAME_MAX) {
            pci_dma_read(d, address, s->frame + length, part);
            length += part;
        }
        if ((opts1 & DESC_LAST) && link_up(s)) {
            qemu_send_packet(qemu_get_queue(s->nic), s->frame, length);
            s->tx_ok++;
            sent = true;
        }
        opts1 = cpu_to_le32(opts1 & ~DESC_OWN);
        pci_dma_write(d, at, &opts1, 4);
        s->tx_index = (le32_to_cpu(opts1) & DESC_END) ? 0 : s->tx_index + 1;
    }
    if (sent) {
        set_status(s, ISR_TOK);
    }
}

static uint64_t reg_read(void *opaque, hwaddr addr, unsigned size)
{
    RTL8168State *s = opaque;
    uint8_t value[4];
    if (addr + size > REGS_SIZE) {
        return 0;
    }
    memcpy(value, s->regs + addr, size);
    /* Registers the chip itself fills, byte by byte. */
    for (unsigned i = 0; i < size; i++) {
        unsigned at = addr + i;
        uint32_t txconfig = (get32(s, R_TXCONFIG) & ~(0x7cfu << 20)) |
                            (uint32_t)(s->xid & 0x7cf) << 20 | TXCFG_EMPTY;
        uint32_t phystatus = phy_status(s);
        if (at >= R_TXCONFIG && at < R_TXCONFIG + 4) {
            value[i] = txconfig >> (8 * (at - R_TXCONFIG));
        } else if (at == R_PHYSTATUS || (s->is8125 && at == R_PHYSTATUS + 1)) {
            value[i] = phystatus >> (8 * (at - R_PHYSTATUS));
        } else if (at == R_MCU) {
            value[i] |= MCU_READY;
        } else if (s->is8125 && at == R_MITIGATE) {
            value[i] |= 0x03;           /* receive and send FIFOs empty */
        } else if (s->is8125 && at == R_MITIGATE + 1) {
            value[i] |= 0x01;
        }
    }
    return ldn_le_p(value, size);
}

static void reg_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    RTL8168State *s = opaque;
    if (addr + size > REGS_SIZE) {
        return;
    }
    uint32_t isr_at = isr_reg(s), isr_size = s->is8125 ? 4 : 2;
    bool unlocked = s->regs[R_CFG9346] == 0xC0;
    for (unsigned i = 0; i < size; i++) {
        unsigned at = addr + i;
        uint8_t byte = data >> (8 * i);
        if (at >= isr_at && at < isr_at + isr_size) {
            s->regs[at] &= ~byte;       /* write 1 to clear */
        } else if (at < 6) {
            if (unlocked) {
                s->regs[at] = byte;     /* the MAC address: only unlocked */
            }
        } else if (at == R_CHIPCMD) {
            if (byte & CMD_RESET) {
                soft_reset(s);
            } else {
                bool rx_was = s->regs[R_CHIPCMD] & CMD_RX;
                s->regs[R_CHIPCMD] = byte & (CMD_RX | CMD_TX);
                if (!rx_was && (byte & CMD_RX)) {
                    qemu_flush_queued_packets(qemu_get_queue(s->nic));
                }
            }
        } else if (!s->is8125 && at == R_TXPOLL) {
            if (byte & TXPOLL_NPQ) {
                transmit(s);
            }
        } else if (s->is8125 && at == R_TXPOLL_8125) {
            if (byte & 0x01) {
                transmit(s);
            }
        } else {
            s->regs[at] = byte;
        }
    }
    /* Side effects of whole registers. */
    if (addr <= R_TXDESC && addr + size > R_TXDESC) {
        s->tx_index = 0;                /* the low half: a new ring */
    }
    if (addr <= R_RXDESC && addr + size > R_RXDESC) {
        s->rx_index = 0;
    }
    if (addr <= R_MISC + 2 && addr + size > R_MISC + 2 &&
        !(get32(s, R_MISC) & MISC_RXDV_GATE)) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
    if (addr == R_PHYAR && size == 4 && !s->newer) {
        uint32_t v = data, reg = v >> 16 & 0x1f;
        if (v & OCP_FLAG) {
            phy_write(s, reg, v & 0xffff);
            put32(s, R_PHYAR, v & ~OCP_FLAG);       /* done */
        } else {
            put32(s, R_PHYAR, OCP_FLAG | reg << 16 | phy_read(s, reg));
        }
    }
    if (addr == R_GPHY_OCP && size == 4 && s->newer) {
        uint32_t v = data, ocp = (v >> 16 & 0x7fff) << 1;
        bool std = ocp >= OCP_PHY_BASE && ocp < OCP_PHY_BASE + 64;
        unsigned reg = (ocp - OCP_PHY_BASE) / 2;
        if (v & OCP_FLAG) {
            if (std) {
                phy_write(s, reg, v & 0xffff);
            }
            put32(s, R_GPHY_OCP, v & ~OCP_FLAG);
        } else {
            put32(s, R_GPHY_OCP, OCP_FLAG | (v & 0x7fff0000) | (std ? phy_read(s, reg) : 0));
        }
    }
    if (addr == R_COUNTERS && size == 4 && (data & COUNTERS_DUMP)) {
        /* The tally counters as the real chip lays them out (64 bytes). */
        uint8_t tally[64] = { 0 };
        stq_le_p(tally + 0, s->tx_ok);
        stq_le_p(tally + 8, s->rx_ok);
        stw_le_p(tally + 28, s->rx_missed);
        stq_le_p(tally + 40, s->rx_mine);
        stq_le_p(tally + 48, s->rx_broadcast);
        uint64_t at = (uint64_t)get32(s, R_COUNTERS + 4) << 32 | (get32(s, R_COUNTERS) & ~0x3fu);
        pci_dma_write(PCI_DEVICE(s), at, tally, sizeof(tally));
        put32(s, R_COUNTERS, (uint32_t)data & ~COUNTERS_DUMP);  /* done */
    }
    if (addr == R_ERIAR && size == 4 && s->newer && !s->is8125) {
        uint32_t v = data, reg = v & 0xffc, mask = v >> 12 & 0xf;
        if (v & ERI_FLAG) {
            for (int i = 0; i < 4; i++) {
                if (mask & (1u << i)) {
                    s->eri[reg + i] = s->regs[R_ERIDR + i];
                }
            }
            put32(s, R_ERIAR, v & ~ERI_FLAG);       /* written */
        } else {
            memcpy(s->regs + R_ERIDR, s->eri + reg, 4);
            put32(s, R_ERIAR, v | ERI_FLAG);        /* read */
        }
    }
    if (addr == R_OCPDR && size == 4) {
        put32(s, R_OCPDR, (uint32_t)data & ~OCP_FLAG);  /* accepted, not modelled */
    }
    update_irq(s);
}

static const MemoryRegionOps reg_ops = {
    .read = reg_read,
    .write = reg_write,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void link_changed(NetClientState *nc)
{
    RTL8168State *s = qemu_get_nic_opaque(nc);
    set_status(s, ISR_LINKCHG);
}

static NetClientInfo net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = can_receive,
    .receive = receive,
    .link_status_changed = link_changed,
};

static void realize(PCIDevice *dev, Error **errp)
{
    RTL8168State *s = RTL81XX(dev);
    DeviceState *d = DEVICE(dev);
    s->is8125 = !strcmp(object_get_typename(OBJECT(dev)), TYPE_RTL8125);
    s->newer = s->is8125 || (s->xid & 0x7cf) >= 0x4c0;
    pci_config_set_device_id(dev->config, s->device_id);
    dev->config[PCI_INTERRUPT_PIN] = 1;
    if (msi_init(dev, 0x50, 1, true, false, errp) < 0) {
        return;
    }
    memory_region_init_io(&s->mmio, OBJECT(s), &reg_ops, s, "rtl8168-mmio", REGS_SIZE);
    memory_region_init_io(&s->io, OBJECT(s), &reg_ops, s, "rtl8168-io", 256);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->mmio);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_info, &s->conf, object_get_typename(OBJECT(dev)), d->id,
                          &d->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    power_on(s);
}

static void uninit(PCIDevice *dev)
{
    qemu_del_nic(RTL81XX(dev)->nic);
}

static void reset(DeviceState *dev)
{
    power_on(RTL81XX(dev));
}

static const Property rtl8168_properties[] = {
    DEFINE_NIC_PROPERTIES(RTL8168State, conf),
    DEFINE_PROP_UINT16("device-id", RTL8168State, device_id, 0x8168),
    DEFINE_PROP_UINT16("xid", RTL8168State, xid, 0x541),
    DEFINE_PROP_BOOL("phy-off", RTL8168State, phy_off, true),
};

static const Property rtl8125_properties[] = {
    DEFINE_NIC_PROPERTIES(RTL8168State, conf),
    DEFINE_PROP_UINT16("device-id", RTL8168State, device_id, 0x8125),
    DEFINE_PROP_UINT16("xid", RTL8168State, xid, 0x609),
    DEFINE_PROP_BOOL("phy-off", RTL8168State, phy_off, true),
};

static bool get_stuck(Object *obj, Error **errp)
{
    return RTL81XX(obj)->stuck;
}

static void set_stuck(Object *obj, bool value, Error **errp)
{
    RTL81XX(obj)->stuck = value;
}

static void common_class_init(ObjectClass *klass, const void *data)
{
    object_class_property_add_bool(klass, "stuck", get_stuck, set_stuck);
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    k->realize = realize;
    k->exit = uninit;
    k->vendor_id = PCI_VENDOR_ID_REALTEK;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    device_class_set_legacy_reset(dc, reset);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static void rtl8168_class_init(ObjectClass *klass, const void *data)
{
    PCI_DEVICE_CLASS(klass)->device_id = 0x8168;
    PCI_DEVICE_CLASS(klass)->revision = 0x15;
    DEVICE_CLASS(klass)->desc = "Realtek RTL8111/8168 (a model for RitterOS's tests)";
    device_class_set_props(DEVICE_CLASS(klass), rtl8168_properties);
}

static void rtl8125_class_init(ObjectClass *klass, const void *data)
{
    PCI_DEVICE_CLASS(klass)->device_id = 0x8125;
    PCI_DEVICE_CLASS(klass)->revision = 0x04;
    DEVICE_CLASS(klass)->desc = "Realtek RTL8125 (a model for RitterOS's tests)";
    device_class_set_props(DEVICE_CLASS(klass), rtl8125_properties);
}

static const TypeInfo types[] = {
    {
        .name          = TYPE_RTL81XX,
        .parent        = TYPE_PCI_DEVICE,
        .abstract      = true,
        .instance_size = sizeof(RTL8168State),
        .class_init    = common_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
    {
        .name          = TYPE_RTL8168,
        .parent        = TYPE_RTL81XX,
        .class_init    = rtl8168_class_init,
    },
    {
        .name          = TYPE_RTL8125,
        .parent        = TYPE_RTL81XX,
        .class_init    = rtl8125_class_init,
    },
};

DEFINE_TYPES(types)
