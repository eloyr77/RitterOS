/*
 * A USB CDC-NCM Ethernet device for QEMU (RitterOS step 50a: our own model,
 * as QEMU has no NCM device, only RNDIS and CDC-ECM). It presents the
 * Network Control Model: Ethernet frames are carried in NCM Transfer Blocks
 * (NTB) on the bulk endpoints, which pack one or more frames with a short
 * header (NTH16) and a datagram-pointer table (NDP16). The device is
 * `-device usb-ncm,netdev=...`, bridged to a QEMU netdev like usb-net.
 *
 * Modelled on QEMU's hw/usb/dev-network.c (the CDC-ECM path), with RNDIS
 * dropped and ECM framing replaced by NCM. It implements one data interface
 * (alt 0 off, alt 1 with the bulk endpoints), the NCM functional
 * descriptor, GET_NTB_PARAMETERS and the NTB input/format/filter setters,
 * and a notification endpoint that reports the link up.
 *
 * Like a real device (a Linux gadget, an adapter), the bulk IN endpoint
 * answers NAK while there is nothing to send, so a read stays pending until
 * a frame comes (usb_wakeup). And like the PlutoSDR's composite gadget, a
 * CDC ACM serial port follows the NCM function (interfaces 2 and 3): its
 * CDC Data interface must not be taken for NCM's. The serial port's bytes
 * go to and come from a QEMU chardev (`chardev=ID`, optional: without one
 * what the guest writes is dropped and nothing comes).
 *
 * scripts/setup-toolchain.sh copies this into QEMU's hw/usb and builds it
 * in (again whenever the file changes). It is our reading of the NCM class
 * (USB CDC NCM 1.0), enough to drive our kernel's usbnet.c and no more.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "desc.h"
#include "net/net.h"
#include "qemu/error-report.h"
#include "system/system.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

/* CDC and NCM constants (not in QEMU's shared headers). */
#define USB_CDC_SUBCLASS_NCM            0x0d
#define USB_CDC_PROTO_NONE              0
#define USB_CDC_HEADER_TYPE             0x00
#define USB_CDC_UNION_TYPE              0x06
#define USB_CDC_ETHERNET_TYPE           0x0f
#define USB_CDC_NCM_TYPE                0x1a
#define USB_CDC_NETWORK_CONNECTION      0x00
#define USB_CDC_SET_ETHERNET_PACKET_FILTER  0x43
#define USB_CDC_GET_NTB_PARAMETERS      0x80
#define USB_CDC_GET_NTB_INPUT_SIZE      0x85
#define USB_CDC_SET_NTB_INPUT_SIZE      0x86
#define USB_CDC_GET_NTB_FORMAT          0x83
#define USB_CDC_SET_NTB_FORMAT          0x84
#define USB_CDC_SET_LINE_CODING         0x20    /* the serial port's (ACM) */
#define USB_CDC_GET_LINE_CODING         0x21
#define USB_CDC_SET_CONTROL_LINE_STATE  0x22
#define ACM_BUF                         4096    /* bytes from the chardev waiting for the guest */

#define LOG2_STATUS_INTERVAL_MSEC       5
#define STATUS_BYTECOUNT                16
#define NTB_MAX                         16384   /* (64n) our NTB size: several frames, as
                                                 * Linux's gadget (16 KiB) */
