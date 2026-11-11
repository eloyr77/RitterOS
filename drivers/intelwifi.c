/*
 * Intel Wi-Fi driver (roadmap item 36): the Dual Band Wireless-AC 8265
 * (8086:24fd). Our own code; the registers, the order of the steps and the
 * firmware file's layout are facts about the chip (from Linux's iwlwifi and
 * OpenBSD's iwm), the code is ours. Real hardware only (no QEMU model): that
 * laptop has no Ethernet port, so until its Wi-Fi works each step is a stick
 * whose log is read afterwards.
 *
 *   36a: the card owned (ready, reset, woken: its MAC clock running; its
 *        revision and radio switch read; its silicon step read from the
 *        periphery through the host's access window, the power block WFPM
 *        on first) and Intel's firmware file read and listed part by part.
 *   36b-1: the init image loaded into the chip (the DMA engine's service
 *        channel), its CPUs started, the receive ring and the interrupt
 *        (MSI) set up, and the firmware's "alive" read: its status, its
 *        versions and where its logs are.
 *   36b-2: the transmit side started after "alive" (the scheduler, the
 *        command queue), the first commands and their replies: ECHO, then
 *        the NVM read section by section, and the card's own data from it
 *        (MAC address, radio, antennas, bands, channels). The init image
 *        has no paged part (Linux pages only the run-time image: 36b-3).
 *   36b-3a: the init image's calibrations (the antennas, the PHY
 *        configuration), their results kept (the PHY database) until the
 *        firmware's "init complete".
 *   36b-3b: the chip stopped and the run-time image loaded and alive, its
 *        paged part handed over (paging), then the antennas, the PHY
 *        database and the PHY configuration sent to it.
 *   36c-1: the rest of the firmware's start-up as Linux's iwl_mvm_up: the
 *        shared memory, the Smart FIFO, Bluetooth coexistence, DQA, the
 *        scanning station and its queue, the PHY contexts, the thermal
 *        commands, LTR and the device's power saving.
 *   36c-2: the firmware's current country (its allowed channels), the
 *        scan's settings, the station's MAC context, the card registered
 *        as wlan0, and the firmware's own scan: passive, every allowed
 *        channel; the frames it hears handed to the Wi-Fi layer. The
 *        driver's thread stays, serving scans and received frames.
 *   36d-1: sending and tuning: a channel's PHY context, the binding, the
 *        session protection; the access point as a station with its
 *        queue; frames as TX commands (management, broadcast through the
 *        scanning station); the firmware's own choice of country
 *        confirmed back.
 *   36d-2: the association in the firmware (the layer's join): the
 *        station's AID and HT, a legacy rate table, the MAC context
 *        associated, the binding's airtime, the Smart FIFO, multicast.
 *   36d-3: QoS data on queues of its own TIDs (DQA: queue 4, then 10-30,
 *        each on its access category's FIFO); the driver's counters in
 *        `net`'s report.
 *   36e-1: Block Ack for receiving (the firmware answers the access
 *        point's bursts; the layer keeps the order).
 *   36e-2: as wide as the access point (40 or 80 MHz: PHY context 0
 *        widened at the association, the station's width).
 *   36e-3: CCMP in the card (set_key: ADD_STA_KEY; frames sealed with the
 *        key in the TX command, opened ones handed up decrypted).
 *   36e-4: VHT rates (LQ_CMD) and rate scaling from the TX responses.
 *   36e-5: bursts for sending (Block Ack; QoS data numbered per TID).
 *   36e-6: the firmware started again when it stops (an error, commands
 *        without a reply, frames stuck), the layer told the link is gone.
 *   36e-7: two streams (rate scaling's ladder).
 *   37f: power saving (powersave = on): once associated, the MAC's power
 *        command with POWER_MANAGEMENT_ENA (the firmware dozes between
 *        beacons, telling the access point; it wakes for traffic and
 *        dozes again 100 ms after the last frame each way), the keep-alive
 *        at least 3 DTIM intervals and 25 s: Linux's iwl_mvm_power_build_cmd
 *        in its default (balanced) scheme, without uAPSD (iwlwifi's default
 *        for a BSS), skipping DTIMs (only its LP scheme) or low-power
 *        receive (by the beacons' rate); off again when leaving.
 *   36f-1: the access point lost (the firmware's missed beacons: the
 *        layer told) and left (`leave`: the station taken down).
 *
 * The firmware is read and started on a thread of the driver's own:
 * start() only looks at the chip, so a failure there costs the boot
 * nothing.
 */
#include "driver.h"
#include "driver_wifi.h"

/* --- registers (BAR 0, "CSR") --- */
#define CSR_HW_IF_CONFIG        0x000
#define   HW_IF_HAP_WAKE_L1A    0x00080000u     /* wake the chip from L1 for host accesses */
#define   HW_IF_NIC_READY       0x00400000u     /* the host owns the chip (it answers 1) */
#define   HW_IF_PREPARE         0x08000000u     /* ask the chip to become ready */
#define CSR_INT_COALESCING      0x004           /* one byte: interrupts gathered, 32 us units */
#define   COALESCING_DEFAULT    0x40            /* 2,048 us */
#define CSR_INT                 0x008           /* the causes; written back to clear them */
#define   INT_FH_RX             0x80000000u     /* the receive ring got something */
#define   INT_HW_ERR            0x20000000u
#define   INT_RX_PERIODIC       0x10000000u
#define   INT_FH_TX             0x08000000u     /* a load into the chip's memory finished */
#define   INT_SW_ERR            0x02000000u     /* the firmware stopped with an error */
#define   INT_RF_KILL           0x00000080u
#define   INT_SW_RX             0x00000008u
#define   INT_WAKEUP            0x00000002u
#define   INT_ALIVE             0x00000001u
#define   INT_RUNNING           (INT_FH_RX | INT_HW_ERR | INT_FH_TX | INT_SW_ERR | INT_RF_KILL | \
                                 INT_SW_RX | INT_WAKEUP | INT_ALIVE | INT_RX_PERIODIC)
#define CSR_INT_MASK            0x00C
#define CSR_FH_INT_STATUS       0x010           /* the DMA engine's causes, cleared the same way */
#define CSR_RESET               0x020
#define   RESET_SW              0x00000080u
#define   RESET_MASTER_OFF      0x00000100u     /* the bus master has stopped */
#define   RESET_STOP_MASTER     0x00000200u
#define CSR_GP_CNTRL            0x024
#define   GP_MAC_CLOCK_READY    0x00000001u
#define   GP_INIT_DONE          0x00000004u
#define   GP_MAC_ACCESS_REQ     0x00000008u
#define   GP_GOING_TO_SLEEP     0x00000010u
#define   GP_RF_KILL_SW         0x08000000u     /* set: the radio switch lets it send */
#define   HW_IF_CONFIG_MASK     0x0000FF0Fu     /* MAC step and dash, PHY type, step, dash, SI bits */
#define CSR_HW_REV              0x028
#define CSR_GIO                 0x03C
#define   GIO_L0S_OFF           0x00000002u     /* (Linux's name says "enabled"; it turns L0s off) */
#define CSR_UCODE_DRV_GP1_CLR   0x05C
#define   GP1_SW_RF_KILL        0x00000002u
#define   GP1_CMD_BLOCKED       0x00000004u
#define CSR_MAC_SHADOW_REG_CTRL 0x0A8
#define   SHADOW_ALL            0x800FFFFFu     /* write pointers without waking the chip */
#define CSR_GIO_CHICKEN         0x100
#define   GIO_L1A_NO_L0S_RX     0x00800000u
#define CSR_DBG_HPET_MEM        0x240
#define   HPET_MEM_VAL          0xFFFF0000u
#define HBUS_TARG_MEM_RADDR     0x40C           /* the chip's memory, read word after word */
#define HBUS_TARG_MEM_WADDR     0x410           /* ... and written */
#define HBUS_TARG_MEM_WDAT      0x418
#define HBUS_TARG_MEM_RDAT      0x41C
#define HBUS_TARG_WRPTR         0x460           /* a queue's write index | queue << 8 */
#define PRPH_WADDR              0x444           /* the periphery's registers, a word at a time */
#define PRPH_RADDR              0x448
#define PRPH_WDAT               0x44C
#define PRPH_RDAT               0x450

/* The periphery (behind PRPH_RADDR). */
#define WFPM_CTRL_REG           0xA03030
#define   WFPM_ENABLE           0x80000000u     /* AUX_MISC reads as filler until it is set */
#define AUX_MISC_REG            0xA200B0        /* 8000 family: the silicon step in bits 27:24 */
#define SCD_SRAM_BASE_ADDR      0xA02C00        /* the scheduler's part of the chip's memory */
#define SCD_DRAM_BASE_ADDR      0xA02C08        /* the byte-count tables, >> 10 */
#define SCD_TXFACT              0xA02C10        /* the scheduler's FIFOs: 0 all off, 0xFF on */
#define SCD_QUEUE_RDPTR0        0xA02C68        /* queue q's: + 4q (q < 20) */
#define SCD_QUEUE_STATUS0       0xA02D0C        /* queue q's: + 4q (q < 20) */
#define   SCD_QUEUE_ACTIVE      (1u << 3)
#define   SCD_QUEUE_WSL         (1u << 4)
#define   SCD_QUEUE_ACT_EN      (1u << 19)
#define   SCD_QUEUE_MSK         0x017F0000u
#define SCD_AGGR_SEL            0xA02E48
#define SCD_EN_CTRL             0xA02E54        /* the queues the scheduler runs */
#define SCD_CONTEXT_MEM         0x600           /* in its memory: 8 bytes a queue, then status */
#define SCD_CLEAR_WORDS         135             /* context, status and translation, 31 queues */
#define WFMP_MAC_ADDR_0         0xA03080        /* the address fused into the chip */
#define WFMP_MAC_ADDR_1         0xA03084
#define SCD_GP_CTRL             0xA02DA8
#define   SCD_ENABLE_31_QUEUES  0x00000001u
#define   SCD_AUTO_ACTIVE       0x00040000u
#define UMAC_CURRENT_PC         0xA05C18        /* where each CPU runs (read through the window) */
#define LMAC_CURRENT_PC         0xA05C1C
#define DBGC_IN_SAMPLE          0xA03C00        /* the debug engine's sampling (Linux stops it first) */
#define DBGC_OUT_CTRL           0xA03C0C
#define SB_CPU_1_STATUS         0xA01E30        /* the boot ROM's verdict on each CPU's image */
#define SB_CPU_2_STATUS         0xA01E34
#define RELEASE_CPU_RESET       0x300C
#define   CPU_RESET_RELEASE     0x01000000u
#define LMPM_CHICK              0xA01FF8
#define   LMPM_EXTENDED_ADDR    0x00000001u     /* loads to 0x40000-0x57FFF */
#define EXTENDED_START          0x40000u
#define EXTENDED_END            0x57FFFu

/* The flow handler: the chip's DMA engine (BAR 0, through the host's access). */
#define FH_TFDIB_CTRL0          0x1948          /* the service channel's (9) buffer: address */
#define FH_TFDIB_CTRL1          0x194C          /* ... high bits << 28 | length */
#define FH_KW_MEM_ADDR          0x197C          /* the keep-warm page, >> 4 */
#define FH_CBBC_0_15            0x19D0          /* each queue's ring, >> 8: queues 0-15 */
#define FH_CBBC_16_19           0x1BF0
#define FH_CBBC_20_31           0x1B20
#define FH_RX_DMA_STATUS        0x1C44          /* bit 24: the receive channel is idle */
#define FH_SRVC_SRAM_ADDR       0x19C8          /* where in the chip a load goes */
#define FH_UCODE_LOAD_STATUS    0x1AF0          /* sections loaded: CPU1 bits 0-15, CPU2 16-31 */
#define FH_RX_STATUS_ADDR       0x1BC0          /* the receive status's address, >> 4 */
#define FH_RX_RING_BASE         0x1BC4          /* the receive descriptors' address, >> 8 */
#define FH_RX_WPTR              0x1BC8          /* descriptors given to the chip (a multiple of 8) */
#define FH_RX_RDPTR             0x1BCC
#define FH_RX_CONFIG            0x1C00
#define   RX_CONFIG_ON          (0x80000000u | 0x00000004u | 0x00001000u | 0x11u << 4 | 8u << 20)
                                /* on, ignore an empty FIFO, interrupts to the host, 4 KiB
                                 * buffers, timeout 0x11, 2^8 descriptors */
#define FH_RX_CONFIG_WPTR       0x1C08
#define FH_RX_FLUSH             0x1C10
#define FH_TCSR_CONFIG0         0x1D00          /* DMA channel ch's: + 0x20 ch */
#define   TCSR_ON_CREDIT        (0x80000000u | 0x00000008u)
#define FH_TX_CHICKEN           0x1E98
#define   TX_CHICKEN_AUTO_RETRY 0x00000002u
#define FH_SRVC_TX_CONFIG       0x1E20          /* channel 9's: pause 0, or on */
#define   SRVC_ON               (0x80000000u | 0x00100000u)     /* on, interrupt at its end */
#define FH_SRVC_TX_BUF_STS      0x1E28
#define FH_TX_STATUS            0x1EB0          /* bit 16 + ch: DMA channel ch is idle */
#define   SRVC_ONE_BUFFER       (1u << 20 | 1u << 12 | 3u)      /* one buffer, index 1, valid */
#define LOAD_CHUNK              0x20000         /* bytes per load: the engine's largest */
#define DMA_LIMIT               (1ull << 36)    /* the 8000 family's addresses are 36 bits */

/* The receive ring (8000 family: one ring of 256 descriptors, each a 4 KiB
 * buffer's address >> 8; the chip writes how far it got to a status in
 * memory). A buffer holds packets, each 64-byte aligned, ended by a marker. */
#define RX_RING                 256
#define TX_QUEUES               31              /* the 8000 family's */
#define TFD_RING_BYTES          (256 * 128)     /* 256 descriptors of 128 bytes, every queue */
#define BC_TABLES_BYTES         (TX_QUEUES * 320 * 2)   /* each queue 256 + 64 counts of 16 bits */

/* Commands (36b-2): queue 0 on FIFO 7, as Linux's mvm; a descriptor's
 * buffer 0 holds the command's first 20 bytes, buffer 1 the rest. */
#define CMD_QUEUE               0
#define CMD_FIFO                7
#define CMD_FRAME_LIMIT         64
#define CMD_FIRST               20
#define CMD_ECHO                0x03
#define CMD_NVM_ACCESS          0x88
#define CMD_TX_ANT              0x98            /* the antennas to send with */
#define CMD_PAGING              0x4F            /* in group 1 (the 8-byte header) */
#define CMD_PHY_DB              0x6C            /* a calibration result back to the run-time image */
#define GROUP_LONG              1
#define GROUP_SYSTEM            2
#define GROUP_PHY_OPS           4
#define GROUP_DATA_PATH         5
/* 36c-1: the rest of Linux's iwl_mvm_up (the commands' numbers) */
#define CMD_SHARED_MEM          0x00            /* group 2 (EXTEND_SHARED_MEM_CFG) */
#define CMD_SF_CFG              0xD1            /* the Smart FIFO */
#define CMD_BT_CONFIG           0x9B
#define CMD_DQA_ENABLE          0x00            /* group 5 */
#define CMD_SCD_QUEUE_CFG       0x1D
#define CMD_ADD_STA             0x18
#define CMD_PHY_CONTEXT         0x08
#define CMD_TEMP_THRESHOLDS     0x04            /* group 4 */
#define CMD_CTDP                0x03            /* group 4: the power budget */
#define CMD_LTR_CONFIG          0xEE
#define CMD_POWER_TABLE         0x77
/* The firmware's capability bits (the file's ENABLED_CAPABILITIES) */
#define CAPA_CT_KILL_BY_FW      74
#define CAPA_TEMP_THS_REPORT    75
#define CAPA_CTDP               76
#define CAPA_BT_MPLUT           67
#define CAPA_EXTEND_SHARED_MEM  80
#define API_STA_TYPE            30              /* (API flags) */
#define AUX_QUEUE               1               /* DQA's queue for the scanning station */
#define FIFO_MCAST              5
#define AUX_STA_ID              1               /* 0 is kept for a network's station, as Linux */
#define MAC_INDEX_AUX           4
#define STA_TYPE_AUX            4
#define NUM_PHY_CTX             3
/* 36c-2: the country, the scan, the station MAC context, receiving */
#define CMD_MCC_UPDATE          0xC8
#define CMD_SCAN_CFG            0x0C            /* group 1 */
#define CMD_SCAN_REQ            0x0D            /* group 1 */
#define CMD_MAC_CONTEXT         0x28
#define NOTIF_RX_PHY            0xC0            /* a received frame's channel and energies */
#define NOTIF_RX_MPDU           0xC1            /* the frame itself */
#define NOTIF_BA                0xC5            /* a burst's Block Ack (36e-5) */
#define NOTIF_MISSED_BEACONS    0xA2            /* (36f-1) the access point's beacons missed */
#define CMD_REMOVE_STA          0x19
#define CMD_TXPATH_FLUSH        0x1E
#define MISSED_BEACONS_MAX      8               /* Linux's IWL_MVM_MISSED_BEACONS_THRESHOLD */
#define NOTIF_SCAN_COMPLETE     0x0F
#define NOTIF_SCAN_ITERATION    0xB5
#define MCC_GET_CURRENT         0x10            /* MCC_UPDATE's source: what the firmware uses */
#define SCAN_REQ_BASE           48              /* the request's version 7 (ADAPTIVE_DWELL) */
#define SCAN_REQ_TAIL           1220            /* schedule, delay, probe template, 20 SSIDs */
#define SCAN_TIMEOUT_US         15000000
#define NVM_CHANNEL_COUNT       51              /* the 8000 family's list, in the NVM's order */
/* 36d-1: tuning, the access point's station, sending */
#define CMD_BINDING             0x2B
#define CMD_TIME_EVENT          0x29
#define NOTIF_TIME_EVENT        0x2A
#define CMD_MAC_POWER           0xA9
#define CMD_TX                  0x1C            /* a frame to send; its response the same number */
#define NOTIF_MCC_CHUB          0xC9            /* the firmware chose a country */
#define MGMT_QUEUE              5               /* DQA's first queue for management and non-QoS */
#define FIFO_VO                 3
#define AP_STA_ID               0
#define SESSION_TU              600             /* Linux's session protection: 600 TU, delay 500 */
#define REQ_CHANNEL             1               /* the layer's calls, done by the driver's thread */
#define TX_PENDING              256             /* frames handed over, not yet on a queue (36e-2:
                                                 * 8 overflowed while a burst went up) */
#define REQ_JOIN                2
#define REQ_BLOCK_ACK           3               /* (36e-1) */
#define REQ_KEY                 4               /* (36e-3) */
#define REQ_LEAVE               5               /* (36f-1) */
#define RESTARTS_QUICK          3               /* (36e-6) restarts within a minute of serving */
#define TWO_STREAMS             1               /* rate scaling's ladder with two streams (36e-7) */
#define CMD_ADD_STA_KEY         0x17
#define API_TKIP_MIC_KEYS       29              /* ADD_STA_KEY version 2 (76 bytes), else 1 (64) */
/* 36d-2: the association in the firmware */
#define CMD_QUOTA               0x2C
#define CMD_LQ                  0x4E            /* the station's rate table */
#define CMD_MCAST_FILTER        0xD0
#define LISTEN_INTERVAL         10              /* (wifid's association request) */
/* 36d-3: data on queues of their own (DQA) */
#define BSS_CLIENT_QUEUE        4               /* the station's first data TID */
#define MIN_DATA_QUEUE          10
#define MAX_DATA_QUEUE          30              /* (31 is the 8000 family's last; 31 queues: 0-30) */
#define RING_HIGH               240             /* frames a ring of 256 holds at most in flight */
#define CMD_PHY_CONFIG          0x6A            /* the radio's setup: calibrations start */
#define NOTIF_INIT_COMPLETE     0x04
#define NOTIF_CALIB_RESULT      0x6B            /* a part of the PHY database */
#define UCODE_REGULAR           0               /* the image types of the file's defaults */
#define UCODE_INIT              1
#define PHY_DB_MAX              48              /* the calibration results kept */

/* Paging (the run-time image): pages of 4 KiB in blocks of 8, block 0 a
 * page holding the paged part's signature; the command's flags: secured,
 * enabled, the last block's pages. */
#define PAGE_BLOCK_BYTES        32768
#define PAGING_BLOCKS_MAX       32
#define PAGING_SECURED          (1u << 9)
#define PAGING_ENABLED          (1u << 8)
#define SEQ_NOTIFICATION        0x8000          /* a sequence's mark: the firmware's own, no reply */

/* The NVM (8000 family): sections read 2 KiB at a time, 32 KiB in all;
 * offsets in 16-bit words. */
#define NVM_SECTIONS            13
#define NVM_CHUNK               2048
#define NVM_MAX                 32768
#define NVM_SW                  1               /* version (32 bits), addresses (word 3) */
#define NVM_REGULATORY          3               /* a flags word per channel, then LAR */
#define NVM_HW                  10
#define NVM_MAC_OVERRIDE        11              /* the address at word 1, if the maker set one */
#define NVM_PHY_SKU             12              /* radio (32 bits), then SKU (word 2) */
#define NVM_STATUS_PAST_END     1               /* a section ended at a 2 KiB boundary */
#define RX_INVALID              0x55550000u     /* the end of a buffer's packets */
#define ALIVE_STATUS_OK         0xCAFE
#define ALIVE_V4_BYTES          116             /* two LMACs; version 3 has one (68 bytes) */

#define PCI_RETRY_TIMEOUT       0x41            /* config byte: 0 (Linux, iwm: PCI retries
                                                 * otherwise disturb the processor's C3) */

/* --- the firmware file (iwl_tlv_ucode_header, then type-length-value parts) --- */
#define FW_NAME                 "iwlwifi-8265-36.ucode"
#define FW_MAGIC                0x0A4C5749u     /* "IWL\n" */
#define FW_HEADER               88              /* zero, magic, 64 bytes of text, ver, build, 8 */
#define TLV_SEC_RT              19              /* the run-time image's sections */
#define TLV_SEC_INIT            20              /* the calibrating image's */
#define TLV_SEC_WOWLAN          21              /* the wake-on-WLAN image's */
#define TLV_DEF_CALIB           22
#define TLV_PHY_SKU             23              /* bands and antennas */
#define TLV_NUM_OF_CPU          27
#define TLV_API_CHANGES_SET     29
#define TLV_ENABLED_CAPA        30
#define TLV_N_SCAN_CHANNELS     31
#define TLV_PAGING              32              /* bytes of the run-time image the host keeps */
#define TLV_FW_VERSION          36
#define TLV_CMD_VERSIONS        48
#define SEP_CPU1_CPU2           0xFFFFCCCCu     /* a section's offset: the second CPU's begin */
#define SEP_PAGING              0xAAAABBBBu     /* the paged part begins */
#define MAX_SECTIONS            24

static const uint8_t nvm_channels[51] = {       /* (the 8000 family's: iwl_ext_nvm_channels) */
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116,
    120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165, 169, 173, 177, 181
};

struct section {                                /* a part of an image, as in the file */
    uint32_t offset;                            /* where in the chip, or a separator */
    const uint8_t *data;
    uint32_t length;
};

struct image {                                  /* one of the file's images, as listed */
    int sections[3];                            /* CPU1, CPU2, paged */
    uint64_t bytes[3];
    struct section list[MAX_SECTIONS];          /* every section, separators too, in order */
    int n;
};

