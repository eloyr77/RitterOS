/*
 * MediaTek MT7921 PCIe Wi-Fi driver (step 24f, mt7921.h). Our own code; the
 * register addresses and the bring-up sequence are facts about the chip (from
 * Linux's mt76 driver), the code is ours, as with the Realtek models. Real
 * hardware only (no QEMU model): tested on the user's laptop over SSH.
 *
 *   24f-1: find the chip, map BAR0, take ownership (the LPCTL handshake).
 *   24f-2: download the firmware. The card probe runs before the file system
 *          is mounted, so a worker thread waits for it, then resets the Wi-Fi
 *          subsystem, sets up the DMA rings (below 4 GiB -- 32-bit DMA), and
 *          downloads the ROM patch and the RAM firmware through the chip's
 *          MCU, waiting for the firmware to report itself running.
 *
 * All the risky work is in the worker thread, which runs after the boot is
 * complete, so a failure there only stops the thread; the system stays up.
 */
#include "driver.h"
#include "driver_net.h"
#include "driver_wifi.h"

/* --- registers (BAR offsets; high chip addresses use the L1 remap) --- */
#define L1_REMAP        0x000FE24C
#define L1_WINDOW       0x00040000
#define MT_HW_CHIPID    0x70010200      /* via L1 */
#define MT_HW_REV       0x70010204
#define MT_HW_BOUND     0x70010020
#define LPCR            0x18060010      /* via L1: MT_TOP_LPCR_HOST_BAND0 */
#define LPCR_DRV_OWN    0x2
#define LPCR_FW_OWN     0x1
#define WFSYS_RST       0x18000140      /* via L1: MT_WFSYS_SW_RST_B */
#define WFSYS_RST_B     (1u << 0)
#define WFSYS_INIT_DONE (1u << 4)

#define LPCTL           0x000E0010      /* MT_CONN_ON_LPCTL (direct) */
#define OWN_SET         0x1
#define OWN_CLR         0x2
#define OWN_SYNC        0x4
#define CONN_ON_MISC    0x000E00F0      /* bits 1:0 == 3: firmware running */
#define FW_N9_RDY       0x3
#define SWDEF_MODE      0x0009F23C      /* 0x0041f23c: 0 = normal */
#define PCIE_MAC_PM     0x00010194      /* bit8 L0S_DIS */
#define PCIE_MAC_INT_EN 0x00010188
#define DUMMY_CR        0x00002120      /* 0x54000120: bit1 "DMA inited" */
#define DMASHDL_CTRL    0x000D6004      /* 0x7c026004: bit28 bypass */

#define WFDMA           0x000D4000
#define GLO_CFG         0x000D4208
#define WFDMA0_RST      0x000D4100      /* bits 4,5 */
#define HOST_INT_ENA    0x000D4204
#define HOST_INT_STA    0x000D4200      /* write 1s to clear */
#define RX_INTS         ((1u << 0) | (1u << 2) | (1u << 22))   /* RX0, RX2, RX4 done */
#define MCU_SW_INT      (1u << 29)      /* the firmware's own interrupt: MCU_CMD says why */
#define MCU_CMD         0x000D41F0      /* write back to acknowledge */
#define MCU_WAKE_RX     (1u << 0)       /* the firmware asks the host to take RX */
#define GLO_CFG_EXT0    0x000D42B0      /* bit6 */
#define RST_DTX_PTR     0x000D420C
#define PRI_DLY_INT0    0x000D42F0
#define MCU2HOST_INT_EN 0x000D41F4

#define GLO_TX_EN       (1u << 0)
#define GLO_TX_BUSY     (1u << 1)
#define GLO_RX_EN       (1u << 2)
#define GLO_RX_BUSY     (1u << 3)

/* Descriptor control-word bits. */
#define DESC_LAST_SEC0  (1u << 30)
#define DESC_DMA_DONE   (1u << 31)
#define DESC_LEN0_SHIFT 16
#define DESC_LEN0_MASK  0x3FFF

/* Ring register bases (BAR). */
#define RING_TX0        0x000D4300
#define RING_TX16       0x000D4400      /* firmware download */
#define RING_TX17       0x000D4410      /* MCU commands */
#define RING_RX0        0x000D4500      /* MCU events before download */
#define RING_RX2        0x000D4520      /* data */
#define RING_RX4        0x000D4540      /* MCU events after download */
#define PREFETCH_TX     0x000D4600
#define PREFETCH_RX     0x000D4680

/* MCU command IDs (legacy). */
#define CMD_TARGET_ADDRESS_LEN 0x01
#define CMD_FW_START           0x02
#define CMD_PATCH_START        0x05
#define CMD_PATCH_FINISH       0x07
#define CMD_PATCH_SEM          0x10
#define CMD_FW_SCATTER         0xEE

/* "CE" command IDs (sent once the RAM firmware is running). */
#define CE_GET_NIC_CAPAB       0x8A
#define NIC_CAP_MAC_ADDR       7                /* the TLV type that holds the MAC */

/* "EXT" commands: cid is 0xED, the real command in ext_cid. */
#define EXT_CID                0xED
#define EXT_EFUSE_BUFFER_MODE  0x21

/* MAC init registers (band 0). The chip's WF_* sub-blocks are not in the L1
 * window; they sit at fixed BAR offsets (mt76's fixed_map), reached with plain
 * reads and writes, like the DMA ring registers. The offset for each is its
 * block's BAR base plus the register's offset within the block. */
#define MDP_DCR0        0x0F000                 /* bit15 DAMSDU_EN, bit19 RX_HDR_TRANS */
#define MDP_DCR1        0x0F004                 /* bits 15:3 MAX_RX_LEN */
#define WTBL_UPDATE     0x34230                 /* bits 9:0 idx, bit12 clear, bit31 busy */
#define TMAC_CTCR0      0x210F4
#define RMAC_MIB_TIME0  0x217C4
#define RMAC_MIB_AIR0   0x21780
#define MIB_SCR1        0x24804
#define DMA_DCR0        0x21E00                 /* bits 15:3 MAX_RX_LEN, bit23 RXD_G5_EN */
#define WTBLOFF_RSCR    0x23408                 /* bits 31:30 RCPI_MODE, 25:24 RCPI_PARAM */

/* More commands for the running firmware. */
#define EXT_MAC_INIT_CTRL      0x46             /* the MAC on or off */
#define EXT_SET_RX_PATH        0x4E             /* the channel the receiver is on */
#define UNI_DEV_INFO_UPDATE    0x0001           /* "unified" commands (a 16-byte header) */
#define UNI_BSS_INFO_UPDATE    0x0002
#define CE_START_HW_SCAN       0x03
#define CE_SET_CLC             0x5C             /* hand the firmware a country's rules */
#define CE_CHIP_CONFIG         0xCA             /* "KeepFullPwr": keep the radio awake */
#define CE_SET_RX_FILTER       0x0A             /* the receive filter's bits (RFCR) */
#define CE_SET_BSS_ABORT       0x17             /* the firmware forgets the joined BSS */
#define CE_SET_PS_PROFILE      0x05             /* the station's power saving (0: awake) */
#define RFCR_DROP_OTHER_BEACON (1u << 11)
#define BEACON_INTERVAL        100              /* TUs; the usual value (only power */
#define DTIM_PERIOD            1                /* saving uses them, and we stay awake) */
#define EXT_CHANNEL_SWITCH     0x08             /* tune the radio for the connection */
#define UNI_STA_REC_UPDATE     0x0003           /* add/update a peer station and its keys */
#define UNI_ROC                0x0027           /* remain on a channel (to hear the AP's reply) */
#define STA_WCID               1                /* the access point's WTBL entry */
#define BMC_WCID               19               /* the interface's own entry: broadcasts and
                                                 * their (group) key, as mt7921's last one */
#define TX_TOKENS              256              /* frames the chip may hold at once (the ring's size;
                                                 * 64 ran out in an 8 MB upload: 32 frames refused) */
#define TOKEN_WAIT_TICKS       2                /* how long a sender waits for a free one */
#define PENDING                256              /* (65e-27) frames waiting for a token */
#define TXWI_SIZE              64               /* TXD (32) + TXP (32) */
#define TX_FRAME_OFF           256              /* where the frame sits in a token's page */
#define EVENT_SCAN_DONE        0x0D             /* an MCU event's id (byte 28 of it) */
#define EVENT_BEACON_LOSS      0x13             /* the joined BSS's beacons stopped */
#define SCAN_REQ_SIZE          1186             /* mt76_connac_hw_scan_req */

/* The receive descriptor (mt76_connac2_mac.h): 24 fixed bytes, then the
 * optional groups rxd1 says are there, then (after 2 * HDR_OFFSET bytes of
 * padding) the 802.11 frame. */
#define RXD0_PKT_TYPE(v)       (((v) >> 27) & 0x1F)
#define RXD0_PKT_FLAG(v)       (((v) >> 16) & 0xF)
#define PKT_TYPE_TXS           0                /* (65e) a frame's TX status (mt76: PKT_TYPE_TXS) */
#define PKT_TYPE_NORMAL        2
#define PKT_TYPE_TXRX_NOTIFY   6                /* "these transmit tokens are done" */
#define PKT_TYPE_RX_EVENT      7
#define PKT_TYPE_NORMAL_MCU    8
#define RXD1_GROUP_1           (1u << 11)       /* 16 bytes */
#define RXD1_GROUP_2           (1u << 12)       /* 8 bytes */
#define RXD1_GROUP_3           (1u << 13)       /* 8 bytes: rate and signal */
#define RXD1_GROUP_4           (1u << 14)       /* 16 bytes */
#define RXD1_GROUP_5           (1u << 15)       /* 72 bytes (we switch it off) */
#define RXD1_SEC_MODE(x)       (((x) >> 16) & 0x1F)    /* the cipher it decrypted with */
#define RXD1_KEY_ID(x)         (((x) >> 21) & 3)
#define RXD1_CM                (1u << 23)       /* cipher mismatch (no key): not opened */
#define RXD1_CLM               (1u << 24)       /* cipher length mismatch: not opened */
#define RXD1_ICV_ERR           (1u << 25)       /* the MIC was wrong */
#define RXD1_FCS_ERR           (1u << 27)
#define RXD1_BAND_IDX          (1u << 28)
#define RXD2_HDR_TRANS         (1u << 13)       /* translated to Ethernet */

#define DL_4K           4096
#define BUF_SIZE        2048
#define CMD_ROOM        (2 * 4096)      /* the command/chunk buffer: two pages */

#define PATCH_FILE "WIFI_MT7961_patch_mcu_1_2_hdr.bin"     /* (in /system/firmware/) */
#define RAM_FILE   "WIFI_RAM_CODE_MT7961_1.bin"

struct desc {
    uint32_t buf0;
    uint32_t ctrl;
    uint32_t buf1;
    uint32_t info;
};

struct ring {
    struct desc *desc;              /* virtual */
    uint32_t desc_phys;             /* physical (< 4 GiB) */
    int count;
    uint32_t base;                  /* register base */
    uint8_t **buf;                  /* per-descriptor buffer (virtual), RX only */
    uint32_t cpu;                   /* our index */
};

struct mt7921 {
    volatile uint8_t *regs;
    char where[16];
    int chipid;
    struct ring tx_fwdl, tx_mcu, rx_event, rx_wa, rx_data;
    int scan_seq;
    uint8_t *cmd;                   /* a command/chunk buffer (virtual) */
    uint32_t cmd_phys;
    int seq;
    bool fw_ready;
    uint8_t mac[6];                 /* the chip's own address (from GET_NIC_CAPAB) */

    /* Once wlan0 is up (24f-3d): the RX thread owns the RX rings; commands
     * come from wifid's system calls too, one at a time. */
    struct net_card *dev;               /* (67d) its card (net_card_new): its priv is this */
    struct wifi_card wlan;
    struct mutex mcu_lock;          /* the command buffer and ring */
    struct wait_queue rx_wq;        /* the RX thread (woken by the interrupt or the tick) */
    struct pci_device *pci;
    bool msi;                       /* receive interrupts (else the tick polls) */
    volatile bool rx_asleep;        /* the RX thread waits (a tick finding work then
                                     * means an interrupt was missed) */
    volatile uint32_t interrupts, found_by_tick;
    volatile bool kicked;           /* an interrupt came: the thread must unmask again */
    uint32_t early;                 /* interrupts that came before their frame was done */
    struct wait_queue reply_wq;     /* a command waiting for its reply */
    struct wait_queue tok_wq;       /* senders waiting for a token to be freed */
    uint64_t tok_waits, tok_refused;  /* how often a sender waited, and in vain */
    uint64_t tok_refused_rx;        /* (65e) of those, refused on the receive thread at once */
    /* (65e-27) Frames waiting for a token, in order (under mcu_lock): with
     * every token at the chip a frame was refused, and on the receive
     * thread (TCP sends there when an ACK comes) at once. */
    uint8_t *pend[PENDING];         /* BUF_SIZE each */
    uint16_t pend_len[PENDING];
    int pend_head, pend_count, pend_max;
    uint64_t pend_queued;           /* frames that waited there */
    struct wlan_ht ap_ht;           /* (65e) what the access point said at the join */
    uint16_t ba_tx_window;          /* (65e) the sending Block Ack agreement's window */
    bool rx_running;
    volatile int reply_seq;         /* the reply to hand over (-1: none) */
    volatile int reply_len;         /* its payload length; -1 until it came */
    uint8_t *reply;                 /* (BUF_SIZE bytes) */
    volatile bool scanning;
    uint64_t scan_started;          /* (ticks) */
    int scan_frames;
    volatile int empty_scans;       /* (36f-3) scans in a row done at once, nothing heard */
    uint32_t empty_scans_all;

    uint8_t *clc;                   /* the firmware's country rules (its CLC region) */
    uint32_t clc_len;
    char country[4];                /* the country last set */

