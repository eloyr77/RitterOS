/*
 * A simple Wi-Fi card of our own invention for RitterOS's tests (step 24d).
 * QEMU has no 802.11 device, so -- as with the Realtek model --
 * scripts/setup-toolchain.sh copies this file into QEMU's hw/net and builds
 * it in. This is NOT a model of any real chip (the MT7921 driver is its own
 * track): it is the simplest card that exercises RitterOS's wireless layer
 * (kernel/wlan.c) and the elf wifid -- a MAC address, a channel, and two
 * rings of 802.11 frames.
 *
 *   -device ritterwifi,netdev=<id>[,mac=...]
 *
 * It is an ordinary QEMU NIC, but what travels on its netdev is not an
 * Ethernet frame: it is a 4-byte header { u8 channel; s8 signal; u16 rsv }
 * followed by one 802.11 frame (no FCS). A test access point on the other
 * end of the netdev (scripts/testwifi.py) speaks the same. The card sends on
 * the channel register's channel and, like a radio, only delivers frames
 * whose header channel matches it (channel 0 on the wire = heard on any).
 *
 * BAR0 (MMIO, little-endian, 32-bit registers):
 *   0x00 ID        R    0x52464931 ("RFI1")
 *   0x04 MAC_LO    R    mac[0..3]
 *   0x08 MAC_HI    R    mac[4..5]
 *   0x0C CHANNEL   RW   the channel it listens and sends on
 *   0x10 ISR       R/W1C  bit0 RX done, bit1 TX done
 *   0x14 IMR       RW   which of those raise the interrupt
 *   0x18 RX_BASE_LO RW  the RX descriptor ring's physical address
 *   0x1C RX_BASE_HI RW
 *   0x20 RX_LEN    RW   descriptors in the RX ring
 *   0x28 TX_BASE_LO RW
 *   0x2C TX_BASE_HI RW
 *   0x30 TX_LEN    RW
 *   0x34 TX_TAIL   RW   the doorbell: descriptors [tx_head, TX_TAIL) are ready
 *   0x38 CTRL      RW   bit0 ENABLE (resets the ring heads when set)
 *
 * A descriptor is 16 bytes: { u64 addr; u16 length; u8 flags (bit0 DONE);
 * s8 signal; u8 channel; u8 rsv[3] }. The card owns a descriptor until it
 * sets DONE; the driver clears DONE to hand an RX buffer back.
 */
#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/core/qdev-properties.h"
#include "net/net.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_RITTERWIFI "ritterwifi"

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
#define REGS_SIZE       0x40

#define ID_MAGIC        0x52464931u
#define ISR_RX          0x1
#define ISR_TX          0x2
#define CTRL_ENABLE     0x1
#define DESC_DONE       0x1

#define FRAME_MAX       2400
#define WIRE_HEADER     4

struct RitterWifiState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    NICState *nic;
    NICConf conf;
    uint32_t channel, isr, imr, rx_len, tx_len, tx_tail, ctrl;
    uint64_t rx_base, tx_base;
    uint32_t rx_head, tx_head;
    uint8_t wire[WIRE_HEADER + FRAME_MAX];
};

typedef struct RitterWifiState RitterWifiState;

#define RITTERWIFI(obj) OBJECT_CHECK(RitterWifiState, (obj), TYPE_RITTERWIFI)

struct desc {
    uint64_t addr;
    uint16_t length;
    uint8_t flags;
    int8_t signal;
    uint8_t channel;
    uint8_t rsv[3];
};

static void update_irq(RitterWifiState *s)
{
    int level = (s->isr & s->imr) != 0;
    if (msi_enabled(&s->parent_obj)) {
        if (level) {
            msi_notify(&s->parent_obj, 0);
        }
    } else {
        pci_set_irq(&s->parent_obj, level);
    }
}

static void raise_irq(RitterWifiState *s, uint32_t bits)
{
    s->isr |= bits;
    update_irq(s);
}

/* Read descriptor n of a ring. */
static void read_desc(RitterWifiState *s, uint64_t base, uint32_t n, struct desc *d)
{
    pci_dma_read(&s->parent_obj, base + (uint64_t)n * 16, d, sizeof(*d));
}

static void write_desc(RitterWifiState *s, uint64_t base, uint32_t n, const struct desc *d)
{
    pci_dma_write(&s->parent_obj, base + (uint64_t)n * 16, d, sizeof(*d));
}

/* Send the descriptors the driver just handed us. */
static void transmit(RitterWifiState *s)
{
    if (!(s->ctrl & CTRL_ENABLE) || !s->tx_len) {
        return;
    }
    while (s->tx_head != s->tx_tail) {
        struct desc d;
        read_desc(s, s->tx_base, s->tx_head, &d);
        uint32_t length = d.length;
        if (length > FRAME_MAX) {
            length = FRAME_MAX;
        }
        s->wire[0] = (uint8_t)s->channel;
        s->wire[1] = 0;
        s->wire[2] = s->wire[3] = 0;
        pci_dma_read(&s->parent_obj, d.addr, s->wire + WIRE_HEADER, length);
        qemu_send_packet(qemu_get_queue(s->nic), s->wire, WIRE_HEADER + length);
        d.flags |= DESC_DONE;
        write_desc(s, s->tx_base, s->tx_head, &d);
        s->tx_head = (s->tx_head + 1) % s->tx_len;
    }
    raise_irq(s, ISR_TX);
}

static bool can_receive(NetClientState *nc)
{
    RitterWifiState *s = qemu_get_nic_opaque(nc);
    return (s->ctrl & CTRL_ENABLE) && s->rx_len;
}

