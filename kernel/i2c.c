/*
 * The DesignWare I2C controller as a master (register facts from Synopsys's
 * documentation as Linux's i2c-designware driver uses them; the code is our
 * own). A transfer: the controller off, the device's address in IC_TAR, on
 * again; the bytes to write into IC_DATA_CMD, then one read command per byte
 * wanted (the first with RESTART after a write, the last with STOP), the
 * answers taken from the receive FIFO as they come. Polled: every wait has
 * a limit (TRANSFER_MS for the whole transfer).
 */
#include "i2c.h"
#include "arch.h"
#include "heap.h"
#include "kprintf.h"
#include "mmio.h"
#include "paging.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "timer.h"

#define IC_CON              0x00
#define IC_TAR              0x04
#define IC_DATA_CMD         0x10
#define IC_SS_SCL_HCNT      0x14
#define IC_SS_SCL_LCNT      0x18
#define IC_FS_SCL_HCNT      0x1C
#define IC_FS_SCL_LCNT      0x20
#define IC_INTR_MASK        0x30
#define IC_RAW_INTR_STAT    0x34
#define IC_RX_TL            0x38
#define IC_TX_TL            0x3C
#define IC_CLR_INTR         0x40
#define IC_ENABLE           0x6C
#define IC_STATUS           0x70
#define IC_TX_ABRT_SOURCE   0x80
#define IC_ENABLE_STATUS    0x9C
#define IC_COMP_PARAM_1     0xF4
#define IC_COMP_VERSION     0xF8
#define IC_COMP_TYPE        0xFC

#define DW_SIGNATURE        0x44570140      /* "DW" 0x0140 */
#define CON_MASTER          (1u << 0)
#define CON_SPEED_STD       (1u << 1)
#define CON_SPEED_FAST      (2u << 1)
#define CON_RESTART_EN      (1u << 5)
#define CON_SLAVE_DISABLE   (1u << 6)
#define CMD_READ            (1u << 8)
#define CMD_STOP            (1u << 9)
#define CMD_RESTART         (1u << 10)
#define STATUS_ACTIVITY     (1u << 0)
#define STATUS_TFNF         (1u << 1)
#define STATUS_RFNE         (1u << 3)
#define INTR_TX_ABRT        (1u << 6)
#define INTR_STOP_DET       (1u << 9)
#define ABRT_NOACK          0x0F            /* the address (7/10-bit) not acknowledged */

#define TRANSFER_MS         50
#define MAX_BUSES           8

struct i2c_bus {
    char name[8];
    uint64_t phys;
    volatile uint8_t *regs;
    int tx_depth, rx_depth;
    struct mutex lock;
    uint32_t clock_khz;             /* (37g-3) for setting it up again after a sleep */
    bool fast;
};

static struct i2c_bus *buses[MAX_BUSES];
static int bus_count;

static uint32_t rd(const struct i2c_bus *b, uint32_t r)
{
    return mmio_read32(b->regs + r);
}

static void wr(const struct i2c_bus *b, uint32_t r, uint32_t v)
{
    mmio_write32(b->regs + r, v);
}

/* Switch the controller on or off and wait until it is (at most ~25 ms). */
static bool set_enable(const struct i2c_bus *b, bool on)
{
    for (int i = 0; i < 250; i++) {
        wr(b, IC_ENABLE, on);
        if ((rd(b, IC_ENABLE_STATUS) & 1) == (uint32_t)on)
            return true;
        uint64_t t = arch_counter();
        while (arch_counter() - t < arch_counter_hz() / 10000)    /* 100 us */
            arch_cpu_relax();
    }
    return false;
}

/* SCL high and low counts for tHIGH/tLOW in ns (plus a 300 ns fall time),
 * as the DesignWare documentation's formulas. */
static uint32_t scl_count(uint32_t clock_khz, uint32_t ns, int offset)
{
    return (uint32_t)(((uint64_t)clock_khz * (ns + 300) + 500000) / 1000000) - (uint32_t)offset;
}

/* The controller's timing and mode (it is off): the boot's, and (37g-3)
 * again after a sleep. */
static void setup(struct i2c_bus *b)
{
    uint32_t clock_khz = b->clock_khz;
    wr(b, IC_SS_SCL_HCNT, scl_count(clock_khz, 4000, 3));      /* 100 kHz: tHIGH 4.0 us */
    wr(b, IC_SS_SCL_LCNT, scl_count(clock_khz, 4700, 1));      /*          tLOW  4.7 us */
    wr(b, IC_FS_SCL_HCNT, scl_count(clock_khz, 600, 3));       /* 400 kHz: tHIGH 0.6 us */
    wr(b, IC_FS_SCL_LCNT, scl_count(clock_khz, 1300, 1));      /*          tLOW  1.3 us */
    wr(b, IC_CON, CON_MASTER | (b->fast ? CON_SPEED_FAST : CON_SPEED_STD) | CON_RESTART_EN |
                  CON_SLAVE_DISABLE);
    wr(b, IC_INTR_MASK, 0);                                     /* polled */
    wr(b, IC_RX_TL, 0);
    wr(b, IC_TX_TL, 0);
    rd(b, IC_CLR_INTR);
}