    struct ring tx_data;            /* 802.11 frames out (ring 0) */
    /* Frames in flight. On PCIe the chip works "cut-through": the DMA
     * descriptor holds only the TXD and a pointer block (TXP) naming a token
     * and the frame's address; the MAC fetches the frame later, so each frame
     * keeps its page until a TXRX_NOTIFY reports its token done. */
    uint8_t *tok[TX_TOKENS];        /* a page each, < 4 GiB: TXD+TXP, frame at 256 */
    uint32_t tok_phys[TX_TOKENS];
    volatile bool tok_busy[TX_TOKENS];
    uint64_t tok_time[TX_TOKENS];   /* (ticks) when it was handed to the chip */
    uint32_t tx_done, tx_failed;    /* completions the chip reported */
    uint64_t tok_cycles[TX_TOKENS];  /* when each token's frame went to the chip */
    uint64_t tx_cycles_sum, tx_cycles_max;  /* handoff to "done" (arch_counter) */
    uint64_t tx_spans[7];           /* (65e) frames by handoff to done: under 1, 2, 5, 10, 20,
                                     * 50 ms, longer */
    uint64_t tx_tries[5];           /* (65e) frames by attempts: 1, 2, 3-4, 5-8, more */
    uint32_t tx_sampled;            /* (65e) data frames sent; every TXS_EVERY-th asks for a TXS */
    uint64_t txs_count, txs_ampdu, txs_failed;
    uint64_t txs_bw[4];             /* (65e) by width: 20, 40, 80, 160 MHz (TXS0 30:29) */
    /* (65e) The chip's MIB counters (they clear when read), added up by the
     * tick once a second: A-MPDUs sent by length (16 ranges), and the time
     * sending, receiving and the channel busy (us). */
    uint64_t mib_aggr[16], mib_tx_us, mib_rx_us, mib_busy_us;
    int mib_ticks;
    bool mib_on;
    uint64_t txs_mode[5];           /* by mode: CCK, OFDM, HT, HT greenfield, VHT */
    uint64_t txs_ht[16];            /* HT: MCS 0-15 */
    uint64_t txs_vht[2][10];        /* VHT: streams 1 and 2, MCS 0-9 */
    uint64_t txs_ofdm[8];           /* OFDM rates by index */
    int tx_in_flight_max;
    uint8_t ap[6];                  /* the access point we are joining */
    int channel;                    /* the channel wifid tuned to (for the band) */
    bool ap_known;                  /* its station record is in the firmware */
    bool key_in_chip;               /* the chip holds the pairwise key: it seals and
                                     * opens the frames to and from the AP */
    bool bmc_known;                 /* the broadcast entry is in the firmware */
    bool joined;                    /* between join and leave */
    bool powersave;                 /* the joined BSS dozes (full power back at leave) */
    int bw;                         /* the channel's width: 0 20 MHz, 1 40, 2 80 */
    int center;                     /* its centre channel (40, 80 MHz) */
    volatile bool recovering;       /* the chip is being restarted (nothing goes in) */
    int mcu_timeouts;               /* commands in a row that got no answer */
    uint32_t tx_posted;             /* frames handed to the chip (the watch compares
                                     * with tx_done: posted, never done = stuck) */
    uint32_t restarts;
    uint32_t sleeps;                /* (37g-3) */
};

static struct mt7921 chips[4];
static int nchips;

static uint32_t rd(struct mt7921 *c, uint32_t off) { return mmio_read32(c->regs + off); }
static void wr(struct mt7921 *c, uint32_t off, uint32_t v) { mmio_write32(c->regs + off, v); }
static void set_bits(struct mt7921 *c, uint32_t off, uint32_t bits) { wr(c, off, rd(c, off) | bits); }
static void clr_bits(struct mt7921 *c, uint32_t off, uint32_t bits) { wr(c, off, rd(c, off) & ~bits); }

static uint32_t l1_read(struct mt7921 *c, uint32_t addr)
{
    wr(c, L1_REMAP, (rd(c, L1_REMAP) & 0xFFFF0000u) | (addr >> 16));
    rd(c, L1_REMAP);
    return rd(c, L1_WINDOW + (addr & 0xFFFF));
}

static void l1_write(struct mt7921 *c, uint32_t addr, uint32_t v)
{
    wr(c, L1_REMAP, (rd(c, L1_REMAP) & 0xFFFF0000u) | (addr >> 16));
    rd(c, L1_REMAP);
    wr(c, L1_WINDOW + (addr & 0xFFFF), v);
}

static void rmw(struct mt7921 *c, uint32_t off, uint32_t clear, uint32_t set)
{
    wr(c, off, (rd(c, off) & ~clear) | set);
}

static bool poll(struct mt7921 *c, uint32_t reg, uint32_t mask, uint32_t want, int ms)
{
    for (int i = 0; i <= ms; i++) {
        if ((rd(c, reg) & mask) == want)
            return true;
        timer_sleep_ms(1);
    }
    return false;
}

static bool poll_l1(struct mt7921 *c, uint32_t addr, uint32_t mask, uint32_t want, int ms)
{
    for (int i = 0; i <= ms; i++) {
        if ((l1_read(c, addr) & mask) == want)
            return true;
        timer_sleep_ms(1);
    }
    return false;
}

static bool take_ownership(struct mt7921 *c)
{
    bool ok = false;
    for (int i = 0; i < 10 && !ok; i++) {
        wr(c, LPCTL, OWN_SET);
        ok = poll(c, LPCTL, OWN_SYNC, OWN_SYNC, 50);
    }
    if (!ok)
        return false;
    ok = false;
    for (int i = 0; i < 10 && !ok; i++) {
        wr(c, LPCTL, OWN_CLR);
        ok = poll(c, LPCTL, OWN_SYNC, 0, 50);
    }
    return ok;
}

/* --- the firmware's files (67d: firmware_load, each kept whole once read) --- */

static struct {
    const char *name;
    uint8_t *data;
    uint64_t size;
} files[2];

static int64_t read_file(const char *name, uint64_t offset, void *buf, uint64_t n, uint64_t *size)
{
    int i = 0;
    while (i < 2 && files[i].name && strcmp(files[i].name, name))
        i++;
    if (i == 2)
        return -1;
    if (!files[i].name) {
        files[i].data = firmware_load(name, &files[i].size);
        if (!files[i].data)
            return -1;
        files[i].name = name;
    }
    if (size)
        *size = files[i].size;
    if (offset >= files[i].size)
        return 0;
    if (n > files[i].size - offset)
        n = files[i].size - offset;
    memcpy(buf, files[i].data + offset, n);
    return (int64_t)n;
}

/* --- DMA rings --- */

static bool ring_alloc(struct ring *r, uint32_t base, int count, bool rx)
{
    r->base = base;
    r->count = count;
    r->cpu = 0;
    if (r->desc) {                              /* a restart: the same memory, armed again */
        for (int i = 0; i < count; i++) {
            if (rx) {
                r->desc[i].buf0 = (uint32_t)virt_to_phys(r->buf[i]);
                r->desc[i].buf1 = 0, r->desc[i].info = 0;
                r->desc[i].ctrl = (BUF_SIZE & DESC_LEN0_MASK) << DESC_LEN0_SHIFT;
            } else {
                memset(&r->desc[i], 0, sizeof(r->desc[i]));
                r->desc[i].ctrl = DESC_DMA_DONE;
            }
        }
        arch_write_barrier();
        return true;
    }
    uint64_t bytes = (uint64_t)count * sizeof(struct desc);
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages_below(pages, 0x100000000ULL);
    if (!phys)
        return false;
    r->desc = phys_to_virt(phys);
    r->desc_phys = (uint32_t)phys;
    memset(r->desc, 0, pages * PAGE_SIZE);
    if (rx) {
        r->buf = kzalloc((size_t)count * sizeof(uint8_t *));
        if (!r->buf)
            return false;
        for (int i = 0; i < count; i++) {
            uint64_t bp = pmm_alloc_pages_below(1, 0x100000000ULL);
            if (!bp)
                return false;
            r->buf[i] = phys_to_virt(bp);
            r->desc[i].buf0 = (uint32_t)bp;
            r->desc[i].ctrl = (BUF_SIZE & DESC_LEN0_MASK) << DESC_LEN0_SHIFT;
        }
    } else {
        for (int i = 0; i < count; i++)
            r->desc[i].ctrl = DESC_DMA_DONE;
    }
    arch_write_barrier();
    return true;
}

static void ring_program(struct mt7921 *c, struct ring *r, bool rx)
{
    wr(c, r->base + 0x0, r->desc_phys);
    wr(c, r->base + 0x4, (uint32_t)r->count);
    wr(c, r->base + 0x8, 0);                    /* cpu_idx */
    wr(c, r->base + 0xC, 0);                    /* dma_idx */
    if (rx) {
        r->cpu = r->count - 1;
        wr(c, r->base + 0x8, r->cpu);           /* hand all but one buffer to the chip */
    }
}

static bool dma_init(struct mt7921 *c)
{
    /* dma_disable(force). */
    clr_bits(c, GLO_CFG, GLO_TX_EN | GLO_RX_EN | (1u << 15) | (1u << 27) | (1u << 28) | (1u << 21));
    if (!poll(c, GLO_CFG, GLO_TX_BUSY | GLO_RX_BUSY, 0, 100))
        return false;
    clr_bits(c, GLO_CFG_EXT0, 1u << 6);
    set_bits(c, DMASHDL_CTRL, 1u << 28);
    clr_bits(c, WFDMA0_RST, (1u << 4) | (1u << 5));
    set_bits(c, WFDMA0_RST, (1u << 4) | (1u << 5));     /* reset pulse ends with
                                                         * the bits set, as Linux
                                                         * leaves them; clearing
                                                         * them again let the DMA
                                                         * engine corrupt memory */

    /* The bootrom answers download commands on RX0; the running firmware
     * answers on RX4 (mt76's "change mcu queue after firmware download"), so
     * arm both and let mcu_wait scan them. */
    if (!ring_alloc(&c->tx_fwdl, RING_TX16, 128, false) ||
        !ring_alloc(&c->tx_mcu, RING_TX17, 256, false) ||
        !ring_alloc(&c->rx_event, RING_RX0, 8, true) ||
        !ring_alloc(&c->rx_wa, RING_RX4, 8, true) ||
        !ring_alloc(&c->rx_data, RING_RX2, 64, true) ||
        !ring_alloc(&c->tx_data, RING_TX0, TX_TOKENS, false))
        return false;
    ring_program(c, &c->tx_fwdl, false);
    ring_program(c, &c->tx_mcu, false);
    ring_program(c, &c->rx_event, true);
    ring_program(c, &c->rx_wa, true);
    ring_program(c, &c->rx_data, true);
    ring_program(c, &c->tx_data, false);

    /* dma_enable. */
    wr(c, PREFETCH_TX + 0 * 4, 0x01400004);     /* TX0 (data) */
    wr(c, PREFETCH_TX + 16 * 4, 0x03400004);    /* TX16 */
    wr(c, PREFETCH_TX + 17 * 4, 0x03800004);    /* TX17 */
    wr(c, PREFETCH_RX + 0 * 4, 0x00000004);     /* RX0 */
    wr(c, PREFETCH_RX + 2 * 4, 0x00400004);     /* RX2 */
    wr(c, PREFETCH_RX + 4 * 4, 0x00c00004);     /* RX4 */
    wr(c, RST_DTX_PTR, 0xFFFFFFFF);
    wr(c, PRI_DLY_INT0, 0);
    set_bits(c, GLO_CFG, 0x5020B870);
    set_bits(c, GLO_CFG, GLO_TX_EN | GLO_RX_EN);
    set_bits(c, DUMMY_CR, 1u << 1);
    kprintf("  mt7921:        DMA regs: GLO 0x%X RST 0x%X EXT0 0x%X DMASHDL 0x%X TX17base 0x%X\n",
            rd(c, GLO_CFG), rd(c, WFDMA0_RST), rd(c, GLO_CFG_EXT0), rd(c, DMASHDL_CTRL),
            rd(c, RING_TX17));
    return true;
}

/* --- MCU --- */

/* A command's sequence number (1-15; replies carry it back). */
static uint8_t next_seq(struct mt7921 *c)
{
    c->seq = (c->seq + 1) & 0xF;
    if (!c->seq)
        c->seq = 1;
    return (uint8_t)c->seq;
}

/* Fill the TXD + 64-byte command header for payload of plen bytes. */
static size_t build_command(struct mt7921 *c, uint8_t *out, uint8_t cid, uint8_t ext_cid,
                            uint8_t set_query, const void *payload, size_t plen)
{
    size_t total = 32 + 32 + plen;              /* TXD(32) + header(32) + payload */
    memset(out, 0, 64);
    uint32_t dw0 = (uint32_t)(total & 0xFFFF) | (2u << 23) | (0x20u << 25);
    uint32_t dw1 = (1u << 31) | (1u << 16);
    out[0] = (uint8_t)dw0, out[1] = (uint8_t)(dw0 >> 8);
    out[2] = (uint8_t)(dw0 >> 16), out[3] = (uint8_t)(dw0 >> 24);
    out[4] = (uint8_t)dw1, out[5] = (uint8_t)(dw1 >> 8);
    out[6] = (uint8_t)(dw1 >> 16), out[7] = (uint8_t)(dw1 >> 24);
    uint16_t len = (uint16_t)(total - 32);
    out[32] = (uint8_t)len, out[33] = (uint8_t)(len >> 8);
    out[34] = 0x00, out[35] = 0x80;             /* pq_id 0x8000 */
    out[36] = cid;
    out[37] = 0xA0;                             /* pkt_type MCU_PKT_ID */
    out[38] = set_query;
    out[39] = next_seq(c);
    out[41] = ext_cid;
    out[43] = ext_cid ? 1 : 0;
    if (plen)
        memcpy(out + 64, payload, plen);
    return total;
}

/* Put one buffer on a TX ring and ring the doorbell; false if the ring is
 * full (the chip has not taken the oldest yet). Data frames stop here: the
 * token keeps the frame's page until the chip reports it done. Waiting for
 * the chip to read each descriptor (as commands do) slept a tick or two per
 * frame -- about 19 ms -- and held whoever sent it, often the RX thread. */
static void mcu_unlock(struct mt7921 *c);

static bool tx_post(struct mt7921 *c, struct ring *r, uint32_t phys, size_t len)
{
    uint32_t next = (r->cpu + 1) % r->count;
    if (next == rd(c, r->base + 0xC))
        return false;                           /* full */
    struct desc *d = &r->desc[r->cpu];
    d->buf0 = phys;
    d->buf1 = 0;
    d->info = 0;
    d->ctrl = DESC_LAST_SEC0 | ((uint32_t)(len & DESC_LEN0_MASK) << DESC_LEN0_SHIFT);
    arch_write_barrier();
    r->cpu = next;
    wr(c, r->base + 0x8, r->cpu);
    return true;
}

/* Wait until the chip has read everything posted on the ring (commands). */
static bool tx_wait(struct mt7921 *c, struct ring *r)
{
    for (int i = 0; i < 1000; i++) {
        if (rd(c, r->base + 0xC) == r->cpu)
            return true;
        timer_sleep_ms(1);
    }
    kprintf("  mt7921:        tx 0x%X stuck: base 0x%X size %u cpu %u dma %u GLO 0x%X\n",
            r->base, rd(c, r->base + 0x0), rd(c, r->base + 0x4), rd(c, r->base + 0x8),
            rd(c, r->base + 0xC), rd(c, GLO_CFG));
    return false;
}

/* A command or firmware chunk: posted, then waited for (commands are one at
 * a time and their buffer is reused at once). */
static bool tx_submit(struct mt7921 *c, struct ring *r, uint32_t phys, size_t len)
{
    return tx_post(c, r, phys, len) && tx_wait(c, r);
}

/* Wait for an MCU event on RX0 whose seq matches; copy up to max bytes of its
 * payload into resp. Returns the event's payload length, or -1. */
/* The next descriptor the chip filled on an RX ring (the one after the last we
 * gave back): its buffer and length, or 0 if none yet. */
static uint8_t *rx_take(struct ring *r, uint32_t *len)
{
    uint32_t next = (r->cpu + 1) % r->count;
    struct desc *d = &r->desc[next];
    if (!(d->ctrl & DESC_DMA_DONE))
        return 0;
    arch_read_barrier();
    *len = (d->ctrl >> DESC_LEN0_SHIFT) & DESC_LEN0_MASK;
    if (*len > BUF_SIZE)
        *len = 0;                               /* (49l: never more than the chip was given) */
    return r->buf[next];
}

/* Hand the descriptor rx_take returned back to the chip. */
static void rx_put(struct mt7921 *c, struct ring *r)
{
    uint32_t next = (r->cpu + 1) % r->count;
    r->desc[next].ctrl = (BUF_SIZE & DESC_LEN0_MASK) << DESC_LEN0_SHIFT;
    arch_write_barrier();
    r->cpu = next;
    wr(c, r->base + 0x8, r->cpu);
}