static ssize_t receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    RitterWifiState *s = qemu_get_nic_opaque(nc);
    if (!can_receive(nc) || size < WIRE_HEADER || size > WIRE_HEADER + FRAME_MAX) {
        return size;
    }
    uint8_t channel = buf[0];
    if (channel != 0 && channel != s->channel) {
        return size;                            /* heard on another channel */
    }
    struct desc d;
    read_desc(s, s->rx_base, s->rx_head, &d);
    if (d.flags & DESC_DONE) {
        return size;                            /* the ring is full: the frame is lost */
    }
    size_t length = size - WIRE_HEADER;
    d.length = (uint16_t)length;
    d.signal = (int8_t)buf[1];
    d.channel = channel ? channel : (uint8_t)s->channel;
    d.flags |= DESC_DONE;
    pci_dma_write(&s->parent_obj, d.addr, buf + WIRE_HEADER, length);
    write_desc(s, s->rx_base, s->rx_head, &d);
    s->rx_head = (s->rx_head + 1) % s->rx_len;
    raise_irq(s, ISR_RX);
    return size;
}

static uint64_t reg_read(void *opaque, hwaddr addr, unsigned size)
{
    RitterWifiState *s = opaque;
    switch (addr) {
    case R_ID:       return ID_MAGIC;
    case R_MAC_LO:   return ldl_le_p(s->conf.macaddr.a);
    case R_MAC_HI:   return lduw_le_p(s->conf.macaddr.a + 4);
    case R_CHANNEL:  return s->channel;
    case R_ISR:      return s->isr;
    case R_IMR:      return s->imr;
    case R_RX_BASE_LO: return (uint32_t)s->rx_base;
    case R_RX_BASE_HI: return (uint32_t)(s->rx_base >> 32);
    case R_RX_LEN:   return s->rx_len;
    case R_TX_BASE_LO: return (uint32_t)s->tx_base;
    case R_TX_BASE_HI: return (uint32_t)(s->tx_base >> 32);
    case R_TX_LEN:   return s->tx_len;
    case R_TX_TAIL:  return s->tx_tail;
    case R_CTRL:     return s->ctrl;
    }
    return 0;
}

static void reg_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    RitterWifiState *s = opaque;
    uint32_t v = (uint32_t)val;
    switch (addr) {
    case R_CHANNEL:  s->channel = v; break;
    case R_ISR:      s->isr &= ~v; update_irq(s); break;   /* write 1 to clear */
    case R_IMR:      s->imr = v; update_irq(s); break;
    case R_RX_BASE_LO: s->rx_base = (s->rx_base & ~0xFFFFFFFFULL) | v; break;
    case R_RX_BASE_HI: s->rx_base = (s->rx_base & 0xFFFFFFFFULL) | ((uint64_t)v << 32); break;
    case R_RX_LEN:   s->rx_len = v; break;
    case R_TX_BASE_LO: s->tx_base = (s->tx_base & ~0xFFFFFFFFULL) | v; break;
    case R_TX_BASE_HI: s->tx_base = (s->tx_base & 0xFFFFFFFFULL) | ((uint64_t)v << 32); break;
    case R_TX_LEN:   s->tx_len = v; break;
    case R_TX_TAIL:
        s->tx_tail = s->tx_len ? v % s->tx_len : 0;
        transmit(s);
        break;
    case R_CTRL:
        if ((v & CTRL_ENABLE) && !(s->ctrl & CTRL_ENABLE)) {
            s->rx_head = s->tx_head = s->tx_tail = 0;
        }
        s->ctrl = v;
        if (v & CTRL_ENABLE) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        break;
    }
}

static const MemoryRegionOps reg_ops = {
    .read = reg_read,
    .write = reg_write,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static NetClientInfo net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = can_receive,
    .receive = receive,
};

static void realize(PCIDevice *dev, Error **errp)
{
    RitterWifiState *s = RITTERWIFI(dev);
    DeviceState *d = DEVICE(dev);
    dev->config[PCI_INTERRUPT_PIN] = 1;
    if (msi_init(dev, 0x50, 1, true, false, errp) < 0) {
        return;
    }
    memory_region_init_io(&s->mmio, OBJECT(s), &reg_ops, s, "ritterwifi-mmio", REGS_SIZE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->mmio);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_info, &s->conf, object_get_typename(OBJECT(dev)), d->id,
                          &d->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    s->channel = 1;
}

static void uninit(PCIDevice *dev)
{
    qemu_del_nic(RITTERWIFI(dev)->nic);
}

static void reset(DeviceState *dev)
{
    RitterWifiState *s = RITTERWIFI(dev);
    s->isr = s->imr = s->rx_len = s->tx_len = s->tx_tail = s->ctrl = 0;
    s->rx_base = s->tx_base = 0;
    s->rx_head = s->tx_head = 0;
    s->channel = 1;
}

static const Property ritterwifi_properties[] = {
    DEFINE_NIC_PROPERTIES(RitterWifiState, conf),
};

static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    k->realize = realize;
    k->exit = uninit;
    k->vendor_id = 0xF055;              /* invented: RitterOS */
    k->device_id = 0x11AC;             /* 802.11ac */
    k->class_id = PCI_CLASS_NETWORK_OTHER;
    k->revision = 1;
    dc->desc = "RitterOS test Wi-Fi card";
    device_class_set_legacy_reset(dc, reset);
    device_class_set_props(dc, ritterwifi_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo types[] = {
    {
        .name          = TYPE_RITTERWIFI,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(RitterWifiState),
        .class_init    = class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(types)