#define NTB_DATAGRAMS                   32      /* frames in one NTB at most */
#define AGGREGATE_NS                    300000  /* (64n) how long frames are gathered into
                                                 * one NTB (Linux's TX_TIMEOUT_NSECS) */
#define ETH_FRAME_LEN                   1514

enum {
    STRING_MANUFACTURER = 1,
    STRING_PRODUCT,
    STRING_ETHADDR,
    STRING_DATA,
    STRING_CONTROL,
    STRING_SERIALNUMBER,
};

static const USBDescStrings usb_ncm_stringtable = {
    [STRING_MANUFACTURER]       = "RitterOS",
    [STRING_PRODUCT]            = "QEMU USB NCM Network Device",
    [STRING_ETHADDR]            = "400102030405",
    [STRING_DATA]               = "QEMU USB Net NCM Data",
    [STRING_CONTROL]            = "QEMU USB Net NCM Control",
    [STRING_SERIALNUMBER]       = "1",
};

static const USBDescIface desc_iface_ncm[] = {
    {
        /* NCM Communications (control) interface */
        .bInterfaceNumber              = 0,
        .bNumEndpoints                 = 1,
        .bInterfaceClass               = USB_CLASS_COMM,
        .bInterfaceSubClass            = USB_CDC_SUBCLASS_NCM,
        .bInterfaceProtocol            = USB_CDC_PROTO_NONE,
        .iInterface                    = STRING_CONTROL,
        .ndesc                         = 4,
        .descs = (USBDescOther[]) {
            {
                /* Header functional descriptor */
                .data = (uint8_t[]) {
                    0x05, USB_DT_CS_INTERFACE, USB_CDC_HEADER_TYPE, 0x10, 0x01,
                },
            },{
                /* Union functional descriptor: control 0, data 1 */
                .data = (uint8_t[]) {
                    0x05, USB_DT_CS_INTERFACE, USB_CDC_UNION_TYPE, 0x00, 0x01,
                },
            },{
                /* Ethernet Networking functional descriptor */
                .data = (uint8_t[]) {
                    0x0d, USB_DT_CS_INTERFACE, USB_CDC_ETHERNET_TYPE,
                    STRING_ETHADDR,             /* iMACAddress */
                    0x00, 0x00, 0x00, 0x00,     /* bmEthernetStatistics */
                    ETH_FRAME_LEN & 0xff, ETH_FRAME_LEN >> 8,   /* wMaxSegmentSize */
                    0x00, 0x00,                 /* wNumberMCFilters */
                    0x00,                       /* bNumberPowerFilters */
                },
            },{
                /* NCM functional descriptor: version 1.0, no capabilities */
                .data = (uint8_t[]) {
                    0x06, USB_DT_CS_INTERFACE, USB_CDC_NCM_TYPE, 0x00, 0x01, 0x00,
                },
            },
        },
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_IN | 0x01,
                .bmAttributes          = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize        = STATUS_BYTECOUNT,
                .bInterval             = 1 << LOG2_STATUS_INTERVAL_MSEC,
            },
        },
    },{
        /* Data interface, alternate setting 0: no endpoints (off) */
        .bInterfaceNumber              = 1,
        .bAlternateSetting             = 0,
        .bNumEndpoints                 = 0,
        .bInterfaceClass               = USB_CLASS_CDC_DATA,
    },{
        /* Data interface, alternate setting 1: the bulk endpoints */
        .bInterfaceNumber              = 1,
        .bAlternateSetting             = 1,
        .bNumEndpoints                 = 2,
        .bInterfaceClass               = USB_CLASS_CDC_DATA,
        .iInterface                    = STRING_DATA,
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_IN | 0x02,
                .bmAttributes          = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize        = 0x200,
            },{
                .bEndpointAddress      = USB_DIR_OUT | 0x02,
                .bmAttributes          = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize        = 0x200,
            },
        },
    },{
        /* A CDC ACM serial port, as composite gadgets have (not served) */
        .bInterfaceNumber              = 2,
        .bNumEndpoints                 = 1,
        .bInterfaceClass               = USB_CLASS_COMM,
        .bInterfaceSubClass            = 0x02,      /* abstract control model */
        .bInterfaceProtocol            = 0x01,
        .ndesc                         = 2,
        .descs = (USBDescOther[]) {
            {
                .data = (uint8_t[]) {
                    0x05, USB_DT_CS_INTERFACE, USB_CDC_HEADER_TYPE, 0x10, 0x01,
                },
            },{
                /* Union functional descriptor: control 2, data 3 */
                .data = (uint8_t[]) {
                    0x05, USB_DT_CS_INTERFACE, USB_CDC_UNION_TYPE, 0x02, 0x03,
                },
            },
        },
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_IN | 0x03,
                .bmAttributes          = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize        = 10,
                .bInterval             = 1 << LOG2_STATUS_INTERVAL_MSEC,
            },
        },
    },{
        /* Its CDC Data interface: bulk endpoints in setting 0 */
        .bInterfaceNumber              = 3,
        .bNumEndpoints                 = 2,
        .bInterfaceClass               = USB_CLASS_CDC_DATA,
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_IN | 0x04,
                .bmAttributes          = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize        = 0x200,
            },{
                .bEndpointAddress      = USB_DIR_OUT | 0x04,
                .bmAttributes          = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize        = 0x200,
            },
        },
    },
};