/* Service one RX ring's next descriptor: -2 nothing ready, -1 a non-matching
 * event consumed, >=0 the matching event's payload length (copied to resp). */
static int rx_service(struct mt7921 *c, struct ring *r, int seq, uint8_t *resp, int max)
{
    uint32_t len;
    uint8_t *f = rx_take(r, &len);
    if (!f)
        return -2;
    int result = -1;
    if (len >= 36) {                                /* a 36-byte event header */
        int ev_seq = f[29];
        if (ev_seq == seq || seq < 0) {
            int plen = (int)len - 36;
            if (plen > max)
                plen = max;
            if (resp && plen > 0)
                memcpy(resp, f + 36, (size_t)plen);
            result = plen < 0 ? 0 : plen;
        }
    }
    rx_put(c, r);
    return result;
}

static int mcu_wait(struct mt7921 *c, int seq, uint8_t *resp, int max)
{
    if (c->rx_running) {                        /* the RX thread hands the reply over */
        bool came = wait_event_until(&c->reply_wq, c->reply_len >= 0, timer_ticks() + 300);
        c->reply_seq = -1;
        if (!came) {
            kprintf("  mt7921:        %s: no reply to seq %d\n", c->where, seq);
            return -1;
        }
        int n = c->reply_len < max ? c->reply_len : max;
        if (resp && n > 0)
            memcpy(resp, c->reply, (size_t)n);
        return n;
    }
    for (int tries = 0; tries < 3000; tries++) {
        int a = rx_service(c, &c->rx_event, seq, resp, max);
        if (a >= 0)
            return a;
        int b = rx_service(c, &c->rx_wa, seq, resp, max);
        if (b >= 0)
            return b;
        if (a == -2 && b == -2)                     /* both rings idle: wait */
            timer_sleep_ms(1);
    }
    kprintf("  mt7921:        mcu_wait: no event for seq %d (rx0 dma %u cpu %u, rx4 dma %u cpu %u)\n",
            seq, rd(c, c->rx_event.base + 0xC), c->rx_event.cpu,
            rd(c, c->rx_wa.base + 0xC), c->rx_wa.cpu);
    return -1;
}

/* Send a command and, if wait, get its response. */
/* Send the command built in c->cmd (total bytes, sequence c->seq) and, if
 * wait, get its reply. Once the RX thread runs, the reply it should hand over
 * is named before the command goes out, so a quick one is not missed. The
 * caller holds mcu_lock. */
static int command_out(struct mt7921 *c, size_t total, bool wait, uint8_t *resp, int max)
{
    int seq = c->seq;
    if (wait && c->rx_running) {
        c->reply_len = -1;
        arch_write_barrier();
        c->reply_seq = seq;
    }
    if (!tx_submit(c, &c->tx_mcu, c->cmd_phys, total)) {
        c->reply_seq = -1;
        return -1;
    }
    if (!wait)
        return 0;
    int r = mcu_wait(c, seq, resp, max);
    if (c->rx_running)                          /* (the watch restarts a chip whose */
        c->mcu_timeouts = r < 0 ? c->mcu_timeouts + 1 : 0;   /* firmware went quiet) */
    return r;
}