struct chip {
    struct pci_device *pci;
    volatile uint8_t *regs;
    char where[16];
    uint32_t hw_rev;
    uint32_t phy_sku;                           /* the firmware file's PHY SKU */
    bool msi;
    struct spinlock lock;                       /* "intelwifi": events */
    uint32_t events;                            /* CSR_INT causes the handler saw, not yet taken */
    uint64_t interrupts, gone, missed;          /* missed: found in the register, no interrupt */
    struct wait_queue waiters;
    uint32_t *rbd;                              /* the receive descriptors (a page) */
    volatile uint16_t *rx_status;               /* the chip's: closed buffer index (same page) */
    uint8_t *rx_buffer[RX_RING];
    uint32_t rx_read;                           /* the next buffer to look at */
    uint64_t keep_warm;
    uint64_t tx_ring[TX_QUEUES];
    uint32_t scd_base, alive_scd;
    uint64_t bc_tables, cmd_page;
    uint32_t cmd_write;                         /* the command queue's next descriptor */
    bool waiting, replied;                      /* a command's reply */
    uint16_t wait_seq;
    uint32_t reply_length;
    uint8_t reply[4096];
    uint32_t fw_errors;                         /* SW_ERR or HW_ERR seen */
    uint32_t calib_flow[3], calib_event[3];     /* the file's defaults per image type */
    uint8_t nvm_tx_ant, nvm_rx_ant;             /* the NVM's radio word's antennas */
    bool init_complete;
    struct phy_entry {                          /* the calibrations' results (36b-3b sends them) */
        uint16_t type, group, length;
        uint8_t *data;
    } phy_db[PHY_DB_MAX];
    int nphy, phy_dropped;
    bool msi_tried;
    uint64_t rbd_phys;
    struct image rt;                            /* the file's run-time image */
    uint32_t paging_bytes;                      /* the file's paged size */
    uint64_t paging[PAGING_BLOCKS_MAX + 1];     /* the signature's page, then the blocks */
    uint32_t api[4], capa[4];                   /* the file's API flags and capabilities */
    uint32_t scan_slots;                        /* the file's N_SCAN_CHANNELS */
    uint8_t mac[6];                             /* the card's address (the NVM's) */
    uint16_t nvm_flags[NVM_CHANNEL_COUNT];      /* the NVM's flags per channel */
    uint8_t allowed[NVM_CHANNEL_COUNT];         /* the channels the firmware's country allows */
    int nallowed;
    struct net_card *card;                      /* wlan0 (36c-2) */
    struct wifi_card wifi;
    bool registered;
    volatile bool scan_wanted;                   /* the layer asked; the thread starts it */
    bool scanning;
    uint64_t scan_started;
    uint8_t phy[64];                            /* the last RX_PHY: the next frame's channel, energy */
    uint32_t heard, beacons, probe_responses, bad_fcs;
    int channel;                                /* where PHY context 0 is (36d-1) */
    int width, center;                          /* its width in MHz and centre channel (36e-2) */
    bool bound;
    bool ap_added;
    uint8_t ap[6];
    uint32_t q_write[TX_QUEUES];
    uint32_t q_read[TX_QUEUES];                 /* (36e-5) the first slot not yet given back */
    uint64_t *tx_page[TX_QUEUES];               /* a page per slot, made when first used */
    uint16_t tx_fc[TX_QUEUES][256];             /* (for the response's line) the frame's type */
    uint8_t antenna;
    bool te_running;                            /* the session protection (time event) */
    uint32_t te_uid;
    uint64_t te_end_us;
    uint32_t tx_sent, tx_acked, tx_failed;
    uint32_t q_sent[TX_QUEUES], q_acked[TX_QUEUES], q_failed[TX_QUEUES];
    uint32_t ring_full;                         /* frames refused: their ring had no room */
    uint64_t *tx_time[TX_QUEUES];               /* each slot's send time (us; 256 per queue) */
    uint64_t tx_slowest;                        /* the longest wait for a TX response (us) */
    uint32_t tx_over_50ms, tx_over_500ms, most_in_flight;
    uint32_t data_responses, data_tries, rts_failures, data_rate, data_logged;   /* (36e-4) */
    int rs_top, rs_nss;                         /* rate scaling: the table's first rung (36f-4: a
                                                 * column times 10 plus its MCS) */
    uint32_t rs_tries, rs_through, rs_good, rs_ups, rs_downs;
    uint32_t rs_need;                           /* good windows before a try up (3, doubling) */
    uint8_t rs_sr[20];                          /* (36e-5) each rung's last success (percent) */
    uint32_t rs_when[20];                       /* the window it was measured in (0: never) */
    int rs_max1, rs_max2;                       /* (36f-4) each column's highest MCS (-1: none) */
    int rs_col_from;                            /* the rung a try of two streams came from */
    uint32_t rs_col_wait, rs_col_need, rs_col_tries, rs_col_kept;
    bool rs_col_probing;
    uint32_t rs_windows;
    bool rs_probing;                            /* the last change was a try up */
    volatile bool lq_wanted;                    /* a new table for the driver's thread to send */
    uint64_t data_airtime;                      /* (us, the responses' wireless media time) */
    uint64_t slowest_rx, slowest_send, slowest_frame_up;   /* the thread's longest parts (us) */
    uint32_t most_in_rx, most_pending;          /* frames in one rx_collect; waiting at once */
    uint8_t tid_queue[8];                       /* a data TID's queue, 0: none yet (36d-3) */
    uint16_t tid_seq[8];                        /* (36e-5) a TID's next sequence number (ours) */
    uint8_t agg_tids;                           /* TIDs sent in bursts */
    uint16_t agg_window;                        /* the access point's window for them */
    uint32_t agg_responses, agg_frames, ba_notifs, ba_txed, ba_acked, seq_off;
    uint32_t ssn_other;                         /* single responses whose index was not slot + 1 */
    uint32_t silent, silent_told;               /* (36e-6) commands without a reply, in a row; told */
    uint32_t restarts;                          /* the firmware started again */
    volatile bool sleep_wanted, asleep;         /* (37g-6) a sleep asked; the chip stopped for it */
    bool thread_ended;                          /* the driver's thread gave up (never serves) */
    struct wait_queue sleep_wq;
    uint32_t sleeps;
    uint64_t served_at;                         /* when the serving last began */
    char stop_why[64];                          /* why the firmware stopped (this start) */
    char last_stop[64];                         /* why, the last time it was started again */
    uint64_t ap_heard_us;                       /* (36f-1) the access point's last frame */
    uint64_t beacon_loss_at;                    /* when the firmware said its beacons stopped */
    uint32_t missed_most, links_lost, leaves;
    bool link_gone;                             /* lost, told: once until a leave or association */
    int probes_sent;                            /* null frames asking the access point (36f-1) */
    uint32_t probes, probes_answered, false_alarms;
    uint32_t sta_queues;                        /* the access point's station's queue mask */
    uint32_t rx_frames, rx_bad;
    char country_code[3];
    uint32_t fw_major, fw_minor, fw_local;      /* the file's version (TLV_FW_VERSION) */
    struct mutex req_lock;                      /* one request at a time */
    struct wait_queue req_waiters;
    volatile int req_kind;
    volatile bool req_done;
    bool req_ok;
    int req_channel;
    struct spinlock pend_lock;                  /* "intelwifi pending": the frames below */
    volatile uint32_t pend_head, pend_tail;
    uint8_t *pend[TX_PENDING];                  /* WLAN_FRAME_MAX each, from the heap */
    uint16_t pend_len[TX_PENDING];
    struct wlan_ht req_ht;                      /* (a join's) */
    uint16_t req_aid;
    bool req_powersave, powersave;              /* (37f) asked by the join; dozing now */
    uint16_t keep_alive;                        /* (37f) seconds, the last MAC power command's */
    int req_tid;                                /* (a Block Ack's: 36e-1) */
    bool req_on, req_sending;
    uint16_t req_ssn, req_window;
    uint8_t rx_ba;                              /* TIDs whose bursts the firmware answers */
    int req_key_kind, req_key_index;            /* (a key's: 36e-3) */
    uint8_t req_key[16];
    bool ptk_in_card;                           /* the card seals and opens with the pairwise key */
    uint8_t ptk[16];                            /* (the TX command carries it) */
    uint8_t groups_in_card;                     /* group keys it opens with (bit = index) */
    uint64_t tx_pn;                             /* the pairwise key's last packet number sent */
    uint32_t sealed, opened, opened_bad, protected_whole;
    uint32_t batched;                           /* frames handed up in this batch */
    uint32_t pend_full, send_failed;            /* frames refused: no place, or not sent */
    uint32_t rx_data, rx_in_burst, rx_rate;     /* data received: in A-MPDUs; the last's rate */
    uint32_t rx_ba_started, rx_ba_ended;
    bool associated;
    uint16_t aid;
    uint16_t ap_bi;                             /* from the access point's beacons: interval, */
    uint8_t ap_dtim_count, ap_dtim_period;      /* the TIM's count and period, */
    uint64_t ap_tsf;                            /* its timestamp, the chip's time it came, */
    uint32_t ap_device_ts;
    int ap_signal;                              /* and its signal */
    volatile bool chub_pending;
    uint16_t chub_mcc;
    uint8_t chub_source;
    bool alive;
    uint16_t alive_status;
    uint32_t error_log, umac_error_log;
    char others[160];                           /* the other notifications, "group.cmd" */
    int nothers;
    struct image init;                          /* the file's init image */
};

static struct chip chips[2];
static int nchips;

static uint32_t rd(struct chip *c, uint32_t off) { return mmio_read32(c->regs + off); }
static void wr(struct chip *c, uint32_t off, uint32_t v) { mmio_write32(c->regs + off, v); }
static void set_bits(struct chip *c, uint32_t off, uint32_t bits) { wr(c, off, rd(c, off) | bits); }

static uint64_t now_us(void)
{
    return arch_counter() / (arch_counter_hz() / 1000000);
}

/* Wait up to limit_us for (register & mask) == want; the microseconds it
 * took, or -1. */
static int64_t poll(struct chip *c, uint32_t off, uint32_t mask, uint32_t want, uint64_t limit_us)
{
    uint64_t start = now_us();
    for (;;) {
        uint64_t t = now_us() - start;
        if ((rd(c, off) & mask) == want)
            return (int64_t)t;
        if (t > limit_us)
            return -1;
    }
}

static void delay_us(uint64_t us)
{
    uint64_t start = now_us();
    while (now_us() - start < us)
        ;
}

/* The chip taken from its firmware's hands: NIC_READY answers 1 at once
 * when it is free, else PREPARE asks it to finish what it does (up to
 * 150 ms). The microseconds it took, or -1. */
static int64_t make_ready(struct chip *c)
{
    set_bits(c, CSR_HW_IF_CONFIG, HW_IF_NIC_READY);
    if (poll(c, CSR_HW_IF_CONFIG, HW_IF_NIC_READY, HW_IF_NIC_READY, 50) >= 0)
        return 0;
    uint64_t start = now_us();
    set_bits(c, CSR_HW_IF_CONFIG, HW_IF_PREPARE);
    while (now_us() - start < 150000) {
        set_bits(c, CSR_HW_IF_CONFIG, HW_IF_NIC_READY);
        if (poll(c, CSR_HW_IF_CONFIG, HW_IF_NIC_READY, HW_IF_NIC_READY, 50) >= 0)
            return (int64_t)(now_us() - start);
        delay_us(200);
    }
    return -1;
}

/* Power the MAC on, as the 8000 family wants it (Linux's iwl_pcie_apm_init
 * without the older families' steps): the microseconds until its clock
 * ran, or -1. L0s goes off where the firmware turned the link's L1 on (a
 * hardware bug between the two), on otherwise. */
static int64_t wake(struct chip *c)
{
    set_bits(c, CSR_GIO_CHICKEN, GIO_L1A_NO_L0S_RX);
    set_bits(c, CSR_DBG_HPET_MEM, HPET_MEM_VAL);
    set_bits(c, CSR_HW_IF_CONFIG, HW_IF_HAP_WAKE_L1A);
    uint8_t pcie = pci_find_capability(c->pci, 0x10);
    bool l1 = pcie && (pci_read16(c->pci, pcie + 0x10) & 0x2);
    wr(c, CSR_GIO, l1 ? rd(c, CSR_GIO) | GIO_L0S_OFF : rd(c, CSR_GIO) & ~GIO_L0S_OFF);
    set_bits(c, CSR_GP_CNTRL, GP_INIT_DONE);
    return poll(c, CSR_GP_CNTRL, GP_MAC_CLOCK_READY, GP_MAC_CLOCK_READY, 25000);
}

/* The host's access to the chip's inside: the chip kept awake while it is
 * held; false if it would not wake (access_end either way). */