void i2c_bus_hold(struct i2c_bus *b)
{
    mutex_lock(&b->lock);
}

void i2c_bus_let_go(struct i2c_bus *b)
{
    mutex_unlock(&b->lock);
}

bool i2c_bus_wake(struct i2c_bus *b)
{
    if (!set_enable(b, false))
        return false;
    setup(b);
    return true;
}

struct i2c_bus *i2c_bus_at(uint64_t phys, uint32_t clock_khz, uint32_t speed_hz, const char *what)
{
    for (int i = 0; i < bus_count; i++)
        if (buses[i]->phys == phys)
            return buses[i];
    if (bus_count == MAX_BUSES)
        return 0;
    volatile uint8_t *regs = paging_map_mmio(phys, 0x1000);
    if (!regs)
        return 0;
    struct i2c_bus *b = kzalloc(sizeof(*b));
    if (!b)
        return 0;
    b->phys = phys;
    b->regs = regs;
    b->lock = (struct mutex)MUTEX_INIT("i2c");
    uint32_t type = rd(b, IC_COMP_TYPE);
    if (type != DW_SIGNATURE) {
        kprintf("  i2c:           %s at 0x%lx: not a DesignWare controller (0x%08x)\n", what,
                phys, type);
        kfree(b);
        return 0;
    }
    uint32_t param = rd(b, IC_COMP_PARAM_1);
    b->rx_depth = (int)((param >> 8) & 0xFF) + 1;
    b->tx_depth = (int)((param >> 16) & 0xFF) + 1;
    if (!set_enable(b, false)) {
        kprintf("  i2c:           %s at 0x%lx: does not switch off\n", what, phys);
        kfree(b);
        return 0;
    }
    bool fast = speed_hz >= 400000;
    b->clock_khz = clock_khz;
    b->fast = fast;
    setup(b);
    ksnprintf(b->name, sizeof(b->name), "i2c%d", bus_count);
    buses[bus_count++] = b;
    char label[16];
    ksnprintf(label, sizeof(label), "%s:", b->name);
    kprintf("  %-15sDesignWare I2C at 0x%lx (%s), %u kHz, FIFO %d/%d, version 0x%08x\n", label,
            phys, what, fast ? 400u : 100u, b->tx_depth, b->rx_depth, rd(b, IC_COMP_VERSION));
    return b;
}

const char *i2c_name(const struct i2c_bus *b)
{
    return b->name;
}

int i2c_transfer(struct i2c_bus *b, uint16_t address, const uint8_t *w, size_t wn, uint8_t *r,
                 size_t rn)
{
    if (!wn && !rn)
        return 0;
    mutex_lock(&b->lock);
    int result = 0;
    if (!set_enable(b, false)) {
        mutex_unlock(&b->lock);
        return -ERR_TIMEDOUT;
    }
    wr(b, IC_TAR, address & 0x7F);
    rd(b, IC_CLR_INTR);
    if (!set_enable(b, true)) {
        mutex_unlock(&b->lock);
        return -ERR_TIMEDOUT;
    }
    uint64_t deadline = timer_ticks() + (TRANSFER_MS * TIMER_HZ + 999) / 1000 + 1;
    size_t sent = 0, asked = 0, got = 0;
    size_t total = wn + rn;                     /* commands: the writes, then the reads */
    while (got < rn || sent + asked < total) {
        uint32_t raw = rd(b, IC_RAW_INTR_STAT);
        if (raw & INTR_TX_ABRT) {
            uint32_t why = rd(b, IC_TX_ABRT_SOURCE);
            rd(b, IC_CLR_INTR);
            result = (why & ABRT_NOACK) ? -ERR_NODEV : -ERR_IO;
            break;
        }
        while (got < rn && (rd(b, IC_STATUS) & STATUS_RFNE))
            r[got++] = (uint8_t)rd(b, IC_DATA_CMD);
        /* Queue commands while the transmit FIFO has room, and never more
         * reads ahead than the receive FIFO can hold. */
        while (sent + asked < total && (rd(b, IC_STATUS) & STATUS_TFNF) &&
               (sent < wn || (int)(asked - got) < b->rx_depth - 1)) {
            uint32_t cmd;
            bool last = sent + asked + 1 == total;
            if (sent < wn) {
                cmd = w[sent++];
            } else {
                cmd = CMD_READ | (asked == 0 && wn ? CMD_RESTART : 0);
                asked++;
            }
            wr(b, IC_DATA_CMD, cmd | (last ? CMD_STOP : 0));
        }
        if (timer_ticks() > deadline) {
            result = -ERR_TIMEDOUT;
            break;
        }
        arch_cpu_relax();
    }
    /* Wait for the stop (the bus free) before the next transfer. */
    while (result == 0 && !(rd(b, IC_RAW_INTR_STAT) & INTR_STOP_DET) && timer_ticks() <= deadline)
        arch_cpu_relax();
    rd(b, IC_CLR_INTR);
    set_enable(b, false);
    mutex_unlock(&b->lock);
    return result;
}