static int mcu_command(struct mt7921 *c, uint8_t cid, uint8_t ext_cid, uint8_t set_query,
                       const void *payload, size_t plen, bool wait, uint8_t *resp, int max)
{
    if (plen > CMD_ROOM - 64)
        return -1;                              /* (49l: larger than the command buffer) */
    mutex_lock(&c->mcu_lock);
    size_t total = build_command(c, c->cmd, cid, ext_cid, set_query, payload, plen);
    int r = command_out(c, total, wait, resp, max);
    mcu_unlock(c);
    return r;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t le32r(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void le16w(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
}

static void le32w(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

/* Download one firmware region: tell the chip (init_download), then stream
 * the data in <=4 KiB chunks. */
static bool download_region(struct mt7921 *c, uint8_t init_cid, uint32_t addr, uint32_t len,
                            uint32_t mode, const char *file, uint64_t file_off)
{
    uint8_t req[12];
    le32w(req, addr);
    le32w(req + 4, len);
    le32w(req + 8, mode);
    if (mcu_command(c, init_cid, 0, 1, req, sizeof(req), true, 0, 0) < 0)
        return false;
    for (uint32_t off = 0; off < len; off += DL_4K) {
        uint32_t chunk = len - off < DL_4K ? len - off : DL_4K;
        if (read_file(file, file_off + off, c->cmd, chunk, 0) != (int64_t)chunk)  /* into DMA buf */
            return false;
        if (!tx_submit(c, &c->tx_fwdl, c->cmd_phys, chunk))
            return false;
    }
    return true;
}

static bool download_patch(struct mt7921 *c)
{
    uint8_t head[96];
    if (read_file(PATCH_FILE, 0, head, sizeof(head), 0) != (int64_t)sizeof(head))
        return false;
    uint32_t n_region = be32(head + 44);
    uint8_t op = 1;                             /* PATCH_SEM_GET */
    uint8_t res[8];
    int r = mcu_command(c, CMD_PATCH_SEM, 0, 1, &op, 1, true, res, sizeof(res));
    kprintf("  mt7921:        patch sem: n_region %u, mcu result %d (res[0]=%u)\n",
            n_region, r, res[0]);
    if (r < 0)
        return false;
    uint8_t sem = res[0];                       /* result byte at payload[0] */
    if (sem == 1) {                             /* already downloaded */
        kprintf("  mt7921:        patch already loaded\n");
        return true;
    }
    for (uint32_t i = 0; i < n_region; i++) {
        uint8_t sec[64];
        read_file(PATCH_FILE, 96 + (uint64_t)i * 64, sec, sizeof(sec), 0);
        uint32_t data_off = be32(sec + 4);
        uint32_t addr = be32(sec + 12), len = be32(sec + 16);
        if (!download_region(c, CMD_PATCH_START, addr, len, 0x80000000u, PATCH_FILE, data_off))
            return false;
    }
    uint8_t fin[4] = { 0, 0, 0, 0 };
    if (mcu_command(c, CMD_PATCH_FINISH, 0, 1, fin, sizeof(fin), true, 0, 0) < 0)
        return false;
    op = 0;                                     /* PATCH_SEM_RELEASE */
    mcu_command(c, CMD_PATCH_SEM, 0, 1, &op, 1, true, res, sizeof(res));
    return true;
}

static bool download_ram(struct mt7921 *c)
{
    uint64_t size = 0;
    uint8_t trailer[36];
    if (read_file(RAM_FILE, 0, trailer, 4, &size) < 0 || size < 36)
        return false;
    read_file(RAM_FILE, size - 36, trailer, 36, 0);
    uint8_t n_region = trailer[2];
    uint64_t region_table = size - 36 - (uint64_t)n_region * 40;
    uint32_t data_off = 0, override = 0;
    for (uint8_t i = 0; i < n_region; i++) {
        uint8_t reg[40];
        read_file(RAM_FILE, region_table + (uint64_t)i * 40, reg, 40, 0);
        /* the RAM trailer's region fields are little-endian (unlike the
         * patch's big-endian section map). */
        uint32_t addr = le32r(reg + 16);
        uint32_t len = le32r(reg + 20);
        uint8_t feature = reg[24], rtype = reg[25];
        if (feature & (1u << 6)) {              /* NON_DL: not firmware code */
            if (rtype == 2 && !c->clc && len && len < 1u << 20) {
                c->clc = kmalloc(len);          /* the country rules (CLC); kept for SET_CLC */
                if (c->clc && read_file(RAM_FILE, data_off, c->clc, len, 0) == (int64_t)len)
                    c->clc_len = len;
                else
                    c->clc = 0;
            }
            data_off += len;
            continue;
        }
        if (feature & (1u << 5))                /* OVERRIDE_ADDR */
            override = addr;
        if (!download_region(c, CMD_TARGET_ADDRESS_LEN, addr, len, 0x80000000u, RAM_FILE, data_off))
            return false;
        data_off += len;
    }
    uint8_t start[8];
    le32w(start, 1);                            /* option: FW_START_OVERRIDE */
    le32w(start + 4, override);
    if (mcu_command(c, CMD_FW_START, 0, 1, start, sizeof(start), true, 0, 0) < 0)
        return false;
    return poll(c, CONN_ON_MISC, FW_N9_RDY, FW_N9_RDY, 1500);
}

/* Ask the running firmware for its capabilities and keep the MAC address.
 * The reply is a 2-byte element count, 2 reserved bytes, then that many
 * little-endian { type, len, data } records; type 7 holds the 6-byte MAC. */
static void read_nic_capability(struct mt7921 *c)
{
    int n = mcu_command(c, CE_GET_NIC_CAPAB, 0, 1 /* MCU_Q_SET */, 0, 0, true, c->cmd, 2048);
    if (n < 4) {
        kprintf("  mt7921:        %s: GET_NIC_CAPAB failed (%d)\n", c->where, n);
        return;
    }
    uint16_t n_elem = (uint16_t)(c->cmd[0] | c->cmd[1] << 8);
    int off = 4;
    for (uint16_t i = 0; i < n_elem && off + 8 <= n; i++) {
        uint32_t type = le32r(c->cmd + off);
        uint32_t len = le32r(c->cmd + off + 4);
        off += 8;
        if (len > (uint32_t)(n - off))           /* (49l: a huge len made off + len negative) */
            break;
        if (type == NIC_CAP_MAC_ADDR && len >= 6)
            memcpy(c->mac, c->cmd + off, 6);
        off += (int)len;
    }
    kprintf("  mt7921:        %s: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", c->where,
            c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5]);
}

/* Tell the firmware to load the chip's EEPROM straight from its efuse
 * (EFUSE_BUFFER_MODE: mode 0 = efuse, format 1 = whole, no buffer). */
static bool set_eeprom(struct mt7921 *c)
{
    uint8_t req[4] = { 0, 1, 0, 0 };
    return mcu_command(c, EXT_CID, EXT_EFUSE_BUFFER_MODE, 1, req, sizeof(req), true, 0, 0) >= 0;
}

/* The MAC initialisation Linux does after the firmware is up: receive length,
 * de-aggregation, clear the station table's counters, and the band-0 timing,
 * MIB and signal-reporting registers. All through the L1 window. */
static void mac_init(struct mt7921 *c)
{
    rmw(c, MDP_DCR1, 0xFFF8u, 1536u << 3);              /* MAX_RX_LEN = 1536 */
    rmw(c, MDP_DCR0, 1u << 19, 1u << 15);               /* de-agg on; RX_HDR_TRANS off (the
                                                         * firmware leaves it on): the
                                                         * wireless layer wants received data
                                                         * frames as 802.11, not Ethernet */
    for (int i = 0; i < 20; i++) {                      /* MT792x_WTBL_SIZE entries */
        wr(c, WTBL_UPDATE, ((uint32_t)i & 0x3FFu) | (1u << 12));
        for (int t = 0; t < 100 && (rd(c, WTBL_UPDATE) & (1u << 31)); t++)
            timer_sleep_ms(1);
    }
    rmw(c, TMAC_CTCR0, 0x3Fu, 0x3Fu | (1u << 17) | (1u << 18));
    set_bits(c, RMAC_MIB_TIME0, 1u << 30);
    set_bits(c, RMAC_MIB_AIR0, 1u << 30);
    set_bits(c, MIB_SCR1, (1u << 8) | (1u << 9));
    rmw(c, DMA_DCR0, 0xFFF8u | (1u << 23), 1536u << 3); /* MAX_RX_LEN, RXD_G5 off */
    rmw(c, WTBLOFF_RSCR, (3u << 30) | (3u << 24), 3u << 24);    /* instant RCPI reporting */
}

/* A "unified" command: the TXD, a 16-byte header (cid, the ack-and-set
 * option), then the payload. Returns the firmware's status (0: done), or -1
 * without a reply. */
/* A unified command; wait: for its reply (its status), else only posted (0).
 * Never wait on the RX thread: only it hands replies over (block_ack runs
 * there, from the access point's ADDBA response). */
static int uni_cmd(struct mt7921 *c, uint16_t cid, const void *payload, size_t plen, bool wait)
{
    if (plen > CMD_ROOM - 48)
        return -1;                              /* (49l: larger than the command buffer) */
    mutex_lock(&c->mcu_lock);
    uint8_t *out = c->cmd;
    size_t total = 32 + 16 + plen;
    memset(out, 0, 48);
    le32w(out, (uint32_t)(total & 0xFFFF) | (2u << 23) | (0x20u << 25));
    le32w(out + 4, (1u << 31) | (1u << 16));
    out[32] = (uint8_t)(total - 32), out[33] = (uint8_t)((total - 32) >> 8);
    out[34] = (uint8_t)cid, out[35] = (uint8_t)(cid >> 8);
    out[37] = 0xA0;                             /* pkt_type MCU_PKT_ID */
    out[39] = next_seq(c);
    out[43] = 0x07;                             /* option: ack, unified, set */
    memcpy(out + 48, payload, plen);
    uint8_t r[8];                               /* { cid, 3 reserved, status } */
    int n = command_out(c, total, wait, r, sizeof(r));
    mcu_unlock(c);
    if (!wait)
        return n < 0 ? -1 : 0;
    return n < 8 ? -1 : (int)le32r(r + 4);
}

static int uni_set(struct mt7921 *c, uint16_t cid, const void *payload, size_t plen)
{
    return uni_cmd(c, cid, payload, plen, true);
}

/* Put the receiver on a channel, 20 MHz (SET_RX_PATH). */
/* Tune the radio to a channel, 20 MHz. ext is SET_RX_PATH (receive only, used
 * before a connection) or CHANNEL_SWITCH (transmit and receive, for joining).
 * The payload is mt7921's set_chan_info struct either way. */
static bool tune(struct mt7921 *c, int channel, uint8_t ext)
{
    uint8_t ch[76] = { 0 };
    ch[0] = (uint8_t)channel;                   /* control channel */
    ch[1] = (uint8_t)(c->bw ? c->center : channel);   /* centre channel */
    ch[2] = (uint8_t)c->bw;                     /* width: 20, 40 or 80 MHz */
    ch[3] = 2;                                  /* streams: the chip's two chains, */
    ch[4] = ext == EXT_SET_RX_PATH ? 3 : 2;     /* their mask (a switch: their number) */
    if (ext != EXT_SET_RX_PATH && channel >= 52 && channel <= 144)
        ch[5] = 5;                              /* switch reason: DFS (radar channels) */
    ch[10] = channel <= 14 ? 0 : 1;             /* 2.4 or 5 GHz */
    return mcu_command(c, EXT_CID, ext, 1, ch, sizeof(ch), true, 0, 0) >= 0;
}

static bool rx_path(struct mt7921 *c, int channel)
{
    return tune(c, channel, EXT_SET_RX_PATH);
}

/* Keep the radio at full power: otherwise, when it is neither scanning nor
 * joined, the firmware lets it doze and the receiver is off, so we never hear
 * the access point's reply to our authentication (CHIP_CONFIG "KeepFullPwr"). */
static bool keep_full_power_set(struct mt7921 *c, bool on)
{
    uint8_t cfg[328];
    memset(cfg, 0, sizeof(cfg));
    memcpy(cfg + 8, on ? "KeepFullPwr 1" : "KeepFullPwr 0", 13);   /* after the header */
    return mcu_command(c, CE_CHIP_CONFIG, 0, 1, cfg, sizeof(cfg), false, 0, 0) >= 0;
}

static bool keep_full_power(struct mt7921 *c)
{
    return keep_full_power_set(c, true);
}

/* Switch the MAC on and put the receiver on channel 1 to start with, as mt76's
 * __mt7921_start does (MAC_INIT_CTRL, then SET_RX_PATH). */
static bool mac_on(struct mt7921 *c)
{
    uint8_t mac[4] = { 1, 0, 0, 0 };            /* enable, band 0 */
    if (mcu_command(c, EXT_CID, EXT_MAC_INIT_CTRL, 1, mac, sizeof(mac), true, 0, 0) < 0)
        return false;
    keep_full_power(c);
    return rx_path(c, 1);
}

/* The firmware's view of us (mt76's uni_add_dev): a device in own-MAC slot 0
 * with our address, then its basic BSS as an infrastructure station that has
 * not joined yet. A scan needs both. */
static void add_station(struct mt7921 *c)
{
    uint8_t dev[16] = { 0 };                    /* omac 0, band 0 */
    dev[6] = 12;                                /* tag 0 (DEV_INFO_ACTIVE), length 12 */
    dev[8] = 1;                                 /* active */
    memcpy(dev + 10, c->mac, 6);
    int a = uni_set(c, UNI_DEV_INFO_UPDATE, dev, sizeof(dev));
    uint8_t bss[36] = { 0 };                    /* bss 0; omac, band, wmm 0 */
    bss[6] = 32;                                /* tag 0 (BSS_INFO_BASIC), length 32 */
    bss[8] = 1;                                 /* active */
    le32w(bss + 12, 0x00010001);                /* CONNECTION_INFRA_STA */
    bss[16] = 1;                                /* conn_state */
    int b = uni_set(c, UNI_BSS_INFO_UPDATE, bss, sizeof(bss));
    kprintf("  mt7921:        %s: station interface: device %d, BSS %d\n", c->where, a, b);
}

/* Tell the firmware the country, so it -- not us -- chooses the channels and
 * power it may use. The firmware carries MediaTek's certified rules for every
 * country in its CLC region (kept at download); here we only find the two
 * records (channels, power) for the given two-letter code and hand each to the
 * firmware with SET_CLC. We read and apply no limits ourselves. Returns how
 * many records were sent. */
static int set_country(struct mt7921 *c, const char *cc)
{
    if (!c->clc || cc[0] < 'A' || cc[1] < 'A')
        return 0;
    int sent = 0;
    uint32_t o = 0;
    while (o + 16 <= c->clc_len) {              /* struct mt7921_clc records */
        uint32_t rlen = le32r(c->clc + o);
        uint8_t idx = c->clc[o + 4];
        if (rlen < 16 || rlen > c->clc_len - o)
            break;                              /* (49l: o + rlen could wrap) */
        for (uint32_t d = o + 16; d + 6 <= o + rlen; ) {    /* struct mt7921_clc_rule */
            uint16_t len = (uint16_t)(c->clc[d + 4] | c->clc[d + 5] << 8);
            if (d + 6 + len > o + rlen)
                break;
            if (c->clc[d] == (uint8_t)cc[0] && c->clc[d + 1] == (uint8_t)cc[1]) {
                uint8_t *req = kzalloc((size_t)76 + len);
                if (req) {
                    req[0] = 1;                 /* ver */
                    uint16_t tot = (uint16_t)(76 + len);
                    req[2] = (uint8_t)tot, req[3] = (uint8_t)(tot >> 8);
                    req[4] = idx;               /* which record (channels or power) */
                    req[5] = 1;                 /* environment: indoor */
                    req[8] = (uint8_t)cc[0], req[9] = (uint8_t)cc[1];
                    req[10] = c->clc[d + 2], req[11] = c->clc[d + 3];   /* the rule's type */
                    req[13] = 0xFF;             /* mtcl_conf: none (no ACPI tables) */
                    memcpy(req + 76, c->clc + d + 6, len);
                    if (mcu_command(c, CE_SET_CLC, 0, 1, req, (size_t)76 + len, false, 0, 0) >= 0)
                        sent++;
                    kfree(req);
                }
            }
            d += 6 + len;
        }
        o += rlen;
    }
    if (sent) {
        c->country[0] = cc[0], c->country[1] = cc[1], c->country[2] = 0;
        kprintf("  mt7921:        %s: country %c%c set (%d rule record(s))\n", c->where,
                cc[0], cc[1], sent);
    }
    return sent;
}

/* Strip a received frame's descriptor: the 802.11 frame, the channel it came
 * on and its signal (dBm, 0 if the descriptor has none). False for frames to
 * drop (bad FCS, the other band, translated to Ethernet, too short). */
/* What the chip said about a frame's protection: opened (decrypted with a
 * key it holds, CCMP header and MIC removed), its packet number and key
 * index, or a wrong MIC. */
struct rx_sec {
    bool opened, bad;
    uint64_t pn;
    int key_index;
};

static bool rx_parse(const uint8_t *b, uint32_t len, const uint8_t **frame, size_t *flen,
                     int *channel, int *signal, struct rx_sec *sec)
{
    if (len < 24)
        return false;
    uint32_t rxd0 = le32r(b), rxd1 = le32r(b + 4), rxd2 = le32r(b + 8), rxd3 = le32r(b + 12);
    if ((rxd0 & 0xFFFF) && (rxd0 & 0xFFFF) < len)
        len = rxd0 & 0xFFFF;                    /* the descriptor's own byte count */
    if ((rxd1 & (RXD1_BAND_IDX | RXD1_FCS_ERR)) || (rxd2 & RXD2_HDR_TRANS))
        return false;
    uint32_t off = 24, rcpi_word = 0;
    memset(sec, 0, sizeof(*sec));
    sec->opened = RXD1_SEC_MODE(rxd1) && !(rxd1 & (RXD1_CM | RXD1_CLM));
    sec->bad = rxd1 & RXD1_ICV_ERR;
    sec->key_index = (int)RXD1_KEY_ID(rxd1);
    if (rxd1 & RXD1_GROUP_4)
        off += 16;
    if (rxd1 & RXD1_GROUP_1) {                  /* its first 6 bytes: the packet number */
        if (off + 6 <= len)
            for (int i = 5; i >= 0; i--)
                sec->pn = sec->pn << 8 | b[off + (uint32_t)i];
        off += 16;
    } else
        sec->opened = false;                    /* (no packet number to check) */
    if (rxd1 & RXD1_GROUP_2)
        off += 8;
    if (rxd1 & RXD1_GROUP_3) {
        if (off + 8 <= len)
            rcpi_word = le32r(b + off + 4);
        off += 8;
        if (rxd1 & RXD1_GROUP_5) {
            if (off + 28 <= len)
                rcpi_word = le32r(b + off + 24);
            off += 72;
        }
    }
    off += 2 * ((rxd2 >> 14) & 3);              /* HDR_OFFSET: padding before the header */
    if (off + 24 > len)
        return false;
    *frame = b + off;
    *flen = len - off;
    *channel = (int)((rxd3 >> 8) & 0xFF);
    int rcpi = (int)(rcpi_word & 0xFF);         /* chain 0 */
    *signal = rcpi ? (rcpi - 220) / 2 : 0;
    return true;
}

/* One received buffer, on the RX thread: a frame for the wireless layer, or
 * an MCU event (a scan's end, or the reply a command waits for). */
/* A scan's end: the layer told. (36f-3) A scan that ends at once (under
 * 100 ms: a real one takes seconds) having heard nothing counts; five in
 * a row and the watch restarts the chip: on the laptop, after the access
 * point's settings changed, its firmware failed to authenticate and then
 * ended every scan so for half an hour (1,129 lines, which also pushed the
 * loss itself out of the log: lines only for the first three now). */
static void scan_finished(struct mt7921 *c)
{
    if (c->scanning) {
        uint64_t ms = (timer_ticks() - c->scan_started) * 10;
        if (!c->scan_frames && ms < 100)
            c->empty_scans++, c->empty_scans_all++;
        else
            c->empty_scans = 0;
        if (c->scan_frames || c->empty_scans <= 3)
            kprintf("  mt7921:        %s: scan done: %d frames in %lu ms%s\n", c->where,
                    c->scan_frames, ms, c->empty_scans == 3 ? " (more such: counted, not shown)"
                                                            : "");
    }
    c->scanning = false;
    wlan_scan_done(c->dev);
}

/* (65e) One TXS record (8 words, mt76's mt7921_mac_add_txs): the rate the
 * frame went at (TXS0 bits 13:0: mode 9:6 -- 0 CCK, 1 OFDM, 2 HT, 3 HT
 * greenfield, 4 VHT --, streams 12:10, MCS 5:0), in an A-MPDU (bit 25),
 * acknowledged (bits 18:16 errors). */
#define TXS_EVERY 64
static void txs_record(struct mt7921 *c, const uint8_t *w)
{
    uint32_t txs0 = le32r(w);
    if (!(txs0 & (1u << 15)))
        return;                                 /* not one asked for by the host */
    c->txs_count++;
    if (txs0 & (1u << 25))
        c->txs_ampdu++;
    if (txs0 & (7u << 16))
        c->txs_failed++;
    c->txs_bw[(txs0 >> 29) & 3]++;
    uint32_t rate = txs0 & 0x3FFF, mode = (rate >> 6) & 0xF, idx = rate & 0x3F;
    uint32_t nss = ((rate >> 10) & 7) + 1;
    if (mode < 5)
        c->txs_mode[mode]++;
    if (mode == 1 && idx < 8)
        c->txs_ofdm[idx]++;
    else if ((mode == 2 || mode == 3) && idx < 16)
        c->txs_ht[idx]++;
    else if (mode == 4 && (idx & 0xF) < 10 && nss <= 2)
        c->txs_vht[nss - 1][idx & 0xF]++;
}

static void rx_dispatch(struct mt7921 *c, const uint8_t *b, uint32_t len)
{
    if (len < 8)
        return;
    uint32_t rxd0 = le32r(b);
    int type = (int)RXD0_PKT_TYPE(rxd0);
    if (type == PKT_TYPE_TXS) {                 /* (65e) records from word 2 on, 8 words each */
        for (uint32_t at = 8; at + 32 <= len; at += 32)
            txs_record(c, b + at);
        return;
    }
    /* struct mt76_connac_tx_free (8 bytes), then a u32 per station and token:
     * one frame's report is only 16 bytes (dropped by a 24-byte minimum, every
     * token waited for its 2 s timeout) */
    if (type == PKT_TYPE_TXRX_NOTIFY) {
        int count = (int)((rxd0 >> 16) & 0x3FF);
        for (int i = 0; i < count && 8 + 4 * (uint32_t)i + 4 <= len; i++) {
            uint32_t info = le32r(b + 8 + 4 * i);
            if (info & (1u << 31)) {            /* a station marker, not a token */
                count++;
                continue;
            }
            uint32_t id = (info >> 16) & 0x7FFF;
            if (id < TX_TOKENS && c->tok_busy[id]) {
                uint64_t took = arch_counter() - c->tok_cycles[id];
                c->tx_cycles_sum += took;
                if (took > c->tx_cycles_max)
                    c->tx_cycles_max = took;
                static const uint32_t spans_ms[6] = { 1, 2, 5, 10, 20, 50 };
                uint64_t ms = arch_counter_hz() ? took * 1000 / arch_counter_hz() : 0;
                int span = 0;
                while (span < 6 && ms >= spans_ms[span])
                    span++;
                c->tx_spans[span]++;
            }
            if (id < TX_TOKENS)
                c->tok_busy[id] = false;
            c->tx_done++;
            wait_queue_wake_all(&c->tok_wq);
            if ((info >> 13) & 3)               /* status: not acknowledged */
                c->tx_failed++;
            uint32_t tries = info & 0x1FFF;     /* (65e) MT_TX_FREE_COUNT: its attempts */
            c->tx_tries[tries <= 1 ? 0 : tries == 2 ? 1 : tries <= 4 ? 2 : tries <= 8 ? 3 : 4]++;
            if (c->tx_done <= 8)                /* the first few, to see the path work */
                kprintf("  mt7921:        %s: TX token %u done, status %u, tries %u\n",
                        c->where, id, (info >> 13) & 3, info & 0x1FFF);
        }
        /* (65e-27) Tokens came free: the frames waiting go. Only if the lock
         * is free now: a command holding it may be waiting for its reply,
         * which this thread brings (its mcu_unlock drains then). */
        if (c->pend_count && mutex_try_lock(&c->mcu_lock))
            mcu_unlock(c);
        return;
    }
    if (len < 24)
        return;
    if (type == PKT_TYPE_RX_EVENT && RXD0_PKT_FLAG(rxd0) != 1) {
        if (len < 36)
            return;
        if (b[28] == EVENT_SCAN_DONE) {
            scan_finished(c);
        } else if (b[28] == EVENT_BEACON_LOSS) {
            /* The firmware watches the beacons (the beacon filter): they
             * stopped. Only the wireless layer is told here; the firmware's
             * BSS is reset when wifid joins again (not from this thread,
             * which must stay free to take the commands' replies). */
            char why[48];
            ksnprintf(why, sizeof(why), "its beacons stopped (reason %u)",
                      (uint32_t)(len > 37 ? b[37] : 0));
            wlan_link_lost(c->dev, why);
        } else if (c->reply_seq >= 0 && b[29] == c->reply_seq && c->reply_len < 0) {
            int n = (int)len - 36 < BUF_SIZE ? (int)len - 36 : BUF_SIZE;
            memcpy(c->reply, b + 36, (size_t)n);
            arch_write_barrier();
            c->reply_len = n;
            wait_queue_wake_all(&c->reply_wq);
        }
        return;
    }
    if (type != PKT_TYPE_NORMAL && type != PKT_TYPE_NORMAL_MCU && type != PKT_TYPE_RX_EVENT)
        return;
    const uint8_t *f;
    size_t n;
    int ch, sig;
    struct rx_sec sec;
    if (rx_parse(b, len, &f, &n, &ch, &sig, &sec)) {
        c->scan_frames++;
        if (sec.opened || sec.bad)
            wlan_receive_decrypted(c->dev, f, n, ch, sig, sec.pn, sec.key_index, sec.bad);
        else
            wlan_receive(c->dev, f, n, ch, sig);
    }
}

static bool ring_filled(const struct ring *r)
{
    return r->desc[(r->cpu + 1) % r->count].ctrl & DESC_DMA_DONE;
}

static bool rx_pending(struct mt7921 *c)
{
    return ring_filled(&c->rx_data) || ring_filled(&c->rx_event) || ring_filled(&c->rx_wa);
}

/* The chip's MSI (interrupt context): mask its interrupts, clear what it
 * raised, wake the RX thread, which unmasks them once the rings are empty
 * (mt76's irq handler and poll-complete). */
static void interrupt(void *context)
{
    struct mt7921 *c = context;
    wr(c, HOST_INT_ENA, 0);
    if (c->recovering)
        return;
    uint32_t sta = rd(c, HOST_INT_STA);
    wr(c, HOST_INT_STA, sta);
    if (sta & MCU_SW_INT)                       /* (a wake-RX request: the thread looks) */
        wr(c, MCU_CMD, rd(c, MCU_CMD));
    c->interrupts++;
    c->kicked = true;
    wait_queue_wake_all(&c->rx_wq);
}

/* The timer tick (interrupt context): look at the rings, wake the thread.
 * With MSI only a safety net: what it finds first, an interrupt missed. */
/* (65e) WF_MIB of band 0 (0x820ed000) sits at BAR offset 0x24800 (mt76's
 * fixed_map); the offsets are mt792x_regs.h's. */
#define MIB(ofs)            (0x24800 + (ofs))
#define MIB_SDR9            MIB(0x02c)          /* the channel busy, us (23:0) */
#define MIB_SDR36           MIB(0x054)          /* sending, us (23:0) */
#define MIB_SDR37           MIB(0x058)          /* receiving, us (23:0) */
#define MIB_ARNG(n)         MIB(0x0b0 + 4 * (n))  /* the A-MPDU length ranges, 8 bits each */
#define MIB_AGG_CNT(n)      MIB(0x7dc + 4 * (n))  /* A-MPDUs per range 0-7, 16 bits each */
#define MIB_AGG_CNT2(n)     MIB(0x7ec + 4 * (n))  /* ranges 8-15 */

/* (65e) Add the MIB counters up (interrupt context: register reads only),
 * as mt792x_mac_update_mib_stats. */
static void mib_collect(struct mt7921 *c)
{
    if (!c->mib_on) {
        for (int i = 0; i < 4; i++)                 /* (cleared; MIB_SCR1's durations are on
                                                     * since the start) */
            rd(c, MIB_AGG_CNT(i)), rd(c, MIB_AGG_CNT2(i));
        rd(c, MIB_SDR9), rd(c, MIB_SDR36), rd(c, MIB_SDR37);
        c->mib_on = true;
        return;
    }
    for (int i = 0; i < 4; i++) {
        uint32_t v = rd(c, MIB_AGG_CNT(i)), v2 = rd(c, MIB_AGG_CNT2(i));
        c->mib_aggr[2 * i] += v & 0xFFFF;
        c->mib_aggr[2 * i + 1] += v >> 16;
        c->mib_aggr[8 + 2 * i] += v2 & 0xFFFF;
        c->mib_aggr[8 + 2 * i + 1] += v2 >> 16;
    }
    c->mib_busy_us += rd(c, MIB_SDR9) & 0xFFFFFF;
    c->mib_tx_us += rd(c, MIB_SDR36) & 0xFFFFFF;
    c->mib_rx_us += rd(c, MIB_SDR37) & 0xFFFFFF;
}

static void tick(void *context)
{
    struct mt7921 *c = context;
    if (c->joined && c->fw_ready && !c->recovering && ++c->mib_ticks >= TIMER_HZ) {
        c->mib_ticks = 0;
        mib_collect(c);
    }
    if (rx_pending(c)) {
        if (c->msi && c->rx_asleep)
            c->found_by_tick++;
        wait_queue_wake_all(&c->rx_wq);
    }
}

/* The RX thread: empties the three RX rings whenever the interrupt or the
 * tick finds work. Every interrupt ends with the chip's interrupts unmasked
 * again, also one that came before its frame was done (the firmware's
 * wake-RX comes ahead of the data, as mt76 knows: it polls the data ring
 * then): waiting only for a filled ring left them masked, and every frame
 * after waited for the tick (pings of exactly 10 ms on the laptop). */
static void receiver(void *context)
{
    struct mt7921 *c = context;
    struct ring *rings[3] = { &c->rx_event, &c->rx_wa, &c->rx_data };
    for (;;) {
        c->rx_asleep = true;
        wait_event(&c->rx_wq, (rx_pending(c) || c->kicked) && !c->recovering);
        c->rx_asleep = false;
        c->kicked = false;
        if (!rx_pending(c))
            c->early++;
        net_batch_begin();                      /* (65e) one ACK per connection for a burst */
        for (int i = 0; i < 3; i++) {
            uint32_t len;
            uint8_t *b;
            for (int k = 0; k < rings[i]->count && (b = rx_take(rings[i], &len)); k++) {
                rx_dispatch(c, b, len);
                rx_put(c, rings[i]);
            }
        }
        net_batch_end();
        if (c->msi)
            wr(c, HOST_INT_ENA, RX_INTS | MCU_SW_INT);  /* (a frame that came meanwhile raises
                                                 * the interrupt again once unmasked) */
    }
}

/* The channels the card scans: 2.4 GHz 1-13 and 5 GHz 36-165, the radar (DFS)
 * channels 52-144 included -- a scan only listens there. */
static const uint8_t scan_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
    144, 149, 153, 157, 161, 165,
};
#define SCAN_DWELL_MS 110                       /* a little more than a beacon interval */

/* Ask the firmware for a passive scan of those channels: it only listens for
 * beacons and transmits nothing (mt76's hw_scan with no SSIDs, so no probes).
 * The first 32 channels go in channels[], the rest in ext_channels[]. */
static bool start_scan(struct mt7921 *c, const uint8_t *ssid, size_t length)
{
    uint8_t *req = kzalloc(SCAN_REQ_SIZE);
    if (!req)
        return false;
    size_t n = sizeof(scan_channels);
    c->scan_seq = (c->scan_seq + 1) & 0x7F;
    req[0] = (uint8_t)c->scan_seq;              /* seq_num (band 0) */
    req[2] = 0;                                 /* scan_type: passive */
    req[3] = 1;                                 /* ssid_type: wildcard */
    if (ssid && length && length <= 32) {       /* asking for one network by name: */
        req[2] = 1;                             /* active (the firmware keeps radar */
        req[3] = 1u << 2;                       /* channels passive), that name only */
        req[1185] = 1;                          /* (ssid_type_ext) */
        req[4] = 1;                             /* ssids_num */
        req[5] = 2;                             /* probe_req_num: two per channel */
        le32w(req + 8, (uint32_t)length);       /* ssids[0]: its length, its name */
        memcpy(req + 12, ssid, length);
    }
    req[6] = 1u << 5;                           /* scan_func: split scan (connac2) */
    req[7] = 1;                                 /* version: the fields after ies[] too */
    req[154] = SCAN_DWELL_MS;                   /* channel_dwell_time */
    uint16_t timeout = (uint16_t)(n * SCAN_DWELL_MS);
    req[156] = (uint8_t)timeout, req[157] = (uint8_t)(timeout >> 8);
    req[158] = 4;                               /* channel_type: the lists below */
    req[159] = (uint8_t)(n < 32 ? n : 32);
    req[826] = (uint8_t)(n > 32 ? n - 32 : 0);  /* ext_channels_num */
    req[828] = SCAN_DWELL_MS;                   /* channel_min_dwell_time */
    for (size_t i = 0; i < n; i++) {
        uint8_t *slot = req + (i < 32 ? 160 + 2 * i : 830 + 2 * (i - 32));
        slot[0] = scan_channels[i] <= 14 ? 1 : 2;           /* band */
        slot[1] = scan_channels[i];
    }
    memset(req + 1110, 0xFF, 6);                /* bssid: any */
    bool ok = mcu_command(c, CE_START_HW_SCAN, 0, 1, req, SCAN_REQ_SIZE, false, 0, 0) >= 0;
    kfree(req);
    return ok;
}

/* --- the wireless layer's callbacks --- */

static bool country_cb(struct net_card *d, const char *cc)
{
    return set_country(d->priv, cc) > 0;
}

static bool scan(struct net_card *d, const uint8_t *ssid, size_t length)
{
    struct mt7921 *c = d->priv;
    if (c->scanning && timer_ticks() - c->scan_started < 1000)
        return true;                            /* one runs: its beacons come too */
    c->scan_started = timer_ticks();
    c->scan_frames = 0;
    c->scanning = true;
    if (!start_scan(c, ssid, length)) {
        c->scanning = false;
        return false;
    }
    return true;
}

/* Ask the firmware to stay on a channel for ms milliseconds (ROC_REQ_JOIN):
 * otherwise, between scans, the receiver is off and we never hear the access
 * point's reply to our authentication. */
static bool start_roc(struct mt7921 *c, int channel, uint32_t ms)
{
    uint8_t p[28];
    memset(p, 0, sizeof(p));                    /* p[0..3]: the 4-byte header */
    le16w(p + 4, 0);                            /* tag UNI_ROC_ACQUIRE */
    le16w(p + 6, 24);                           /* len (the TLV) */
    p[9] = 1;                                   /* tokenid */
    p[10] = (uint8_t)channel;                   /* control channel */
    p[12] = channel <= 14 ? 1 : 2;              /* band */
    p[13] = 0;                                  /* bw: 20 MHz */
    p[14] = (uint8_t)channel;                   /* centre channel */
    p[17] = (uint8_t)channel;                   /* centre channel from AP */
    p[19] = 0;                                  /* reqtype: ROC_REQ_JOIN */
    le32w(p + 20, ms);                          /* how long to stay */
    p[24] = 0xFF;                               /* band: auto */
    return uni_set(c, UNI_ROC, p, sizeof(p)) >= 0;
}

/* wifid tunes the card to the access point's channel before joining: switch
 * the radio there (CHANNEL_SWITCH, so it can transmit) and hold it on the
 * channel long enough for the whole handshake. */
/* The joined BSS's channel context (UNI BSS_INFO RLM, mt76's
 * uni_set_chctx): the firmware's notion of where this BSS lives. Without it
 * the radio was held on the channel only by the join's remain-on-channel,
 * and sending stalled again and again (pings 4-77 ms, a frame done after
 * 41 ms). Sent at the join and when the access point moves (a channel
 * switch it announced). */
static int bss_channel(struct mt7921 *c)
{
    uint8_t rlm[20] = { 0 };                    /* bss 0; RLM (tag 2, 16 bytes) */
    rlm[4] = 2, rlm[6] = 16;
    rlm[8] = (uint8_t)c->channel;               /* control channel */
    rlm[9] = (uint8_t)(c->bw ? c->center : c->channel);   /* centre channel */
    rlm[11] = (uint8_t)c->bw;                   /* width: 20, 40 or 80 MHz */
    rlm[12] = 2, rlm[13] = 3;                   /* tx streams, rx chain mask (2x2) */
    rlm[14] = 1;                                /* short slot time */
    if (c->bw) {
        rlm[15] = 4;                            /* ht_op_info: 40 MHz allowed */
        rlm[16] = c->channel < c->center ? 1 : 3;   /* the secondary above or below */
    }
    rlm[17] = c->channel <= 14 ? 0 : 1;         /* band: 2.4 or 5 GHz */
    return uni_set(c, UNI_BSS_INFO_UPDATE, rlm, sizeof(rlm));
}

static bool set_channel(struct net_card *d, int channel)
{
    struct mt7921 *c = d->priv;
    c->channel = channel;
    c->bw = 0;                                  /* (a move: 20 MHz until the next join) */
    bool ok = tune(c, channel, EXT_CHANNEL_SWITCH);
    if (c->joined) {                            /* the access point moved: so does */
        mutex_lock(&c->mcu_lock);               /* the BSS (no stay needed) */
        ok = bss_channel(c) == 0 && ok;
        mcu_unlock(c);
        return ok;
    }
    start_roc(c, channel, 10000);               /* 10 s on the channel */
    timer_sleep_ms(50);                         /* let the stay take effect */
    return ok;
}

/* The firmware's record of the access point (STA_REC_UPDATE), as Linux's
 * mt7921_mcu_sta_update builds it: BASIC (the peer, PORT_SECURE as mt7921's
 * offloading firmware wants), PHY and RA (the legacy rates we offer), STATE
 * (where the host's join is: NONE before authentication, ASSOC after), and a
 * WTBL entry so the chip transmits to it. Without STATE the firmware guesses
 * the station's state and sent frames of its own that the AP answered with a
 * deauthentication ("class 2 frame from a non-authenticated station"). */
static bool sta_rec(struct mt7921 *c, const uint8_t *bssid, int state, uint16_t aid,
                    const struct wlan_ht *ht)
{
    bool g = c->channel <= 14;                  /* 2.4 GHz: 802.11b/g; else 802.11a */
    bool n = ht && ht->on;                      /* and 802.11n (HT) */
    bool ac = n && ht->vht;                     /* and 802.11ac (VHT) */
    uint8_t p[208];
    memset(p, 0, sizeof(p));
    /* struct sta_req_hdr */
    p[1] = STA_WCID;                            /* wlan_idx_lo (bss_idx 0) */
    p[2] = (uint8_t)(5 + n + ac);               /* tlv_num: BASIC PHY RA STATE (HT VHT) WTBL */
    p[4] = 1;                                   /* is_tlv_append */
    size_t o = 8;
    /* STA_REC_BASIC (tag 0, 20 bytes) */
    p[o + 2] = 20;
    le32w(p + o + 4, 0x00010002);               /* CONNECTION_INFRA_AP */
    p[o + 8] = 2;                               /* conn_state: PORT_SECURE */
    le16w(p + o + 10, aid);
    memcpy(p + o + 12, bssid, 6);               /* peer_addr */
    le16w(p + o + 18, state == 0 ? 3 : 1);      /* extra_info: VER, NEW when first sent */
    o += 20;
    /* STA_REC_PHY (tag 0x15, 12 bytes) */
    p[o + 0] = 0x15, p[o + 2] = 12;
    le16w(p + o + 4, g ? 0x000F : 0x0015);      /* basic rates: 1-11 Mb/s, or 6/12/24 */
    p[o + 6] = (g ? 0x03 : 0x08) | (n ? 0x10 : 0) | (ac ? 0x20 : 0);  /* phy_type: HR-DSSS|ERP,
                                                 * OFDM, and HT (not BSS_INFO's phymode) */
    if (n)
        p[o + 7] = ht->ampdu & 0x1F;            /* ampdu: its factor and density */
    p[o + 9] = 220 - 80;                        /* rcpi: about -40 dBm (refined by RX) */
    o += 12;
    /* STA_REC_RA (tag 1, 16 bytes): the legacy rates, OFDM bits 13:6, CCK 3:0 */
    p[o + 0] = 1, p[o + 2] = 16;
    le16w(p + o + 4, g ? (0xFF << 6 | 0xF) : (0xFF << 6));
    if (n) {                                    /* rx_mcs_bitmask: what the access point */
        p[o + 6] = ht->mcs[0];                  /* receives, up to our two streams */
        p[o + 7] = ht->mcs[1];                  /* (MCS 0-15) */
    }
    o += 16;
    /* STA_REC_STATE (tag 7, 12 bytes) */
    p[o + 0] = 7, p[o + 2] = 12;
    p[o + 8] = (uint8_t)state;                  /* 0 NONE, 1 AUTH, 2 ASSOC */
    if (ac) {
        /* (65e) vht_opmode: the width it receives (0 20, 1 40, 2 80 MHz)
         * and its streams - 1 at bit 4, as mt76's sta_tlv. Left 0 the
         * firmware sent every frame at 20 MHz on a 40 MHz network (TXS:
         * 1,580 of 1,580), VHT MCS 8 x2 at 156 Mbit/s instead of 324. */
        int nss = 0;
        while (nss < 2 && ((ht->vht_rx_mcs >> (2 * nss)) & 3) != 3)
            nss++;
        p[o + 9] = (uint8_t)((ht->width >= 80 ? 2 : ht->width >= 40 ? 1 : 0) |
                             (nss ? nss - 1 : 0) << 4);
    }
    o += 12;
    if (n) {                                    /* STA_REC_HT (tag 9, 8 bytes): its */
        p[o + 0] = 9, p[o + 2] = 8;             /* capabilities as far as 20 MHz goes */
        le16w(p + o + 4, (uint16_t)(ht->cap & (ht->width >= 40 ? 0xFFFF : ~0x0002)));
        o += 8;                                 /* (40 MHz only when the width is) */
    }
    if (ac) {                                   /* STA_REC_VHT (tag 10, 16 bytes): its */
        p[o + 0] = 10, p[o + 2] = 16;           /* capabilities as far as ours go (the */
        le32w(p + o + 4, ht->vht_cap & 0x23);   /* MPDU length, short GI at 80 MHz), */
        le16w(p + o + 8, ht->vht_rx_mcs);       /* its MCS maps */
        le16w(p + o + 10, ht->vht_tx_mcs);
        o += 16;
    }
    /* STA_REC_WTBL (tag 13): a wtbl_req_hdr + GENERIC, RX, HDR_TRANS */
    size_t wtlv = o;
    p[o + 0] = 13;
    o += 4;
    p[o + 0] = STA_WCID;                        /* wtbl_req_hdr: wlan_idx_lo */
    p[o + 1] = 1;                               /* operation: RESET_AND_SET */
    p[o + 2] = (uint8_t)(3 + n + ac);           /* tlv_num */
    o += 8;
    p[o + 0] = 0, p[o + 2] = 20;                /* WTBL_GENERIC */
    memcpy(p + o + 4, bssid, 6);
    le16w(p + o + 16, aid);                     /* partial_aid */
    o += 20;
    p[o + 0] = 1, p[o + 2] = 12;                /* WTBL_RX: rca1, rca2, rv (a station) */
    p[o + 5] = 1, p[o + 6] = 1, p[o + 7] = 1;
    o += 12;
    p[o + 0] = 6, p[o + 2] = 8;                 /* WTBL_HDR_TRANS: to_ds, and no RX */
    p[o + 4] = 1;                               /* translation for this peer (802.11 */
    p[o + 6] = 1;                               /* frames to the wireless layer) */
    o += 8;
    if (n) {                                    /* WTBL_HT (tag 2, 12 bytes): HT on, no */
        p[o + 0] = 2, p[o + 2] = 12;            /* LDPC, its A-MPDU factor and density */
        p[o + 4] = 1;
        /* (65e-28) The longest A-MPDU it takes (8 KiB << af): with VHT the
         * larger of HT's factor (at most 3: 64 KiB) and VHT's exponent (up
         * to 7: 1 MiB), as mt76's wtbl_ht_tlv; with HT's alone a burst
         * stopped at 64 KiB, about 43 frames of a 64-frame window. */
        uint8_t af = ht->ampdu & 3, vexp = (uint8_t)((ht->vht_cap >> 23) & 7);
        p[o + 6] = ac && vexp > af ? vexp : af;
        p[o + 7] = (ht->ampdu >> 2) & 7;
        o += 12;
    }
    if (ac) {                                   /* WTBL_VHT (tag 3, 12 bytes): VHT on */
        p[o + 0] = 3, p[o + 2] = 12;
        p[o + 6] = 1;
        o += 12;
    }
    le16w(p + wtlv + 2, (uint16_t)(o - wtlv));
    int r = uni_set(c, UNI_STA_REC_UPDATE, p, o);
    kprintf("  mt7921:        %s: AP %02x:%02x:%02x:%02x:%02x:%02x station record, state %d: %d\n",
            c->where, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], state, r);
    return r == 0;
}