static bool access_begin(struct chip *c)
{
    set_bits(c, CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
    return poll(c, CSR_GP_CNTRL, GP_MAC_CLOCK_READY | GP_GOING_TO_SLEEP,
                GP_MAC_CLOCK_READY, 15000) >= 0;
}

static void access_end(struct chip *c)
{
    wr(c, CSR_GP_CNTRL, rd(c, CSR_GP_CNTRL) & ~GP_MAC_ACCESS_REQ);
}

/* Periphery registers, with the access held (the 8000 family's addresses
 * are 20 bits). */
static uint32_t prph_rd(struct chip *c, uint32_t addr)
{
    wr(c, PRPH_RADDR, (addr & 0x000FFFFF) | (3u << 24));
    return rd(c, PRPH_RDAT);
}

static void prph_wr(struct chip *c, uint32_t addr, uint32_t value)
{
    wr(c, PRPH_WADDR, (addr & 0x000FFFFF) | (3u << 24));
    wr(c, PRPH_WDAT, value);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* The chip's interrupt (MSI): its causes taken and cleared, the thread
 * woken. A chip that is gone reads all ones (or Intel's 0xA5A5A5Ax). */
static void interrupt(void *context)
{
    struct chip *c = context;
    uint32_t causes = rd(c, CSR_INT);
    if (causes == 0xFFFFFFFFu || (causes & 0xFFFFFFF0u) == 0xA5A5A5A0u) {
        c->gone++;
        return;
    }
    wr(c, CSR_FH_INT_STATUS, rd(c, CSR_FH_INT_STATUS));
    wr(c, CSR_INT, causes);
    uint64_t state = spin_lock(&c->lock);
    c->events |= causes;
    c->interrupts++;
    spin_unlock(&c->lock, state);
    wait_queue_wake_all(&c->waiters);
}

static uint32_t take_events(struct chip *c, uint32_t want)
{
    uint64_t state = spin_lock(&c->lock);
    uint32_t seen = c->events & want;
    c->events &= ~seen;
    spin_unlock(&c->lock, state);
    return seen;
}

/* Wait up to ms for any of the causes: the ones seen, 0 if none came. A
 * cause in the register after a tick with no interrupt (MSI missing or
 * lost) counts as missed and is taken from there. */
static uint32_t wait_events(struct chip *c, uint32_t want, uint32_t ms)
{
    uint64_t deadline = timer_ticks() + (uint64_t)ms * TIMER_HZ / 1000 + 1;
    for (;;) {
        uint32_t seen = take_events(c, want);
        if (seen)
            return seen;
        wait_event_until(&c->waiters, c->events & want, timer_ticks() + 1);
        if ((seen = take_events(c, want)))
            return seen;
        uint32_t raw = rd(c, CSR_INT) & want;
        if (raw) {
            wr(c, CSR_FH_INT_STATUS, rd(c, CSR_FH_INT_STATUS));
            wr(c, CSR_INT, raw);
            c->missed++;
            return raw;
        }
        if (timer_ticks() >= deadline)
            return 0;
    }
}

/* The receive ring: descriptors and status in one page, a page per
 * buffer; the chip told where (with the access held), its write pointer
 * at 248 (every buffer but the last 8: it may not catch the read side). */
static bool rx_start(struct chip *c)
{
    if (!c->rbd) {                              /* made once, kept for every image */
        uint64_t page = pmm_alloc_pages_below(1, DMA_LIMIT);
        if (!page)
            return false;
        c->rbd_phys = page;
        c->rbd = phys_to_virt(page);
        memset(c->rbd, 0, 4096);
        c->rx_status = (volatile uint16_t *)((uint8_t *)c->rbd + 2048);
        for (int i = 0; i < RX_RING; i++) {
            uint64_t buffer = pmm_alloc_pages_below(1, DMA_LIMIT);
            if (!buffer)
                return false;
            c->rx_buffer[i] = phys_to_virt(buffer);
            c->rbd[i] = (uint32_t)(buffer >> 8);
        }
    }
    uint64_t page = c->rbd_phys;
    memset((uint8_t *)c->rbd + 2048, 0, 16);
    c->rx_read = 0;
    arch_write_barrier();
    if (!access_begin(c)) {
        access_end(c);
        return false;
    }
    wr(c, FH_RX_CONFIG, 0);
    wr(c, FH_RX_CONFIG_WPTR, 0);
    wr(c, FH_RX_FLUSH, 0);
    wr(c, FH_RX_RDPTR, 0);
    wr(c, FH_RX_WPTR, 0);
    wr(c, FH_RX_RING_BASE, (uint32_t)(page >> 8));
    wr(c, FH_RX_STATUS_ADDR, (uint32_t)((page + 2048) >> 4));
    wr(c, FH_RX_CONFIG, RX_CONFIG_ON);
    access_end(c);
    mmio_write8(c->regs + CSR_INT_COALESCING, COALESCING_DEFAULT);
    wr(c, FH_RX_WPTR, RX_RING - 8);
    return true;
}

/* (36e-4, 36e-5) Rate scaling's decision every 64 tries at the table's
 * top (single frames' first two tries; a burst's frames from its Block
 * Ack), as Linux's rs.c decides: by expected throughput, each MCS's rate
 * times its success, against its neighbours' last measured ones (fresh:
 * within 32 windows). Down when below 15% (SR_FORCE_DECREASE) or when the
 * MCS below carried more; up when the one above carried more, or as a try
 * when it is not known and this one does above 85% for rs_need windows (a
 * try that falls back doubles rs_need, 3 to 96). Bare thresholds (36e-4)
 * drifted down to MCS 1 with bursts: 88% got through on average, single
 * windows fell below 60% and three above 85% rarely came in a row.
 * (36f-4) Two columns, as Linux's: one stream (rungs 0-9) and two (10-19),
 * up and down within one; from one stream, every rs_col_need windows (8,
 * doubling to 128 after a try that fell back) a window with two streams
 * at the lowest MCS that carries more, kept if it carried more than the
 * rung it came from; with two streams, a fresh one-stream rung that
 * carries more takes it back. The single ladder of 36e-7 reached two
 * streams only past one stream's MCS 9, which at 80 MHz rarely gets
 * through: uploads stayed at one stream MCS 7. */
static uint32_t rung_rate(int r)
{
    static const uint16_t rate1[10] = { 15, 30, 45, 60, 90, 120, 135, 150, 180, 200 };
    return (uint32_t)(r / 10 + 1) * rate1[r % 10];
}

static bool rung_fresh(const struct chip *c, int r)
{
    return r >= 0 && r < 20 && c->rs_when[r] && c->rs_windows - c->rs_when[r] <= 32;
}

static void rs_decide(struct chip *c)
{
    if (c->rs_tries < 64 || c->rs_top < 0)
        return;
    int m = c->rs_top, was = c->rs_top, col = m / 10, k = m % 10;
    int topc = col ? c->rs_max2 : c->rs_max1;
    uint32_t percent = c->rs_through * 100 / c->rs_tries;
    if (percent > 100)
        percent = 100;
    /* (36e-7) A rung's success is a running average (a quarter the new
     * window) while it is fresh: with bursts a window is one or two of
     * them, and one lost Block Ack read 0% and forced it down. */
    bool fresh = rung_fresh(c, m);
    c->rs_windows++;
    c->rs_sr[m] = (uint8_t)(fresh ? (3u * c->rs_sr[m] + percent) / 4 : percent);
    c->rs_when[m] = c->rs_windows;
    percent = c->rs_sr[m];
    uint32_t tp = rung_rate(m) * c->rs_sr[m];
    bool below = k > 0 && rung_fresh(c, m - 1), above = k < topc && rung_fresh(c, m + 1);
    if (c->rs_col_probing) {                    /* a try of two streams: its first window */
        c->rs_col_probing = false;
        int from = c->rs_col_from;
        if (rung_fresh(c, from) && rung_rate(from) * c->rs_sr[from] >= tp) {
            c->rs_top = from;                   /* it carried less: back */
            if (c->rs_col_need < 128)
                c->rs_col_need *= 2;
        } else {
            c->rs_col_need = 8, c->rs_col_kept++;
        }
    } else if (k > 0 && (percent < 15 || (below && rung_rate(m - 1) * c->rs_sr[m - 1] > tp))) {
        c->rs_top--, c->rs_downs++, c->rs_good = 0;
        if (c->rs_probing && c->rs_need < 96)
            c->rs_need *= 2;
        c->rs_probing = false;
    } else if (above && rung_rate(m + 1) * c->rs_sr[m + 1] > tp) {
        c->rs_top++, c->rs_ups++, c->rs_good = 0;
    } else if (!above && k < topc && percent > 85 && ++c->rs_good >= c->rs_need) {
        c->rs_top++, c->rs_ups++, c->rs_good = 0;
        c->rs_probing = true;
    } else {
        if (c->rs_probing)                      /* (the try up held a window) */
            c->rs_need = 3, c->rs_probing = false;
        if (percent <= 85)
            c->rs_good = 0;
        int best = -1;                          /* (two streams: one stream carrying more?) */
        for (int r = 0; col == 1 && r <= c->rs_max1; r++)
            if (rung_fresh(c, r) && rung_rate(r) * c->rs_sr[r] > tp &&
                (best < 0 || rung_rate(r) * c->rs_sr[r] > rung_rate(best) * c->rs_sr[best]))
                best = r;
        if (best >= 0 || (col == 1 && k == 0 && percent < 15)) {
            c->rs_top = best >= 0 ? best : 0;
        } else if (col == 0 && c->rs_max2 >= 0 && ++c->rs_col_wait >= c->rs_col_need) {
            int k2 = 0;                         /* two streams: the lowest that carries more */
            while (k2 < c->rs_max2 && rung_rate(10 + k2) <= rung_rate(m))
                k2++;
            c->rs_col_wait = 0, c->rs_col_tries++;
            c->rs_col_from = m;
            c->rs_top = 10 + k2;
            c->rs_col_probing = true;
        }
    }
    if (c->rs_top != was)
        c->lq_wanted = true;
    c->rs_tries = c->rs_through = 0;
}

/* (36e-5) The firmware gave back queue q's slots up to (not including)
 * upto: each one's time from the ring to here measured; the frames
 * counted done. */
static uint32_t reclaim(struct chip *c, uint32_t q, uint32_t upto)
{
    uint32_t n = 0;
    upto &= 255;
    if (q >= TX_QUEUES || ((upto - c->q_read[q]) & 255) > ((c->q_write[q] - c->q_read[q]) & 255))
        return 0;                               /* (not within what is in flight) */
    for (; c->q_read[q] != upto; c->q_read[q] = (c->q_read[q] + 1) & 255, n++) {
        uint32_t slot = c->q_read[q];
        if (c->tx_time[q] && c->tx_time[q][slot]) {
            uint64_t took = now_us() - c->tx_time[q][slot];
            c->tx_time[q][slot] = 0;
            if (took > c->tx_slowest)
                c->tx_slowest = took;
            c->tx_over_50ms += took > 50000;
            c->tx_over_500ms += took > 500000;
        }
    }
    return n;
}

static uint32_t header_length(const uint8_t *f);

/* A packet from the firmware: "alive" read, anything else listed. */
static void packet(struct chip *c, const uint8_t *p, uint32_t size)
{
    uint8_t cmd = p[4], group = p[5];
    const uint8_t *d = p + 8;
    uint32_t length = size >= 4 ? size - 4 : 0;
    if (cmd == 1 && group == 0 && length >= 68) {
        bool two = length == ALIVE_V4_BYTES;
        const uint8_t *umac = d + 4 + (two ? 96 : 48);
        c->alive = true;
        c->alive_status = le16(d);
        c->error_log = le32(d + 4 + 16);
        c->umac_error_log = le32(umac + 8);
        c->alive_scd = le32(d + 4 + 36);
        kprintf("  intelwifi:     %s: alive: status 0x%X%s, flags 0x%X; LMAC %u.%u (type %u, "
                "subtype %u)%s, UMAC %u.%u; error log at 0x%X, events at 0x%X, UMAC error log "
                "at 0x%X, scheduler at 0x%X\n", c->where, c->alive_status,
                c->alive_status == ALIVE_STATUS_OK ? " (ok)" : " (not ok)", le16(d + 2),
                le32(d + 4), le32(d + 8), d[4 + 9], d[4 + 8], two ? " and a second" : "",
                le32(umac), le32(umac + 4), c->error_log, le32(d + 4 + 20), c->umac_error_log,
                le32(d + 4 + 36));
        return;
    }
    uint16_t seq = le16(p + 6);
    if (c->waiting && !(seq & SEQ_NOTIFICATION) && seq == c->wait_seq) {
        c->reply_length = length < sizeof(c->reply) ? length : sizeof(c->reply);
        memcpy(c->reply, d, c->reply_length);
        c->replied = true;
        return;
    }
    if (group == 0 && cmd == NOTIF_CALIB_RESULT && length >= 4) {
        uint16_t type = le16(d), bytes = le16(d + 2);
        if (bytes <= length - 4 && c->nphy < PHY_DB_MAX) {
            struct phy_entry *e = &c->phy_db[c->nphy];
            e->type = type;
            e->group = bytes >= 2 && (type == 4 || type == 5) ? le16(d + 4) : 0;
            e->length = bytes;
            if ((e->data = kmalloc(bytes ? bytes : 1))) {
                memcpy(e->data, d + 4, bytes);
                c->nphy++;
                return;
            }
        }
        c->phy_dropped++;
        return;
    }
    if (group == 0 && cmd == NOTIF_RX_PHY) {
        uint32_t n = length < sizeof(c->phy) ? length : (uint32_t)sizeof(c->phy);
        memset(c->phy, 0, sizeof(c->phy));
        memcpy(c->phy, d, n);
        return;
    }
    if (group == 0 && cmd == NOTIF_RX_MPDU && length >= 4) {
        /* The frame (byte count, assist, the frame, a status word): its
         * channel and the antennas' energies from the RX_PHY before it;
         * the signal is the strongest antenna's (dBm). */
        uint32_t bytes = le16(d);
        if (4 + bytes + 4 > length)
            return;
        uint32_t status = le32(d + 4 + bytes);
        if ((status & 3) != 3) {                /* the FCS wrong, or an overrun */
            c->bad_fcs++;
            c->rx_bad++;
            return;
        }
        c->rx_frames++;
        uint32_t energy = le32(c->phy + 28);
        int a = (int)(energy & 0xFF), b = (int)((energy >> 8) & 0xFF);
        int signal = a && b ? -(a < b ? a : b) : a ? -a : b ? -b : 0;
        int channel = le16(c->phy + 22);
        const uint8_t *f = d + 4;
        c->heard++;
        if (bytes >= 1 && (f[0] & 0xFC) == 0x80)
            c->beacons++;
        else if (bytes >= 1 && (f[0] & 0xFC) == 0x50)
            c->probe_responses++;
        if (c->ap_added && bytes >= 16 && !memcmp(f + 10, c->ap, 6)) {
            if (signal)
                c->ap_signal = signal;          /* (36e-4: any frame of it, not only beacons) */
            c->ap_heard_us = now_us();          /* (36f-1) */
        }
        if (c->ap_added && bytes >= 36 && (f[0] & 0xFC) == 0x80 && !memcmp(f + 16, c->ap, 6)) {
            /* Our access point's beacon: what the association needs */
            c->ap_tsf = (uint64_t)le32(f + 24) | (uint64_t)le32(f + 28) << 32;
            c->ap_bi = le16(f + 32);
            c->ap_device_ts = le32(c->phy + 4);
            c->ap_signal = signal;
            for (uint32_t at = 36; at + 2 <= bytes && at + 2 + f[at + 1] <= bytes; at += 2 + f[at + 1])
                if (f[at] == 5 && f[at + 1] >= 2) {
                    c->ap_dtim_count = f[at + 2];
                    c->ap_dtim_period = f[at + 3];
                }
        }
        if (bytes >= 24 && (f[0] & 0x0C) == 0x08) {
            /* (36e-1) RX_PHY's rate_n_flags (56) and phy_flags (20: bit 7,
             * part of an A-MPDU) of the data frames: how they come */
            c->rx_data++;
            c->rx_in_burst += (le16(c->phy + 20) >> 7) & 1;
            c->rx_rate = le32(c->phy + 56);
        }
        uint32_t hl = bytes >= 24 ? header_length(f) : 0;
        if (c->registered && hl && (f[1] & 0x40) && (status & 0x700) == 0x200 &&
            bytes >= hl + 16) {
            /* (36e-3) Opened by the card (CCM; its MIC checked, bit 6):
             * the CCMP header and MIC still there, as Linux's mac80211
             * gets them; the layer gets the header and the plain text, the
             * packet number and the key's index for its replay check. */
            const uint8_t *iv = f + hl;
            uint64_t pn = (uint64_t)iv[0] | (uint64_t)iv[1] << 8 | (uint64_t)iv[4] << 16 |
                          (uint64_t)iv[5] << 24 | (uint64_t)iv[6] << 32 | (uint64_t)iv[7] << 40;
            int index = iv[3] >> 6;
            bool bad = !(status & 0x40);
            uint8_t *moved = (uint8_t *)f + 8;  /* (the chip's buffer is ours until handed back) */
            for (uint32_t i = hl; i-- > 0;)       /* (later in the buffer: from the end) */
                moved[i] = f[i];
            c->opened++;
            c->opened_bad += bad;
            uint64_t t0 = now_us();
            wlan_receive_decrypted(c->card, moved, bytes - 16, channel, signal, pn, index, bad);
            if (now_us() - t0 > c->slowest_frame_up)
                c->slowest_frame_up = now_us() - t0;
            c->batched++;
        } else if (c->registered && bytes >= 24) {
            c->protected_whole += (f[1] & 0x40) != 0;
            uint64_t t0 = now_us();
            wlan_receive(c->card, f, bytes, channel, signal);
            if (now_us() - t0 > c->slowest_frame_up)
                c->slowest_frame_up = now_us() - t0;
            c->batched++;
        }
        return;
    }
    if ((group == 0 || group == GROUP_LONG) && cmd == NOTIF_SCAN_COMPLETE && length >= 7) {
        if (c->scanning) {
            c->scanning = false;
            kprintf("  intelwifi:     %s: scan: done in %lu ms (status %u, %u iteration(s)): %u "
                    "frame(s) heard, %u beacon(s), %u probe response(s), %u with a bad FCS\n",
                    c->where, (now_us() - c->scan_started) / 1000, d[6], d[5] + 1u, c->heard,
                    c->beacons, c->probe_responses, c->bad_fcs);
            if (c->registered)
                wlan_scan_done(c->card);
        }
        return;
    }
    if ((group == 0 || group == GROUP_LONG) && cmd == NOTIF_SCAN_ITERATION)
        return;
    if (group == 0 && cmd == CMD_TX && (seq >> 8 & 0x1F) != CMD_QUEUE && length >= 40) {
        /* A frame's end (TX_RSP version 3): its status (0x01 sent and
         * acknowledged), the tries; a line for each management frame. */
        uint32_t q = seq >> 8 & 0x1F, slot = seq & 0xFF, status = le16(d + 36) & 0xFF;
        uint16_t fc = c->tx_fc[q][slot];
        if (d[0] > 1) {
            /* (36e-5) A burst's response (frame_count frames, a status
             * each): nothing given back yet, the BA notification does. */
            c->agg_responses++;
            c->agg_frames += d[0];
            c->data_rate = le32(d + 4);
            return;
        }
        *(status == 1 ? &c->tx_acked : &c->tx_failed) += 1;
        if (q < TX_QUEUES)
            *(status == 1 ? &c->q_acked[q] : &c->q_failed[q]) += 1;
        /* (36e-5) the slots given back: up to the SCD's index after the
         * status entries (Linux's iwl_mvm_get_scd_ssn), else this one */
        if (length >= 44) {
            c->ssn_other += (le32(d + 40) & 0xFF) != ((slot + 1) & 0xFF);
            reclaim(c, q, le32(d + 40) & 0xFF);
        }
        reclaim(c, q, slot + 1);                /* (at least this one: nothing if past it) */
        if ((fc & 0xFC) == 0x48 && status == 1) {
            c->ap_heard_us = now_us();          /* (36f-1) a probe answered: it is there */
            c->probes_answered++;
        }
        if ((fc & 0x0C) == 0x08) {
            /* (36e-4) TX_RSP v3: failure_rts (2), failure_frame (3), the
             * rate that went (4), wireless media time (8, us) */
            c->data_responses++;
            c->data_tries += d[3] + 1u;
            c->rts_failures += d[2];
            c->data_rate = le32(d + 4);
            c->data_airtime += le16(d + 8);
            if (c->associated && c->req_ht.on) {
                /* (36e-4) Rate scaling: the table's first two entries are
                 * its top rate, so a frame's tries tell how often that
                 * rate got through: every 64 tries there, below 60% one
                 * MCS down, above 85% rs_need times running one up (Linux's
                 * rs.c: SR_FORCE_DECREASE 15%, SR_NO_DECREASE 85%; ours
                 * goes down sooner, as each lost try costs a backoff). A
                 * try up that falls back doubles rs_need (3 to 96): it
                 * cost lost tries 8 times a second; one that holds sets
                 * it to 3 again. */
                uint32_t tries = d[3] + 1u;
                c->rs_tries += tries < 2 ? tries : 2;
                c->rs_through += status == 1 && tries <= 2;
                rs_decide(c);
            }
            if (c->associated && c->data_logged < 8) {
                c->data_logged++;
                kprintf("  intelwifi:     %s: a data frame on queue %u: status 0x%X after %u "
                        "tr%s (RTS failed %u), rate 0x%X, %u us on the air\n", c->where, q, status,
                        d[3] + 1u, d[3] ? "ies" : "y", d[2], le32(d + 4), le16(d + 8));
            }
        }
        if ((fc & 0x0C) == 0x00 || status != 1) {
            static const char *const names[16] = { "association request", "association response",
                "reassociation request", "reassociation response", "probe request",
                "probe response", "6", "7", "beacon", "ATIM", "disassociation",
                "authentication", "deauthentication", "action", "action (no ack)", "15" };
            kprintf("  intelwifi:     %s: sent %s on queue %u: status 0x%X after %u tr%s, rate "
                    "0x%X\n", c->where, (fc & 0x0C) == 0 ? names[(fc >> 4) & 0xF] : "a data frame",
                    q, status, d[3] + 1u, d[3] ? "ies" : "y", le32(d + 4));
        }
        return;
    }
    if (group == 0 && cmd == NOTIF_MISSED_BEACONS && length >= 20) {
        /* (36f-1) struct iwl_missed_beacons_notif: our MAC (0), the
         * beacons missed since anything came (4), in a row (8). Over 8
         * Linux's mac80211 probes the access point, and so does the serving
         * loop (probe_ap; a second without its answer or any frame of it:
         * the link is lost). Waiting for a frame alone failed: right after
         * an association the firmware passes few beacons up. */
        uint32_t since = le32(d + 4), row = le32(d + 8);
        if (row > c->missed_most)
            c->missed_most = row;
        if (le32(d) == 0 && c->associated && since > MISSED_BEACONS_MAX && !c->beacon_loss_at &&
            !c->link_gone) {
            c->beacon_loss_at = now_us();
            c->probes_sent = 0;                 /* (the serving loop asks the access point) */
            kprintf("  intelwifi:     %s: the access point's beacons missed: %u since anything "
                    "came, %u in a row\n", c->where, since, row);
        }
        return;
    }
    if (group == 0 && cmd == NOTIF_BA && length >= 28) {
        /* (36e-5) A burst's Block Ack (struct iwl_mvm_ba_notif): queue
         * scd_flow (20) given back up to scd_ssn (22); txed (24) frames
         * sent, txed_2_done (25) acknowledged; rate scaling counts them as
         * tries at the top rate. */
        uint32_t q = le16(d + 20), done = reclaim(c, q, le16(d + 22));
        c->ba_notifs++;
        c->ba_txed += d[24];
        c->ba_acked += d[25];
        c->tx_acked += done;
        if (q < TX_QUEUES)
            c->q_acked[q] += done;
        if (c->associated && c->req_ht.on) {
            c->rs_tries += d[24];
            c->rs_through += d[25];
            rs_decide(c);
        }
        return;
    }
    if (group == 0 && cmd == NOTIF_TIME_EVENT && length >= 24) {
        if ((le32(d + 16) & 2) && le32(d + 8) == c->te_uid)
            c->te_running = false;
        kprintf("  intelwifi:     %s: session protection %s (event %u, status %u)\n", c->where,
                le32(d + 16) & 1 ? "started" : le32(d + 16) & 2 ? "ended" : "changed",
                le32(d + 8), le32(d + 20));
        return;
    }
    if (group == 0 && cmd == NOTIF_MCC_CHUB && length >= 3) {
        c->chub_mcc = le16(d);
        c->chub_source = d[2];
        c->chub_pending = true;
        return;
    }
    if (group == 0 && cmd == NOTIF_INIT_COMPLETE) {
        c->init_complete = true;
        return;
    }
    if (c->nothers < 24) {
        size_t used = strlen(c->others);
        ksnprintf(c->others + used, sizeof(c->others) - used, " %X.%X", group, cmd);
    }
    c->nothers++;
}

/* Every buffer the chip closed since the last look: its packets, then the
 * write pointer moved on (the buffers stay where they are). */
static void rx_collect(struct chip *c)
{
    arch_read_barrier();
    uint32_t closed = c->rx_status[0] & (RX_RING - 1);
    arch_read_barrier();
    /* (36e-1) A burst is a batch for the network stack (one ACK per
     * connection at its end, at most NET_BATCH_MAX frames each): TCP's
     * acknowledgements come back to this thread's 8 places for frames,
     * which a burst of 64 answered one by one overflowed. */
    net_batch_begin();
    c->batched = 0;
    while (c->rx_read != closed) {
        const uint8_t *b = c->rx_buffer[c->rx_read];
        for (uint32_t at = 0; at + 12 < 4096;) {
            uint32_t flags = le32(b + at);
            if (flags == RX_INVALID)
                break;
            uint32_t size = flags & 0x3FFF;
            if (size < 4 || at + 4 + size > 4096)
                break;
            packet(c, b + at, size);
            if (c->batched >= NET_BATCH_MAX) {
                net_batch_end();
                net_batch_begin();
                c->batched = 0;
            }
            at += (size + 4 + 63) & ~63u;
        }
        c->rx_read = (c->rx_read + 1) & (RX_RING - 1);
    }
    net_batch_end();
    wr(c, FH_RX_WPTR, ((c->rx_read + RX_RING - 1) & (RX_RING - 1)) & ~7u);
}

/* One piece of a section into the chip's memory through the service
 * channel, then its end awaited (Linux allows 5 s). */
static bool load_chunk(struct chip *c, uint32_t to, uint64_t from, uint32_t bytes)
{
    /* An end seen twice (the interrupt and the register both) must not
     * count for this load: it would go on while the chip still reads. */
    take_events(c, INT_FH_TX);
    if (!access_begin(c)) {
        access_end(c);
        return false;
    }
    wr(c, FH_SRVC_TX_CONFIG, 0);
    wr(c, FH_SRVC_SRAM_ADDR, to);
    wr(c, FH_TFDIB_CTRL0, (uint32_t)from);
    wr(c, FH_TFDIB_CTRL1, (uint32_t)((from >> 32) & 0xF) << 28 | bytes);
    wr(c, FH_SRVC_TX_BUF_STS, SRVC_ONE_BUFFER);
    wr(c, FH_SRVC_TX_CONFIG, SRVC_ON);
    access_end(c);
    return wait_events(c, INT_FH_TX, 5000) != 0;
}

static void prph_change(struct chip *c, uint32_t addr, uint32_t set, uint32_t clear)
{
    if (access_begin(c))
        prph_wr(c, addr, (prph_rd(c, addr) & ~clear) | set);
    access_end(c);
}

static uint32_t direct_rd(struct chip *c, uint32_t off)
{
    uint32_t v = access_begin(c) ? rd(c, off) : 0xFFFFFFFFu;
    access_end(c);
    return v;
}

static void direct_wr(struct chip *c, uint32_t off, uint32_t v)
{
    if (access_begin(c))
        wr(c, off, v);
    access_end(c);
}

struct loader {
    uint8_t *buffer;                            /* LOAD_CHUNK bytes (or a page) in a row */
    uint64_t phys;
    uint32_t size;
    int chunks, sections;
    uint64_t bytes;
};

/* One CPU's sections (Linux's 8000 order: up to the next separator), each
 * one's bit set in FH_UCODE_LOAD_STATUS after it, then all of them. */
static bool load_cpu(struct chip *c, struct loader *l, const struct image *im, int cpu, int *index)
{
    int shift = cpu == 1 ? 0 : 16;
    uint32_t bit = 1;
    for (; *index < im->n; (*index)++) {
        const struct section *s = &im->list[*index];
        if (s->offset == SEP_CPU1_CPU2 || s->offset == SEP_PAGING)
            break;
        for (uint32_t done = 0; done < s->length;) {
            uint32_t n = s->length - done < l->size ? s->length - done : l->size;
            uint32_t to = s->offset + done;
            bool extended = to >= EXTENDED_START && to <= EXTENDED_END;
            if (extended)
                prph_change(c, LMPM_CHICK, LMPM_EXTENDED_ADDR, 0);
            memcpy(l->buffer, s->data + done, n);
            arch_write_barrier();
            bool ok = load_chunk(c, to, l->phys, n);
            if (extended)
                prph_change(c, LMPM_CHICK, 0, LMPM_EXTENDED_ADDR);
            if (!ok) {
                kprintf("  intelwifi:     %s: CPU%d section %d (%u bytes to 0x%X): the load of "
                        "0x%X did not end in 5 s\n", c->where, cpu, *index, s->length, s->offset,
                        to);
                return false;
            }
            l->chunks++;
            done += n;
        }
        l->sections++;
        l->bytes += s->length;
        direct_wr(c, FH_UCODE_LOAD_STATUS, direct_rd(c, FH_UCODE_LOAD_STATUS) | bit << shift);
        bit = bit << 1 | 1;
    }
    wr(c, CSR_INT_MASK, INT_RUNNING);
    direct_wr(c, FH_UCODE_LOAD_STATUS, cpu == 1 ? 0xFFFF : 0xFFFFFFFFu);
    return true;
}

/* The boot ROM's state for each CPU's image (Linux prints them when
 * "alive" does not come). */
static void secure_boot(struct chip *c, uint32_t *cpu1, uint32_t *cpu2)
{
    *cpu1 = *cpu2 = 0xFFFFFFFFu;
    if (access_begin(c)) {
        *cpu1 = prph_rd(c, SB_CPU_1_STATUS);
        *cpu2 = prph_rd(c, SB_CPU_2_STATUS);
    }
    access_end(c);
}

/* The chip's own memory, words read in a row (the firmware's logs). */
static void mem_read(struct chip *c, uint32_t addr, uint32_t *words, int n)
{
    if (access_begin(c)) {
        wr(c, HBUS_TARG_MEM_RADDR, addr);
        for (int i = 0; i < n; i++)
            words[i] = rd(c, HBUS_TARG_MEM_RDAT);
    }
    access_end(c);
}

/* The firmware's error logs, as Linux prints them (iwl_mvm_dump_nic_error_log):
 * the LMAC's (its error ID: 0x71 means the UMAC failed, whose own log
 * then says why) and the UMAC's, each with the last host command it
 * took. */
static void error_logs(struct chip *c)
{
    uint32_t l[32] = { 0 }, u[16] = { 0 };
    mem_read(c, c->error_log, l, 32);
    mem_read(c, c->umac_error_log, u, 16);
    kprintf("  intelwifi:     %s: LMAC error log at 0x%X: valid %u, error 0x%X, TRM 0x%X 0x%X, "
            "blink2 0x%X, ilink 0x%X 0x%X, data 0x%X 0x%X 0x%X, version %u.%u, log pc 0x%X, last "
            "host command 0x%X, last handled 0x%X\n", c->where, c->error_log, l[0], l[1], l[2], l[3],
            l[4], l[5], l[6], l[7], l[8], l[9], l[16], l[17], l[20], l[23], l[29]);
    kprintf("  intelwifi:     %s: UMAC error log at 0x%X: valid %u, error 0x%X, blink 0x%X 0x%X, "
            "ilink 0x%X 0x%X, data 0x%X 0x%X 0x%X, version %u.%u, last host command 0x%X, ISR 0x%X\n",
            c->where, c->umac_error_log, u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9],
            u[10], u[13], u[14]);
}

/* The chip's memory written word after word (0s when words is 0), with
 * the access held. */
static void mem_write_held(struct chip *c, uint32_t addr, const uint32_t *words, int n)
{
    wr(c, HBUS_TARG_MEM_WADDR, addr);
    for (int i = 0; i < n; i++)
        wr(c, HBUS_TARG_MEM_WDAT, words ? words[i] : 0);
}

/* 36b-2: the transmit side after "alive", as Linux's iwl_pcie_tx_start:
 * the scheduler's context cleared, its byte-count tables given, the
 * command queue enabled (inactive while set up, no aggregation, window
 * and frame limit 64, on its FIFO, the only queue the scheduler runs),
 * then every FIFO and DMA channel on. */
static bool tx_start(struct chip *c)
{
    uint32_t pages = (BC_TABLES_BYTES + 4095) / 4096;
    if (!c->bc_tables && !(c->bc_tables = pmm_alloc_pages_below(pages, DMA_LIMIT)))
        return false;
    if (!c->cmd_page && !(c->cmd_page = pmm_alloc_pages_below(1, DMA_LIMIT)))
        return false;
    memset(phys_to_virt(c->bc_tables), 0, pages * 4096);
    if (!access_begin(c)) {
        access_end(c);
        return false;
    }
    c->scd_base = prph_rd(c, SCD_SRAM_BASE_ADDR);
    mem_write_held(c, c->scd_base + SCD_CONTEXT_MEM, 0, SCD_CLEAR_WORDS);
    prph_wr(c, SCD_DRAM_BASE_ADDR, (uint32_t)(c->bc_tables >> 10));
    prph_wr(c, SCD_EN_CTRL, 0);
    prph_wr(c, SCD_QUEUE_STATUS0 + 4 * CMD_QUEUE, SCD_QUEUE_ACT_EN);
    prph_wr(c, SCD_AGGR_SEL, prph_rd(c, SCD_AGGR_SEL) & ~(1u << CMD_QUEUE));
    c->cmd_write = 0;
    wr(c, HBUS_TARG_WRPTR, 0 | CMD_QUEUE << 8);
    prph_wr(c, SCD_QUEUE_RDPTR0 + 4 * CMD_QUEUE, 0);
    uint32_t context[2] = { 0, CMD_FRAME_LIMIT | (uint32_t)CMD_FRAME_LIMIT << 16 };
    mem_write_held(c, c->scd_base + SCD_CONTEXT_MEM + 8 * CMD_QUEUE, context, 2);
    prph_wr(c, SCD_QUEUE_STATUS0 + 4 * CMD_QUEUE,
            SCD_QUEUE_ACTIVE | CMD_FIFO | SCD_QUEUE_WSL | SCD_QUEUE_MSK);
    prph_wr(c, SCD_EN_CTRL, 1u << CMD_QUEUE);
    prph_wr(c, SCD_TXFACT, 0xFF);
    for (int ch = 0; ch < 8; ch++)
        wr(c, FH_TCSR_CONFIG0 + 0x20 * ch, TCSR_ON_CREDIT);
    wr(c, FH_TX_CHICKEN, rd(c, FH_TX_CHICKEN) | TX_CHICKEN_AUTO_RETRY);
    access_end(c);
    return true;
}

/* A descriptor's buffer n: its address (36 bits) and length. */
static void set_buffer(uint8_t *tfd, int n, uint64_t addr, uint32_t length)
{
    uint8_t *tb = tfd + 4 + 6 * n;
    uint16_t hi_n_len = (uint16_t)(length << 4 | ((addr >> 32) & 0xF));
    tb[0] = (uint8_t)addr;
    tb[1] = (uint8_t)(addr >> 8);
    tb[2] = (uint8_t)(addr >> 16);
    tb[3] = (uint8_t)(addr >> 24);
    tb[4] = (uint8_t)hi_n_len;
    tb[5] = (uint8_t)(hi_n_len >> 8);
    tfd[3] = (uint8_t)(n + 1);
}

/* One command on the command queue, its reply awaited (Linux allows 2 s):
 * the reply's length in c->reply, or -1. Group 0 has a 4-byte header
 * (command, group, sequence), the others 8 (and the data's length, a
 * reserved byte, a version). The command page holds the first 20 bytes
 * at 0 and the whole at 256. */
static int command(struct chip *c, uint8_t cmd, uint8_t group, const void *data, uint32_t length,
                   uint64_t *us)
{
    uint8_t *page = phys_to_virt(c->cmd_page), *whole = page + 256;
    uint32_t index = c->cmd_write, header = group ? 8 : 4, total = header + length;
    uint16_t seq = (uint16_t)(CMD_QUEUE << 8 | index);
    if (total > 4096 - 256)
        return -1;
    whole[0] = cmd;
    whole[1] = group;
    whole[2] = (uint8_t)seq;
    whole[3] = (uint8_t)(seq >> 8);
    if (group) {
        whole[4] = (uint8_t)length;
        whole[5] = (uint8_t)(length >> 8);
        whole[6] = 0;
        whole[7] = 0;
    }
    memcpy(whole + header, data, length);
    uint32_t first = total < CMD_FIRST ? total : CMD_FIRST;
    memcpy(page, whole, first);
    uint8_t *tfd = (uint8_t *)phys_to_virt(c->tx_ring[CMD_QUEUE]) + index * 128;
    memset(tfd, 0, 128);
    set_buffer(tfd, 0, c->cmd_page, first);
    if (total > first)
        set_buffer(tfd, 1, c->cmd_page + 256 + first, total - first);
    arch_write_barrier();
    c->wait_seq = seq;
    c->replied = false;
    c->waiting = true;
    c->cmd_write = (index + 1) & 255;
    uint64_t start = now_us();
    wr(c, HBUS_TARG_WRPTR, c->cmd_write | CMD_QUEUE << 8);
    while (!c->replied && now_us() - start < 2000000) {
        uint32_t seen = wait_events(c, INT_RUNNING, 20);
        if (seen & (INT_SW_ERR | INT_HW_ERR))
            c->fw_errors++;
        rx_collect(c);
    }
    c->waiting = false;
    *us = now_us() - start;
    c->silent = c->replied ? 0 : c->silent + 1;  /* (36e-6: two in a row stop the serving) */
    return c->replied ? (int)c->reply_length : -1;
}

struct nvm {
    uint8_t *data;                              /* NVM_MAX bytes: the sections one after another */
    uint32_t used;
    uint32_t start[NVM_SECTIONS], length[NVM_SECTIONS];
    uint16_t status[NVM_SECTIONS];              /* the first refusal's, for the sections not there */
};

/* Every section, as Linux's iwl_nvm_init reads them: 2 KiB pieces until a
 * shorter one; a section that is not there answers with a status. */
static bool read_nvm(struct chip *c, struct nvm *n, uint64_t *us)
{
    *us = 0;
    for (int sec = 0; sec < NVM_SECTIONS; sec++) {
        n->start[sec] = n->used;
        uint32_t offset = 0;
        for (;;) {
            if (n->used + offset + NVM_CHUNK > NVM_MAX)
                break;
            uint8_t req[8] = { 0, 0, (uint8_t)sec, 0, (uint8_t)offset, (uint8_t)(offset >> 8),
                               (uint8_t)NVM_CHUNK, (uint8_t)(NVM_CHUNK >> 8) };
            uint64_t t;
            int got = command(c, CMD_NVM_ACCESS, 0, req, sizeof(req), &t);
            *us += t;
            if (got < 8) {
                kprintf("  intelwifi:     %s: NVM section %d at %u: no reply in 2 s\n", c->where,
                        sec, offset);
                return false;
            }
            uint16_t at = le16(c->reply), bytes = le16(c->reply + 2), status = le16(c->reply + 6);
            if (status) {
                if (!offset || status != NVM_STATUS_PAST_END)
                    n->status[sec] = status;
                break;
            }
            if (at != offset || bytes > NVM_CHUNK || (uint32_t)got < 8u + bytes)
                break;
            memcpy(n->data + n->used + offset, c->reply + 8, bytes);
            offset += bytes;
            if (bytes < NVM_CHUNK)
                break;
        }
        n->length[sec] = offset;
        n->used += offset;
    }
    return true;
}

static uint32_t nvm_le32(const struct nvm *n, int sec, uint32_t word)
{
    return n->length[sec] >= 2 * word + 4 ? le32(n->data + n->start[sec] + 2 * word) : 0;
}

static uint16_t nvm_le16(const struct nvm *n, int sec, uint32_t word)
{
    return n->length[sec] >= 2 * word + 2 ? le16(n->data + n->start[sec] + 2 * word) : 0;
}

static bool mac_valid(const uint8_t *m)
{
    return !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

/* The card's own data, as Linux's iwl_parse_nvm_data reads it for the
 * 8000 family: the address from the MAC override section unless it is
 * missing, invalid or Intel's reserved one, else the fused registers; the
 * radio and SKU from the PHY SKU section; the channels' flags and LAR from
 * the regulatory section. */
static void card_data(struct chip *c, const struct nvm *n)
{
    static const uint8_t reserved[6] = { 0x02, 0xCC, 0xAA, 0xFF, 0xEE, 0x00 };
    uint8_t mac[6] = { 0 };
    const char *from = "the MAC override section";
    if (n->length[NVM_MAC_OVERRIDE] >= 8)
        memcpy(mac, n->data + n->start[NVM_MAC_OVERRIDE] + 2, 6);
    if (!mac_valid(mac) || !memcmp(mac, reserved, 6)) {
        uint32_t a0 = 0, a1 = 0;
        if (access_begin(c)) {
            a0 = prph_rd(c, WFMP_MAC_ADDR_0);
            a1 = prph_rd(c, WFMP_MAC_ADDR_1);
        }
        access_end(c);
        mac[0] = (uint8_t)(a0 >> 24);
        mac[1] = (uint8_t)(a0 >> 16);
        mac[2] = (uint8_t)(a0 >> 8);
        mac[3] = (uint8_t)a0;
        mac[4] = (uint8_t)(a1 >> 8);
        mac[5] = (uint8_t)a1;
        from = "the fused registers";
    }
    uint32_t version = nvm_le32(n, NVM_SW, 0), addresses = nvm_le32(n, NVM_SW, 3) & 0xF;
    uint32_t radio = nvm_le32(n, NVM_PHY_SKU, 0), sku = nvm_le32(n, NVM_PHY_SKU, 2);
    c->nvm_tx_ant = (uint8_t)((radio >> 24) & 0xF);
    c->nvm_rx_ant = (uint8_t)((radio >> 28) & 0xF);
    int valid24 = 0, valid5 = 0, wide = 0;
    for (uint32_t ch = 0; ch < 51; ch++) {         /* 14 at 2.4 GHz, 37 at 5 GHz */
        uint16_t flags = nvm_le16(n, NVM_REGULATORY, ch);
        if (flags & 1)
            *(ch < 14 ? &valid24 : &valid5) += 1;
        if ((flags & 1) && (flags & (1u << 10)))
            wide++;
    }
    uint16_t lar = nvm_le16(n, NVM_REGULATORY, version < 0xE39 ? 0x4C7 : 0x507);
    memcpy(c->mac, mac, 6);
    for (uint32_t ch = 0; ch < NVM_CHANNEL_COUNT; ch++)
        c->nvm_flags[ch] = nvm_le16(n, NVM_REGULATORY, ch);
    kprintf("  intelwifi:     %s: card: MAC %02x:%02x:%02x:%02x:%02x:%02x (%s%s), %u address(es); "
            "NVM version 0x%X\n", c->where, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], from,
            mac_valid(mac) ? "" : ", not valid", addresses, version);
    kprintf("  intelwifi:     %s: card: radio 0x%X (type %u, step %u, dash %u, flavor %u), antennas "
            "send 0x%X receive 0x%X; SKU 0x%X:%s%s%s%s%s; channels %d of 14 at 2.4 GHz, %d of 37 "
            "at 5 GHz (%d with 80 MHz); LAR %s\n", c->where, radio, (radio >> 12) & 0xFFF,
            (radio >> 8) & 0xF, (radio >> 4) & 0xF, radio & 0xF, (radio >> 24) & 0xF,
            (radio >> 28) & 0xF, sku, sku & 1 ? " 2.4 GHz" : "", sku & 2 ? " 5 GHz" : "",
            sku & 4 ? " 11n" : "", (sku & 4) && (sku & 8) ? " 11ac" : "",
            sku & 0x20 ? " (MIMO off)" : " MIMO", valid24, valid5, wide,
            lar & 7 ? "on (the firmware's country)" : "off");
}

static void u32_to(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* 36b-3a: the init image's calibrations, as Linux's iwl_run_init_mvm_ucode
 * for the 8000 family (no Bluetooth setting there): the antennas to send
 * with, then the PHY configuration (the file's PHY SKU, its antenna fields
 * narrowed to the NVM's, and the init image's calibration triggers); the
 * results come as PHY database parts until "init complete" (2 s). */
static void calibrate(struct chip *c)
{
    uint8_t fw_tx = (uint8_t)((c->phy_sku >> 16) & 0xF), fw_rx = (uint8_t)((c->phy_sku >> 20) & 0xF);
    uint8_t tx = c->nvm_tx_ant ? fw_tx & c->nvm_tx_ant : fw_tx;
    uint8_t rx = c->nvm_rx_ant ? fw_rx & c->nvm_rx_ant : fw_rx;
    uint32_t phy_cfg = c->phy_sku & (~0x00FF0000u | (uint32_t)tx << 16 | (uint32_t)rx << 20);
    uint8_t ant[4], phy[12];
    u32_to(ant, tx);
    uint64_t t_ant, t_phy;
    if (command(c, CMD_TX_ANT, 0, ant, sizeof(ant), &t_ant) < 0) {
        kprintf("  intelwifi:     %s: calibration: the antenna command had no reply\n", c->where);
        return;
    }
    u32_to(phy, phy_cfg);
    u32_to(phy + 4, c->calib_flow[UCODE_INIT]);
    u32_to(phy + 8, c->calib_event[UCODE_INIT]);
    uint64_t start = now_us();
    if (command(c, CMD_PHY_CONFIG, 0, phy, sizeof(phy), &t_phy) < 0) {
        kprintf("  intelwifi:     %s: calibration: the PHY configuration had no reply\n", c->where);
        return;
    }
    kprintf("  intelwifi:     %s: calibration: send antennas 0x%X (reply in %lu us); PHY "
            "configuration 0x%X, triggers flow 0x%X event 0x%X (reply in %lu us)\n", c->where, tx,
            t_ant, phy_cfg, c->calib_flow[UCODE_INIT], c->calib_event[UCODE_INIT], t_phy);
    while (!c->init_complete && now_us() - start < 2000000) {
        if (wait_events(c, INT_RUNNING, 20) & (INT_SW_ERR | INT_HW_ERR))
            c->fw_errors++;
        rx_collect(c);
    }
    uint64_t took = now_us() - start;
    uint32_t count[6] = { 0 }, bytes[6] = { 0 }, total = 0;
    for (int i = 0; i < c->nphy; i++) {
        int t = c->phy_db[i].type < 6 ? c->phy_db[i].type : 0;
        count[t]++;
        bytes[t] += c->phy_db[i].length;
        total += c->phy_db[i].length;
    }
    kprintf("  intelwifi:     %s: calibration: %s after %lu us; PHY database %d part(s), %u bytes: "
            "configuration %u (%u bytes), channels %u (%u), PAPD groups %u (%u), TX power groups "
            "%u (%u), other %u; %d not kept\n", c->where,
            c->init_complete ? "init complete" : "no init complete", took, c->nphy, total,
            count[1], bytes[1], count[2], bytes[2], count[4], bytes[4], count[5], bytes[5],
            count[0] + count[3], c->phy_dropped);
}

/* 36b-2: the transmit side started, the first commands: ECHO, then the
 * NVM read and the card's data. */
static void first_commands(struct chip *c)
{
    if (!tx_start(c)) {
        kprintf("  intelwifi:     %s: no memory or no access to start the transmit side\n",
                c->where);
        return;
    }
    uint64_t us;
    int echo = command(c, CMD_ECHO, 0, 0, 0, &us);
    kprintf("  intelwifi:     %s: commands: queue %d on FIFO %d, scheduler at 0x%X (alive: 0x%X); "
            "ECHO %s%lu us\n", c->where, CMD_QUEUE, CMD_FIFO, c->scd_base, c->alive_scd,
            echo >= 0 ? "answered in " : "not answered in ", us);
    if (echo < 0)
        return;
    struct nvm n = { 0 };
    if (!(n.data = kmalloc(NVM_MAX)))
        return;
    if (read_nvm(c, &n, &us)) {
        char list[160];
        size_t used = 0;
        list[0] = 0;
        for (int sec = 0; sec < NVM_SECTIONS; sec++)
            if (n.length[sec] && used + 16 < sizeof(list))
                used += (size_t)ksnprintf(list + used, sizeof(list) - used, " %d:%u", sec,
                                          n.length[sec]);
        kprintf("  intelwifi:     %s: NVM: %u bytes in %lu us; sections (number:bytes)%s\n",
                c->where, n.used, us, used ? list : " none");
        card_data(c, &n);
        calibrate(c);
    }
    kfree(n.data);
}

/* An image into the chip and its CPUs started, as Linux's
 * iwl_trans_pcie_start_fw does it for the 8000 family (36b-1; the run-time
 * image the same way, 36b-3b); then the firmware's "alive" awaited (Linux
 * allows 1 s), and what else it says in 2 s more. True when it is alive
 * and reported no error. The interrupt, rings and pages are made once. */
static bool run_image(struct chip *c, const struct image *im, const char *name)
{
    /* The host's ownership again, as Linux's start_fw does (a reset
     * clears it). */
    bool owned = rd(c, CSR_HW_IF_CONFIG) & HW_IF_NIC_READY;
    if (make_ready(c) < 0) {
        kprintf("  intelwifi:     %s: the chip did not become ready again\n", c->where);
        return false;
    }
    wr(c, CSR_INT_MASK, 0);
    wr(c, CSR_INT, 0xFFFFFFFFu);
    if (!c->msi_tried) {
        uint64_t address;
        uint32_t data;
        c->msi_tried = true;
        c->msi = pci_msi_enable(c->pci, interrupt, c, &address, &data);
    }
    take_events(c, 0xFFFFFFFFu);
    c->alive = false;
    c->nothers = 0;
    c->others[0] = 0;
    uint64_t interrupts = c->interrupts, missed = c->missed;
    wr(c, CSR_UCODE_DRV_GP1_CLR, GP1_SW_RF_KILL);
    wr(c, CSR_UCODE_DRV_GP1_CLR, GP1_CMD_BLOCKED);
    wr(c, CSR_INT, 0xFFFFFFFFu);
    if (wake(c) < 0) {                          /* Linux's nic_init powers it up again */
        kprintf("  intelwifi:     %s: the MAC clock did not come back\n", c->where);
        return false;
    }
    /* The configuration register: the MAC's step and dash from the
     * revision (8000 family: the step in bits 1:0), the radio's type, step
     * and dash from the file's PHY SKU (bits 1:0, 3:2, 5:4); no SI bits. */
    uint32_t mac_step = c->hw_rev & 3, sku = c->phy_sku;
    uint32_t config = mac_step << 2 | (sku & 3) << 10 | ((sku >> 2) & 3) << 14 |
                      ((sku >> 4) & 3) << 12;
    wr(c, CSR_HW_IF_CONFIG, (rd(c, CSR_HW_IF_CONFIG) & ~HW_IF_CONFIG_MASK) | config);
    /* (On a failure from here on the pages stay: the chip may still write there.) */
    if (!rx_start(c)) {
        kprintf("  intelwifi:     %s: no memory or no access for the receive ring\n", c->where);
        return false;
    }
    if (!c->keep_warm && !(c->keep_warm = pmm_alloc_pages_below(1, DMA_LIMIT)))
        return false;
    memset(phys_to_virt(c->keep_warm), 0, 4096);
    /* Every queue's ring, as Linux's tx_init gives them before the load
     * (the firmware is started with the scheduler set up), then the
     * scheduler's mode: queues active by themselves, all 31 of them. */
    for (int q = 0; q < TX_QUEUES; q++) {
        if (!c->tx_ring[q] &&
            !(c->tx_ring[q] = pmm_alloc_pages_below(TFD_RING_BYTES / 4096, DMA_LIMIT)))
            return false;
        memset(phys_to_virt(c->tx_ring[q]), 0, TFD_RING_BYTES);
    }
    if (access_begin(c)) {
        prph_wr(c, SCD_TXFACT, 0);
        wr(c, FH_KW_MEM_ADDR, (uint32_t)(c->keep_warm >> 4));
        for (int q = 0; q < TX_QUEUES; q++) {
            uint32_t reg = q < 16 ? FH_CBBC_0_15 + 4 * q : q < 20 ? FH_CBBC_16_19 + 4 * (q - 16)
                                                                   : FH_CBBC_20_31 + 4 * (q - 20);
            wr(c, reg, (uint32_t)(c->tx_ring[q] >> 8));
        }
        prph_wr(c, SCD_GP_CTRL, prph_rd(c, SCD_GP_CTRL) | SCD_AUTO_ACTIVE | SCD_ENABLE_31_QUEUES);
    }
    access_end(c);
    wr(c, CSR_MAC_SHADOW_REG_CTRL, rd(c, CSR_MAC_SHADOW_REG_CTRL) | SHADOW_ALL);
    wr(c, CSR_INT_MASK, INT_FH_TX);
    wr(c, CSR_UCODE_DRV_GP1_CLR, GP1_SW_RF_KILL);

    struct loader l = { 0 };
    l.size = LOAD_CHUNK;
    if (!(l.phys = pmm_alloc_pages_below(LOAD_CHUNK / 4096, DMA_LIMIT))) {
        l.size = 4096;                          /* as Linux: a page at a time instead */
        if (!(l.phys = pmm_alloc_pages_below(1, DMA_LIMIT)))
            return false;
    }
    l.buffer = phys_to_virt(l.phys);
    uint64_t start = now_us();
    if (access_begin(c))
        prph_wr(c, RELEASE_CPU_RESET, CPU_RESET_RELEASE);
    access_end(c);
    int index = 0;
    bool ok = load_cpu(c, &l, im, 1, &index);
    index++;
    ok = ok && load_cpu(c, &l, im, 2, &index);
    uint64_t loaded = now_us();
    uint32_t sb1, sb2;
    secure_boot(c, &sb1, &sb2);
    kprintf("  intelwifi:     %s: %s image %s: %d section(s), %lu bytes in %d load(s) of up to "
            "%u KiB, %lu us; FH_UCODE_LOAD_STATUS 0x%X; secure boot CPU1 0x%X CPU2 0x%X; "
            "interrupts %s (%lu, %lu missed); NIC_READY %s\n", c->where, name,
            ok ? "loaded" : "not loaded", l.sections, l.bytes, l.chunks, l.size / 1024,
            loaded - start, direct_rd(c, FH_UCODE_LOAD_STATUS), sb1, sb2,
            c->msi ? "by MSI" : "not available: the register is watched",
            c->interrupts - interrupts, c->missed - missed, owned ? "kept" : "set again");
    if (l.size == LOAD_CHUNK)
        pmm_free_pages(l.phys, LOAD_CHUNK / 4096);
    else
        pmm_free_page(l.phys);
    if (!ok)
        return false;

    uint32_t seen = 0;
    while (!c->alive && now_us() - loaded < 1000000) {
        seen |= wait_events(c, INT_RUNNING, 50);
        rx_collect(c);
    }
    uint64_t alive_at = now_us();
    if (!c->alive) {
        secure_boot(c, &sb1, &sb2);
        uint32_t umac[3] = { 0 }, lmac[3] = { 0 }, rx_dma = 0;
        for (int i = 0; i < 3; i++) {
            if (access_begin(c)) {
                umac[i] = prph_rd(c, UMAC_CURRENT_PC);
                lmac[i] = prph_rd(c, LMAC_CURRENT_PC);
                rx_dma = rd(c, FH_RX_DMA_STATUS);
            }
            access_end(c);
            delay_us(1000);
        }
        kprintf("  intelwifi:     %s: UMAC at 0x%X 0x%X 0x%X, LMAC at 0x%X 0x%X 0x%X (1 ms apart); "
                "CSR_INT 0x%X, receive DMA 0x%X\n", c->where, umac[0], umac[1], umac[2], lmac[0],
                lmac[1], lmac[2], rd(c, CSR_INT), rx_dma);
        kprintf("  intelwifi:     %s: no alive in 1 s: causes 0x%X, GP_CNTRL 0x%X, CSR_RESET 0x%X, "
                "receive status %u, FH_UCODE_LOAD_STATUS 0x%X, secure boot CPU1 0x%X CPU2 0x%X, "
                "interrupts %lu (%lu missed)\n", c->where, seen, rd(c, CSR_GP_CNTRL),
                rd(c, CSR_RESET), c->rx_status[0], direct_rd(c, FH_UCODE_LOAD_STATUS), sb1, sb2,
                c->interrupts - interrupts, c->missed - missed);
        return false;
    }
    while (now_us() - alive_at < 2000000) {
        seen |= wait_events(c, INT_RUNNING, 100);
        rx_collect(c);
    }
    kprintf("  intelwifi:     %s: alive %lu us after the load; in the 2 s after: %d other "
            "notification(s)%s; causes 0x%X; interrupts %lu (%lu missed, %lu with the chip gone)\n",
            c->where, alive_at - loaded, c->nothers, c->nothers ? c->others : "", seen,
            c->interrupts - interrupts, c->missed - missed, c->gone);
    if (seen & INT_SW_ERR) {
        kprintf("  intelwifi:     %s: the firmware reported an error after alive\n", c->where);
        error_logs(c);
        return false;
    }
    return true;
}

/* The chip stopped, as Linux's _iwl_trans_pcie_stop_device (not into low
 * power): the debug engine's sampling, interrupts, the transmit and
 * receive DMA, the bus master, then the chip reset (twice, 5 ms each, as
 * apm_stop and stop_device each do) and NIC_READY taken again. */
static void stop_device(struct chip *c)
{
    if (access_begin(c)) {
        prph_wr(c, DBGC_IN_SAMPLE, 0);
        delay_us(100);
        prph_wr(c, DBGC_OUT_CTRL, 0);
    }
    access_end(c);
    wr(c, CSR_INT_MASK, 0);
    wr(c, CSR_INT, 0xFFFFFFFFu);
    wr(c, CSR_FH_INT_STATUS, 0xFFFFFFFFu);
    int64_t tx = -1, rx = -1;
    if (access_begin(c)) {
        prph_wr(c, SCD_TXFACT, 0);
        for (int ch = 0; ch < 8; ch++)
            wr(c, FH_TCSR_CONFIG0 + 0x20 * ch, 0);
        tx = poll(c, FH_TX_STATUS, 0xFFu << 16, 0xFFu << 16, 5000);
        wr(c, FH_RX_CONFIG, 0);
        rx = poll(c, FH_RX_DMA_STATUS, 0x01000000u, 0x01000000u, 1000);
    }
    access_end(c);
    set_bits(c, CSR_RESET, RESET_STOP_MASTER);
    int64_t master = poll(c, CSR_RESET, RESET_MASTER_OFF, RESET_MASTER_OFF, 100);
    set_bits(c, CSR_RESET, RESET_SW);
    delay_us(5000);
    wr(c, CSR_GP_CNTRL, rd(c, CSR_GP_CNTRL) & ~GP_INIT_DONE);
    set_bits(c, CSR_RESET, RESET_SW);
    delay_us(5000);
    wr(c, CSR_INT_MASK, 0);
    wr(c, CSR_INT, 0xFFFFFFFFu);
    wr(c, CSR_FH_INT_STATUS, 0xFFFFFFFFu);
    wr(c, CSR_INT_MASK, INT_RF_KILL);
    int64_t ready = make_ready(c);
    kprintf("  intelwifi:     %s: stopped: transmit DMA idle %s%ld us, receive %s%ld us, bus master "
            "%s%ld us; ready again %s\n", c->where, tx >= 0 ? "after " : "not after ",
            tx >= 0 ? tx : 5000L, rx >= 0 ? "after " : "not after ", rx >= 0 ? rx : 1000L,
            master >= 0 ? "off after " : "not off after ", master >= 0 ? master : 100L,
            ready >= 0 ? "yes" : "no");
}

/* The run-time image's paged part handed over, as Linux's iwl_save_fw_paging
 * and iwl_send_paging_cmd: the part's signature in a page of its own, the
 * code in blocks of 8 pages (32 KiB, aligned as Linux's allocation of 8
 * pages is), the last block holding the rest; the command gives their
 * addresses >> 12. The pages stay the firmware's for good. */
static bool paging(struct chip *c)
{
    int at = -1;
    for (int i = 0; i + 1 < c->rt.n; i++)
        if (c->rt.list[i].offset == SEP_PAGING) {
            at = i + 1;
            break;
        }
    uint32_t pages = c->paging_bytes / 4096;
    uint32_t blocks = (pages + 7) / 8, last = pages - 8 * (blocks - 1);
    if (at < 0 || at + 1 >= c->rt.n || !pages || blocks > PAGING_BLOCKS_MAX ||
        c->rt.list[at].length > 4096 || c->rt.list[at + 1].length != c->paging_bytes) {
        kprintf("  intelwifi:     %s: paging: the file's paged part is not as expected\n",
                c->where);
        return false;
    }
    const struct section *css = &c->rt.list[at], *code = &c->rt.list[at + 1];
    if (!c->paging[0] && !(c->paging[0] = pmm_alloc_pages_below(1, DMA_LIMIT)))
        return false;                           /* (36e-6: a restart reuses the blocks) */
    memset(phys_to_virt(c->paging[0]), 0, 4096);
    memcpy(phys_to_virt(c->paging[0]), css->data, css->length);
    for (uint32_t b = 1; b <= blocks; b++) {
        uint64_t aligned = c->paging[b];
        if (!aligned) {
            uint64_t run = pmm_alloc_pages_below(15, DMA_LIMIT);
            if (!run)
                return false;
            aligned = (run + PAGE_BLOCK_BYTES - 1) & ~(uint64_t)(PAGE_BLOCK_BYTES - 1);
            uint64_t head = (aligned - run) / 4096, tail = 15 - head - 8;
            if (head)
                pmm_free_pages(run, head);
            if (tail)
                pmm_free_pages(aligned + PAGE_BLOCK_BYTES, tail);
            c->paging[b] = aligned;
        }
        uint32_t from = (b - 1) * PAGE_BLOCK_BYTES;
        uint32_t n = b == blocks ? last * 4096 : PAGE_BLOCK_BYTES;
        memset(phys_to_virt(aligned), 0, PAGE_BLOCK_BYTES);
        memcpy(phys_to_virt(aligned), code->data + from, n);
    }
    arch_write_barrier();
    uint8_t cmd[12 + 4 * (PAGING_BLOCKS_MAX + 1)];
    memset(cmd, 0, sizeof(cmd));
    u32_to(cmd, PAGING_SECURED | PAGING_ENABLED | last);
    u32_to(cmd + 4, 15);                        /* the block size: 2^15 */
    u32_to(cmd + 8, blocks);
    for (uint32_t b = 0; b <= blocks; b++)
        u32_to(cmd + 12 + 4 * b, (uint32_t)(c->paging[b] >> 12));
    uint64_t us;
    int got = command(c, CMD_PAGING, GROUP_LONG, cmd, sizeof(cmd), &us);
    kprintf("  intelwifi:     %s: paging: signature %u bytes, %u bytes in %u block(s) of 32 KiB "
            "(%u pages in the last); the command %s%lu us\n", c->where, css->length,
            c->paging_bytes, blocks, last, got >= 0 ? "answered in " : "not answered in ", us);
    return got >= 0;
}

/* The calibration results back to the run-time image, as Linux's
 * iwl_send_phy_db_data: the configuration, the channels, then every PAPD
 * group and every TX power group in group order. */
static bool send_phy_db(struct chip *c, int *sent, uint64_t *us)
{
    static const uint16_t order[4] = { 1, 2, 4, 5 };
    *sent = 0;
    *us = 0;
    for (int k = 0; k < 4; k++) {
        for (uint16_t g = 0; g < PHY_DB_MAX; g++) {
            struct phy_entry *e = 0;
            for (int i = 0; i < c->nphy; i++)
                if (c->phy_db[i].type == order[k] && c->phy_db[i].group == g) {
                    e = &c->phy_db[i];
                    break;
                }
            if (!e) {
                if (order[k] == 1 || order[k] == 2)
                    break;                      /* one each, group 0 */
                continue;
            }
            uint8_t *buf = kmalloc(4u + e->length);
            if (!buf)
                return false;
            buf[0] = (uint8_t)e->type;
            buf[1] = (uint8_t)(e->type >> 8);
            buf[2] = (uint8_t)e->length;
            buf[3] = (uint8_t)(e->length >> 8);
            memcpy(buf + 4, e->data, e->length);
            uint64_t t;
            int got = command(c, CMD_PHY_DB, 0, buf, 4u + e->length, &t);
            kfree(buf);
            *us += t;
            if (got < 0) {
                kprintf("  intelwifi:     %s: PHY database part (type %u, group %u) not answered\n",
                        c->where, e->type, e->group);
                return false;
            }
            (*sent)++;
            if (order[k] == 1 || order[k] == 2)
                break;
        }
    }
    return true;
}

static bool has_capa(const struct chip *c, int bit)
{
    return (c->capa[bit / 32] >> (bit % 32)) & 1;
}

static bool has_api(const struct chip *c, int bit)
{
    return (c->api[bit / 32] >> (bit % 32)) & 1;
}

/* One start-up command: its reply's length (c->reply), or -1 with a line
 * saying which; the time added to *us. */
static int step(struct chip *c, const char *what, uint8_t cmd, uint8_t group, const void *data,
                uint32_t length, uint64_t *us)
{
    uint64_t t;
    int got = command(c, cmd, group, data, length, &t);
    *us += t;
    if (got < 0 && c->silent_told++ < 5) {
        /* (36e-6: 5 a start; an hour of them pushed the start out of the log)
         * The queue as the chip and we see it: its read pointer, our slot,
         * the slot's descriptor (num_tbs, then the buffers: address and
         * length << 4), the command's first bytes. (36c-2: it showed the
         * descriptor right and the UMAC reading garbage after a command
         * 4 bytes too long: MCC_UPDATE is 28 bytes, we sent 32.) */
        uint32_t slot = (c->cmd_write + 255) & 255, rdptr = 0xFFFFFFFFu;
        if (access_begin(c))
            rdptr = prph_rd(c, SCD_QUEUE_RDPTR0 + 4 * CMD_QUEUE);
        access_end(c);
        const uint8_t *t = (const uint8_t *)phys_to_virt(c->tx_ring[CMD_QUEUE]) + slot * 128;
        const uint8_t *p = phys_to_virt(c->cmd_page);
        kprintf("  intelwifi:     %s: %s had no reply in 2 s; slot %u, the chip's read "
                "pointer %u; descriptor %02x %02x%02x%02x%02x %02x%02x %02x%02x%02x%02x %02x%02x; "
                "first buffer %02x %02x %02x %02x %02x %02x %02x %02x; at 256: %02x %02x %02x %02x\n",
                c->where, what, slot, rdptr, t[3], t[7], t[6], t[5], t[4], t[9], t[8], t[13], t[12],
                t[11], t[10], t[15], t[14], p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[256],
                p[257], p[258], p[259]);
    }
    return got;
}

/* 36c-1, the first part of the rest of Linux's iwl_mvm_up (before the
 * antennas and calibrations): the shared memory's layout asked (this
 * firmware has EXTEND_SHARED_MEM_CFG: the system group's command; the
 * 8000 family's reply is version 2), then the Smart FIFO set "init off"
 * as iwl_mvm_sf_update with no interface (Linux's watermarks and timers). */
static bool up_first(struct chip *c)
{
    uint64_t t_mem = 0, t_sf = 0;
    int got = has_capa(c, CAPA_EXTEND_SHARED_MEM)
        ? step(c, "the shared memory query", CMD_SHARED_MEM, GROUP_SYSTEM, 0, 0, &t_mem)
        : -1;
    if (got < 0)
        return false;
    uint32_t rx0 = got >= 60 ? le32(c->reply + 52) : 0, rx1 = got >= 60 ? le32(c->reply + 56) : 0;
    uint32_t tx_total = 0;
    for (int i = 0; i < 8 && got >= 52; i++)
        tx_total += le32(c->reply + 20 + 4 * i);
    uint8_t sf[92];
    memset(sf, 0, sizeof(sf));
    u32_to(sf, 3);                              /* SF_INIT_OFF */
    u32_to(sf + 4, 4096);                       /* the long delay's watermark (scanning) */
    u32_to(sf + 8, 8192);                       /* full on's (two streams, not associated) */
    for (int i = 0; i < 10; i++)
        u32_to(sf + 12 + 4 * i, 1000000);       /* long delay: aging and idle, 5 scenarios */
    for (int i = 0; i < 5; i++) {
        u32_to(sf + 52 + 8 * i, 400);           /* full on: aging 0.4 ms */
        u32_to(sf + 56 + 8 * i, 160);           /* idle 150 us */
    }
    if (step(c, "the Smart FIFO", CMD_SF_CFG, 0, sf, sizeof(sf), &t_sf) < 0)
        return false;
    kprintf("  intelwifi:     %s: start-up: shared memory %d bytes (RX FIFOs %u and %u bytes, TX "
            "FIFOs %u in all) in %lu us; Smart FIFO \"init off\" in %lu us\n", c->where, got, rx0,
            rx1, tx_total, t_mem, t_sf);
    return true;
}

/* 36c-1, the rest of iwl_mvm_up after the calibrations, for this firmware:
 * Bluetooth coexistence (iwl_mvm_send_bt_init_conf), the queues' mode
 * (DQA, the command queue 0), the scanning station (iwl_mvm_add_aux_sta:
 * its queue 1 on the multicast FIFO, enabled here and in the firmware's
 * scheduler, then ADD_STA), the 3 PHY contexts on channel 1 at 20 MHz,
 * the temperature thresholds (none: the firmware does CT-kill itself)
 * and its power budget (cTDP, 2000 mW), LTR if the card's PCIe link has
 * it on, and the device's power saving (Linux's default scheme). */
static bool up_rest(struct chip *c)
{
    uint64_t t_bt = 0, t_dqa = 0, t_queue = 0, t_sta = 0, t_phy = 0, t_temp = 0, t_ctdp = 0;
    uint64_t t_ltr = 0, t_power = 0;
    uint8_t bt[8];
    uint32_t modules = 0x4 | 0x10 | (has_capa(c, CAPA_BT_MPLUT) ? 0x1 : 0);
    u32_to(bt, 1);                              /* BT_COEX_NW */
    u32_to(bt + 4, modules);                    /* SYNC2SCO, HIGH_BAND_RET, MPLUT */
    if (step(c, "Bluetooth coexistence", CMD_BT_CONFIG, 0, bt, sizeof(bt), &t_bt) < 0)
        return false;
    uint8_t dqa[4];
    u32_to(dqa, CMD_QUEUE);
    if (step(c, "DQA", CMD_DQA_ENABLE, GROUP_DATA_PATH, dqa, sizeof(dqa), &t_dqa) < 0)
        return false;
    /* The scanning station's queue: on our side its pointers at 0 (Linux's
     * txq_enable without a scheduler setup: the firmware does that), then
     * SCD_QUEUE_CFG: queue, enable, FIFO, window 64, TID 8 (no TID). */
    if (access_begin(c))
        wr(c, HBUS_TARG_WRPTR, 0 | AUX_QUEUE << 8);
    access_end(c);
    uint8_t q[12] = { 0, AUX_STA_ID, 8, AUX_QUEUE, 1, 0, FIFO_MCAST, CMD_FRAME_LIMIT, 0, 0, 0, 0 };
    if (step(c, "the scanning queue", CMD_SCD_QUEUE_CFG, 0, q, sizeof(q), &t_queue) < 0)
        return false;
    uint8_t sta[48];
    memset(sta, 0, sizeof(sta));
    sta[2] = 0xFF;                              /* tid_disable_tx: every TID */
    sta[3] = 0xFF;
    u32_to(sta + 4, MAC_INDEX_AUX);             /* MAC index, color 0 */
    sta[16] = AUX_STA_ID;
    if (has_api(c, API_STA_TYPE))
        sta[35] = STA_TYPE_AUX;
    u32_to(sta + 40, 1u << AUX_QUEUE);
    int got = step(c, "the scanning station", CMD_ADD_STA, 0, sta, sizeof(sta), &t_sta);
    if (got < 0)
        return false;
    uint32_t sta_status = got >= 4 ? le32(c->reply) & 0xFF : 0;
    /* PHY contexts: channel 1 (2.4 GHz), 20 MHz, control below; receive
     * with the valid antennas, both chains idle and active (diversity:
     * two antennas), send with the valid ones. */
    uint8_t tx = (uint8_t)((c->phy_sku >> 16) & 0xF), rx = (uint8_t)((c->phy_sku >> 20) & 0xF);
    if (c->nvm_tx_ant)
        tx &= c->nvm_tx_ant;
    if (c->nvm_rx_ant)
        rx &= c->nvm_rx_ant;
    uint32_t rx_chain = (uint32_t)rx << 1 | 2u << 10 | 2u << 12;
    for (uint32_t id = 0; id < NUM_PHY_CTX; id++) {
        uint8_t ctx[36];
        memset(ctx, 0, sizeof(ctx));
        u32_to(ctx, id);                        /* id, color 0 */
        u32_to(ctx + 4, 1);                     /* add */
        ctx[16] = 1;                            /* 2.4 GHz */
        ctx[17] = 1;                            /* channel 1 */
        u32_to(ctx + 20, tx);
        u32_to(ctx + 24, rx_chain);
        if (step(c, "a PHY context", CMD_PHY_CONTEXT, 0, ctx, sizeof(ctx), &t_phy) < 0)
            return false;
    }
    bool tt_in_fw = has_capa(c, CAPA_CT_KILL_BY_FW) && has_capa(c, CAPA_TEMP_THS_REPORT);
    if (tt_in_fw) {
        uint8_t ths[20];
        memset(ths, 0, sizeof(ths));
        if (step(c, "the temperature thresholds", CMD_TEMP_THRESHOLDS, GROUP_PHY_OPS, ths,
                 sizeof(ths), &t_temp) < 0)
            return false;
    }
    uint32_t ctdp_status = 0;
    if (has_capa(c, CAPA_CTDP)) {
        uint8_t ctdp[12];
        memset(ctdp, 0, sizeof(ctdp));
        u32_to(ctdp, 1);                        /* start */
        u32_to(ctdp + 4, 2000);                 /* mW: cooling state 0 */
        if ((got = step(c, "the power budget", CMD_CTDP, GROUP_PHY_OPS, ctdp, sizeof(ctdp),
                        &t_ctdp)) < 0)
            return false;
        ctdp_status = got >= 4 ? le32(c->reply) : 0;
    }
    uint8_t pcie = pci_find_capability(c->pci, 0x10);
    bool ltr = pcie && (pci_read16(c->pci, pcie + 0x28) & 0x0400);
    if (ltr) {
        uint8_t l[32];
        memset(l, 0, sizeof(l));
        u32_to(l, 1);                           /* LTR_CFG_FLAG_FEATURE_ENABLE */
        if (step(c, "LTR", CMD_LTR_CONFIG, 0, l, sizeof(l), &t_ltr) < 0)
            return false;
    }
    uint8_t power[4] = { 1, 0, 0, 0 };          /* power saving on */
    if (step(c, "the device's power", CMD_POWER_TABLE, 0, power, sizeof(power), &t_power) < 0)
        return false;
    kprintf("  intelwifi:     %s: start-up: Bluetooth coexistence (modules 0x%X) %lu us; DQA %lu "
            "us; scanning station %d on queue %d (FIFO %d): queue %lu us, station %lu us (status "
            "0x%X)\n", c->where, modules, t_bt, t_dqa, AUX_STA_ID, AUX_QUEUE, FIFO_MCAST, t_queue,
            t_sta, sta_status);
    kprintf("  intelwifi:     %s: start-up: %d PHY contexts on channel 1 (send 0x%X, receive 0x%X) "
            "%lu us; temperature %s%lu us; power budget %s%lu us (status 0x%X); LTR %s%lu us; "
            "power saving on %lu us\n", c->where, NUM_PHY_CTX, tx, rx_chain, t_phy,
            tt_in_fw ? "thresholds (none, the firmware's) " : "not the firmware's ", t_temp,
            has_capa(c, CAPA_CTDP) ? "2000 mW " : "not supported ", t_ctdp, ctdp_status,
            ltr ? "on " : "off on the PCIe link ", t_ltr, t_power);
    return true;
}

/* 36c-2: the firmware's current country, as Linux's first query at the
 * start (iwl_mvm_init_mcc: no country known yet, "ZZ" with the source
 * "get current"; this firmware has WIFI_MCC_UPDATE and LAR_SUPPORT_V2):
 * its reply's flags per channel (the NVM's list) say which are allowed.
 * Without a reply the NVM's own flags do. */
static void country(struct chip *c, uint16_t code_in, uint8_t source)
{
    uint8_t mcc[28];                            /* code, source, reserved, key, 20 reserved */
    memset(mcc, 0, sizeof(mcc));
    mcc[0] = (uint8_t)code_in;                  /* (le16: first letter << 8 | second) */
    mcc[1] = (uint8_t)(code_in >> 8);
    mcc[2] = source;
    uint64_t us = 0;
    int got = step(c, "the country (MCC)", CMD_MCC_UPDATE, 0, mcc, sizeof(mcc), &us);
    c->nallowed = 0;
    if (got >= 16) {
        uint32_t status = le32(c->reply), n = le32(c->reply + 12);
        uint16_t code = le16(c->reply + 4);
        char cc[3] = { (char)(code >> 8), (char)code, 0 };
        if (!code)
            cc[0] = cc[1] = '0';                /* (Linux: the world domain comes as 0) */
        memcpy(c->country_code, cc, 3);
        int at24 = 0, at5 = 0;
        for (uint32_t i = 0; i < n && i < NVM_CHANNEL_COUNT && 16 + 4 * i + 4 <= (uint32_t)got; i++)
            if (le32(c->reply + 16 + 4 * i) & 1) {
                c->allowed[c->nallowed++] = nvm_channels[i];
                *(i < 14 ? &at24 : &at5) += 1;
            }
        if (c->registered) {                    /* (a later country: the layer's list too) */
            for (int i = 0; i < c->nallowed && i < (int)sizeof(c->wifi.channels); i++)
                c->wifi.channels[i] = c->allowed[i];
            c->wifi.nchannels = c->nallowed;
        }
        kprintf("  intelwifi:     %s: country: the firmware's \"%s\" (status 0x%X, source 0x%X, "
                "capabilities 0x%X); %d of %u channel(s) allowed: %d at 2.4 GHz, %d at 5 GHz; %lu "
                "us\n", c->where, cc, status, c->reply[7], c->reply[6], c->nallowed, n, at24, at5,
                us);
    }
    if (!c->nallowed) {
        for (int i = 0; i < NVM_CHANNEL_COUNT; i++)
            if (c->nvm_flags[i] & 1)
                c->allowed[c->nallowed++] = nvm_channels[i];
        kprintf("  intelwifi:     %s: country: the NVM's channels instead: %d\n", c->where,
                c->nallowed);
    }
}

/* The scan's settings, as Linux's iwl_mvm_config_scan with no interface
 * associated (version 1 of the command for this family: the "unassoc"
 * timing, 0): the chains, all legacy rates, Linux's dwell times, our
 * address, the scanning station, every channel the NVM lists. */
static bool scan_config(struct chip *c)
{
    uint32_t size = 36 + (c->scan_slots ? c->scan_slots : NVM_CHANNEL_COUNT);
    uint8_t *cfg = kzalloc(size);
    if (!cfg)
        return false;
    uint8_t tx = (uint8_t)((c->phy_sku >> 16) & 0xF), rx = (uint8_t)((c->phy_sku >> 20) & 0xF);
    if (c->nvm_tx_ant)
        tx &= c->nvm_tx_ant;
    if (c->nvm_rx_ant)
        rx &= c->nvm_rx_ant;
    uint32_t flags = 0x1 | 0x8 | 0x100 | 0x200 | 0x400 | 0x800 | 0x2000 | 0x4000 | 0x8000 |
                     0x20000 | (uint32_t)NVM_CHANNEL_COUNT << 26;
    u32_to(cfg, flags);                         /* activate, the "set" bits, not fragmented */
    u32_to(cfg + 4, tx);
    u32_to(cfg + 8, rx);
    u32_to(cfg + 12, 0x0FFF0FFF);               /* 12 legacy rates: basic and supported */
    cfg[24] = 10;                               /* dwell: active, passive, fragmented, */
    cfg[25] = 110;                              /* extended (ms) */
    cfg[26] = 44;
    cfg[27] = 90;
    memcpy(cfg + 28, c->mac, 6);
    cfg[34] = AUX_STA_ID;
    cfg[35] = 0xF;                              /* EBS, accurate, add; passive to active */
    for (int i = 0; i < NVM_CHANNEL_COUNT && (uint32_t)i < size - 36; i++)
        cfg[36 + i] = nvm_channels[i];
    uint64_t us = 0;
    int got = step(c, "the scan configuration", CMD_SCAN_CFG, GROUP_LONG, cfg, size, &us);
    kfree(cfg);
    if (got >= 0)
        kprintf("  intelwifi:     %s: scan configuration: %u bytes, %d channels, flags 0x%X; %lu us\n",
                c->where, size, NVM_CHANNEL_COUNT, flags, us);
    return got >= 0;
}

/* The station's MAC context, as Linux adds it when the interface comes up
 * (iwl_mvm_mac_ctxt_cmd_sta, not associated): id 0, TSF 0, our address,
 * the broadcast BSSID, Linux's acknowledgement rates without basic rates
 * (CCK 1-11, OFDM 6, 12, 24), group frames and other networks' beacons
 * let in; mac80211's EDCA for a station (no QoS before associating: cw
 * 15-1023, AIFS 2) on each access category's FIFO. */
static bool mac_context(struct chip *c, uint32_t action, const uint8_t *bssid)
{
    uint8_t m[148];
    memset(m, 0, sizeof(m));
    u32_to(m + 4, action);                      /* 1 add, 2 modify (36d-1: the BSSID) */
    u32_to(m + 8, 5);                           /* FW_MAC_TYPE_BSS_STA */
    memcpy(m + 16, c->mac, 6);
    if (bssid)
        memcpy(m + 24, bssid, 6);
    else
        memset(m + 24, 0xFF, 6);
    u32_to(m + 32, 0xF);
    u32_to(m + 36, 0x15);
    u32_to(m + 52, 0x4 | 0x40);                 /* ACCEPT_GRP, IN_BEACON */
    for (int fifo = 0; fifo < 4; fifo++) {      /* BK, BE, VI, VO */
        uint8_t *ac = m + 60 + 8 * fifo;
        ac[0] = 15;
        ac[2] = 0xFF, ac[3] = 0x03;             /* 1023 */
        ac[4] = 2;
        ac[5] = (uint8_t)(1u << fifo);
    }
    uint64_t us = 0;
    int got = step(c, "the MAC context", CMD_MAC_CONTEXT, 0, m, sizeof(m), &us);
    if (got >= 0 && action == 1)
        kprintf("  intelwifi:     %s: MAC context 0: station %02x:%02x:%02x:%02x:%02x:%02x, not "
                "associated; %lu us\n", c->where, c->mac[0], c->mac[1], c->mac[2], c->mac[3],
                c->mac[4], c->mac[5], us);
    return got >= 0;
}

/* A passive scan of every allowed channel in the firmware, as Linux's
 * iwl_mvm_scan_umac for a regular scan without SSIDs (one round, pass
 * all, adaptive dwell; version 7 of the request): the frames come up as
 * RX_MPDU, its end as SCAN_COMPLETE_UMAC. */
static void start_scan(struct chip *c)
{
    uint32_t slots = c->scan_slots ? c->scan_slots : NVM_CHANNEL_COUNT;
    uint32_t size = SCAN_REQ_BASE + 8 * slots + SCAN_REQ_TAIL;
    uint8_t *r = kzalloc(size);
    if (!r)
        return;
    r[12] = 0x0C, r[13] = 0x20;                 /* PASS_ALL, PASSIVE, ADAPTIVE_DWELL */
    r[16] = 10;                                 /* active dwell */
    r[17] = 110;                                /* passive dwell */
    r[18] = 44;                                 /* fragmented dwell */
    r[19] = 2;                                  /* adaptive dwell: default APs */
    r[20] = 10;                                 /* ... on social channels */
    r[22] = (uint8_t)300, r[23] = (uint8_t)(300 >> 8);   /* its budget: a full scan */
    u32_to(r + 40, 6);                          /* priority: EXT_6 */
    int n = 0;
    for (int i = 0; i < c->nallowed && (uint32_t)i < slots; i++, n++) {
        uint8_t *ch = r + SCAN_REQ_BASE + 8 * i;
        ch[4] = c->allowed[i];
        ch[5] = 1;                              /* once */
    }
    r[45] = (uint8_t)n;
    uint8_t *tail = r + SCAN_REQ_BASE + 8 * slots;
    tail[2] = 1;                                /* schedule 0: one iteration */
    c->heard = c->beacons = c->probe_responses = c->bad_fcs = 0;
    uint64_t us = 0;
    c->scan_started = now_us();
    c->scanning = true;
    int got = step(c, "the scan request", CMD_SCAN_REQ, GROUP_LONG, r, size, &us);
    kfree(r);
    if (got < 0) {
        c->scanning = false;
        if (c->registered)
            wlan_scan_done(c->card);
        return;
    }
    kprintf("  intelwifi:     %s: scan: %d channel(s), passive, accepted in %lu us (status 0x%X)\n",
            c->where, n, us, got >= 4 ? le32(c->reply) : 0);
}

/* The session protected, as Linux's iwl_mvm_protect_session before each
 * authentication and association (mac80211's mgd_prepare_tx): one still
 * running with at least 400 TU left is kept; an older one is removed
 * first; a new one asks for 600 TU, at most 500 TU away, notified at its
 * start and end (TIME_EVENT_CMD; its reply carries the event's id). */
static bool session(struct chip *c, uint64_t *us)
{
    if (c->te_running && now_us() + 400 * 1024 < c->te_end_us)
        return true;
    uint8_t te[36];
    if (c->te_running) {
        memset(te, 0, sizeof(te));
        u32_to(te + 4, 3);                      /* remove */
        u32_to(te + 8, c->te_uid);
        step(c, "the old session protection's removal", CMD_TIME_EVENT, 0, te, sizeof(te), us);
        c->te_running = false;
    }
    memset(te, 0, sizeof(te));
    u32_to(te + 4, 1);                          /* add, for MAC 0 */
    u32_to(te + 16, 500);                       /* max delay (TU) */
    u32_to(te + 24, 1);                         /* interval */
    u32_to(te + 28, SESSION_TU);
    te[32] = 1;                                 /* repeat */
    te[34] = 0x03, te[35] = 0x08;               /* notify start and end; start at once */
    int got = step(c, "the session protection", CMD_TIME_EVENT, 0, te, sizeof(te), us);
    if (got < 12 || le32(c->reply)) {
        kprintf("  intelwifi:     %s: the session protection was refused (status 0x%X)\n", c->where,
                got >= 4 ? le32(c->reply) : 0);
        return false;
    }
    c->te_uid = le32(c->reply + 8);
    c->te_running = true;
    c->te_end_us = now_us() + (500 + SESSION_TU) * 1024ull;   /* (at the latest) */
    return true;
}

/* 36d-1: to a channel, as Linux when a station gets its channel and before
 * it authenticates: PHY context 0 moved there (20 MHz), our MAC context
 * bound to it the first time (then the MAC's power: allowed, keep-alive
 * 25 s, nothing more before associating), and the session protected
 * (TIME_EVENT: 600 TU, at most 500 TU away, notified at start and end)
 * so that the radio stays there for the authentication. */
static bool tune(struct chip *c, int channel)
{
    uint64_t t_phy = 0, t_bind = 0, t_power = 0, t_te = 0;
    uint8_t tx = (uint8_t)((c->phy_sku >> 16) & 0xF), rx = (uint8_t)((c->phy_sku >> 20) & 0xF);
    if (c->nvm_tx_ant)
        tx &= c->nvm_tx_ant;
    if (c->nvm_rx_ant)
        rx &= c->nvm_rx_ant;
    uint8_t ctx[36];
    memset(ctx, 0, sizeof(ctx));
    u32_to(ctx + 4, 2);                         /* modify */
    ctx[16] = channel <= 14 ? 1 : 0;            /* the band: 2.4 GHz (1) or 5 GHz (0) */
    ctx[17] = (uint8_t)channel;
    u32_to(ctx + 20, tx);
    u32_to(ctx + 24, (uint32_t)rx << 1 | 2u << 10 | 2u << 12);
    if (step(c, "the PHY context", CMD_PHY_CONTEXT, 0, ctx, sizeof(ctx), &t_phy) < 0)
        return false;
    uint32_t bind_status = 0;
    if (!c->bound) {
        uint8_t b[24];
        memset(b, 0, sizeof(b));
        u32_to(b + 4, 1);                       /* add; PHY context 0 */
        u32_to(b + 12, 0xFFFFFFFFu);            /* MAC 0 in it, the others unused */
        u32_to(b + 16, 0xFFFFFFFFu);
        int got = step(c, "the binding", CMD_BINDING, 0, b, sizeof(b), &t_bind);
        if (got < 0)
            return false;
        bind_status = got >= 4 ? le32(c->reply) : 0;
        uint8_t pw[40];
        memset(pw, 0, sizeof(pw));
        pw[4] = 1;                              /* POWER_SAVE_ENA */
        pw[6] = 25;                             /* keep-alive seconds */
        if (step(c, "the MAC's power", CMD_MAC_POWER, 0, pw, sizeof(pw), &t_power) < 0)
            return false;
        c->bound = true;
    }
    c->channel = channel;
    c->width = 20, c->center = channel;
    c->te_running = false;                      /* (a new channel: a new session) */
    if (!session(c, &t_te))
        return false;
    kprintf("  intelwifi:     %s: channel %d: PHY context %lu us; binding %s%lu us (status 0x%X); "
            "session protected %u TU (event %u) %lu us\n", c->where, channel, t_phy,
            t_bind ? "added " : "kept ", t_bind + t_power, bind_status, SESSION_TU, c->te_uid, t_te);
    return true;
}

static bool leave_ap(struct chip *c);

/* 36d-1: the access point as station 0, as Linux before authenticating
 * (mac80211 adds it, then its first management frame gets a queue): our
 * MAC context given its BSSID, the station added (type link, every TID
 * without aggregation), DQA's first management queue (5) on the voice
 * FIFO enabled (our pointers at the first frame's sequence number, then
 * SCD_QUEUE_CFG: TID 8, window 64) and given to the station. */
static bool add_ap(struct chip *c, const uint8_t *bssid, uint16_t ssn)
{
    uint64_t t_mac = 0, t_sta = 0, t_queue = 0, t_mod = 0;
    if (!mac_context(c, 2, bssid))
        return false;
    t_mac = 1;
    uint8_t sta[48];
    memset(sta, 0, sizeof(sta));
    sta[2] = 0xFF, sta[3] = 0xFF;               /* tid_disable_tx */
    memcpy(sta + 8, bssid, 6);
    sta[16] = AP_STA_ID;
    u32_to(sta + 24, 3u << 26 | 3u << 28 | 1u << 17);   /* the masks: width, MIMO, RTS */
    int got = step(c, "the access point's station", CMD_ADD_STA, 0, sta, sizeof(sta), &t_sta);
    if (got < 0)
        return false;
    uint32_t added = got >= 4 ? le32(c->reply) & 0xFF : 0;
    c->q_write[MGMT_QUEUE] = c->q_read[MGMT_QUEUE] = ssn & 0xFF;
    if (access_begin(c))
        wr(c, HBUS_TARG_WRPTR, c->q_write[MGMT_QUEUE] | MGMT_QUEUE << 8);
    access_end(c);
    uint8_t q[12] = { 0, AP_STA_ID, 8, MGMT_QUEUE, 1, 0, FIFO_VO, CMD_FRAME_LIMIT,
                      (uint8_t)ssn, (uint8_t)(ssn >> 8), 0, 0 };
    if (step(c, "the access point's queue", CMD_SCD_QUEUE_CFG, 0, q, sizeof(q), &t_queue) < 0)
        return false;
    sta[0] = 1;                                 /* modify */
    sta[17] = 1u << 7;                          /* STA_MODIFY_QUEUES */
    u32_to(sta + 40, 1u << MGMT_QUEUE);
    got = step(c, "the station's queues", CMD_ADD_STA, 0, sta, sizeof(sta), &t_mod);
    if (got < 0)
        return false;
    memcpy(c->ap, bssid, 6);
    c->ap_added = true;
    c->sta_queues = 1u << MGMT_QUEUE;
    memset(c->tid_queue, 0, sizeof(c->tid_queue));
    c->rx_ba = 0;
    c->data_logged = 0;
    c->agg_tids = 0, c->agg_window = 0;
    c->ptk_in_card = false, c->groups_in_card = 0;  /* (a new station holds no key) */
    kprintf("  intelwifi:     %s: the access point %02x:%02x:%02x:%02x:%02x:%02x: MAC context's "
            "BSSID set; station %d added (status 0x%X) %lu us; queue %d on FIFO %d from %u, %lu us; "
            "queues set (status 0x%X) %lu us\n", c->where, bssid[0], bssid[1], bssid[2], bssid[3],
            bssid[4], bssid[5], AP_STA_ID, added, t_sta, MGMT_QUEUE, FIFO_VO, ssn & 0xFFF, t_queue,
            got >= 4 ? le32(c->reply) & 0xFF : 0, t_mod);
    (void)t_mac;
    return true;
}

static uint32_t header_length(const uint8_t *f)
{
    uint32_t n = 24;
    if ((f[1] & 3) == 3)
        n += 6;                                 /* four addresses */
    if ((f[0] & 0x0C) == 0x08 && (f[0] & 0x80)) {
        n += 2;                                 /* QoS data */
        if (f[1] & 0x80)
            n += 4;                             /* HT control */
    }
    return n;
}

/* One frame onto a queue, as Linux's iwl_mvm_set_tx_cmd and
 * iwl_trans_pcie_tx for the 8000 family: the TX command (56 bytes) with
 * the 802.11 header after it, padded to 4 bytes; buffer 0 its first 20
 * bytes (the scratch field the chip writes, its address in the command),
 * buffer 1 the rest, buffer 2 the body; the lowest rate of the band on
 * each antenna in turn (management and, for now, data frames: the rate
 * table comes with associating); the scheduler's byte count (4-byte words,
 * the station). Each slot's page is made when first used. */
static bool tx_frame(struct chip *c, int q, uint8_t sta, const uint8_t *f, uint32_t len)
{
    uint32_t hdr = header_length(f);
    if (len < hdr || len > WLAN_FRAME_MAX)
        return false;
    if (!c->tx_page[q] && !(c->tx_page[q] = kzalloc(256 * sizeof(uint64_t))))
        return false;
    /* What the firmware has not given back (a single frame's TX response
     * or a burst's BA notification: q_read) is still in the ring, and a
     * slot written over would be sent wrong. */
    if (((c->q_write[q] - c->q_read[q]) & 255) >= RING_HIGH) {
        c->ring_full++;
        return false;
    }
    uint32_t slot = c->q_write[q];
    if (!c->tx_page[q][slot] && !(c->tx_page[q][slot] = pmm_alloc_pages_below(1, DMA_LIMIT)))
        return false;
    if (!c->tx_time[q] && !(c->tx_time[q] = kzalloc(256 * sizeof(uint64_t))))
        return false;
    uint64_t phys = c->tx_page[q][slot];
    uint8_t *page = phys_to_virt(phys), *cmd = page + 64, *t = cmd + 4;
    memset(cmd, 0, 4 + 56 + 40);
    uint16_t seq = (uint16_t)(q << 8 | slot);
    cmd[0] = CMD_TX;
    cmd[2] = (uint8_t)seq, cmd[3] = (uint8_t)(seq >> 8);
    bool mgmt = (f[0] & 0x0C) == 0x00, data = (f[0] & 0x0C) == 0x08, unicast = !(f[4] & 1);
    uint8_t subtype = f[0] >> 4;
    bool eapol = data && len >= hdr + 8 && f[hdr] == 0xAA && f[hdr + 1] == 0xAA &&
                 f[hdr + 6] == 0x88 && f[hdr + 7] == 0x8E;
    uint32_t flags = unicast ? 1u << 3 : 0;    /* ACK */
    if (c->channel <= 14 && ((mgmt && subtype != 10) || (data && !unicast)))
        flags |= 3u << 11;                      /* Bluetooth priority 3 */
    /* (36e-3) A protected frame to the access point while the card holds
     * the pairwise key: plain text, sealed by the firmware with the key in
     * the TX command (Linux's iwl_mvm_set_tx_cmd_ccmp); the CCMP header is
     * ours, with our packet number (iwl_mvm_set_tx_cmd_pn), the MIC the
     * firmware's (counted in the byte count). */
    bool seal = (f[1] & 0x40) && c->ptk_in_card && unicast;
    uint32_t body = len - hdr + (seal ? 8 : 0), tb1 = 4 + 56 + hdr - 20,
             tb1_padded = (tb1 + 3) & ~3u;
    if (seal)
        len += 8;
    if (tb1_padded != tb1)
        flags |= 1u << 20;                      /* MH_PAD */
    t[0] = (uint8_t)len, t[1] = (uint8_t)(len >> 8);
    if (hdr % 4)
        t[3] = 1u << (13 - 8);                  /* offload assist: TX_CMD_OFFLD_PAD */
    u32_to(t + 4, flags);
    if (data && c->associated && sta == AP_STA_ID) {
        flags |= 1u << 4;                       /* STA_RATE: the station's rate table (36d-2) */
        u32_to(t + 4, flags);
    } else {
        c->antenna = c->antenna == 1 ? 2 : 1;
        uint32_t rate = c->channel <= 14 ? 10u | 1u << 9 : 13u;     /* 1 Mbit/s CCK, or 6 OFDM */
        u32_to(t + 12, rate | (uint32_t)c->antenna << 14);
    }
    t[16] = sta;
    u32_to(t + 40, 0xFFFFFFFFu);                /* life time: infinite */
    uint64_t scratch = phys + 12;               /* (buffer 0 at the page: header 4, then 8) */
    u32_to(t + 44, (uint32_t)scratch);
    t[48] = (uint8_t)((scratch >> 32) & 0xF);
    t[49] = 60;                                 /* RTS retries */
    t[50] = 15;                                 /* data retries */
    t[51] = 8;                                  /* TID: non-QoS */
    if (data && (f[0] & 0x80))                  /* QoS data: its own TID */
        t[51] = f[(f[1] & 3) == 3 ? 30 : 24] & 0xF;
    uint16_t pm = 0;
    if (mgmt)
        pm = subtype == 0 || subtype == 2 ? 3 : subtype == 13 ? 0 : 2;
    else if (eapol)
        pm = 2;
    t[52] = (uint8_t)pm;
    memcpy(t + 56, f, hdr);
    int qtid = data && (f[0] & 0x80) ? f[(f[1] & 3) == 3 ? 30 : 24] & 7 : -1;
    bool numbered = qtid >= 0 && c->associated && sta == AP_STA_ID && c->tid_queue[qtid] == q;
    if (numbered) {
        /* (36e-5) QoS data numbered per TID by us, as Linux's driver does
         * (tid_data's seq_number): a burst queue's slot is its frame's
         * sequence number (low 8 bits), which the layer's one counter for
         * every frame would not keep. */
        uint16_t sc = (uint16_t)(c->tid_seq[qtid] << 4 | (le16(f + 22) & 0xF));
        t[56 + 22] = (uint8_t)sc, t[56 + 23] = (uint8_t)(sc >> 8);
        c->seq_off += (c->tid_seq[qtid] & 0xFF) != slot;
    }
    memcpy(page, cmd, 20);
    if (seal) {
        uint64_t pn = ++c->tx_pn;
        uint8_t *iv = page + 512;
        iv[0] = (uint8_t)pn, iv[1] = (uint8_t)(pn >> 8), iv[2] = 0, iv[3] = 0x20;   /* key 0 */
        for (int i = 0; i < 4; i++)
            iv[4 + i] = (uint8_t)(pn >> (16 + 8 * i));
        memcpy(page + 512 + 8, f + hdr, body - 8);
        t[17] = 2;                              /* sec_ctl: TX_CMD_SEC_CCM */
        memcpy(t + 20, c->ptk, 16);
        c->sealed++;
    } else {
        memcpy(page + 512, f + hdr, body);
    }
    uint8_t *tfd = (uint8_t *)phys_to_virt(c->tx_ring[q]) + slot * 128;
    memset(tfd, 0, 128);
    set_buffer(tfd, 0, phys, 20);
    set_buffer(tfd, 1, phys + 64 + 20, tb1_padded);
    if (body)
        set_buffer(tfd, 2, phys + 512, body);
    uint16_t bc = (uint16_t)(((len + 8 + (seal ? 8 : 0) + 3) / 4) | (uint32_t)sta << 12);
    uint8_t *bct = (uint8_t *)phys_to_virt(c->bc_tables) + q * 640;
    bct[slot * 2] = (uint8_t)bc, bct[slot * 2 + 1] = (uint8_t)(bc >> 8);
    if (slot < 64)
        bct[(256 + slot) * 2] = (uint8_t)bc, bct[(256 + slot) * 2 + 1] = (uint8_t)(bc >> 8);
    c->tx_fc[q][slot] = (uint16_t)(f[0] | f[1] << 8);
    arch_write_barrier();
    c->q_write[q] = (slot + 1) & 255;
    wr(c, HBUS_TARG_WRPTR, c->q_write[q] | (uint32_t)q << 8);
    c->tx_sent++;
    c->q_sent[q]++;
    if (numbered)
        c->tid_seq[qtid] = (c->tid_seq[qtid] + 1) & 0xFFF;
    c->tx_time[q][slot] = now_us();
    uint32_t flying = (c->q_write[q] - c->q_read[q]) & 255;
    if (flying > c->most_in_flight)
        c->most_in_flight = flying;
    return true;
}

/* 36d-3: a data TID's own queue, as Linux's iwl_mvm_sta_alloc_queue in
 * DQA: the station's reserved queue (4) for its first TID, the next free
 * of 10-30 for others; on its access category's FIFO (TID 0, 3 best
 * effort; 1, 2 background; 4, 5 video; 6, 7 voice), our pointers at the
 * frame's sequence number, SCD_QUEUE_CFG (a data queue: aggregation
 * allowed, window 64), then the station's queues. 0 if none could be had. */
static int data_queue(struct chip *c, int tid, uint16_t ssn)
{
    static const uint8_t fifo_of_tid[8] = { 1, 0, 0, 1, 2, 2, 3, 3 };
    if (c->tid_queue[tid])
        return c->tid_queue[tid];
    int q = 0;
    if (!(c->sta_queues & 1u << BSS_CLIENT_QUEUE))
        q = BSS_CLIENT_QUEUE;
    for (int i = MIN_DATA_QUEUE; !q && i <= MAX_DATA_QUEUE; i++)
        if (!(c->sta_queues & 1u << i))
            q = i;
    if (!q)
        return 0;
    uint64_t t_queue = 0, t_mod = 0;
    c->q_write[q] = c->q_read[q] = ssn & 0xFF;
    c->tid_seq[tid] = ssn & 0xFFF;              /* (36e-5) numbered by us from here */
    if (access_begin(c))
        wr(c, HBUS_TARG_WRPTR, c->q_write[q] | (uint32_t)q << 8);
    access_end(c);
    uint8_t cfg[12] = { 0, AP_STA_ID, (uint8_t)tid, (uint8_t)q, 1, 1, fifo_of_tid[tid],
                        CMD_FRAME_LIMIT, (uint8_t)ssn, (uint8_t)(ssn >> 8), 0, 0 };
    if (step(c, "a data queue", CMD_SCD_QUEUE_CFG, 0, cfg, sizeof(cfg), &t_queue) < 0)
        return 0;
    uint8_t sta[48];
    memset(sta, 0, sizeof(sta));
    sta[0] = 1;                                 /* modify */
    sta[2] = 0xFF, sta[3] = 0xFF;
    memcpy(sta + 8, c->ap, 6);
    sta[16] = AP_STA_ID;
    sta[17] = 1u << 7;                          /* STA_MODIFY_QUEUES (no flags changed: 36e-5) */
    u32_to(sta + 40, c->sta_queues | 1u << q);
    int got = step(c, "the station's queues", CMD_ADD_STA, 0, sta, sizeof(sta), &t_mod);
    if (got < 0)
        return 0;
    c->sta_queues |= 1u << q;
    c->tid_queue[tid] = (uint8_t)q;
    kprintf("  intelwifi:     %s: TID %d's data on queue %d (FIFO %u) from %u: %lu us; queues set "
            "(status 0x%X) %lu us\n", c->where, tid, q, fifo_of_tid[tid], ssn & 0xFFF, t_queue,
            got >= 4 ? le32(c->reply) & 0xFF : 0, t_mod);
    return q;
}

/* The layer's frame (the driver's thread): unicast ones to the access
 * point (added at the first), broadcast ones through the scanning station. */
static bool transmit_now(struct chip *c, const uint8_t *f, uint32_t len)
{
    if (len < 24)
        return false;
    if (f[4] & 1)
        return tx_frame(c, AUX_QUEUE, AUX_STA_ID, f, len);
    /* (37g-7) Another access point than the station's: the old station out
     * first, as mac80211 when an authentication ends (its station removed
     * before the next one's added). Added again as station 0 while the
     * firmware still held the first, ADD_STA got no answer and the firmware
     * stopped (0x2073: a join after a wake, the first try lost to the scan,
     * then the other band's access point). */
    if (c->ap_added && memcmp(c->ap, f + 4, 6))
        leave_ap(c);
    if (!c->ap_added)
        if (!add_ap(c, f + 4, (uint16_t)(le16(f + 22) >> 4)))
            return false;
    uint8_t subtype = f[0] >> 4;
    if (f[0] == 0xD0 && len >= 24 + 9 && f[24] == 3 && f[25] == 0) {
        /* (36e-5) the layer's ADDBA request: the starting sequence number
         * is the card's (layer's contract): the TID's next of ours */
        int tid = (le16(f + 27) >> 2) & 15;
        if (tid < 8 && c->tid_queue[tid]) {
            uint8_t *w = (uint8_t *)f;          /* (send_pending's buffer) */
            uint16_t ssn = (uint16_t)(c->tid_seq[tid] << 4);
            w[31] = (uint8_t)ssn, w[32] = (uint8_t)(ssn >> 8);
        }
    }
    if (c->associated && (f[0] & 0x0C) == 0x08 && (f[0] & 0x80)) {
        int tid = f[(f[1] & 3) == 3 ? 30 : 24] & 7;     /* QoS data: its TID's own queue */
        int q = data_queue(c, tid, (uint16_t)(le16(f + 22) >> 4));
        if (q)
            return tx_frame(c, q, AP_STA_ID, f, len);
    }
    if ((f[0] & 0x0C) == 0 && (subtype == 11 || subtype == 0 || subtype == 2)) {
        uint64_t us = 0;                        /* authentication, (re)association */
        if (!session(c, &us))
            return false;
    }
    return tx_frame(c, MGMT_QUEUE, AP_STA_ID, f, len);
}

/* A frame from another thread: queued for the driver's thread, which
 * sends it (the layer holds its lock while it hands a frame over, and the
 * driver's thread takes that lock to hand received frames up: waiting
 * here for the thread would hold both). False when TX_PENDING wait already. */
static bool queue_frame(struct chip *c, const void *frame, size_t length)
{
    if (length > WLAN_FRAME_MAX)
        return false;
    uint64_t state = spin_lock(&c->pend_lock);
    bool room = c->pend_head - c->pend_tail < TX_PENDING && c->pend[0];
    if (room) {
        uint32_t slot = c->pend_head % TX_PENDING;
        c->pend_len[slot] = (uint16_t)length;
        memcpy(c->pend[slot], frame, length);
        c->pend_head++;
    }
    spin_unlock(&c->pend_lock, state);
    if (room)
        wait_queue_wake_all(&c->waiters);
    return room;
}

/* The queued frames sent (the driver's thread). */
static void send_pending(struct chip *c)
{
    static uint8_t frame[WLAN_FRAME_MAX];       /* (the driver's thread only) */
    for (;;) {
        uint64_t state = spin_lock(&c->pend_lock);
        bool any = c->pend_tail != c->pend_head;
        uint32_t length = 0;
        if (any) {
            uint32_t slot = c->pend_tail % TX_PENDING;
            length = c->pend_len[slot];
            memcpy(frame, c->pend[slot], length);
            c->pend_tail++;
        }
        spin_unlock(&c->pend_lock, state);
        if (!any)
            return;
        if (!transmit_now(c, frame, length) && c->card) {
            c->card->tx_dropped++;
            c->send_failed++;
        }
    }
}

/* 36e-2: PHY context 0 as wide as the access point runs, as Linux after
 * mac80211 changes the channel context at the association
 * (iwl_mvm_phy_ctxt_changed): the channel stays the control channel, with
 * the width (PHY_VHT_CHANNEL_MODE20/40/80: 0, 1, 2) and where the control
 * channel lies from the centre (iwl_mvm_get_ctrl_pos: 10 MHz below it is
 * POS_1_BELOW 0, 30 below 1, 50 below 2, 70 below 3; above 4 to 7). On a
 * 40 MHz network at 20 the card could not hear the access point's wide
 * frames, and its rate control fell to MCS 1 at 20 MHz (36e-1's
 * measurement). False: the width stays 20. */
static bool widen(struct chip *c, int width, int center, uint64_t *us)
{
    int mhz = (c->channel - center) * 5;
    int pos = mhz == -10 ? 0 : mhz == -30 ? 1 : mhz == -50 ? 2 : mhz == -70 ? 3
            : mhz == 10 ? 4 : mhz == 30 ? 5 : mhz == 50 ? 6 : mhz == 70 ? 7 : -1;
    if (pos < 0 || (width == 40 && (pos & 3) != 0) || (width == 80 && (pos & 3) > 1))
        return false;
    uint8_t tx = (uint8_t)((c->phy_sku >> 16) & 0xF), rx = (uint8_t)((c->phy_sku >> 20) & 0xF);
    if (c->nvm_tx_ant)
        tx &= c->nvm_tx_ant;
    if (c->nvm_rx_ant)
        rx &= c->nvm_rx_ant;
    uint8_t ctx[36];
    memset(ctx, 0, sizeof(ctx));
    u32_to(ctx + 4, 2);                         /* modify */
    ctx[16] = c->channel <= 14 ? 1 : 0;
    ctx[17] = (uint8_t)c->channel;
    ctx[18] = width == 80 ? 2 : 1;
    ctx[19] = (uint8_t)pos;
    u32_to(ctx + 20, tx);
    u32_to(ctx + 24, (uint32_t)rx << 1 | 2u << 10 | 2u << 12);
    if (step(c, "the PHY context's width", CMD_PHY_CONTEXT, 0, ctx, sizeof(ctx), us) < 0)
        return false;
    c->width = width, c->center = center;
    return true;
}

/* 36e-4: the access point's rate table (LQ_CMD), as Linux's
 * rs_build_rates_table for its first column: 3 rates of one stream
 * (SISO, antenna A) twice each, then legacy rates from 36 Mbit/s down to
 * the end (antennas taking turns); no two-stream entries (mimo_delim 0):
 * Linux starts with one stream and only its rate scaling tries two (on
 * the other laptop every two-stream try failed: 6 lost tries a frame). The first rate from the signal as Linux's
 * rs_optimal_rates_vht/ht (VHT MCS 9 at -60 dBm or better, 8 at 20 MHz
 * where 9 is not allowed; HT MCS 7). rate_n_flags: VHT bit 26 (MCS bits
 * 0-3, streams - 1 bits 4-5), HT bit 8 (MCS index 0-15), the width at bit
 * 11, the short guard interval bit 13, antennas from bit 14. The
 * firmware steps down the table when a try fails; legacy only without HT.
 * text: the first rate, for the log. */
static void rate_table(struct chip *c, const struct wlan_ht *ht, int nss, uint8_t lq[88],
                       char *text, size_t size)
{
    static const uint8_t ofdm[8] = { 3, 1, 11, 9, 7, 5, 15, 13 };      /* 54 48 36 24 18 12 9 6 */
    static const int legacy_at[8] = { -60, -64, -72, -80, -84, -85, -87, -200 };
    static const int vht_at[10] = { -60, -64, -68, -72, -80, -84, -85, -87, -200, -200 };
    memset(lq, 0, 88);
    lq[0] = AP_STA_ID;
    lq[6] = 1;                                  /* single stream: antenna A */
    lq[7] = 3;                                  /* dual: A and B */
    lq[12] = (uint8_t)4000, lq[13] = (uint8_t)(4000 >> 8);   /* aggregation time limit (us) */
    lq[14] = 3;                                 /* aggregation disable start */
    lq[15] = c->agg_window && c->agg_window < 63 ? (uint8_t)c->agg_window : 63;   /* a burst's frames */
    int signal = c->ap_signal ? c->ap_signal : -70, n = 0;
    uint32_t entry[16];
    if (ht->on) {
        bool vht = ht->vht;
        int top = vht ? (c->width == 20 ? 8 : 9) : 7;
        if (vht) {                              /* the access point's highest for one stream */
            int map = ht->vht_rx_mcs & 3;
            top = map == 3 ? 7 : top < 7 + map ? top : 7 + map;
        }
        int mcs = top;
        for (int k = 0; k < 9 && signal < vht_at[k]; k++)
            mcs = top - (k + 1) >= 0 ? top - (k + 1) : 0;
        /* (36f-4) Two columns (see rs_decide): one stream up to top, two
         * streams up to the access point's highest for two (its MCS map,
         * bits 2-3; HT: MCS 15; no MCS 9 at 20 MHz). It starts with one
         * stream, from the signal, as Linux does. */
        int top2 = -1;
        if (nss == 2) {
            top2 = vht ? (c->width == 20 ? 8 : 9) : 7;
            if (vht) {
                int map = (ht->vht_rx_mcs >> 2) & 3;
                top2 = map == 3 ? -1 : top2 < 7 + map ? top2 : 7 + map;
            }
        }
        c->rs_max1 = top, c->rs_max2 = top2;
        int rung = mcs;
        if (c->rs_top >= 0) {                   /* (rate scaling's rung, within the limits) */
            int col = c->rs_top / 10 == 1 && top2 >= 0 ? 1 : 0, k = c->rs_top % 10;
            int lim = col ? top2 : top;
            rung = col * 10 + (k < lim ? k : lim);
        }
        c->rs_top = rung;
        uint32_t width = (uint32_t)(c->width == 80 ? 2 : c->width == 40 ? 1 : 0) << 11;
        bool sgi = vht ? (c->width == 80 ? (ht->vht_cap & 1u << 5) : c->width == 40 ?
                          (ht->cap & 1u << 6) : (ht->cap & 1u << 5)) != 0
                       : (c->width == 40 ? ht->cap & 1u << 6 : ht->cap & 1u << 5) != 0;
        uint32_t kind = vht ? 1u << 26 : 1u << 8;
        /* The rung and the two below it in its column, twice each (two
         * streams on antennas A and B, one on A); with two streams then
         * one stream's same three once each, antennas taking turns, no
         * short guard interval (Linux's secondary column); mimo_delim
         * counts the two-stream entries. */
        int col = rung / 10, k = rung % 10, two = 0;
        for (int m = k; m > k - 3 && m >= 0; m--)
            for (int t = 0; t < 2; t++, n++) {
                uint32_t code = vht ? (uint32_t)col << 4 | (uint32_t)m : (uint32_t)(col * 8 + m);
                entry[n] = kind | width | (sgi ? 1u << 13 : 0) | (col ? 3u : 1u) << 14 | code;
                two += col;
            }
        for (int m = k < top ? k : top; col && m > k - 3 && m >= 0; m--, n++)
            entry[n] = kind | width | (n & 1 ? 2u : 1u) << 14 | (uint32_t)m;
        lq[5] = (uint8_t)two;                   /* mimo_delim */
        ksnprintf(text, size, "%s MCS %d x %d stream(s)%s", vht ? "VHT" : "HT", k, col + 1,
                  sgi ? ", short GI" : "");
    }
    int first = 2;                              /* (after HT: from 36 Mbit/s) */
    if (!ht->on) {
        first = 0;
        while (first < 7 && signal < legacy_at[first])
            first++;
        static const unsigned mbit[8] = { 54, 48, 36, 24, 18, 12, 9, 6 };
        ksnprintf(text, size, "%u Mbit/s", mbit[first]);
    }
    for (int r = first; n < 16; r = r < 7 ? r + 1 : 7)
        for (int k = 0; k < (ht->on || r == 7 ? 1 : 2) && n < 16; k++, n++)
            entry[n] = ofdm[r] | (n & 1 ? 2u : 1u) << 14;
    for (int i = 0; i < 16; i++)
        u32_to(lq + 20 + 4 * i, entry[i]);
}

static uint32_t reciprocal(uint32_t v)
{
    return v ? 0xFFFFFFFFu / v : 0;             /* (the firmware's 2^32 / v) */
}

/* (37f) The MAC's power command (iwl_mac_power_cmd, 40 bytes; MAC 0):
 * power saving allowed, and with pm the station's power management. */
static bool mac_power(struct chip *c, bool pm, uint64_t *us)
{
    uint32_t bi = c->ap_bi ? c->ap_bi : 100, period = c->ap_dtim_period ? c->ap_dtim_period : 1;
    uint32_t keep = (uint32_t)((3ull * period * bi * 1024 + 999999) / 1000000);
    if (keep < 25)
        keep = 25;                              /* POWER_KEEP_ALIVE_PERIOD_SEC */
    uint8_t pw[40];
    memset(pw, 0, sizeof(pw));
    pw[4] = pm ? 1 | 2 : 1;                     /* POWER_SAVE_ENA, POWER_MANAGEMENT_ENA */
    pw[6] = (uint8_t)keep, pw[7] = (uint8_t)(keep >> 8);
    if (pm) {
        u32_to(pw + 8, 100000);                 /* rx_data_timeout: 100 ms (us) */
        u32_to(pw + 12, 100000);                /* tx_data_timeout */
    }
    if (step(c, "the MAC's power", CMD_MAC_POWER, 0, pw, sizeof(pw), us) < 0)
        return false;
    c->powersave = pm;
    c->keep_alive = (uint16_t)keep;
    return true;
}

/* 36d-2: associated, as Linux after mac80211's station state ASSOC and its
 * BSS_CHANGED_ASSOC (iwl_mvm_update_sta, the rate table, then
 * iwl_mvm_bss_info_changed_station), at 20 MHz for now: the access point's
 * station updated (AID; HT: two streams or one, A-MPDU size and density),
 * a rate table of legacy rates from the one the signal allows (Linux's rate
 * scaling comes later), the MAC context associated (the beacon interval,
 * DTIM and their times from the access point's last beacon, no foreign
 * beacons any more, QoS and HT as it does), the binding's whole airtime
 * (TIME_QUOTA, so the firmware stays on the channel), the Smart FIFO "full
 * on", multicast passed, and the session protection stopped. */
static bool associate(struct chip *c)
{
    const struct wlan_ht *ht = &c->req_ht;
    uint64_t t_sta = 0, t_lq = 0, t_mac = 0, t_quota = 0, t_sf = 0, t_mc = 0, t_wide = 0;
    if (ht->on && (ht->width == 40 || ht->width == 80) && ht->center && c->width == 20)
        widen(c, ht->width, ht->center, &t_wide);
    int nss = 1;
    if (ht->vht) {
        nss = ((ht->vht_rx_mcs >> 2) & 3) != 3 ? 2 : 1;
    } else if (ht->on && ht->mcs[1]) {
        nss = 2;
    }
    uint8_t sta[48];
    memset(sta, 0, sizeof(sta));
    sta[0] = 1;                                 /* modify */
    sta[2] = 0xFF, sta[3] = 0xFF;
    sta[16] = AP_STA_ID;
    uint32_t flags = 0, mask = 3u << 26 | 3u << 28 | 1u << 17;
    if (ht->on) {
        uint32_t agg = ht->vht ? (ht->vht_cap >> 23) & 7 : ht->ampdu & 3;
        flags |= (nss == 2 ? 1u << 28 : 0) | agg << 19 | (uint32_t)((ht->ampdu >> 2) & 7) << 23;
        flags |= (c->width == 80 ? 2u : c->width == 40 ? 1u : 0) << 26;   /* STA_FLG_FAT_EN */
        mask |= 7u << 19 | 7u << 23;
    }
    u32_to(sta + 20, flags);
    u32_to(sta + 24, mask);
    sta[36] = (uint8_t)c->req_aid, sta[37] = (uint8_t)(c->req_aid >> 8);
    if (ht->wmm) {
        sta[17] = 1u << 2;                      /* STA_MODIFY_UAPSD_ACS (none asked) */
        sta[46] = 128;                          /* sp_length */
    }
    int got = step(c, "the station's association", CMD_ADD_STA, 0, sta, sizeof(sta), &t_sta);
    if (got < 0)
        return false;
    uint32_t sta_status = got >= 4 ? le32(c->reply) & 0xFF : 0;
    uint8_t lq[88];
    char rates[64];
    c->rs_top = -1, c->rs_nss = TWO_STREAMS && nss == 2 ? 2 : 1;
    c->rs_tries = c->rs_through = c->rs_good = 0;
    c->rs_need = 3, c->rs_probing = false;
    c->rs_windows = 0;
    c->rs_col_wait = 0, c->rs_col_need = 8, c->rs_col_probing = false;
    memset(c->rs_when, 0, sizeof(c->rs_when));
    rate_table(c, ht, c->rs_nss, lq, rates, sizeof(rates));
    if (step(c, "the rate table", CMD_LQ, 0, lq, sizeof(lq), &t_lq) < 0)
        return false;
    char list[16 * 10 + 1];
    size_t at = 0;
    for (int i = 0; i < 16; i++)
        at += (size_t)ksnprintf(list + at, sizeof(list) - at, " %X", le32(lq + 20 + 4 * i));
    kprintf("  intelwifi:     %s: rate table (two streams in the first %u):%s\n", c->where, lq[5],
            list);
    /* The MAC context, associated (iwl_mvm_mac_ctxt_cmd_sta) */
    uint8_t m[148];
    memset(m, 0, sizeof(m));
    u32_to(m + 4, 2);                           /* modify */
    u32_to(m + 8, 5);
    memcpy(m + 16, c->mac, 6);
    memcpy(m + 24, c->ap, 6);
    u32_to(m + 32, 0xF);
    u32_to(m + 36, 0x15);
    u32_to(m + 44, 1u << 5);                    /* short preamble */
    u32_to(m + 48, c->channel > 14 ? 1u << 4 : 0);   /* short slot: always at 5 GHz */
    u32_to(m + 52, 0x4);                        /* ACCEPT_GRP (no foreign beacons now) */
    u32_to(m + 56, (ht->wmm ? 1u : 0) | (ht->on ? 2u : 0));   /* UPDATE_EDCA, TGN */
    static const uint16_t cwmin[4] = { 15, 15, 7, 3 }, cwmax[4] = { 1023, 1023, 15, 7 };
    static const uint8_t aifs[4] = { 7, 3, 2, 2 };
    static const uint16_t txop[4] = { 0, 0, 3008, 1504 };    /* BK BE VI VO (us) */
    for (int fifo = 0; fifo < 4; fifo++) {      /* mac80211's QoS defaults, or no QoS */
        uint8_t *ac = m + 60 + 8 * fifo;
        uint16_t lo = ht->wmm ? cwmin[fifo] : 15, hi = ht->wmm ? cwmax[fifo] : 1023;
        ac[0] = (uint8_t)lo, ac[1] = (uint8_t)(lo >> 8);
        ac[2] = (uint8_t)hi, ac[3] = (uint8_t)(hi >> 8);
        ac[4] = ht->wmm ? aifs[fifo] : 2;
        ac[5] = (uint8_t)(1u << fifo);
        uint16_t op = ht->wmm ? txop[fifo] : 0;
        ac[6] = (uint8_t)op, ac[7] = (uint8_t)(op >> 8);
    }
    uint32_t bi = c->ap_bi ? c->ap_bi : 100, period = c->ap_dtim_period ? c->ap_dtim_period : 1;
    uint64_t dtim_us = (uint64_t)c->ap_dtim_count * bi * 1024;
    uint8_t *st = m + 100;
    u32_to(st, 1);                              /* is_assoc */
    u32_to(st + 4, c->ap_device_ts + (uint32_t)dtim_us);
    u32_to(st + 8, (uint32_t)(c->ap_tsf + dtim_us));
    u32_to(st + 12, (uint32_t)((c->ap_tsf + dtim_us) >> 32));
    u32_to(st + 16, bi);
    u32_to(st + 20, reciprocal(bi));
    u32_to(st + 24, bi * period);
    u32_to(st + 28, reciprocal(bi * period));
    u32_to(st + 32, LISTEN_INTERVAL);
    u32_to(st + 36, c->req_aid);
    u32_to(st + 40, c->ap_device_ts);
    if (step(c, "the MAC context's association", CMD_MAC_CONTEXT, 0, m, sizeof(m), &t_mac) < 0)
        return false;
    uint8_t quota[64];
    memset(quota, 0, sizeof(quota));
    u32_to(quota + 4, 128);                     /* binding 0: all the airtime */
    for (int i = 1; i < 4; i++)
        u32_to(quota + 16 * i, 0xFFFFFFFFu);
    if (step(c, "the quota", CMD_QUOTA, 0, quota, sizeof(quota), &t_quota) < 0)
        return false;
    uint8_t sf[92];                             /* the Smart FIFO "full on" with the station */
    memset(sf, 0, sizeof(sf));
    u32_to(sf, 1);
    u32_to(sf + 4, 4096);
    u32_to(sf + 8, !ht->on ? 4096 : nss == 2 ? 8192 : 6144);
    for (int i = 0; i < 10; i++)
        u32_to(sf + 12 + 4 * i, 1000000);
    static const uint32_t full_on[10] = { 2016, 320, 2016, 320, 10016, 2016, 2016, 320, 2016, 320 };
    for (int i = 0; i < 10; i++)
        u32_to(sf + 52 + 4 * i, full_on[i]);
    if (step(c, "the Smart FIFO", CMD_SF_CFG, 0, sf, sizeof(sf), &t_sf) < 0)
        return false;
    uint8_t mc[12] = { 1, 0, 0, 1 };            /* own frames filtered, MAC 0, pass all */
    memcpy(mc + 4, c->ap, 6);
    if (step(c, "the multicast filter", CMD_MCAST_FILTER, 0, mc, sizeof(mc), &t_mc) < 0)
        return false;
    uint64_t t_pm = 0;                          /* (37f) dozing, if asked: now that the DTIM is known */
    if (!mac_power(c, c->req_powersave, &t_pm))
        return false;
    if (c->te_running) {                        /* (Linux: once the beacon's details are known) */
        uint8_t te[36];
        uint64_t us = 0;
        memset(te, 0, sizeof(te));
        u32_to(te + 4, 3);
        u32_to(te + 8, c->te_uid);
        step(c, "the session protection's removal", CMD_TIME_EVENT, 0, te, sizeof(te), &us);
        c->te_running = false;
    }
    c->associated = true;
    c->link_gone = false;
    c->aid = c->req_aid;
    kprintf("  intelwifi:     %s: channel %d at %d MHz (centre %d; the access point's %d)%s %lu "
            "us\n", c->where, c->channel, c->width, c->center, ht->on ? ht->width : 20,
            t_wide ? ": PHY context widened" : "", t_wide);
    kprintf("  intelwifi:     %s: associated: AID %u, %s%s, %d stream(s); beacon every %u TU, DTIM "
            "%u (count %u), signal %d dBm; station %lu us (status 0x%X), rate table from %s %lu "
            "us, MAC context %lu us, quota %lu us, Smart FIFO %lu us, multicast %lu us; power "
            "saving %s (keep-alive %u s) %lu us\n",
            c->where, c->req_aid, ht->vht ? "VHT" : ht->on ? "HT" : "legacy", ht->wmm ? ", WMM" : "",
            nss, bi, period, c->ap_dtim_count, c->ap_signal, t_sta, sta_status, rates, t_lq, t_mac,
            t_quota, t_sf, t_mc, c->powersave ? "on" : "off", c->keep_alive, t_pm);
    return true;
}

/* A request from another thread, done by the driver's (3 s at most). */
static bool request(struct chip *c, int kind)
{
    c->req_done = false;
    c->req_kind = kind;
    wait_queue_wake_all(&c->waiters);
    bool done = wait_event_until(&c->req_waiters, c->req_done, timer_ticks() + 3 * TIMER_HZ);
    return done && c->req_ok;
}

/* 36e-1: a Block Ack agreement for receiving, as Linux's
 * iwl_mvm_sta_rx_agg: the access point's station modified with the TID,
 * the first sequence number and the window to add (STA_MODIFY_ADD_BA_TID)
 * or the TID to remove (STA_MODIFY_REMOVE_BA_TID); the firmware then
 * answers the access point's bursts with one Block Ack each. The layer
 * keeps the frames in order (this family hands every frame up as it
 * comes, the firmware reorders nothing). */
static bool rx_block_ack(struct chip *c, int tid, bool on, uint16_t ssn, uint16_t window)
{
    if (!c->associated || tid < 0 || tid > 7)
        return false;
    uint8_t sta[48];
    memset(sta, 0, sizeof(sta));
    sta[0] = 1;                                 /* modify */
    sta[2] = 0xFF, sta[3] = 0xFF;
    memcpy(sta + 8, c->ap, 6);
    sta[16] = AP_STA_ID;
    if (on) {
        sta[17] = 1u << 3;                      /* STA_MODIFY_ADD_BA_TID */
        sta[28] = (uint8_t)tid;
        sta[30] = (uint8_t)ssn, sta[31] = (uint8_t)(ssn >> 8);
        sta[44] = (uint8_t)window, sta[45] = (uint8_t)(window >> 8);
    } else {
        sta[17] = 1u << 4;                      /* STA_MODIFY_REMOVE_BA_TID */
        sta[29] = (uint8_t)tid;
    }
    /* (36e-5) no station flags changed: their mask with flags 0 had set
     * the station back to 20 MHz and one stream after the association */
    u32_to(sta + 40, c->sta_queues);
    uint64_t us = 0;
    int got = step(c, on ? "a Block Ack for receiving" : "a Block Ack's end", CMD_ADD_STA, 0, sta,
                   sizeof(sta), &us);
    uint32_t status = got >= 4 ? le32(c->reply) & 0xFF : 0;
    bool ok = got >= 0 && status == 1;          /* ADD_STA_SUCCESS (4: no room for another) */
    if (ok && on)
        c->rx_ba |= (uint8_t)(1u << tid), c->rx_ba_started++;
    else if (ok)
        c->rx_ba &= (uint8_t)~(1u << tid), c->rx_ba_ended++;
    kprintf("  intelwifi:     %s: Block Ack for receiving TID %d %s (from %u, up to %u frames): "
            "status 0x%X, %lu us\n", c->where, tid, on ? "on" : "off", ssn & 0xFFF, window, status,
            us);
    return ok;
}

/* 36e-3: a CCMP-128 key into the card, as Linux's iwl_mvm_send_sta_key:
 * ADD_STA_KEY for the access point's station, in a key slot of its own
 * (the pairwise key 0, a group key its index 1-3; STA_KEY_MAX_NUM is 16),
 * flags CCM (2) with the key map bit (3), the key's index (bits 8-9) and,
 * for a group key, multicast (bit 14); version 2 of the command (76 bytes)
 * where the firmware has the API flag TKIP_MIC_KEYS, else version 1 (64).
 * The card then opens what comes with that key, and seals what we send
 * with the pairwise one (the TX command carries it). */
static bool add_key(struct chip *c, int kind, int index, const uint8_t key[16])
{
    if (!c->ap_added || (kind != WLAN_KEY_PAIRWISE && (kind != WLAN_KEY_GROUP || index < 1 ||
                                                       index > 3)))
        return false;
    bool group = kind == WLAN_KEY_GROUP;
    uint8_t k[76];
    memset(k, 0, sizeof(k));
    k[0] = AP_STA_ID;
    k[1] = (uint8_t)(group ? index : 0);        /* key_offset */
    uint16_t flags = 2 | 1u << 3 | (uint16_t)((group ? index : 0) << 8) | (group ? 1u << 14 : 0);
    k[2] = (uint8_t)flags, k[3] = (uint8_t)(flags >> 8);
    memcpy(k + 4, key, 16);
    uint32_t size = has_api(c, API_TKIP_MIC_KEYS) ? 76 : 64;
    uint64_t us = 0;
    int got = step(c, group ? "a group key" : "the pairwise key", CMD_ADD_STA_KEY, 0, k, size, &us);
    uint32_t status = got >= 4 ? le32(c->reply) & 0xFF : 0;
    bool ok = got >= 0 && status == 1;          /* ADD_STA_SUCCESS */
    if (ok && group) {
        c->groups_in_card |= (uint8_t)(1u << index);
    } else if (ok) {
        memcpy(c->ptk, key, 16);
        c->tx_pn = 0;                           /* (a new key: its numbers start again) */
        c->ptk_in_card = true;
    }
    kprintf("  intelwifi:     %s: %s %s the card (slot %u, %u bytes): status 0x%X, %lu us\n",
            c->where, group ? "a group key" : "the pairwise key", ok ? "in" : "not taken by",
            k[1], size, status, us);
    return ok;
}

/* The layer's operations (another thread's calls): the scan is started by
 * the driver's thread, which hands every frame up. (36d-1) Frames to send
 * are queued for it; tuning is a request it carries out (the layer calls
 * set_channel without its lock). */
static bool wifi_scan(struct net_card *card, const uint8_t *ssid, size_t length)
{
    (void)ssid;
    (void)length;
    struct chip *c = card->priv;
    c->scan_wanted = true;
    wait_queue_wake_all(&c->waiters);
    return true;
}

static bool wifi_transmit(struct net_card *card, const void *frame, size_t length)
{
    struct chip *c = card->priv;
    bool ok = queue_frame(c, frame, length);
    if (ok) {
        card->tx_frames++;
        card->tx_bytes += length;
    } else {
        card->tx_dropped++;
        c->pend_full++;
    }
    return ok;
}

/* The layer's join (after the association response; without its lock). */
static bool wifi_join(struct net_card *card, const uint8_t *bssid, uint16_t aid, int channel,
                      const struct wlan_ht *ht, bool powersave)
{
    struct chip *c = card->priv;
    if (!c->ap_added || memcmp(bssid, c->ap, 6) || channel != c->channel)
        return false;
    mutex_lock(&c->req_lock);
    c->req_aid = aid;
    c->req_ht = *ht;
    c->req_powersave = powersave;               /* (37f) */
    bool ok = request(c, REQ_JOIN);
    mutex_unlock(&c->req_lock);
    return ok;
}

/* 36e-5: sending in bursts for a TID, as Linux's iwl_mvm_sta_tx_agg_oper
 * once the access point accepted (its queue already allows aggregation,
 * SCD_QUEUE_CFG at its start): the station's tid_disable_tx without the
 * TID (STA_MODIFY_TID_DISABLE_TX, with its queues), then the rate table
 * again with the burst limit (min of the window and 63). */
static bool tx_block_ack(struct chip *c, int tid, bool on, uint16_t window)
{
    if (!c->associated || tid < 0 || tid > 7 || (on && !c->tid_queue[tid]))
        return false;
    uint8_t tids = on ? (uint8_t)(c->agg_tids | 1u << tid) : (uint8_t)(c->agg_tids & ~(1u << tid));
    uint8_t sta[48];
    memset(sta, 0, sizeof(sta));
    sta[0] = 1;                                 /* modify */
    uint16_t disable = (uint16_t)~tids;
    sta[2] = (uint8_t)disable, sta[3] = (uint8_t)(disable >> 8);
    memcpy(sta + 8, c->ap, 6);
    sta[16] = AP_STA_ID;
    sta[17] = 1u << 1 | 1u << 7;                /* STA_MODIFY_TID_DISABLE_TX, _QUEUES */
    u32_to(sta + 40, c->sta_queues);
    uint64_t us = 0, t_lq = 0;
    int got = step(c, on ? "bursts for sending" : "bursts' end", CMD_ADD_STA, 0, sta, sizeof(sta),
                   &us);
    uint32_t status = got >= 4 ? le32(c->reply) & 0xFF : 0;
    bool ok = got >= 0 && status == 1;
    if (ok) {
        c->agg_tids = tids;
        if (on)
            c->agg_window = window;
        uint8_t lq[88];
        char rates[64];
        rate_table(c, &c->req_ht, c->rs_nss, lq, rates, sizeof(rates));
        step(c, "the rate table", CMD_LQ, 0, lq, sizeof(lq), &t_lq);
    }
    kprintf("  intelwifi:     %s: sending in bursts on TID %d %s (queue %u, up to %u frames): "
            "status 0x%X, %lu us; rate table %lu us\n", c->where, tid, on ? "on" : "off",
            c->tid_queue[tid & 7], window, status, us, t_lq);
    return ok;
}

/* 36f-1: the access point left, as Linux takes the station down after
 * mac80211's disassociation: the MAC context unassociated
 * (iwl_mvm_bss_info_changed_station), the station's queues flushed
 * (TXPATH_FLUSH, DUMP_TX_FIFO_FLUSH: the frames come back with their TX
 * responses, a second at most), each disabled (SCD_QUEUE_CFG, action 0),
 * the station removed (REMOVE_STA: its keys and agreements with it). A
 * later join starts clean: before, a join again kept the old station,
 * its keys, queues and "associated". Nothing to do without a station. */
static bool leave_ap(struct chip *c)
{
    if (!c->ap_added)
        return true;
    uint64_t t_mac = 0, t_flush = 0, t_queues = 0, t_rm = 0, t_pm = 0;
    if (c->associated && c->powersave)
        mac_power(c, false, &t_pm);             /* (37f) awake to leave */
    if (c->associated)
        mac_context(c, 2, c->ap);
    t_mac = c->associated ? 1 : 0;
    c->associated = false;
    uint8_t flush[8] = { 0 };
    u32_to(flush, c->sta_queues);
    flush[4] = 1u << 1;                         /* DUMP_TX_FIFO_FLUSH */
    step(c, "the station's flush", CMD_TXPATH_FLUSH, 0, flush, sizeof(flush), &t_flush);
    uint64_t start = now_us();
    for (bool busy = true; busy && now_us() - start < 1000000;) {
        busy = false;
        for (int q = 1; q < TX_QUEUES; q++)
            busy |= (c->sta_queues & 1u << q) && c->q_read[q] != c->q_write[q];
        if (busy) {
            wait_events(c, INT_RUNNING, 20);
            rx_collect(c);
        }
    }
    int queues = 0;
    for (int q = 1; q < TX_QUEUES; q++) {
        if (!(c->sta_queues & 1u << q))
            continue;
        uint8_t cfg[12] = { 0, AP_STA_ID, 0, (uint8_t)q, 0 };   /* action 0: disable */
        step(c, "a queue's end", CMD_SCD_QUEUE_CFG, 0, cfg, sizeof(cfg), &t_queues);
        c->q_write[q] = c->q_read[q] = 0;
        queues++;
    }
    uint8_t rm[4] = { AP_STA_ID, 0, 0, 0 };
    int got = step(c, "the station's removal", CMD_REMOVE_STA, 0, rm, sizeof(rm), &t_rm);
    kprintf("  intelwifi:     %s: left the access point: MAC context %s; flushed %lu us (%lu us "
            "until the queues were empty); %d queue(s) ended %lu us; station removed (status "
            "0x%X) %lu us\n", c->where, t_mac ? "unassociated" : "kept", t_flush,
            now_us() - start, queues, t_queues, got >= 4 ? le32(c->reply) & 0xFF : 0, t_rm);
    c->ap_added = false;
    c->ptk_in_card = false, c->groups_in_card = 0;
    c->sta_queues = 0, c->agg_tids = 0, c->agg_window = 0, c->rx_ba = 0;
    memset(c->tid_queue, 0, sizeof(c->tid_queue));
    c->beacon_loss_at = 0, c->link_gone = false;
    c->leaves++;
    return true;
}

/* (36f-1) The layer's leave: the driver's thread takes the station down. */
static void wifi_leave(struct net_card *card)
{
    struct chip *c = card->priv;
    mutex_lock(&c->req_lock);
    request(c, REQ_LEAVE);
    mutex_unlock(&c->req_lock);
}

/* (36e-3) A key: true when the card took it (the layer then hands frames
 * over plain and takes the card's opened ones). */
static bool wifi_set_key(struct net_card *card, int kind, int index, const uint8_t key[16])
{
    struct chip *c = card->priv;
    mutex_lock(&c->req_lock);
    c->req_key_kind = kind, c->req_key_index = index;
    memcpy(c->req_key, key, 16);
    bool ok = request(c, REQ_KEY);
    memset(c->req_key, 0, 16);
    mutex_unlock(&c->req_lock);
    return ok;
}

/* (36e-1, 36e-5) Receiving or sending in bursts: done by the driver's
 * thread. */
static bool wifi_block_ack(struct net_card *card, int tid, bool sending, bool on, uint16_t ssn,
                           uint16_t window)
{
    struct chip *c = card->priv;
    mutex_lock(&c->req_lock);
    c->req_tid = tid, c->req_on = on, c->req_ssn = ssn, c->req_window = window;
    c->req_sending = sending;
    bool ok = request(c, REQ_BLOCK_ACK);
    mutex_unlock(&c->req_lock);
    return ok;
}

static bool wifi_set_channel(struct net_card *card, int channel)
{
    struct chip *c = card->priv;
    mutex_lock(&c->req_lock);
    c->req_channel = channel;
    bool ok = request(c, REQ_CHANNEL);
    mutex_unlock(&c->req_lock);
    return ok;
}

/* 36d-3: the driver's lines in `net`'s report: the link as the firmware
 * has it, what was sent (by queue: sent, acknowledged, failed), what came
 * in, and how the chip has behaved. */
static void wifi_details(struct net_card *card)
{
    struct chip *c = card->priv;
    kprintf("       intelwifi: firmware %u.%08x.%u, country \"%s\"; channel %d at %d MHz; %s",
            c->fw_major, c->fw_minor, c->fw_local, c->country_code[0] ? c->country_code : "?",
            c->channel, c->width,
            c->associated ? "associated, AID " : "not associated\n");
    if (c->associated)
        kprintf("%u, rate table from the signal %d dBm; power saving %s (keep-alive %u s)\n",
                c->aid, c->ap_signal, c->powersave ? "on" : "off", c->keep_alive);
    kprintf("       intelwifi: sent %u frame(s): %u acknowledged, %u failed; received %u (%u with a "
            "bad FCS)\n", c->tx_sent, c->tx_acked, c->tx_failed, c->rx_frames, c->rx_bad);
    char line[200];
    size_t used = 0;
    line[0] = 0;
    for (int q = 1; q < TX_QUEUES; q++)
        if (c->q_sent[q] && used + 40 < sizeof(line))
            used += (size_t)ksnprintf(line + used, sizeof(line) - used, " %d: %u/%u/%u", q,
                                      c->q_sent[q], c->q_acked[q], c->q_failed[q]);
    kprintf("       intelwifi: by queue (sent/acknowledged/failed):%s\n", used ? line : " none");
    kprintf("       intelwifi: TX responses: the slowest after %lu us, %u after more than 50 ms, %u "
            "after more than 500 ms; at most %u frame(s) in flight on a queue\n", c->tx_slowest,
            c->tx_over_50ms, c->tx_over_500ms, c->most_in_flight);
    kprintf("       intelwifi: data frames: %u answered after %u tries (RTS failed %u), %lu us on "
            "the air; the last at rate 0x%X\n", c->data_responses, c->data_tries, c->rts_failures,
            c->data_airtime, c->data_rate);
    if (c->associated && c->req_ht.on && c->rs_top >= 0) {
        char cols[2][10 * 6 + 1];
        for (int col = 0; col < 2; col++) {
            size_t at = 0;
            cols[col][0] = 0;
            for (int k = 0; k <= (col ? c->rs_max2 : c->rs_max1); k++)
                at += (size_t)ksnprintf(cols[col] + at, sizeof(cols[col]) - at, " %u",
                                        c->rs_sr[col * 10 + k]);
        }
        kprintf("       intelwifi: rate scaling: at %s MCS %d x %d stream(s); %u time(s) up, %u "
                "down; two streams tried %u time(s), kept %u; success (%%, MCS 0 up): one "
                "stream%s; two%s\n", c->req_ht.vht ? "VHT" : "HT", c->rs_top % 10,
                c->rs_top / 10 + 1, c->rs_ups, c->rs_downs, c->rs_col_tries, c->rs_col_kept,
                cols[0], c->rs_max2 >= 0 ? cols[1] : " none");
    }
    kprintf("       intelwifi: the driver's thread: the longest receive pass %lu us, sending pass %lu "
            "us, one frame handed up %lu us; at most %u frame(s) waiting\n", c->slowest_rx,
            c->slowest_send, c->slowest_frame_up, c->most_pending);
    kprintf("       intelwifi: Block Ack for receiving: TIDs 0x%02X; %u started, %u ended\n",
            c->rx_ba, c->rx_ba_started, c->rx_ba_ended);
    kprintf("       intelwifi: bursts sent: TIDs 0x%02X (up to %u frames); %u burst(s) of %u "
            "frame(s), %u Block Ack(s): %u sent, %u acknowledged; sequence numbers off their slot "
            "%u; single responses' index not the next slot %u\n", c->agg_tids, c->agg_window,
            c->agg_responses, c->agg_frames, c->ba_notifs, c->ba_txed, c->ba_acked, c->seq_off,
            c->ssn_other);
    kprintf("       intelwifi: keys in the card: pairwise %s, groups 0x%X; frames sealed %u, opened "
            "%u (%u with a wrong MIC), protected ones handed up whole %u\n",
            c->ptk_in_card ? "yes" : "no", c->groups_in_card, c->sealed, c->opened, c->opened_bad,
            c->protected_whole);
    uint32_t r = c->rx_rate;                    /* (Linux's RATE_MCS_*: HT bit 8, VHT bit 26) */
    static const char *const widths[4] = { "20", "40", "80", "160" };
    if (r & 1u << 26)
        kprintf("       intelwifi: data received: %u, %u in bursts; the last at VHT MCS %u x %u "
                "stream(s), %s MHz%s (0x%X)\n", c->rx_data, c->rx_in_burst, r & 0xF,
                ((r >> 4) & 3) + 1, widths[(r >> 11) & 3], r & 1u << 13 ? ", short GI" : "", r);
    else if (r & 1u << 8)
        kprintf("       intelwifi: data received: %u, %u in bursts; the last at HT MCS %u, %s "
                "MHz%s (0x%X)\n", c->rx_data, c->rx_in_burst, r & 0x1F, widths[(r >> 11) & 3],
                r & 1u << 13 ? ", short GI" : "", r);
    else
        kprintf("       intelwifi: data received: %u, %u in bursts; the last at a legacy rate "
                "(0x%X)\n", c->rx_data, c->rx_in_burst, r);
    kprintf("       intelwifi: the access point left %u time(s), lost %u time(s) (beacons missed: "
            "at most %u in a row); asked %u time(s), %u answered, %u false alarm(s)\n", c->leaves,
            c->links_lost, c->missed_most, c->probes, c->probes_answered, c->false_alarms);
    kprintf("       intelwifi: the firmware started again %u time(s)%s%s%s\n", c->restarts,
            c->restarts ? " (the last: " : "", c->restarts ? c->last_stop : "",
            c->restarts ? ")" : "");
    kprintf("       intelwifi: interrupts %lu (%lu found in the register instead), firmware errors "
            "%u; frames refused: %u with no place to wait, %u not sent (%u of them by a full "
            "ring)\n", c->interrupts, c->missed, c->fw_errors, c->pend_full, c->send_failed,
            c->ring_full);
}

/* wlan0: the card registered with the Wi-Fi layer (its channels the
 * allowed ones), before anything is handed up. */
static bool register_card(struct chip *c)
{
    for (int i = 0; i < TX_PENDING; i++)        /* (each its own block: no long run of pages) */
        if (!c->pend[i] && !(c->pend[i] = kmalloc(WLAN_FRAME_MAX))) {
            kprintf("  intelwifi:     %s: no memory for frames waiting to be sent\n", c->where);
            return false;
        }
    if (!(c->card = net_card_new()))
        return false;
    memcpy(c->card->mac, c->mac, 6);
    c->card->driver = "intelwifi";
    ksnprintf(c->card->model, sizeof(c->card->model), "Intel Dual Band Wireless-AC 8265");
    ksnprintf(c->card->where, sizeof(c->card->where), "%s", c->where);
    c->card->pci = c->pci;
    c->card->priv = c;
    c->card->details = wifi_details;
    for (int i = 0; i < c->nallowed && i < (int)sizeof(c->wifi.channels); i++)
        c->wifi.channels[i] = c->allowed[i];
    c->wifi.nchannels = c->nallowed;
    c->wifi.transmit = wifi_transmit;
    c->wifi.set_channel = wifi_set_channel;
    c->wifi.scan = wifi_scan;
    c->wifi.join = wifi_join;
    c->wifi.block_ack = wifi_block_ack;
    c->wifi.set_key = wifi_set_key;
    c->wifi.leave = wifi_leave;
    wlan_register(c->card, &c->wifi);
    c->registered = true;
    return true;
}

/* (36f-1) A null data frame to the access point (To DS, not protected),
 * as mac80211 asks it after a beacon loss (ieee80211_mgd_probe_ap): its
 * acknowledgement (TX response status 1) counts as hearing it. */
static void probe_ap(struct chip *c)
{
    uint8_t f[24];
    memset(f, 0, sizeof(f));
    f[0] = 0x48, f[1] = 0x01;                   /* data, null function; To DS */
    memcpy(f + 4, c->ap, 6);
    memcpy(f + 10, c->mac, 6);
    memcpy(f + 16, c->ap, 6);
    if (tx_frame(c, MGMT_QUEUE, AP_STA_ID, f, sizeof(f)))
        c->probes_sent++, c->probes++;
}

/* (36e-1) Work for the driver's thread other than the chip's: frames
 * handed over, a request, a scan wanted, the firmware's country. */
static bool work_waiting(struct chip *c)
{
    return c->pend_head != c->pend_tail || c->req_kind || c->scan_wanted || c->chub_pending ||
           c->sleep_wanted;
}

/* The serving loop's wait (up to ms): until the chip's interrupt or work
 * handed over (wait_events looks only at the chip: a frame queued while
 * it slept waited up to 20 ms, and the 8 places overflowed with TCP's
 * acknowledgements once frames came in bursts). */
static uint32_t serve_wait(struct chip *c, uint32_t ms)
{
    if (!work_waiting(c))
        wait_event_until(&c->waiters, (c->events & INT_RUNNING) || work_waiting(c),
                         timer_ticks() + (uint64_t)ms * TIMER_HZ / 1000 + 1);
    uint32_t seen = take_events(c, INT_RUNNING);
    if (seen)
        return seen;
    uint32_t raw = rd(c, CSR_INT) & INT_RUNNING;
    if (raw) {                                  /* (no interrupt: as wait_events) */
        wr(c, CSR_FH_INT_STATUS, rd(c, CSR_FH_INT_STATUS));
        wr(c, CSR_INT, raw);
        c->missed++;
    }
    return raw;
}

/* The driver's thread from here on: scans the layer asked for started,
 * every packet taken (frames handed up), a scan without its end given up
 * after 15 s, a firmware error's log shown once. */
static void serve(struct chip *c)
{
    uint32_t errors = c->fw_errors;
    for (;;) {
        if (c->sleep_wanted) {                  /* (37g-6) */
            ksnprintf(c->stop_why, sizeof(c->stop_why), "the machine goes to sleep");
            return;
        }
        /* (36e-6) The firmware stopped: an error it reported, two
         * commands in a row unanswered, or a queue's oldest frame waiting
         * 5 s for it. The caller starts it again. */
        if (c->fw_errors != errors) {
            ksnprintf(c->stop_why, sizeof(c->stop_why), "it reported an error");
            return;
        }
        if (c->silent >= 2) {
            ksnprintf(c->stop_why, sizeof(c->stop_why), "%u commands had no reply", c->silent);
            return;
        }
        if (c->beacon_loss_at) {                /* (36f-1) */
            uint64_t since = now_us() - c->beacon_loss_at;
            if (c->ap_heard_us > c->beacon_loss_at || !c->associated) {
                c->false_alarms += c->associated;
                c->beacon_loss_at = 0;          /* (it answered or came back, or we left) */
            } else if (c->probes_sent < (since > 500000 ? 2 : 1)) {
                probe_ap(c);                    /* (at once, and after half a second) */
            } else if (since > 1000000) {
                c->beacon_loss_at = 0;
                c->links_lost++;
                c->link_gone = true;            /* (the firmware goes on telling: once) */
                wlan_link_lost(c->card, "the access point's beacons stopped");
            }
        }
        for (int q = 1; q < TX_QUEUES; q++) {
            uint32_t slot = c->q_read[q];
            if (c->q_write[q] != slot && c->tx_time[q] && c->tx_time[q][slot] &&
                now_us() - c->tx_time[q][slot] > 5000000) {
                ksnprintf(c->stop_why, sizeof(c->stop_why), "queue %d's frames waited 5 s", q);
                return;
            }
        }
        if (c->scan_wanted && !c->scanning) {
            c->scan_wanted = false;
            start_scan(c);
        }
        if (c->req_kind) {
            int kind = c->req_kind;
            c->req_kind = 0;
            c->req_ok = kind == REQ_CHANNEL ? tune(c, c->req_channel)
                       : kind == REQ_JOIN ? associate(c)
                       : kind == REQ_BLOCK_ACK && c->req_sending ? tx_block_ack(c, c->req_tid,
                                                                  c->req_on, c->req_window)
                       : kind == REQ_BLOCK_ACK ? rx_block_ack(c, c->req_tid, c->req_on,
                                                              c->req_ssn, c->req_window)
                       : kind == REQ_KEY ? add_key(c, c->req_key_kind, c->req_key_index,
                                                   c->req_key)
                       : kind == REQ_LEAVE && leave_ap(c);
            c->req_done = true;
            wait_queue_wake_all(&c->req_waiters);
        }
        uint64_t t1 = now_us();
        uint32_t waiting = c->pend_head - c->pend_tail;
        if (waiting > c->most_pending)
            c->most_pending = waiting;
        send_pending(c);
        if (now_us() - t1 > c->slowest_send)
            c->slowest_send = now_us() - t1;
        if (c->chub_pending) {                  /* the firmware's country, confirmed as Linux */
            c->chub_pending = false;
            kprintf("  intelwifi:     %s: country: the firmware chose \"%c%c\" (source %u)\n",
                    c->where, c->chub_mcc >> 8, c->chub_mcc & 0xFF, c->chub_source);
            country(c, c->chub_mcc, c->chub_source);
        }
        if (serve_wait(c, 20) & (INT_SW_ERR | INT_HW_ERR))
            c->fw_errors++;
        uint64_t t0 = now_us();
        rx_collect(c);
        if (now_us() - t0 > c->slowest_rx)
            c->slowest_rx = now_us() - t0;
        if (c->lq_wanted && c->associated) {    /* (36e-4) rate scaling's new table */
            c->lq_wanted = false;
            uint8_t lq[88];
            char rates[64];
            uint64_t us = 0;
            rate_table(c, &c->req_ht, c->rs_nss, lq, rates, sizeof(rates));
            step(c, "the rate table", CMD_LQ, 0, lq, sizeof(lq), &us);
        }
        if (c->scanning && now_us() - c->scan_started > SCAN_TIMEOUT_US) {
            c->scanning = false;
            kprintf("  intelwifi:     %s: scan: no end in 15 s (%u frame(s) heard)\n", c->where,
                    c->heard);
            wlan_scan_done(c->card);
        }
    }
}

/* 36b-3b: from the calibrated init image to the run-time image, as Linux's
 * iwl_mvm_load_rt_fw and the start of iwl_mvm_up: the chip stopped and
 * started again (ready, reset, MAC up), the run-time image loaded and
 * alive, the transmit side started, paging; then the antennas, the PHY
 * database and the PHY configuration with the run-time triggers. */
static void run_time(struct chip *c)
{
    uint32_t errors = c->fw_errors;             /* (36e-6: this start's, not the boot's) */
    stop_device(c);
    set_bits(c, CSR_RESET, RESET_SW);
    delay_us(5000);
    if (wake(c) < 0) {
        kprintf("  intelwifi:     %s: the MAC clock did not come back after the stop\n", c->where);
        return;
    }
    if (!run_image(c, &c->rt, "run-time"))
        return;
    if (!tx_start(c)) {
        kprintf("  intelwifi:     %s: no memory or no access to start the transmit side\n",
                c->where);
        return;
    }
    if (!paging(c))
        return;
    if (!up_first(c))
        return;
    uint8_t ant[4], phy[12];
    uint8_t tx = (uint8_t)((c->phy_sku >> 16) & 0xF), rx = (uint8_t)((c->phy_sku >> 20) & 0xF);
    if (c->nvm_tx_ant)
        tx &= c->nvm_tx_ant;
    if (c->nvm_rx_ant)
        rx &= c->nvm_rx_ant;
    uint32_t phy_cfg = c->phy_sku & (~0x00FF0000u | (uint32_t)tx << 16 | (uint32_t)rx << 20);
    u32_to(ant, tx);
    uint64_t t_ant, t_db, t_phy;
    int sent = 0;
    if (command(c, CMD_TX_ANT, 0, ant, sizeof(ant), &t_ant) < 0) {
        kprintf("  intelwifi:     %s: run-time: the antenna command had no reply\n", c->where);
        return;
    }
    if (!send_phy_db(c, &sent, &t_db))
        return;
    u32_to(phy, phy_cfg);
    u32_to(phy + 4, c->calib_flow[UCODE_REGULAR]);
    u32_to(phy + 8, c->calib_event[UCODE_REGULAR]);
    int got = command(c, CMD_PHY_CONFIG, 0, phy, sizeof(phy), &t_phy);
    kprintf("  intelwifi:     %s: run-time: send antennas 0x%X (reply in %lu us); PHY database "
            "%d of %d part(s) sent in %lu us; PHY configuration 0x%X, triggers flow 0x%X event "
            "0x%X (%s%lu us)\n", c->where, tx, t_ant, sent, c->nphy, t_db, phy_cfg,
            c->calib_flow[UCODE_REGULAR], c->calib_event[UCODE_REGULAR],
            got >= 0 ? "reply in " : "no reply in ", t_phy);
    if (got < 0)
        return;
    c->nothers = 0;
    c->others[0] = 0;
    uint64_t start = now_us();
    bool ok = up_rest(c);
    for (uint64_t watch = now_us(); now_us() - watch < 1000000;) {
        if (wait_events(c, INT_RUNNING, 100) & (INT_SW_ERR | INT_HW_ERR))
            c->fw_errors++;
        rx_collect(c);
    }
    kprintf("  intelwifi:     %s: start-up %s in %lu us; notifications meanwhile and 1 s after: "
            "%d%s\n", c->where, ok ? "done" : "stopped", now_us() - start, c->nothers,
            c->nothers ? c->others : "");
    if (!ok || c->fw_errors != errors)
        return;
    /* 36c-2: the country and the scan's settings (the end of iwl_mvm_up),
     * the station's MAC context (the interface up), wlan0, then serving. */
    country(c, 'Z' << 8 | 'Z', MCC_GET_CURRENT);
    if (!scan_config(c) || !mac_context(c, 1, 0))
        return;
    if (!c->registered && !register_card(c)) {
        kprintf("  intelwifi:     %s: no memory for the card\n", c->where);
        return;
    }
    c->served_at = now_us();
    serve(c);
}

/* (36e-6) What the stopped firmware held, forgotten before it starts
 * again: the association and station (queues, agreements, keys), the
 * rings' indexes (the rings are cleared at the start), the PHY database
 * (calibration sends it again), frames waiting, a scan in progress and a
 * request a caller waits for (answered false). */
static void forget_firmware_state(struct chip *c)
{
    c->associated = c->ap_added = c->bound = c->te_running = c->init_complete = false;
    c->ptk_in_card = false, c->groups_in_card = 0;
    c->sta_queues = 0, c->agg_tids = 0, c->agg_window = 0, c->rx_ba = 0;
    memset(c->tid_queue, 0, sizeof(c->tid_queue));
    c->width = 20, c->channel = 0;
    for (int q = 0; q < TX_QUEUES; q++) {
        c->q_write[q] = c->q_read[q] = 0;
        if (c->tx_time[q])
            memset(c->tx_time[q], 0, 256 * sizeof(uint64_t));
    }
    for (int i = 0; i < c->nphy; i++)
        kfree(c->phy_db[i].data);
    c->nphy = 0, c->phy_dropped = 0;
    c->silent = c->silent_told = 0;
    c->beacon_loss_at = 0, c->link_gone = false;
    uint64_t state = spin_lock(&c->pend_lock);
    c->pend_tail = c->pend_head;
    spin_unlock(&c->pend_lock, state);
    if (c->scanning) {
        c->scanning = false;
        wlan_scan_done(c->card);
    }
    c->scan_wanted = false;
    if (c->req_kind) {
        c->req_kind = 0;
        c->req_ok = false;
        c->req_done = true;
        wait_queue_wake_all(&c->req_waiters);
    }
}

static bool start_hw(struct chip *c);

/* (37g-6) A sleep, as Linux's mac80211 without Wake-on-WLAN (the device
 * stopped, after it started from the beginning): the layer told the link
 * is gone (wifid joins again), what the firmware held forgotten, the chip
 * stopped, then the wait for the wake; then the chip made ready as at the
 * boot (Linux's iwl_trans_start_hw), and the caller runs the firmware's
 * start again. */
static void sleep_through(struct chip *c)
{
    kprintf("  intelwifi:     %s: the machine goes to sleep: the chip stopped\n", c->where);
    if (c->registered)
        wlan_link_lost(c->card, "the machine goes to sleep");
    forget_firmware_state(c);
    stop_device(c);
    c->asleep = true;
    wait_queue_wake_all(&c->sleep_wq);
    wait_event(&c->sleep_wq, !c->sleep_wanted);
    c->asleep = false;
    c->sleeps++;
    bool up = start_hw(c);
    kprintf("  intelwifi:     %s: after the sleep: %s\n", c->where,
            up ? "the chip ready; its firmware starts again" : "the chip did not come back");
}

/* The firmware's start: the init image (36b-1), its first commands and
 * calibration (36b-2, 36b-3a), then the run-time image (36b-3b). */
static void start_firmware(struct chip *c, const struct image *im)
{
    c->lock = (struct spinlock)SPINLOCK_INIT("intelwifi");
    c->req_lock = (struct mutex)MUTEX_INIT("intelwifi requests");
    c->pend_lock = (struct spinlock)SPINLOCK_INIT("intelwifi pending");
    int quick = 0;                              /* restarts in a row with under a minute served */
    for (;;) {
        uint32_t errors = c->fw_errors;
        c->served_at = 0;
        c->stop_why[0] = 0;
        if (run_image(c, im, "init")) {
            first_commands(c);
            if (c->init_complete && c->fw_errors == errors)
                run_time(c);                    /* (serves until the firmware stops) */
        }
        if (c->sleep_wanted) {
            sleep_through(c);                   /* (37g-6: then the boot's path again) */
            quick = 0;
            continue;
        }
        if (!c->stop_why[0])
            ksnprintf(c->stop_why, sizeof(c->stop_why), "its start failed");
        kprintf("  intelwifi:     %s: the firmware stopped (%s)\n", c->where, c->stop_why);
        error_logs(c);
        quick = c->served_at && now_us() - c->served_at > 60000000 ? 0 : quick + 1;
        if (!c->registered || quick > RESTARTS_QUICK) {
            kprintf("  intelwifi:     %s: not started again (%s)\n", c->where,
                    c->registered ? "it stopped 3 times within a minute of serving"
                                  : "it never served");
            return;
        }
        /* (36e-6) Started again as at the boot (Linux's restart runs the
         * init image and calibration again too): the layer told the link
         * is gone (wifid joins again), the driver's state of the
         * association, queues, keys and requests cleared, the chip
         * stopped. wlan0 stays registered. */
        c->restarts++;
        memcpy(c->last_stop, c->stop_why, sizeof(c->last_stop));
        wlan_link_lost(c->card, "its firmware stopped; starting it again");
        forget_firmware_state(c);
        stop_device(c);
    }
}

static int bits_set(uint32_t v)
{
    int n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

/* A section of an image: kept in the list; separators move on to the
 * second CPU's part or the paged part; anything else counts where it is. */
static void count_section(struct image *im, int *part, const uint8_t *data, uint32_t length)
{
    uint32_t offset = length >= 4 ? le32(data) : 0;
    if (length >= 4 && im->n < MAX_SECTIONS)
        im->list[im->n++] = (struct section){ offset, data + 4, length - 4 };
    if (offset == SEP_CPU1_CPU2) {
        *part = 1;
        return;
    }
    if (offset == SEP_PAGING) {
        *part = 2;
        return;
    }
    im->sections[*part]++;
    im->bytes[*part] += length >= 4 ? length - 4 : 0;
}

static void print_image(const char *name, const struct image *im)
{
    kprintf("  intelwifi:     firmware %s image: CPU1 %d section(s) %lu bytes, CPU2 %d section(s) "
            "%lu bytes, paged %d section(s) %lu bytes\n", name, im->sections[0], im->bytes[0],
            im->sections[1], im->bytes[1], im->sections[2], im->bytes[2]);
}

/* Intel's file, read and listed: its text, version, every part by type;
 * the file (kfree it), with the init image's sections in *init and the PHY
 * SKU in the chip, or 0. */
static uint8_t *list_firmware(struct chip *c, struct image *init)
{
    uint64_t size = 0;
    uint8_t *fw = firmware_load(FW_NAME, &size);
    if (!fw) {
        kprintf("  intelwifi:     firmware %s: not in /system/firmware\n", FW_NAME);
        return 0;
    }
    if (size < FW_HEADER || le32(fw) != 0 || le32(fw + 4) != FW_MAGIC) {
        kprintf("  intelwifi:     firmware %s: %lu bytes, not Intel's layout\n", FW_NAME, size);
        kfree(fw);
        return 0;
    }
    char text[65];
    memcpy(text, fw + 8, 64);
    text[64] = 0;
    kprintf("  intelwifi:     firmware %s: %lu bytes, \"%s\", file version 0x%X build %u\n",
            FW_NAME, size, text, le32(fw + 72), le32(fw + 76));
    struct image *rt = &c->rt, wowlan = { 0 };
    memset(rt, 0, sizeof(*rt));
    memset(init, 0, sizeof(*init));
    int rt_part = 0, init_part = 0, wowlan_part = 0, parts = 0, api = 0, capa = 0, cmds = 0;
    uint32_t sku = 0, scan_channels = 0, paging = 0, cpus = 0, calib = 0;
    uint32_t major = 0, minor = 0, local = 0;
    char types[256];                            /* the other parts' types, in order */
    size_t used = 0;
    types[0] = 0;
    uint64_t at = FW_HEADER;
    while (at + 8 <= size) {
        uint32_t type = le32(fw + at), length = le32(fw + at + 4);
        const uint8_t *data = fw + at + 8;
        if (length > size - at - 8) {
            kprintf("  intelwifi:     firmware part %d (type %u) at %lu: %u bytes, past the end\n",
                    parts, type, at, length);
            break;
        }
        parts++;
        switch (type) {
        case TLV_SEC_RT: count_section(rt, &rt_part, data, length); break;
        case TLV_SEC_INIT: count_section(init, &init_part, data, length); break;
        case TLV_SEC_WOWLAN: count_section(&wowlan, &wowlan_part, data, length); break;
        case TLV_PHY_SKU: if (length >= 4) sku = le32(data); break;
        case TLV_N_SCAN_CHANNELS:
            if (length >= 4)
                c->scan_slots = scan_channels = le32(data);
            break;
        case TLV_PAGING: if (length >= 4) paging = le32(data); break;
        case TLV_NUM_OF_CPU: if (length >= 4) cpus = le32(data); break;
        case TLV_DEF_CALIB:
            calib++;
            if (length >= 12 && le32(data) < 3) {
                c->calib_flow[le32(data)] = le32(data + 4);
                c->calib_event[le32(data)] = le32(data + 8);
            }
            break;
        case TLV_API_CHANGES_SET:
            if (length >= 8) {
                api += bits_set(le32(data + 4));
                if (le32(data) < 4)
                    c->api[le32(data)] |= le32(data + 4);
            }
            break;
        case TLV_ENABLED_CAPA:
            if (length >= 8) {
                capa += bits_set(le32(data + 4));
                if (le32(data) < 4)
                    c->capa[le32(data)] |= le32(data + 4);
            }
            break;
        case TLV_CMD_VERSIONS: cmds += (int)(length / 4); break;
        case TLV_FW_VERSION:
            if (length >= 12) {
                major = le32(data);
                minor = le32(data + 4);
                local = le32(data + 8);
            }
            break;
        default:
            if (used + 12 < sizeof(types))
                used += (size_t)ksnprintf(types + used, sizeof(types) - used, " %u", type);
            break;
        }
        at += 8 + ((length + 3) & ~3u);
    }
    c->fw_major = major, c->fw_minor = minor, c->fw_local = local;
    kprintf("  intelwifi:     firmware: %d part(s), version %u.%08x.%u, %u CPU(s), "
            "%d calibration default(s)\n", parts, major, minor, local, cpus, calib);
    print_image("run-time", rt);
    print_image("init", init);
    print_image("wake-on-WLAN", &wowlan);
    kprintf("  intelwifi:     firmware: PHY SKU 0x%X (antennas: send 0x%X, receive 0x%X), %u scan "
            "channels, %u bytes paged, %d API flag(s), %d capabilities, %d command versions\n",
            sku, (sku >> 16) & 0xF, (sku >> 20) & 0xF, scan_channels, paging, api, capa, cmds);
    kprintf("  intelwifi:     firmware: other parts by type:%s\n", used ? types : " none");
    if (init->n == MAX_SECTIONS || rt->n == MAX_SECTIONS) {
        kprintf("  intelwifi:     firmware: an image has more than %d sections\n",
                MAX_SECTIONS);
        kfree(fw);
        return 0;
    }
    c->phy_sku = sku;
    c->paging_bytes = paging;
    return fw;
}

static void worker(void *arg)
{
    struct chip *c = arg;
    uint8_t *fw = list_firmware(c, &c->init);
    if (fw) {
        start_firmware(c, &c->init);
        kfree(fw);
    }
    c->thread_ended = true;
}

static bool start(struct pci_device *pci)
{
    if (nchips == (int)(sizeof(chips) / sizeof(chips[0])))
        return false;
    struct chip *c = &chips[nchips];
    c->pci = pci;
    ksnprintf(c->where, sizeof(c->where), "%02x:%02x.%u", pci->bus, pci->device, pci->function);
    struct pci_bar *bar = &pci->bars[0];
    if (!bar->size || bar->is_io) {
        kprintf("  intelwifi:     %s: no register BAR\n", c->where);
        return false;
    }
    pci_enable_dma(pci);
    pci_write32(pci, 0x40, pci_read32(pci, 0x40) & ~(0xFFu << 8));     /* PCI_RETRY_TIMEOUT */
    c->regs = paging_map_mmio(bar->address, bar->size);
    uint32_t subsystem = pci_read32(pci, 0x2C);
    c->hw_rev = rd(c, CSR_HW_REV);
    kprintf("  intelwifi:     %s: Intel Dual Band Wireless-AC 8265 (subsystem %04x:%04x), BAR 0 "
            "%lu KiB, hardware revision 0x%X (type 0x%X, step %u)\n", c->where,
            subsystem & 0xFFFF, subsystem >> 16, bar->size / 1024, c->hw_rev,
            (c->hw_rev >> 4) & 0xFFF, (c->hw_rev >> 2) & 3);
    if (!start_hw(c))
        return false;
    nchips++;
    thread_create("intelwifi", worker, c);
    return true;
}

/* The chip made ready (at the start and, 37g-6, after a sleep). */
static bool start_hw(struct chip *c)
{
    wr(c, CSR_INT_MASK, 0);                     /* (nothing handles its interrupts yet) */
    wr(c, CSR_INT, 0xFFFFFFFFu);
    int64_t ready = make_ready(c);
    if (ready < 0) {
        kprintf("  intelwifi:     %s: the chip did not become ready (HW_IF_CONFIG 0x%X)\n",
                c->where, rd(c, CSR_HW_IF_CONFIG));
        return false;
    }
    /* Linux's order: the silicon step read with the clock on (the power
     * block WFPM on first, then AUX_MISC), then the whole chip reset (which
     * turns WFPM off again; Linux waits 5 ms), then the MAC powered up. */
    set_bits(c, CSR_GP_CNTRL, GP_INIT_DONE);
    delay_us(2);
    if (poll(c, CSR_GP_CNTRL, GP_MAC_CLOCK_READY, GP_MAC_CLOCK_READY, 25000) >= 0 &&
        access_begin(c)) {
        uint32_t wfpm = prph_rd(c, WFPM_CTRL_REG);
        prph_wr(c, WFPM_CTRL_REG, wfpm | WFPM_ENABLE);
        uint32_t now = prph_rd(c, WFPM_CTRL_REG), aux = prph_rd(c, AUX_MISC_REG);
        access_end(c);
        kprintf("  intelwifi:     %s: periphery WFPM_CTRL 0x%X, then 0x%X; AUX_MISC 0x%X "
                "(silicon step %u)\n", c->where, wfpm, now, aux, (aux >> 24) & 0xF);
    } else {
        access_end(c);
        kprintf("  intelwifi:     %s: periphery not reached (GP_CNTRL 0x%X)\n", c->where,
                rd(c, CSR_GP_CNTRL));
    }
    set_bits(c, CSR_RESET, RESET_SW);
    delay_us(5000);
    int64_t clock = wake(c);
    uint32_t gp = rd(c, CSR_GP_CNTRL), wfpm = 0xFFFFFFFFu;
    if (clock >= 0 && access_begin(c))
        wfpm = prph_rd(c, WFPM_CTRL_REG);
    access_end(c);
    kprintf("  intelwifi:     %s: ready after %ld us; reset; MAC clock %s%ld us; radio switch %s; "
            "GP_CNTRL 0x%X, GIO 0x%X, WFPM_CTRL 0x%X\n", c->where, ready,
            clock >= 0 ? "after " : "not running after ", clock >= 0 ? clock : 25000L,
            gp & GP_RF_KILL_SW ? "on" : "off (the radio is killed)", gp, rd(c, CSR_GIO), wfpm);
    return clock >= 0;
}

static struct chip *chip_of(struct pci_device *pci)
{
    for (int i = 0; i < nchips; i++)
        if (chips[i].pci == pci)
            return &chips[i];
    return 0;
}

/* (37g-6) The driver's thread stops the chip (at most 3 s: a thread still
 * starting the firmware calls the sleep off). */
static bool suspend(struct pci_device *pci)
{
    struct chip *c = chip_of(pci);
    if (!c || c->thread_ended)
        return true;
    c->sleep_wanted = true;
    wait_queue_wake_all(&c->waiters);
    wait_event_until(&c->sleep_wq, c->asleep, timer_ticks() + 3 * TIMER_HZ);
    if (c->asleep)
        return true;
    c->sleep_wanted = false;
    wait_queue_wake_all(&c->sleep_wq);
    kprintf("  intelwifi:     %s: no sleep now: its thread did not stop the chip within 3 s\n",
            c->where);
    return false;
}

/* PCI's retry timeout off again (Linux's iwl_pci_resume), then the
 * driver's thread starts the chip and its firmware again. */
static bool resume(struct pci_device *pci)
{
    struct chip *c = chip_of(pci);
    if (!c || c->thread_ended)
        return true;
    pci_write32(pci, 0x40, pci_read32(pci, 0x40) & ~(0xFFu << 8));     /* PCI_RETRY_TIMEOUT */
    c->sleep_wanted = false;
    wait_queue_wake_all(&c->sleep_wq);
    return true;
}

static const struct module_pci ids[] = {
    { 0x8086, 0x24FD },                         /* Dual Band Wireless-AC 8265 */
};

MODULE(intelwifi, start, suspend, resume, ids)