static const USBDescDevice desc_device_ncm = {
    .bcdUSB                        = 0x0200,
    .bDeviceClass                  = 0,     /* see the interfaces (a composite device) */
    .bMaxPacketSize0               = 0x40,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 4,
            .bConfigurationValue   = 1,
            .iConfiguration        = STRING_PRODUCT,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
            .bMaxPower             = 0x32,
            .nif = ARRAY_SIZE(desc_iface_ncm),
            .ifs = desc_iface_ncm,
        },
    },
};

static const USBDesc desc_ncm = {
    .id = {
        .idVendor          = 0x0525,
        .idProduct         = 0xa4a5,
        .bcdDevice         = 0,
        .iManufacturer     = STRING_MANUFACTURER,
        .iProduct          = STRING_PRODUCT,
        .iSerialNumber     = STRING_SERIALNUMBER,
    },
    .full = &desc_device_ncm,
    .high = &desc_device_ncm,
    .str  = usb_ncm_stringtable,
};

struct USBNcmState {
    USBDevice dev;
    USBEndpoint *intr;
    USBEndpoint *bulk_in;
    NICState *nic;
    NICConf conf;
    uint16_t connection;
    uint16_t tx_seq;                    /* NTB sequence number, host to guest */
    uint8_t in_buf[NTB_MAX];            /* an NTB waiting for the guest */
    uint32_t in_len, in_ptr;            /* in_len 0: frames still gathered (below) */
    /* (64n) Frames gathered for the next NTB (already in in_buf, from 12 on,
     * each at a multiple of 4), until the timer runs out or it is full. */
    uint32_t gather_end, gather_count;
    uint16_t gather_at[NTB_DATAGRAMS], gather_len[NTB_DATAGRAMS];
    QEMUTimer *gather_timer;
    bool gathering;                     /* the timer runs */
    bool split;                         /* (64n) property: two NTBs in one transfer */
    uint8_t split_buf[NTB_MAX];
    uint8_t out_buf[NTB_MAX];           /* an NTB coming from the guest */
    uint32_t out_ptr;
    char usbstring_mac[13];
    CharFrontend cs;                    /* the serial port's other end */
    USBEndpoint *acm_in;
    uint8_t acm_buf[ACM_BUF];
    uint32_t acm_len;
    uint8_t line_coding[7];             /* as the guest set it (115200 8N1 at first) */
    uint16_t line_state;                /* DTR (bit 0), RTS (bit 1) */
};

#define TYPE_USB_NCM "usb-ncm"
OBJECT_DECLARE_SIMPLE_TYPE(USBNcmState, USB_NCM)

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff, p[1] = v >> 8;
}

static uint16_t get16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static void usb_ncm_reset_in(USBNcmState *s)
{
    s->in_ptr = s->in_len = 0;
    s->gather_count = 0;
    s->gather_end = 12;
}

static bool usb_ncm_compose_split(USBNcmState *s);

/* (64n) The NTB of the gathered frames: its NDP after them, then the
 * header (Linux's gadget puts the NDP at the end too). */
static void usb_ncm_compose(USBNcmState *s)
{
    if (s->split && usb_ncm_compose_split(s))
        return;
    uint8_t *b = s->in_buf;
    uint32_t ndp = (s->gather_end + 3) & ~3u;
    uint32_t ndp_len = 8 + 4 * (s->gather_count + 1);
    memcpy(b + ndp, "NCM0", 4);
    put16(b + ndp + 4, ndp_len);                /* wLength */
    put16(b + ndp + 6, 0);                      /* wNextNdpIndex */
    for (uint32_t i = 0; i < s->gather_count; i++) {
        put16(b + ndp + 8 + 4 * i, s->gather_at[i]);
        put16(b + ndp + 10 + 4 * i, s->gather_len[i]);
    }
    put16(b + ndp + 8 + 4 * s->gather_count, 0);    /* the terminating entry */
    put16(b + ndp + 10 + 4 * s->gather_count, 0);
    uint32_t total = ndp + ndp_len;
    memcpy(b, "NCMH", 4);
    put16(b + 4, 12);                           /* wHeaderLength */
    put16(b + 6, s->tx_seq++);                  /* wSequence */
    put16(b + 8, total);                        /* wBlockLength */
    put16(b + 10, ndp);                         /* wNdpIndex */
    s->in_len = total;
    s->in_ptr = 0;
    s->gather_count = 0;
}