/* Set (op 1) or clear (op 2) bits of the receive filter (mt76's
 * mt7921_mcu_set_rxfilter). */
static int rx_filter(struct mt7921 *c, uint8_t op, uint32_t bits)
{
    uint8_t f[68] = { 0 };
    f[4] = 2;                                   /* mode: update the bit map */
    le32w(f + 12, bits);
    f[16] = op;
    return mcu_command(c, CE_SET_RX_FILTER, 0, 1, f, sizeof(f), false, 0, 0);
}

/* The frame types the firmware hands up (mt7921_configure_filter): with only
 * ENABLE set, frames of other networks, control frames and frames with a bad
 * checksum stay in the chip -- a station needs none of them. Without this
 * the chip passed everything it heard (about 20 management frames a second
 * on the laptop, none of them beacons, while joined). */
#define FILTER_ENABLE (1u << 31)
static int rx_filter_types(struct mt7921 *c, uint32_t flags)
{
    uint8_t f[68] = { 0 };
    f[4] = 1;                                   /* mode: the frame types (fif) */
    le32w(f + 8, flags);
    return mcu_command(c, CE_SET_RX_FILTER, 0, 1, f, sizeof(f), false, 0, 0);
}

/* The BSS the firmware sees (UNI BSS_INFO_BASIC + QBSS, mt76's uni_add_bss):
 * joined to bssid, or not joined (bssid 0). */
static int bss_basic(struct mt7921 *c, const uint8_t *bssid, bool joined, bool n, bool qos,
                     bool ac)
{
    bool g = c->channel <= 14;
    uint8_t b[44] = { 0 };                      /* bss 0 */
    b[4] = 0, b[6] = 32;                        /* BASIC (tag 0, 32 bytes) */
    b[8] = 1;                                   /* active */
    le32w(b + 12, 0x00010001);                  /* CONNECTION_INFRA_STA */
    b[16] = joined ? 0 : 1;                     /* conn_state: 0 connected */
    if (joined) {
        memcpy(b + 18, bssid, 6);
        le16w(b + 24, BMC_WCID);                /* bmc_tx_wlan_idx: the broadcast entry */
        le16w(b + 26, BEACON_INTERVAL);
        b[28] = DTIM_PERIOD;
        b[29] = (g ? 0x06 : 0x01) | (n ? (g ? 0x08 : 0x10) : 0) | (ac ? 0x20 : 0);
                                                /* phymode: B|G, or A; GN or AN with
                                                 * 802.11n; AC with 802.11ac */
        le16w(b + 30, BMC_WCID);                /* sta_idx (the interface's, as mt76) */
        le16w(b + 32, g ? 0x03 : 0x08);         /* basic PHY: HR-DSSS|ERP, or OFDM */
    }
    b[36] = 15, b[38] = 8;                      /* QBSS (tag 15, 8 bytes): qos when */
    b[40] = qos;                                /* the AP does WMM (our data is QoS then) */
    return uni_set(c, UNI_BSS_INFO_UPDATE, b, sizeof(b));
}

/* The interface's own entry (mt76's record for its vif, sta NULL, in its
 * offload form: only a WTBL part): where broadcasts from the access point
 * are looked up, and so where the group key goes (one entry holds one
 * cipher: the AP's entry has the pairwise key). */
static bool bmc_rec(struct mt7921 *c, const uint8_t *bssid)
{
    uint8_t p[8 + 4 + 8 + 20 + 12 + 8] = { 0 };
    p[1] = BMC_WCID;                            /* sta_req_hdr: wlan_idx_lo (bss 0) */
    p[2] = 1;                                   /* tlv_num: WTBL */
    p[4] = 1;                                   /* is_tlv_append */
    size_t o = 8, wtlv = o;
    p[o + 0] = 13;                              /* STA_REC_WTBL */
    o += 4;
    p[o + 0] = BMC_WCID;                        /* wtbl_req_hdr: wlan_idx_lo */
    p[o + 1] = 1;                               /* RESET_AND_SET */
    p[o + 2] = 3;                               /* tlv_num */
    o += 8;
    p[o + 0] = 0, p[o + 2] = 20;                /* WTBL_GENERIC: the BSSID, MUAR 0xe */
    memcpy(p + o + 4, bssid, 6);
    p[o + 10] = 0xE;
    o += 20;
    p[o + 0] = 1, p[o + 2] = 12;                /* WTBL_RX: rca1, rca2, rv */
    p[o + 5] = 1, p[o + 6] = 1, p[o + 7] = 1;
    o += 12;
    p[o + 0] = 6, p[o + 2] = 8;                 /* WTBL_HDR_TRANS: to_ds, no RX */
    p[o + 4] = 1, p[o + 6] = 1;                 /* translation */
    o += 8;
    le16w(p + wtlv + 2, (uint16_t)(o - wtlv));
    int r = uni_set(c, UNI_STA_REC_UPDATE, p, o);
    return r == 0;
}

/* Take a key into the chip (the key part of a station record,
 * STA_REC_KEY_V2, as mt76's add_key): CCMP-128 under its index -- the
 * pairwise key on the AP's entry, a group key on the broadcast entry. */
static bool set_key(struct net_card *d, int kind, int index, const uint8_t key[16])
{
    struct mt7921 *c = d->priv;
    bool pairwise = kind == WLAN_KEY_PAIRWISE;
    if (!(pairwise ? c->ap_known : kind == WLAN_KEY_GROUP && c->bmc_known))
        return false;
    uint8_t p[8 + 44] = { 0 };
    p[1] = pairwise ? STA_WCID : BMC_WCID;      /* sta_req_hdr: wlan_idx_lo (bss 0) */
    p[2] = 1;                                   /* tlv_num */
    p[4] = 1;                                   /* is_tlv_append */
    size_t o = 8;
    p[o + 0] = 17, p[o + 2] = 44;               /* STA_REC_KEY_V2, 44 bytes */
    p[o + 4] = 0;                               /* add: 0 is mac80211's SET_KEY */
    p[o + 5] = 1;                               /* n_cipher */
    o += 8;
    p[o + 0] = 5;                               /* cipher: AES-CCMP */
    p[o + 1] = 36;                              /* cipher_len: this struct */
    p[o + 2] = (uint8_t)index;                  /* key id */
    p[o + 3] = 16;                              /* key length */
    memcpy(p + o + 4, key, 16);
    mutex_lock(&c->mcu_lock);
    int r = uni_set(c, UNI_STA_REC_UPDATE, p, sizeof(p));
    if (pairwise)
        c->key_in_chip = r == 0;
    mcu_unlock(c);
    kprintf("  mt7921:        %s: %s key (index %d) in the chip: %d\n", c->where,
            pairwise ? "pairwise" : "group", index, r);
    return r == 0;
}

/* A Block Ack agreement with the access point (the BA part of its station
 * record, as mt76's sta_ba): sending, we are its originator and the firmware
 * sends the TID's frames in bursts; receiving, its recipient. */
static bool block_ack(struct net_card *d, int tid, bool sending, bool on, uint16_t ssn,
                      uint16_t window)
{
    struct mt7921 *c = d->priv;
    if (!c->ap_known || tid < 0 || tid > 7)
        return false;
    if (sending) {
        /* (65e) First the hardware's table (WTBL_BA, as mt76's
         * mt76_connac_mcu_sta_ba sends before STA_REC_BA): without it the
         * firmware knew the agreement but the MAC sent no A-MPDU (TXS: 0 of
         * 655 data frames in one), each frame on its own at ~22 Mbit/s of a
         * 360 Mbit/s rate. A STA_REC_WTBL holding a wtbl_req_hdr (WTBL_SET)
         * and the WTBL_BA (tag 8, 28 bytes: tid, ba_type originator, sn,
         * ba_en, ba_winsize). */
        uint8_t w[8 + 4 + 8 + 28] = { 0 };
        w[1] = STA_WCID;                        /* sta_req_hdr: wlan_idx_lo (bss 0) */
        w[2] = 1;                               /* tlv_num */
        w[4] = 1;                               /* is_tlv_append */
        w[8] = 13;                              /* STA_REC_WTBL */
        le16w(w + 10, 4 + 8 + 28);
        w[12] = STA_WCID;                       /* wtbl_req_hdr: wlan_idx_lo */
        w[13] = 2;                              /* operation: WTBL_SET */
        le16w(w + 14, 1);                       /* tlv_num */
        uint8_t *ba = w + 20;
        le16w(ba + 0, 8);                       /* WTBL_BA */
        le16w(ba + 2, 28);
        ba[4] = (uint8_t)tid;
        ba[5] = 1;                              /* MT_BA_TYPE_ORIGINATOR */
        le16w(ba + 8, on ? ssn : 0);            /* sn */
        ba[10] = on ? 1 : 0;                    /* ba_en */
        le16w(ba + 12, on ? window : 0);        /* ba_winsize */
        int rw = uni_cmd(c, UNI_STA_REC_UPDATE, w, sizeof(w), false);
        if (rw != 0)
            kprintf("  mt7921:        %s: block ack sending TID %d: WTBL_BA not taken (%d)\n",
                    c->where, tid, rw);
    }
    if (sending)
        c->ba_tx_window = on ? window : 0;      /* (65e: for the report) */
    uint8_t p[8 + 12] = { 0 };
    p[1] = STA_WCID;                            /* sta_req_hdr: wlan_idx_lo (bss 0) */
    p[2] = 1;                                   /* tlv_num */
    p[4] = 1;                                   /* is_tlv_append */
    p[8] = 6, p[10] = 12;                       /* STA_REC_BA, 12 bytes */
    p[12] = (uint8_t)tid;
    p[13] = sending ? 1 : 2;                    /* originator or recipient */
    p[14] = 0;                                  /* no A-MSDU inside */
    p[15] = (uint8_t)(on ? 1u << tid : 0);      /* ba_en */
    le16w(p + 16, ssn);
    le16w(p + 18, window);
    int r = uni_cmd(c, UNI_STA_REC_UPDATE, p, sizeof(p), false);   /* (on the RX thread) */
    kprintf("  mt7921:        %s: block ack %s TID %d %s (window %u): %s\n", c->where,
            sending ? "sending" : "receiving", tid, on ? "on" : "off", window,
            r == 0 ? "sent" : "not sent");
    return r == 0;
}

/* wifid has associated with bssid (aid from the access point): the firmware
 * is told the BSS and the station's state, and from now on watches the
 * access point's beacons itself (beacon filter: they no longer come to us,
 * and other networks' beacons are dropped), as mt76 does on BSS_CHANGED_ASSOC. */