/* (64n) With `split=on`, the gathered frames go out as two NTBs one after
 * the other in one transfer, as an adapter that leaves a transfer's end
 * out between NTBs would send them (the driver must find the second). */
static uint32_t usb_ncm_build(USBNcmState *s, uint8_t *b, const uint8_t *src,
                              const uint16_t *at, const uint16_t *len, uint32_t n)
{
    uint32_t end = 12;
    uint16_t place[NTB_DATAGRAMS];
    for (uint32_t i = 0; i < n; i++) {
        end = (end + 3) & ~3u;
        memcpy(b + end, src + at[i], len[i]);
        place[i] = end;
        end += len[i];
    }
    uint32_t ndp = (end + 3) & ~3u;
    uint32_t ndp_len = 8 + 4 * (n + 1);
    memcpy(b + ndp, "NCM0", 4);
    put16(b + ndp + 4, ndp_len);
    put16(b + ndp + 6, 0);
    for (uint32_t i = 0; i < n; i++) {
        put16(b + ndp + 8 + 4 * i, place[i]);
        put16(b + ndp + 10 + 4 * i, len[i]);
    }
    put16(b + ndp + 8 + 4 * n, 0);
    put16(b + ndp + 10 + 4 * n, 0);
    uint32_t total = ndp + ndp_len;
    memcpy(b, "NCMH", 4);
    put16(b + 4, 12);
    put16(b + 6, s->tx_seq++);
    put16(b + 8, total);
    put16(b + 10, ndp);
    return total;
}

static bool usb_ncm_compose_split(USBNcmState *s)
{
    uint32_t n = s->gather_count, k = n / 2;
    if (n < 2 || s->gather_end + 12 + 8 + 4 * (n + 2) + 8 > NTB_MAX)
        return false;
    memcpy(s->split_buf, s->in_buf, s->gather_end);
    uint32_t first = usb_ncm_build(s, s->in_buf, s->split_buf, s->gather_at, s->gather_len, k);
    uint32_t second = usb_ncm_build(s, s->in_buf + first, s->split_buf, s->gather_at + k,
                                    s->gather_len + k, n - k);
    s->in_len = first + second;
    s->in_ptr = 0;
    s->gather_count = 0;
    return true;
}

static void usb_ncm_gathered(void *opaque)
{
    USBNcmState *s = opaque;
    s->gathering = false;
    usb_wakeup(s->bulk_in, 0);                  /* the pending read takes the NTB */
}

static void usb_ncm_handle_reset(USBDevice *dev)
{
    USBNcmState *s = USB_NCM(dev);
    if (s->gather_timer)
        timer_del(s->gather_timer);
    s->gathering = false;
    usb_ncm_reset_in(s);
    s->out_ptr = 0;
}

/* GET_NTB_PARAMETERS answer (28 bytes): NTB-16 only, our sizes. */
static int ntb_parameters(USBNcmState *s, uint8_t *data)
{
    (void)s;
    memset(data, 0, 28);
    put16(data, 28);                    /* wLength */
    put16(data + 2, 0x0001);            /* bmNtbFormatsSupported: NTB-16 */
    data[4] = NTB_MAX & 0xff, data[5] = (NTB_MAX >> 8) & 0xff;  /* dwNtbInMaxSize */
    put16(data + 8, 4);                 /* wNdpInDivisor */
    put16(data + 10, 0);                /* wNdpInPayloadRemainder */
    put16(data + 12, 4);                /* wNdpInAlignment */
    data[16] = NTB_MAX & 0xff, data[17] = (NTB_MAX >> 8) & 0xff; /* dwNtbOutMaxSize */
    put16(data + 20, 4);                /* wNdpOutDivisor */
    put16(data + 22, 0);                /* wNdpOutPayloadRemainder */
    put16(data + 24, 4);                /* wNdpOutAlignment */
    put16(data + 26, 0);                /* wNtbOutMaxDatagrams: 0 = no limit */
    return 28;
}