static bool join(struct net_card *d, const uint8_t *bssid, uint16_t aid, int channel,
                 const struct wlan_ht *ht, bool powersave)
{
    struct mt7921 *c = d->priv;
    (void)channel;                              /* (set_channel came first) */
    mutex_lock(&c->mcu_lock);
    c->ap_ht = *ht;                             /* (65e: for the report) */
    if (ht->on && ht->width > 20) {             /* the full width the access point runs */
        c->bw = ht->width == 80 ? 2 : 1;
        c->center = ht->center;
        tune(c, c->channel, EXT_CHANNEL_SWITCH);
    }
    c->bmc_known = bmc_rec(c, bssid);           /* (the BSS names it as its broadcast entry) */
    c->joined = true;
    int b = bss_basic(c, bssid, true, ht->on, ht->wmm, ht->on && ht->vht);
    int ch = bss_channel(c);
    memcpy(c->ap, bssid, 6);
    c->ap_known = sta_rec(c, bssid, 2, aid, ht);    /* ASSOC */
    uint8_t ft[12] = { 0 };                     /* bss 0; BCNFT (tag 22, 8 bytes) */
    ft[4] = 22, ft[6] = 8;
    le16w(ft + 8, BEACON_INTERVAL);
    ft[10] = DTIM_PERIOD;
    int f = uni_set(c, UNI_BSS_INFO_UPDATE, ft, sizeof(ft));
    int r = rx_filter(c, 1, RFCR_DROP_OTHER_BEACON);
    /* Power saving (powersave = on): the firmware dozes between beacons and
     * wakes for the ones that say frames wait for us (BSS_INFO PS state 2
     * and the PS profile 2, dynamic, as mt76 with power saving on; the
     * "keep full power" of the boot let go). Off, the default: the station
     * stays awake (state 0, profile 0) -- left to itself the firmware dozed
     * once idle without that set up, and the machine could not be reached. */
    uint8_t state = powersave ? 2 : 0;
    if (powersave)
        keep_full_power_set(c, false);
    uint8_t ps[12] = { 0 };                     /* bss 0; PS (tag 21, 8 bytes): its state */
    ps[4] = 21, ps[6] = 8;
    ps[8] = state;
    int p = uni_set(c, UNI_BSS_INFO_UPDATE, ps, sizeof(ps));
    uint8_t profile[2] = { 0, state };          /* bss 0, ps_state */
    mcu_command(c, CE_SET_PS_PROFILE, 0, 1, profile, sizeof(profile), false, 0, 0);
    c->powersave = powersave;
    mcu_unlock(c);
    kprintf("  mt7921:        %s: joined (AID %u, %s%s%s): BSS %d, channel %d, beacon filter %d,"
            " receive filter %d, awake %d\n", c->where, aid,
            !ht->on ? "802.11g" : ht->vht ? "802.11ac" : ht->mcs[1] ? "802.11n, MCS 0-15" :
            "802.11n, MCS 0-7", ht->width == 80 ? ", 80 MHz" : ht->width == 40 ? ", 40 MHz" : "",
            ht->wmm ? (powersave ? ", QoS, power saving" : ", QoS") : (powersave ?
            ", power saving" : ""),
            b, ch, f, r, p);
    return b == 0 && c->ap_known;
}

/* The link is gone: the firmware forgets the BSS and hands up every beacon
 * again (wifid scans next). The AP's station record is made anew on the
 * next transmission to an access point. */
static void leave(struct net_card *d)
{
    struct mt7921 *c = d->priv;
    mutex_lock(&c->mcu_lock);
    uint8_t hdr[4] = { 0 };                     /* bss 0 */
    mcu_command(c, CE_SET_BSS_ABORT, 0, 1, hdr, sizeof(hdr), false, 0, 0);
    rx_filter(c, 2, RFCR_DROP_OTHER_BEACON);
    bss_basic(c, 0, false, false, false, false);
    c->ap_known = false;
    c->key_in_chip = false;
    c->bmc_known = false;
    c->joined = false;
    c->bw = 0;
    if (c->powersave)                           /* joining needs the radio awake */
        keep_full_power_set(c, true);
    c->powersave = false;
    mcu_unlock(c);
}

/* Build the 32-byte transmit descriptor for one 802.11 frame (mt76's connac2
 * write_txwi). The firmware adds the sequence number, the FCS, and -- when
 * protect is set and a key is installed for the peer -- the CCMP header, PN
 * and MIC; we hand over the plaintext frame. */
static void build_txd(uint8_t *out, const uint8_t *f, size_t flen, int wcid, bool protect,
                      bool five, uint8_t pid)
{
    uint16_t fc = (uint16_t)(f[0] | f[1] << 8);
    int ftype = (fc >> 2) & 3, fstype = (fc >> 4) & 0xf;
    bool data = ftype == 2;
    bool qos = data && (fstype & 8);
    bool mcast = f[4] & 1;                       /* addr1 is a group address */
    int hdrlen = qos ? 26 : 24;
    int q_idx = data ? 1 : 3;                    /* data on AC_BE, management on AC_VO */
    bool fix_rate = !data || mcast;              /* a fixed low rate for management */
    memset(out, 0, 32);
    le32w(out + 0, (uint32_t)((flen + 32) & 0xFFFF) | (0u << 23) | ((uint32_t)q_idx << 25));
    le32w(out + 4, (1u << 31) | ((uint32_t)wcid & 0x3FF) | (2u << 16) | ((uint32_t)(hdrlen / 2) << 11));
    uint32_t dw2 = ((uint32_t)ftype << 4) | (uint32_t)fstype | (mcast ? 1u << 10 : 0);
    uint32_t dw3 = 15u << 11;                    /* REM_TX_COUNT */
    uint32_t dw6 = 0;
    if (protect)
        dw3 |= 1u << 1;                          /* PROTECT_FRAME: the chip encrypts */
    if (fix_rate) {
        dw2 |= (1u << 31) | (1u << 13);          /* FIX_RATE | HTC_VLD */
        dw3 |= 1u << 28;                         /* BA_DISABLE */
        /* FIXED_BW and the lowest rate of the band (mt76's tx_rate_val): CCK
         * 1 Mbit/s at 2.4 GHz; at 5 GHz, where there is no CCK, OFDM 6 Mbit/s
         * (mode 1, index 11) */
        dw6 = 1u << 2 | (five ? (1u << 6 | 11u) << 16 : 0);
    }
    if (ftype == 0 && fstype == 13 && flen >= 29 && f[24] == 3 && f[25] == 0) {
        /* An ADDBA request: ADD_BA, and its TID, so the hardware fills in the
         * starting sequence number it will use (as mt76's write_txwi). */
        uint32_t tid = ((uint32_t)(f[27] | f[28] << 8) >> 2) & 7;
        le32w(out + 20, 1u << 14);              /* TXD5: ADD_BA */
        le32w(out + 4, le32r(out + 4) | tid << 20);  /* TXD1: TID */
    }
    if (pid)                                    /* (65e) a TXS for this one: its real rate */
        le32w(out + 20, le32r(out + 20) | 1u << 10 | pid);  /* TXD5: TX_STATUS_HOST, PID */
    le32w(out + 8, dw2);
    le32w(out + 12, dw3);
    le32w(out + 24, dw6);
    le32w(out + 28, ((uint32_t)ftype << 20) | ((uint32_t)fstype << 16));
}

/* A free token, or -1 (one stuck 2 s is taken as lost). */
static int free_token(struct mt7921 *c)
{
    for (int i = 0; i < TX_TOKENS; i++)
        if (!c->tok_busy[i] || timer_ticks() - c->tok_time[i] > 200)
            return i;
    return -1;
}

static bool post_frame(struct mt7921 *c, int t, const uint8_t *f, size_t length);

/* (65e-27) The frames waiting for a token go to the chip, in order, while
 * tokens are free. Under mcu_lock. */
static void drain(struct mt7921 *c)
{
    while (c->pend_count && !c->recovering && c->fw_ready) {
        int t = free_token(c);
        if (t < 0 || !post_frame(c, t, c->pend[c->pend_head], c->pend_len[c->pend_head]))
            return;                             /* (the next frame done tries again) */
        c->pend_head = (c->pend_head + 1) % PENDING;
        c->pend_count--;
    }
}

/* (65e-27) Every holder of mcu_lock gives it back through here: frames that
 * waited while a command held it go first. */
static void mcu_unlock(struct mt7921 *c)
{
    drain(c);
    mutex_unlock(&c->mcu_lock);
}

static bool transmit(struct net_card *d, const void *frame, size_t length)
{
    struct mt7921 *c = d->priv;
    const uint8_t *f = frame;
    if (length < 24 || length + 32 > BUF_SIZE || c->recovering || !c->fw_ready)
        return false;
    mutex_lock(&c->mcu_lock);
    /* The access point is addr1 for the frames wifid sends; add its station
     * record the first time we transmit to it (wifid sends auth before it asks
     * for the link, so we cannot wait for a join call). */
    bool unicast = !(f[4] & 1);
    if (unicast && (!c->ap_known || memcmp(c->ap, f + 4, 6))) {
        memcpy(c->ap, f + 4, 6);
        c->ap_known = sta_rec(c, c->ap, 0, 0, 0); /* NONE: the host authenticates */
        c->key_in_chip = false;                 /* (a new record holds no key) */
    }
    int t = -1;
    if (c->pend_count || (t = free_token(c)) < 0) {
        /* (65e-27) Every token with the chip, or frames already waiting:
         * this one waits behind them, and goes as soon as tokens come free
         * (drain). Only with the queue full a sender waits a little for
         * room, as a card's full ring makes it wait; not while delivering a
         * received frame: that may be the receive thread, which frees
         * them. Refused, the frame is lost and TCP sends it again. */
        if (c->pend_count == PENDING && !net_receiving()) {
            c->tok_waits++;
            mcu_unlock(c);
            wait_event_until(&c->tok_wq, c->pend_count < PENDING, timer_ticks() + TOKEN_WAIT_TICKS);
            mutex_lock(&c->mcu_lock);
        }
        if (c->pend_count == PENDING || !c->pend[0]) {
            c->tok_refused++;
            c->tok_refused_rx += net_receiving();
            mcu_unlock(c);
            return false;
        }
        int slot = (c->pend_head + c->pend_count++) % PENDING;
        memcpy(c->pend[slot], f, length);
        c->pend_len[slot] = (uint16_t)length;
        c->pend_queued++;
        if (c->pend_count > c->pend_max)
            c->pend_max = c->pend_count;
        mcu_unlock(c);                          /* (takes what tokens there are) */
        return true;
    }
    bool ok = post_frame(c, t, f, length);
    mcu_unlock(c);
    return ok;
}

/* Hand a frame to the chip with token t. Under mcu_lock. */
static bool post_frame(struct mt7921 *c, int t, const uint8_t *f, size_t length)
{
    uint8_t *w = c->tok[t];
    memcpy(w + TX_FRAME_OFF, f, length);
    /* A protected frame while the chip holds the key: plain text with the
     * Protected bit, and the chip adds the CCMP header, packet number, MIC. */
    uint8_t pid = 0;                            /* (65e) every TXS_EVERY-th data frame */
    if (((f[0] >> 2) & 3) == 2 && !(f[4] & 1) && ++c->tx_sampled % TXS_EVERY == 0)
        pid = 1;
    build_txd(w, f, length, STA_WCID, c->key_in_chip && (f[1] & 0x40), c->channel > 14, pid);
    memset(w + 32, 0, 32);                      /* struct mt76_connac_hw_txp */
    le16w(w + 32, (uint16_t)(t | 0x8000));      /* msdu_id[0]: token | VALID */
    le32w(w + 40, c->tok_phys[t] + TX_FRAME_OFF);  /* ptr[0].buf0 */
    le16w(w + 44, (uint16_t)((length & 0xFFF) | 0x8000));  /* len0 | LAST */
    c->tok_busy[t] = true;
    c->tok_time[t] = timer_ticks();
    c->tok_cycles[t] = arch_counter();
    int flying = 1;
    for (int i = 0; i < TX_TOKENS; i++)
        flying += c->tok_busy[i] && i != t;
    if (flying > c->tx_in_flight_max)
        c->tx_in_flight_max = flying;
    arch_write_barrier();
    bool ok = tx_post(c, &c->tx_data, c->tok_phys[t], TXWI_SIZE);   /* (no wait) */
    c->tx_posted += ok;
    if (!ok)
        c->tok_busy[t] = false;
    return ok;
}

/* Start the RX thread and the tick, then make the chip wlanN. */
static void start_wlan(struct mt7921 *c)
{
    c->reply = kmalloc(BUF_SIZE);
    if (!c->reply) {
        kprintf("  mt7921:        %s: out of memory\n", c->where);
        return;
    }
    for (int i = 0; i < TX_TOKENS; i++) {
        uint64_t pp = pmm_alloc_pages_below(1, 0x100000000ULL);
        if (!pp) {
            kprintf("  mt7921:        %s: out of memory\n", c->where);
            return;
        }
        c->tok[i] = phys_to_virt(pp);
        c->tok_phys[i] = (uint32_t)pp;
    }
    for (int i = 0; i < PENDING; i++)           /* (65e-27) the queue's frames */
        if (!(c->pend[i] = kmalloc(BUF_SIZE))) {
            for (int k = 0; k < i; k++)
                kfree(c->pend[k]), c->pend[k] = 0;
            break;                              /* (none: full means refused, as before) */
        }
    c->reply_seq = -1;
    memcpy(c->dev->mac, c->mac, 6);
    for (size_t i = 0; i < sizeof(scan_channels); i++)
        c->wlan.channels[i] = scan_channels[i];
    c->wlan.nchannels = (int)sizeof(scan_channels);
    c->wlan.transmit = transmit;
    c->wlan.set_channel = set_channel;
    c->wlan.scan = scan;
    c->wlan.set_country = country_cb;
    c->wlan.join = join;
    c->wlan.leave = leave;
    c->wlan.set_key = set_key;
    c->wlan.block_ack = block_ack;
    /* Registered before anything is received: the layer's state for the card
     * is made here, and a frame handed to it earlier met a null pointer (a
     * page fault on the laptop, once the interrupts answered at once). */
    wlan_register(c->dev, &c->wlan);
    c->rx_running = true;                       /* from here, replies come through the thread */
    if (!thread_create("mt7921 rx", receiver, c) || !timer_add_tick_handler(tick, c)) {
        kprintf("  mt7921:        %s: no thread for receiving\n", c->where);
        return;
    }
    rx_filter_types(c, FILTER_ENABLE);
    uint64_t address;
    uint32_t data;
    c->msi = pci_msi_enable(c->pci, interrupt, c, &address, &data);
    if (c->msi) {                               /* as mt7921e: RX done, the firmware's */
        set_bits(c, MCU2HOST_INT_EN, MCU_WAKE_RX);      /* wake-RX request */
        wr(c, HOST_INT_STA, rd(c, HOST_INT_STA));
        wr(c, HOST_INT_ENA, RX_INTS | MCU_SW_INT);
    }
    kprintf("  mt7921:        %s: is %s (%s)\n", c->where, c->dev->name,
            c->msi ? "MSI" : "polled by the tick");
}

static bool bring_up(struct mt7921 *c)
{
    /* WFSYS reset first: it resets the Wi-Fi subsystem and hands the MCU back
     * to firmware-own, so the driver-own handshake must come *after* it (Linux
     * takes it last, right before the firmware download). Taking it before the
     * reset -- as we did -- left the MCU in firmware-own when we posted the
     * first command, and it never drained the ring (dma_idx stuck at 0). */
    l1_write(c, WFSYS_RST, l1_read(c, WFSYS_RST) & ~WFSYS_RST_B);
    timer_sleep_ms(50);
    l1_write(c, WFSYS_RST, l1_read(c, WFSYS_RST) | WFSYS_RST_B);
    if (!poll_l1(c, WFSYS_RST, WFSYS_INIT_DONE, WFSYS_INIT_DONE, 500)) {
        kprintf("  mt7921:        %s: WFSYS reset timed out\n", c->where);
        return false;
    }

    wr(c, HOST_INT_ENA, 0);
    wr(c, PCIE_MAC_INT_EN, 0xff);
    if (!dma_init(c)) {
        kprintf("  mt7921:        %s: DMA init failed\n", c->where);
        return false;
    }
    kprintf("  mt7921:        %s: reset + DMA up (GLO_CFG 0x%X)\n", c->where, rd(c, GLO_CFG));

    /* driver own via the L1 LPCR, now that the reset is done and DMA is up. */
    l1_write(c, LPCR, LPCR_DRV_OWN);
    if (!poll_l1(c, LPCR, LPCR_FW_OWN, 0, 500)) {
        kprintf("  mt7921:        %s: driver-own timed out\n", c->where);
        return false;
    }
    set_bits(c, PCIE_MAC_PM, 1u << 8);          /* L0s disable */

    wr(c, SWDEF_MODE, 0);
    if (!download_patch(c)) {
        kprintf("  mt7921:        %s: patch download failed\n", c->where);
        return false;
    }
    kprintf("  mt7921:        %s: patch downloaded\n", c->where);
    if (!download_ram(c)) {
        kprintf("  mt7921:        %s: RAM download failed (MISC 0x%X)\n", c->where,
                rd(c, CONN_ON_MISC));
        return false;
    }
    c->fw_ready = true;
    kprintf("  mt7921:        %s: FIRMWARE RUNNING (MISC 0x%X)\n", c->where, rd(c, CONN_ON_MISC));
    read_nic_capability(c);
    if (!set_eeprom(c)) {
        kprintf("  mt7921:        %s: set_eeprom failed\n", c->where);
        return false;
    }
    mac_init(c);
    kprintf("  mt7921:        %s: MAC init done (MDP_DCR0 0x%X DMA_DCR0 0x%X)\n", c->where,
            rd(c, MDP_DCR0), rd(c, DMA_DCR0));
    if (!mac_on(c)) {
        kprintf("  mt7921:        %s: MAC enable failed\n", c->where);
        return false;
    }
    add_station(c);
    return true;
}