static void usb_ncm_handle_control(USBDevice *dev, USBPacket *p, int request, int value,
                                   int index, int length, uint8_t *data)
{
    USBNcmState *s = USB_NCM(dev);
    if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0)
        return;
    switch (request) {
    case ClassInterfaceRequest | USB_CDC_GET_NTB_PARAMETERS:
        p->actual_length = ntb_parameters(s, data);
        break;
    case ClassInterfaceRequest | USB_CDC_GET_NTB_FORMAT:
        put16(data, 0);                 /* NTB-16 */
        p->actual_length = 2;
        break;
    case ClassInterfaceOutRequest | USB_CDC_SET_NTB_FORMAT:
    case ClassInterfaceOutRequest | USB_CDC_SET_NTB_INPUT_SIZE:
    case ClassInterfaceOutRequest | USB_CDC_SET_ETHERNET_PACKET_FILTER:
        break;                          /* accepted */
    case ClassInterfaceOutRequest | USB_CDC_SET_LINE_CODING:
        if (length >= 7)
            memcpy(s->line_coding, data, 7);
        break;
    case ClassInterfaceRequest | USB_CDC_GET_LINE_CODING:
        memcpy(data, s->line_coding, 7);
        p->actual_length = 7;
        break;
    case ClassInterfaceOutRequest | USB_CDC_SET_CONTROL_LINE_STATE:
        s->line_state = value;
        break;
    default:
        fprintf(stderr, "usb-ncm: unknown control 0x%x value 0x%x index 0x%x length 0x%x\n",
                request, value, index, length);
        p->status = USB_RET_STALL;
        break;
    }
}

/* The notification endpoint: report the link connected on every poll. */
static void usb_ncm_statusin(USBNcmState *s, USBPacket *p)
{
    uint8_t buf[8];
    if (p->iov.size < 8) {
        p->status = USB_RET_STALL;
        return;
    }
    uint16_t rt = ClassInterfaceRequest | USB_CDC_NETWORK_CONNECTION;
    buf[0] = rt >> 8;                           /* bmRequestType 0xa1 */
    buf[1] = rt & 0xff;                         /* bNotification: NETWORK_CONNECTION */
    put16(buf + 2, s->connection);              /* wValue: 1 = connected */
    put16(buf + 4, 0);                          /* wIndex: interface 0 */
    put16(buf + 6, 0);                          /* wLength */
    usb_packet_copy(p, buf, 8);
}

/* The bulk IN endpoint: hand the waiting NTB to the guest (with a zero
 * length packet after one whose length is a multiple of the packet size). */