/* Restart the chip whose firmware went quiet: receiving paused, the link
 * reported lost (wifid joins again afterwards), then the boot's bring-up
 * once more -- the subsystem reset, the DMA rings (the same memory, armed
 * again), the firmware download, the MAC -- and what start_wlan set up:
 * the receive filter, the country, the interrupts. */
/* The first half of a restart (and, 37g-3, of a sleep): nothing goes in
 * (recovering), receiving paused, the link reported lost (wifid joins
 * again afterwards), the commands' lock taken. */
static void quiet(struct mt7921 *c, const char *why)
{
    c->recovering = true;
    c->rx_running = false;                      /* (commands poll the rings themselves) */
    wr(c, HOST_INT_ENA, 0);
    wlan_link_lost(c->dev, why);
    for (int i = 0; i < 50 && !c->rx_asleep; i++)
        timer_sleep_ms(10);                     /* let the RX thread finish its round */
    mutex_lock(&c->mcu_lock);
}

static bool up_again(struct mt7921 *c);

static void restart(struct mt7921 *c, const char *why)
{
    kprintf("  mt7921:        %s: the firmware stopped working (%s); restarting the chip\n",
            c->where, why);
    quiet(c, "the card's firmware stopped answering; restarting it");
    bool ok = up_again(c);
    if (ok)
        c->restarts++;
    kprintf("  mt7921:        %s: %s\n", c->where,
            ok ? "the chip is running again" : "the restart failed (tried again in 10 s)");
}

/* The second half: the firmware's state forgotten, the boot's bring-up
 * once more, then what start_wlan set up; the lock given back. */
static bool up_again(struct mt7921 *c)
{
    c->fw_ready = false;
    c->ap_known = c->key_in_chip = c->bmc_known = c->joined = false;
    c->scanning = false;
    c->empty_scans = 0;
    c->reply_seq = -1;
    c->mcu_timeouts = 0;
    for (int i = 0; i < TX_TOKENS; i++)
        c->tok_busy[i] = false;
    c->pend_head = c->pend_count = 0;           /* (65e-27: lost, as those with the chip) */
    bool ok = bring_up(c);
    if (ok) {
        rx_filter_types(c, FILTER_ENABLE);
        if (c->country[0])
            set_country(c, c->country);
        if (c->msi) {
            set_bits(c, MCU2HOST_INT_EN, MCU_WAKE_RX);
            wr(c, HOST_INT_STA, rd(c, HOST_INT_STA));
            wr(c, HOST_INT_ENA, RX_INTS | MCU_SW_INT);
        }
        c->rx_running = true;
    }
    mcu_unlock(c);
    c->recovering = false;
    wait_queue_wake_all(&c->rx_wq);
    return ok;
}

/* The thread "mt7921 watch": every second, is the firmware still there?
 * Gone when commands got no answer twice in a row, or when frames kept
 * going in for 5 s and none was reported done. */
static void watch(void *context)
{
    struct mt7921 *c = context;
    uint32_t last_posted = 0, last_done = 0;
    int stalled = 0;
    for (;;) {
        timer_sleep_ms(1000);
        if (c->recovering)
            continue;
        if (c->tx_posted != last_posted && c->tx_done == last_done)
            stalled++;
        else
            stalled = 0;
        last_posted = c->tx_posted, last_done = c->tx_done;
        if (c->mcu_timeouts >= 2)
            restart(c, "commands without an answer");
        else if (c->empty_scans >= 5)
            restart(c, "its scans end at once, hearing nothing");
        else if (stalled >= 5)
            restart(c, "frames sent, none reported done");
        else if (!c->fw_ready && (c->restarts || c->sleeps))
            restart(c, "the last start failed");
        else
            continue;
        stalled = 0;
        last_posted = c->tx_posted, last_done = c->tx_done;
        if (!c->fw_ready)
            timer_sleep_ms(10000);
    }
}

static void worker(void *arg)
{
    struct mt7921 *c = arg;
    uint64_t phys = pmm_alloc_pages_below(2, 0x100000000ULL);   /* command/chunk buffer */
    if (!phys) {
        kprintf("  mt7921:        %s: no low DMA memory\n", c->where);
        return;
    }
    c->cmd = phys_to_virt(phys);
    c->cmd_phys = (uint32_t)phys;
    if (bring_up(c)) {
        start_wlan(c);
        thread_create("mt7921 watch", watch, c);
    }
}

static const struct {
    uint16_t device;
    const char *model;
} models[] = {
    { 0x7961, "MediaTek MT7921" },
    { 0x7922, "MediaTek MT7922" },
    { 0x0608, "MediaTek MT7921" },
    { 0x7920, "MediaTek MT7920" },
};

static const char *mt7921_model(const struct pci_device *pci)
{
    if (pci->vendor_id != 0x14c3)
        return 0;
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++)
        if (models[i].device == pci->device_id)
            return models[i].model;
    return 0;
}

/* net's report: what the chip said about the frames it was given. */
static void details(struct net_card *d)
{
    struct mt7921 *c = d->priv;
    int busy = 0;
    for (int i = 0; i < TX_TOKENS; i++)
        busy += c->tok_busy[i];
    kprintf("       mt7921: %u frames sent by the chip, %u not acknowledged, %d waiting; power "
            "saving %s\n", c->tx_done, c->tx_failed, busy, c->powersave ? "on" : "off");
    uint64_t hz = arch_counter_hz() ? arch_counter_hz() : 1;
    kprintf("       mt7921: send to done: average %lu us, longest %lu us; at most %d in flight "
            "of %d; %lu waited for one, %lu in vain\n",
            c->tx_done ? c->tx_cycles_sum / c->tx_done * 1000000 / hz : (uint64_t)0,
            c->tx_cycles_max * 1000000 / hz, c->tx_in_flight_max, TX_TOKENS, c->tok_waits,
            c->tok_refused);
    /* (65e) Refused on the receive thread: it cannot wait (it frees the
     * tokens), and TCP sends from it when an ACK comes. The access point's
     * A-MPDU limits: HT's factor (8 KiB << it) and density, VHT's exponent
     * (8 KiB << it), and the sending agreement's window. */
    const struct wlan_ht *h = &c->ap_ht;
    kprintf("       mt7921: %lu waited for a token in the queue, at most %d at once (of %d), "
            "%d waiting now\n", c->pend_queued, c->pend_max, PENDING, c->pend_count);
    kprintf("       mt7921: %lu refused on the receive thread; the access point: HT A-MPDU "
            "factor %u (%u KiB), density %u, VHT exponent %u (%u KiB), cap 0x%X; sending window %u\n",
            c->tok_refused_rx, h->ampdu & 3, 8u << (h->ampdu & 3), (h->ampdu >> 2) & 7,
            h->vht ? (h->vht_cap >> 23) & 7 : 0u, h->vht ? 8u << ((h->vht_cap >> 23) & 7) : 0u,
            h->vht_cap, c->ba_tx_window);
    const uint64_t *t = c->tx_spans;        /* (65e) */
    kprintf("       mt7921: send to done: under 1 ms %lu, 2 ms %lu, 5 ms %lu, 10 ms %lu, 20 ms %lu, "
            "50 ms %lu, longer %lu\n", t[0], t[1], t[2], t[3], t[4], t[5], t[6]);
    const uint64_t *a = c->tx_tries;            /* (65e) */
    kprintf("       mt7921: attempts per frame: 1 %lu, 2 %lu, 3-4 %lu, 5-8 %lu, more %lu\n",
            a[0], a[1], a[2], a[3], a[4]);
    kprintf("       mt7921: rates of %lu sampled data frames (TXS, 1 in %d): CCK %lu, OFDM %lu, "
            "HT %lu, HT-GF %lu, VHT %lu; in an A-MPDU %lu, not acknowledged %lu\n", c->txs_count,
            TXS_EVERY, c->txs_mode[0], c->txs_mode[1], c->txs_mode[2], c->txs_mode[3],
            c->txs_mode[4], c->txs_ampdu, c->txs_failed);
    char line[256];
    size_t at = 0;
    for (int i = 0; i < 16; i++)
        if (c->txs_ht[i])
            at += ksnprintf(line + at, sizeof(line) - at, " HT%d:%lu", i, c->txs_ht[i]);
    for (int s2 = 0; s2 < 2; s2++)
        for (int i = 0; i < 10; i++)
            if (c->txs_vht[s2][i])
                at += ksnprintf(line + at, sizeof(line) - at, " VHT%dx%d:%lu", i, s2 + 1,
                                c->txs_vht[s2][i]);
    for (int i = 0; i < 8; i++)
        if (c->txs_ofdm[i])
            at += ksnprintf(line + at, sizeof(line) - at, " OFDM%d:%lu", i, c->txs_ofdm[i]);
    if (at)
        kprintf("       mt7921: by rate:%s; by width: 20 MHz %lu, 40 MHz %lu, 80 MHz %lu, "
                "160 MHz %lu\n", line, c->txs_bw[0], c->txs_bw[1], c->txs_bw[2], c->txs_bw[3]);
    /* (65e) The A-MPDUs by length: range i holds up to bound[i] frames
     * (each range register's bytes, plus one), the last everything longer. */
    if (c->mib_on) {
        uint32_t arng[4];
        for (int i = 0; i < 4; i++)
            arng[i] = rd(c, MIB_ARNG(i));
        at = 0;
        line[0] = 0;
        uint64_t bursts = 0, frames = 0;
        int low = 1;
        for (int i = 0; i < 16; i++) {
            int high = i < 15 ? (int)((arng[i / 4] >> (8 * (i % 4))) & 0xFF) + 1 : 256;
            if (c->mib_aggr[i] && at < sizeof(line))
                at += ksnprintf(line + at, sizeof(line) - at, " %d-%d:%lu", low, high,
                                c->mib_aggr[i]);
            bursts += c->mib_aggr[i];
            frames += c->mib_aggr[i] * (uint64_t)(low + high) / 2;
            low = high + 1;
        }
        kprintf("       mt7921: A-MPDUs sent by length (the chip's MIB):%s; %lu, about %lu "
                "frames each\n", line[0] ? line : " none", bursts,
                bursts ? frames / bursts : (uint64_t)0);
        kprintf("       mt7921: airtime since joining (the chip's MIB): sending %lu ms, receiving "
                "%lu ms, the channel busy %lu ms\n", c->mib_tx_us / 1000, c->mib_rx_us / 1000,
                c->mib_busy_us / 1000);
    }
    kprintf("       mt7921: interrupts: %s, %u (%u before their frame), found by the tick instead "
            "%u; chip restarts %u, sleeps %u; scans done at once hearing nothing %u\n",
            c->msi ? "MSI" : "none (polled)", c->interrupts, c->early, c->found_by_tick,
            c->restarts, c->sleeps, c->empty_scans_all);
}

static struct net_card *mt7921_init(struct pci_device *pci, const char *model)
{
    if (nchips == 4)
        return 0;
    struct mt7921 *c = &chips[nchips];
    if (!c->dev && !(c->dev = net_card_new()))
        return 0;
    ksnprintf(c->where, sizeof(c->where), "%02x:%02x.%u", pci->bus, pci->device, pci->function);
    c->mcu_lock = (struct mutex)MUTEX_INIT("mt7921");
    c->dev->driver = "mt7921";
    ksnprintf(c->dev->model, sizeof(c->dev->model), "%s", model);
    ksnprintf(c->dev->where, sizeof(c->dev->where), "%s", c->where);
    c->dev->pci = pci;
    c->pci = pci;
    c->dev->priv = c;
    c->dev->details = details;
    struct pci_bar *bar = &pci->bars[0];
    if (!bar->size || bar->is_io) {
        kprintf("  mt7921:        %s: no register BAR\n", c->where);
        return 0;
    }
    pci_enable_dma(pci);
    c->regs = paging_map_mmio(bar->address, bar->size);
    c->chipid = (int)(l1_read(c, MT_HW_CHIPID) & 0xFFFF);
    if (c->chipid == 0x7961 && (l1_read(c, MT_HW_BOUND) & (1u << 7)))
        c->chipid = 0x7920;
    uint32_t rev = l1_read(c, MT_HW_REV) & 0xFF;
    bool owned = take_ownership(c);
    kprintf("  mt7921:        %s at %s, chip 0x%X revision 0x%X, %s\n", model, c->where,
            c->chipid, rev, owned ? "host owns" : "the firmware did not hand over");
    if (!owned)
        return 0;
    nchips++;
    thread_create("mt7921", worker, c);
    return 0;
}

/* Its setup runs on a thread of its own (worker): taken when the chip
 * handed itself over. */
static bool start(struct pci_device *pci)
{
    const char *model = mt7921_model(pci);
    if (!model)
        return false;
    int before = nchips;
    mt7921_init(pci, model);
    return nchips > before;
}

/* (37g-3) Sleep: in S3 the chip loses its firmware. Before it the first
 * half of a restart (the link reported lost: wifid joins again after it),
 * the commands' lock kept; after it the chip taken from its firmware as at
 * the boot, then the restart's second half (the firmware loaded again,
 * the MAC, the filter, the country, the interrupts). */
static struct mt7921 *chip_of(const struct pci_device *pci)
{
    for (int i = 0; i < nchips; i++)
        if (chips[i].pci == pci)
            return &chips[i];
    return 0;
}

static bool suspend(struct pci_device *pci)
{
    struct mt7921 *c = chip_of(pci);
    if (c)
        quiet(c, "the machine goes to sleep");
    return true;
}

static bool resume(struct pci_device *pci)
{
    struct mt7921 *c = chip_of(pci);
    if (!c)
        return true;
    take_ownership(c);
    c->sleeps++;
    bool ok = up_again(c);
    kprintf("  mt7921:        %s: after the sleep: %s\n", c->where,
            ok ? "the chip is running again" : "its firmware did not start");
    return ok;
}

/* The chips mt7921_model knows, by the same IDs. */
static const struct module_pci ids[] = {
    { 0x14C3, 0x7961 }, { 0x14C3, 0x7922 }, { 0x14C3, 0x0608 }, { 0x14C3, 0x7920 },
};

MODULE(mt7921, start, suspend, resume, ids)