static void usb_ncm_datain(USBNcmState *s, USBPacket *p)
{
    if (!s->in_len) {
        if (!s->gather_count || s->gathering) {
            p->status = USB_RET_NAK;    /* idle or still gathering: the read waits (usb_wakeup) */
            return;
        }
        usb_ncm_compose(s);
    }
    int len = s->in_len - s->in_ptr;
    if (len > (int)p->iov.size)
        len = p->iov.size;
    usb_packet_copy(p, &s->in_buf[s->in_ptr], len);
    s->in_ptr += len;
    if (s->in_ptr >= s->in_len && ((s->in_len & 0x1ff) || !len)) {
        usb_ncm_reset_in(s);
        /* (64n) The netdev held the frames that came while this NTB waited
         * (usb_ncm_receive answered 0): it gives them only when asked. Until
         * 64n nothing asked, so the first two frames close together, as any
         * download sends, stopped receiving for good. */
        if (s->nic)
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

/* Parse an NTB the guest sent and give each Ethernet frame to the netdev. */
static void usb_ncm_deliver(USBNcmState *s, const uint8_t *ntb, uint32_t total)
{
    if (total < 12 || memcmp(ntb, "NCMH", 4) != 0)
        return;
    uint32_t ndp = get16(ntb + 10);             /* wNdpIndex */
    while (ndp && ndp + 8 <= total) {
        if (memcmp(ntb + ndp, "NCM0", 4) != 0)
            break;
        uint32_t ndp_len = get16(ntb + ndp + 4);
        uint32_t next = get16(ntb + ndp + 6);
        for (uint32_t e = ndp + 8; e + 4 <= ndp + ndp_len && e + 4 <= total; e += 4) {
            uint32_t idx = get16(ntb + e), dlen = get16(ntb + e + 2);
            if (!idx && !dlen)
                break;                          /* the terminating null entry */
            if (idx + dlen <= total && dlen >= 14)
                qemu_send_packet(qemu_get_queue(s->nic), ntb + idx, dlen);
        }
        ndp = next;
    }
}

static void usb_ncm_dataout(USBNcmState *s, USBPacket *p)
{
    uint32_t room = sizeof(s->out_buf) - s->out_ptr, n = p->iov.size;
    if (n > room)
        n = room;
    usb_packet_copy(p, &s->out_buf[s->out_ptr], n);
    s->out_ptr += n;
    if (p->iov.size % 0x200 || p->iov.size == 0) {  /* a short packet ends the NTB */
        usb_ncm_deliver(s, s->out_buf, s->out_ptr);
        s->out_ptr = 0;
    }
}

/* The serial port's bulk IN: what the chardev sent, or NAK while nothing. */
static void usb_ncm_acm_in(USBNcmState *s, USBPacket *p)
{
    if (!s->acm_len) {
        p->status = USB_RET_NAK;
        return;
    }
    uint32_t n = s->acm_len < p->iov.size ? s->acm_len : p->iov.size;
    usb_packet_copy(p, s->acm_buf, n);
    memmove(s->acm_buf, s->acm_buf + n, s->acm_len - n);
    s->acm_len -= n;
    qemu_chr_fe_accept_input(&s->cs);
}

/* The serial port's bulk OUT: to the chardev (dropped without one). */
static void usb_ncm_acm_out(USBNcmState *s, USBPacket *p)
{
    uint8_t buf[0x200];
    uint32_t n = p->iov.size < sizeof(buf) ? p->iov.size : sizeof(buf);
    usb_packet_copy(p, buf, n);
    if (qemu_chr_fe_backend_connected(&s->cs))
        qemu_chr_fe_write_all(&s->cs, buf, n);
}

static int usb_ncm_acm_can_read(void *opaque)
{
    USBNcmState *s = opaque;
    return s->dev.attached ? ACM_BUF - (int)s->acm_len : 0;
}

static void usb_ncm_acm_read(void *opaque, const uint8_t *buf, int size)
{
    USBNcmState *s = opaque;
    if (size > (int)(ACM_BUF - s->acm_len))
        size = ACM_BUF - s->acm_len;
    memcpy(s->acm_buf + s->acm_len, buf, size);
    s->acm_len += size;
    usb_wakeup(s->acm_in, 0);
}

static void usb_ncm_handle_data(USBDevice *dev, USBPacket *p)
{
    USBNcmState *s = USB_NCM(dev);
    if (p->pid == USB_TOKEN_IN && p->ep->nr == 1)
        usb_ncm_statusin(s, p);
    else if (p->pid == USB_TOKEN_IN && p->ep->nr == 2)
        usb_ncm_datain(s, p);
    else if (p->pid == USB_TOKEN_OUT && p->ep->nr == 2)
        usb_ncm_dataout(s, p);
    else if (p->pid == USB_TOKEN_IN && p->ep->nr == 3)
        p->status = USB_RET_NAK;        /* the serial port's notifications: none */
    else if (p->pid == USB_TOKEN_IN && p->ep->nr == 4)
        usb_ncm_acm_in(s, p);
    else if (p->pid == USB_TOKEN_OUT && p->ep->nr == 4)
        usb_ncm_acm_out(s, p);
    else
        p->status = USB_RET_STALL;
}

/* A frame from the netdev: (64n) gathered with those that come within
 * AGGREGATE_NS into one NTB, as Linux's gadget (the PlutoSDR's) does;
 * until 64n each frame was an NTB of its own, so the driver's handling of
 * several frames in one was never tried in QEMU. 0 (QEMU holds the frame)
 * while an NTB waits to be taken or this one is full. */
static ssize_t usb_ncm_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    USBNcmState *s = qemu_get_nic_opaque(nc);
    if (!s->dev.config)
        return -1;
    if (12 + 4 + size + 8 + 8 > sizeof(s->in_buf))
        return -1;
    if (s->in_len > 0)
        return 0;                               /* the previous NTB is not yet taken */
    uint32_t at = (s->gather_end + 3) & ~3u;
    uint32_t ndp_after = ((at + size + 3) & ~3u) + 8 + 4 * (s->gather_count + 2);
    if (s->gather_count == NTB_DATAGRAMS || ndp_after > sizeof(s->in_buf)) {
        /* Full: hand it over as soon as possible, but from the timer, never
         * from here: inside the netdev's delivery, a read ended at once
         * would ask for the held frames while it still delivers, and QEMU's
         * queue does not deliver twice at once (they stayed held). */
        s->gathering = true;
        timer_mod(s->gather_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        return 0;
    }
    memcpy(s->in_buf + at, buf, size);
    s->gather_at[s->gather_count] = at;
    s->gather_len[s->gather_count] = size;
    s->gather_count++;
    s->gather_end = at + size;
    if (!s->gathering) {
        s->gathering = true;
        timer_mod(s->gather_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + AGGREGATE_NS);
    }
    return size;
}

static void usb_ncm_cleanup(NetClientState *nc)
{
    USBNcmState *s = qemu_get_nic_opaque(nc);
    s->nic = NULL;
}

static void usb_ncm_unrealize(USBDevice *dev)
{
    USBNcmState *s = USB_NCM(dev);
    timer_free(s->gather_timer);
    qemu_chr_fe_deinit(&s->cs, false);  /* the chardev is free for the next device_add */
    qemu_del_nic(s->nic);
}

static NetClientInfo net_usb_ncm_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = usb_ncm_receive,
    .cleanup = usb_ncm_cleanup,
};

static void usb_ncm_realize(USBDevice *dev, Error **errp)
{
    USBNcmState *s = USB_NCM(dev);
    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    s->connection = 1;
    s->tx_seq = 0;
    s->gather_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usb_ncm_gathered, s);
    s->gathering = false;
    usb_ncm_reset_in(s);
    s->out_ptr = 0;
    s->intr = usb_ep_get(dev, USB_TOKEN_IN, 1);
    s->bulk_in = usb_ep_get(dev, USB_TOKEN_IN, 2);
    s->acm_in = usb_ep_get(dev, USB_TOKEN_IN, 4);
    static const uint8_t line[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 };    /* 115200 8N1 */
    memcpy(s->line_coding, line, sizeof(line));
    s->acm_len = 0;
    if (qemu_chr_fe_backend_connected(&s->cs))
        qemu_chr_fe_set_handlers(&s->cs, usb_ncm_acm_can_read, usb_ncm_acm_read, NULL, NULL, s,
                                 NULL, true);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_usb_ncm_info, &s->conf, object_get_typename(OBJECT(s)),
                          s->dev.qdev.id, &s->dev.qdev.mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    snprintf(s->usbstring_mac, sizeof(s->usbstring_mac), "%02x%02x%02x%02x%02x%02x",
             s->conf.macaddr.a[0], s->conf.macaddr.a[1], s->conf.macaddr.a[2],
             s->conf.macaddr.a[3], s->conf.macaddr.a[4], s->conf.macaddr.a[5]);
    usb_desc_set_string(dev, STRING_ETHADDR, s->usbstring_mac);
}

static const VMStateDescription vmstate_usb_ncm = {
    .name = "usb-ncm",
    .unmigratable = 1,
};

static const Property usb_ncm_properties[] = {
    DEFINE_NIC_PROPERTIES(USBNcmState, conf),
    DEFINE_PROP_CHR("chardev", USBNcmState, cs),
    DEFINE_PROP_BOOL("split", USBNcmState, split, false),
};

static void usb_ncm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);
    uc->realize        = usb_ncm_realize;
    uc->product_desc   = "QEMU USB NCM Network Device";
    uc->usb_desc       = &desc_ncm;
    uc->handle_reset   = usb_ncm_handle_reset;
    uc->handle_control = usb_ncm_handle_control;
    uc->handle_data    = usb_ncm_handle_data;
    uc->unrealize      = usb_ncm_unrealize;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->fw_name = "network";
    dc->vmsd = &vmstate_usb_ncm;
    device_class_set_props(dc, usb_ncm_properties);
}

static const TypeInfo usb_ncm_info = {
    .name          = TYPE_USB_NCM,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBNcmState),
    .class_init    = usb_ncm_class_init,
};

static void usb_ncm_register_types(void)
{
    type_register_static(&usb_ncm_info);
}

type_init(usb_ncm_register_types)
