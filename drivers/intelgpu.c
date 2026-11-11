/*
 * (66a) Intel Bay Trail (Valleyview) HD Graphics, the J1900's onboard
 * graphics (PCI 8086:0f30-0f33): its outputs for the graphics layer
 * (gpu.c), and reading their monitors' EDID.
 *
 * The display engine's registers sit at VLV_DISPLAY_BASE (0x180000) in
 * BAR 0. Each output's DDC wires (the I2C bus a monitor answers on, its
 * EDID at address 0x50) are a pin pair of GMBUS, the engine's I2C
 * controller: VGA's pin 2, HDMI ports B and C pins 5 and 4 (Bay Trail has
 * no port D: Cherry Trail's; boards differ in which are wired). A read:
 * GMBUS0 selects the pins and the rate, GMBUS1 starts the cycle (the
 * address, the index to read from, the byte count, SW_RDY), and each 4
 * bytes wait for GMBUS2's HW_RDY (SATOER: no one answered) and come from
 * GMBUS3; then a STOP cycle, wait until no longer ACTIVE, GMBUS0 to 0.
 * Pins nobody is wired to never finish (the J1900's port C): the wait is
 * short (5 ms; ~0.6 ms is enough) and the bus is cleared at once, so such
 * an output costs ~5 ms a look instead of 60 (66a: gpu0 used 5.6% of a
 * core with ports C and D waiting 50 + 10 ms).
 * DisplayPort monitors answer over the DP AUX channel instead (not yet).
 * Which output shows the firmware's picture is its port's enable bit (bit
 * 31 of ADPA, HDMIB, HDMIC). The picture itself is not touched (66a).
 *
 * (66b-1) The mode the firmware set, read back (the report): each pipe's
 * timings (registers at 0x60000 + 0x1000 per pipe: active - 1 in the low
 * half, total or end - 1 in the high half), its PLL (DPLL: on and locked;
 * its dividers through the IOSF sideband from the DPIO unit, port 0x12:
 * pixel clock = 100 MHz x m1 x m2 / n / (p1 x p2 x 5)), the primary plane
 * (format, stride, its address in the GPU's page table, the GTT: 4-byte
 * entries in BAR 0's second 2 MiB, physical address bits 31-12 and 39-32
 * in bits 11-4), and the stolen memory the firmware's framebuffer lies in.
 *
 * (66b-2) Setting a mode on the output that shows the picture (HDMI, pipe
 * A so far), in i915's order: the plane, the port and the pipe off (the
 * pipe's state bit says when), the PLL down to its reference clock; the
 * timings and source size written; the PLL's dividers found as i915's
 * search does (n 1-5, p1 3-2, p2 20-2, m1 2-3, m2 rounded; the VCO
 * 4-6 GHz; the least error, a bigger p preferred within 100 ppm) and
 * written with the sideband values i915 uses for HDMI on pipe A, the PLL
 * on and locked; the port's sync directions; the pipe on, the plane on
 * at the new stride, the port on. The framebuffer stays where the
 * firmware's is (GPU address 0, the start of stolen memory); GTT entries
 * it grows into are filled from stolen memory. Limits: the output's (the
 * VBT's, else HDMI's 225 MHz on this generation), the display engine's,
 * and (66b-7) the FIFO: the firmware's watermarks are kept, so a mode's
 * need (i915's method 2) must fit in the reserve they keep.
 *
 * (37e-2d) An HDMI monitor to standby with the screen (the layer's
 * backlight operation): the port's enable bit cleared, so no signal goes
 * out and the monitor sleeps by itself; the pipe and plane keep running
 * (the vertical blank goes on), and the port counts as showing meanwhile;
 * on writes the port's register back.
 *
 * Register facts from Intel's Bay Trail display documentation and Linux's
 * i915 headers and sources (intel_gmbus_regs.h, intel_display_regs.h,
 * intel_crt_regs.h, i9xx_plane_regs.h, vlv_dpio_phy_regs.h, vlv_iosf_sb.c,
 * intel_dpll.c's vlv_calc_dpll_params).
 */
#include "driver.h"
#include "driver_gpu.h"

#define DISPLAY_BASE     0x180000u              /* VLV_DISPLAY_BASE */
#define GMBUS0           (DISPLAY_BASE + 0x5100)
#define GMBUS1           (DISPLAY_BASE + 0x5104)
#define GMBUS2           (DISPLAY_BASE + 0x5108)
#define GMBUS3           (DISPLAY_BASE + 0x510C)
#define GMBUS_FREQ_VLV   (DISPLAY_BASE + 0x6510)   /* the GMBUS clock, from CDCLK (MHz) */
#define GMBUS_RATE_100K  (0u << 8)
#define GMBUS_SW_CLR_INT (1u << 31)
#define GMBUS_SW_RDY     (1u << 30)
#define GMBUS_CYCLE_WAIT (1u << 25)
#define GMBUS_CYCLE_INDEX (2u << 25)
#define GMBUS_CYCLE_STOP (4u << 25)
#define GMBUS_INUSE      (1u << 15)             /* read: taken (if free); write 1: given back */
#define GMBUS_HW_RDY     (1u << 11)
#define GMBUS_SATOER     (1u << 10)             /* no acknowledge: nobody answered */
#define GMBUS_ACTIVE     (1u << 9)
#define EDID_ADDRESS     0x50
#define PIPE_TIMINGS(p)  (DISPLAY_BASE + 0x60000 + (p) * 0x1000)   /* HTOTAL; +4 HBLANK ... */
#define PIPE_CONF(p)     (DISPLAY_BASE + 0x70008 + (p) * 0x1000)
#define PIPE_SRC(p)      (DISPLAY_BASE + 0x6001C + (p) * 0x1000)
#define PIPE_ON          (1u << 31)
#define PLANE_CNTR(p)    (DISPLAY_BASE + 0x70180 + (p) * 0x1000)
#define PLANE_STRIDE(p)  (DISPLAY_BASE + 0x70188 + (p) * 0x1000)
#define PLANE_SURF(p)    (DISPLAY_BASE + 0x7019C + (p) * 0x1000)
#define PLANE_ON         (1u << 31)
#define PLANE_TILED      (1u << 10)
#define DPLL(p)          (DISPLAY_BASE + 0x6014 + (p) * 4)
#define DPLL_VCO_ON      (1u << 31)
#define DPLL_LOCKED      (1u << 15)
#define PORT_PIPE_B      (1u << 30)             /* SDVO_PIPE_SEL, ADPA_PIPE_SEL */
#define SB_DOORBELL      (DISPLAY_BASE + 0x2100)
#define SB_DATA          (DISPLAY_BASE + 0x2104)
#define SB_ADDR          (DISPLAY_BASE + 0x2108)
#define SB_BUSY          1u
#define SB_PORT_DPIO     0x12
#define SB_READ          0x00                   /* SB_MRD_NP */
#define DPIO_PLL_DW3(ch) (0x8000 + (ch) * 0x20 + 3 * 4)    /* the dividers; channel = pipe */
#define GTT_OFFSET       (2u << 20)             /* the GTT: BAR 0's second 2 MiB */
#define PIPE_STATE       (1u << 30)             /* the pipe is running (follows PIPE_ON) */
#define DPLL_MD(p)       (DISPLAY_BASE + 0x601C + (p) * 4)
#define DPLL_EXT_BUFFER  (1u << 30)
#define DPLL_REF_CLK     (1u << 29)
#define DPLL_VGA_MODE_DIS (1u << 28)
#define DPLL_INTEGRATED_REF (1u << 13)
#define SB_WRITE         0x01                   /* SB_MWR_NP */
#define DPIO_PLL_DW5(ch)  (0x8000 + (ch) * 0x20 + 5 * 4)
#define DPIO_PLL_DW7(ch)  (0x8000 + (ch) * 0x20 + 7 * 4)
#define DPIO_PLL_DW16(ch) (0x8000 + (ch) * 0x20 + 16 * 4)
#define DPIO_PLL_DW18(ch) (0x8000 + (ch) * 0x20 + 18 * 4)
#define DPIO_PLL_DW19(ch) (0x8000 + (ch) * 0x20 + 19 * 4)
#define DPIO_CMN_DW0      0x8100
#define DPIO_PCS_DW17_BCAST (0xC000 + 17 * 4)
#define DPIO_CALIBRATE   (1u << 11)
#define SYNC_V_HIGH      (1u << 4)              /* SDVO_VSYNC_ACTIVE_HIGH */
#define SYNC_H_HIGH      (1u << 3)
#define GTT_FLUSH        0x101008u              /* GFX_FLSH_CNTL_GEN6 */
#define HDMI_MAX_KHZ     225000
#define DSPARB           (DISPLAY_BASE + 0x70030)   /* the FIFO's split between the planes */
#define DSPARB2          (DISPLAY_BASE + 0x70060)   /* its ninth bits */
#define DSPFW1           (DISPLAY_BASE + 0x70034)   /* self-refresh, cursor B, planes B and A */
#define DSPFW2           (DISPLAY_BASE + 0x70038)   /* sprite B, cursor A, sprite A */
#define DSPFW3           (DISPLAY_BASE + 0x7003C)   /* self-refresh's cursor */
#define DSPHOWM          (DISPLAY_BASE + 0x70064)   /* the watermarks' high bits */
#define VLV_DDL(p)       (DISPLAY_BASE + 0x70050 + (p) * 4)   /* drain latency */
#define FW_BLC_SELF      (DISPLAY_BASE + 0x6500)    /* bit 15: memory self-refresh allowed */
#define VLV_IER          (DISPLAY_BASE + 0x20A0)    /* the display's interrupts: enabled */
#define VLV_IIR          (DISPLAY_BASE + 0x20A4)    /* ... raised (write 1 to clear) */
#define VLV_IMR          (DISPLAY_BASE + 0x20A8)    /* ... masked */
#define PIPE_A_EVENT     (1u << 6)                  /* in them: pipe A's PIPESTAT has news */
#define PIPESTAT(p)      (DISPLAY_BASE + 0x70024 + (p) * 0x1000)
#define START_VBLANK_ON  (1u << 18)                 /* PIPESTAT: interrupt at a vblank's start */
#define START_VBLANK     (1u << 2)                  /* ... its status (write 1 to clear) */
#define FRAME_COUNT(p)   (DISPLAY_BASE + 0x70040 + (p) * 0x1000)
#define PLANE_SURFLIVE(p) (DISPLAY_BASE + 0x701AC + (p) * 0x1000)  /* the address shown now */
#define MASTER_IER       0x4400Cu                   /* the graphics unit's: bit 31 on */
#define GTIMR            0x44014u                   /* the graphics core's (kept off) */
#define GTIIR            0x44018u
#define GTIER            0x4401Cu
#define PMIMR            0x44024u                   /* its power management's (kept off) */
#define PMIIR            0x44028u
#define PMIER            0x4402Cu
#define WM_LATENCY_US    3                      /* the memory's wake-up (PM2), i915's */
#define FIFO_LINES       511                    /* a pipe's FIFO, 64-byte lines */
#define PORT_ENABLE      (1u << 31)             /* ADPA_DAC_ENABLE, SDVO_ENABLE */

/* The chip's ports' registers (the family's own knowledge: no register
 * says where another is). */
#define PORT_ADPA        (DISPLAY_BASE + 0x61100)   /* VGA */
#define PORT_HDMIB       (DISPLAY_BASE + 0x61140)
#define PORT_HDMIC       (DISPLAY_BASE + 0x61160)
#define PORT_DPB         (DISPLAY_BASE + 0x64100)
#define PORT_DPC         (DISPLAY_BASE + 0x64200)
#define HOTPLUG_STAT     (DISPLAY_BASE + 0x61114)   /* live status: B bit 29, C 28 */
#define ASLS             0xFC                   /* PCI config: the OpRegion's address */

/* (66b-4) The outputs this board has, as its VBT describes them (the
 * firmware's table in the OpRegion: each wired port, its kind, its DDC
 * pins, what it may carry); without a VBT, every output the chip can have,
 * with Intel's usual pins (the fallback). */
static struct output_hw {
    const char *kind, *where;
    uint32_t pin, port;                 /* its GMBUS pin pair, its port register */
    uint32_t max_khz;                   /* the most it may carry (0: no limit of its own) */
    const char *limit_from;             /* where that came from */
} ports[GPU_OUTPUTS];
static int port_count;
static const char *outputs_from = "the driver's table (no VBT)";

static const struct output_hw fallback[] = {
    { "vga", "the VGA port", 2, PORT_ADPA, 0, "" },
    { "hdmi", "port B", 5, PORT_HDMIB, HDMI_MAX_KHZ, "the chip's HDMI" },
    { "hdmi", "port C", 4, PORT_HDMIC, HDMI_MAX_KHZ, "the chip's HDMI" },
};

/* What the VBT said, for the report. */
static struct {
    uint32_t asls, opregion_version, version, children;
    bool crt;
    uint32_t crt_pin;
    struct vbt_child { uint16_t type; uint8_t port, ddc, aux; } child[8];
} vbt;

static struct gpu_card bay_trail;

/* (66c-2) The vertical blank: pipe A's start-of-vblank interrupt, by MSI.
 * Counted, and waiters woken (gpu.h's wait_vblank). */
static bool vblank_irq;
static volatile uint64_t vblanks, interrupts;
static struct wait_queue vblank_waiters;

/* (66d-1) The screen buffers (see buffers_ready). */
static uint32_t buffer_at[2];
static uint64_t buffer_bytes;           /* each; 0: buffer 1 not made for this mode */
static uint64_t flips;
static int buffer_shown;
static volatile uint32_t standby;               /* (37e-2d) outputs whose signal is off (bits) */
static uint32_t standby_port[8];                /* their ports' registers as they were */
static uint64_t standby_offs, standby_ons;
static volatile uint8_t *regs;
static uint32_t start_gmbus0, start_gmbus2, start_freq;  /* as the firmware left them */

/* How each port's reads went (66a: the J1900 read nothing; only real
 * hardware shows why). Written by the layer's thread, read by the report. */
static struct {
    uint64_t reads, whole, nak, late, stuck;  /* stuck: the bus stayed active after it */
    uint32_t last_status, last_at;      /* GMBUS2 at the last failure, the byte it stopped at */
} tally[GPU_OUTPUTS];

static uint32_t rd(uint32_t off)
{
    return mmio_read32(regs + off);
}

static void wr(uint32_t off, uint32_t v)
{
    mmio_write32(regs + off, v);
}

/* Wait (at most ms) until GMBUS2 has any of `bits`: 0 when it does, 1 on a
 * NAK, 2 out of time; *status is GMBUS2 as last read. The bus runs at
 * 100 kHz: 4 bytes take ~0.4 ms. */
static int gmbus_wait(uint32_t bits, uint32_t ms, uint32_t *status)
{
    uint64_t hz = arch_counter_hz(), end = arch_counter() + hz * ms / 1000;
    for (;;) {
        uint32_t s = *status = rd(GMBUS2);
        if (s & GMBUS_SATOER)
            return 1;
        if (s & bits)
            return 0;
        if (arch_counter() >= end)
            return 2;
        arch_cpu_relax();
    }
}

/* Wait (at most 10 ms) until the bus is idle: false if it stays active. */
static bool gmbus_idle(void)
{
    uint64_t hz = arch_counter_hz(), end = arch_counter() + hz * 10 / 1000;
    while (rd(GMBUS2) & GMBUS_ACTIVE)
        if (arch_counter() >= end)
            return false;
        else
            arch_cpu_relax();
    return true;
}

/* After a transfer (why 0), a NAK (1) or running out of time (2): back
 * to idle, and the bus given back. A NAK is cleared only once the bus is
 * idle (cleared while it is active, it sticks and every later transfer
 * fails: 66a's first J1900 boot, all four outputs "no answer"); out of
 * time it never goes idle by itself: cleared at once. Reading GMBUS2 takes
 * its in-use bit (the semaphore the firmware's graphics driver shares:
 * held, the J1900's warm reboot hung); writing 1 to it gives it back.
 * False if the bus is still active. */
static bool gmbus_reset(int why)
{
    if (why == 0)
        wr(GMBUS1, GMBUS_CYCLE_STOP | GMBUS_SW_RDY);
    bool idle = why == 2 || gmbus_idle();
    if (why) {
        wr(GMBUS1, GMBUS_SW_CLR_INT);
        wr(GMBUS1, 0);
    }
    wr(GMBUS0, 0);
    if (why == 2)
        idle = !(rd(GMBUS2) & GMBUS_ACTIVE);
    wr(GMBUS2, GMBUS_INUSE);
    return idle;
}

static bool read_edid(struct gpu_card *g, int i, uint32_t offset, uint8_t *buf, uint32_t len)
{
    (void)g;
    if (len > 256 || offset > 255 || strcmp(ports[i].kind, "dp") == 0)
        return false;                                   /* (DisplayPort: AUX, not yet) */
    wr(GMBUS0, GMBUS_RATE_100K | ports[i].pin);
    wr(GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_INDEX | GMBUS_CYCLE_WAIT | len << 16 | offset << 8 |
               EDID_ADDRESS << 1 | 1);
    tally[i].reads++;
    for (uint32_t got = 0; got < len; got += 4) {
        uint32_t status;
        int why = gmbus_wait(GMBUS_HW_RDY, 5, &status);
        if (why) {
            if (why == 1)
                tally[i].nak++;
            else
                tally[i].late++;
            tally[i].last_status = status;
            tally[i].last_at = got;
            if (!gmbus_reset(why))
                tally[i].stuck++;
            return false;
        }
        uint32_t v = rd(GMBUS3);
        for (uint32_t k = 0; k < 4 && got + k < len; k++)
            buf[got + k] = (uint8_t)(v >> (8 * k));
    }
    if (!gmbus_reset(0))
        tally[i].stuck++;
    tally[i].whole++;
    return true;
}

/* Wait (at most ms) until the sideband is not busy. */
static bool sb_idle(uint32_t ms)
{
    uint64_t hz = arch_counter_hz(), end = arch_counter() + hz * ms / 1000;
    while (rd(SB_DOORBELL) & SB_BUSY)
        if (arch_counter() >= end)
            return false;
        else
            arch_cpu_relax();
    return true;
}

/* Read or write a DPIO register through the IOSF sideband; false if it
 * never answers. */
static bool dpio_access(uint32_t opcode, uint32_t reg, uint32_t *val)
{
    static struct mutex sideband = MUTEX_INIT("intelgpu sideband");
    mutex_lock(&sideband);
    bool ok = sb_idle(5);
    if (ok) {
        wr(SB_ADDR, reg);
        wr(SB_DATA, opcode == SB_READ ? 0 : *val);
        wr(SB_DOORBELL, opcode << 16 | SB_PORT_DPIO << 8 | 0xFu << 4 | SB_BUSY);
        ok = sb_idle(10);
        if (ok && opcode == SB_READ)
            *val = rd(SB_DATA);
    }
    mutex_unlock(&sideband);
    return ok;
}

static bool dpio_read(uint32_t reg, uint32_t *val)
{
    return dpio_access(SB_READ, reg, val);
}

static bool dpio_write(uint32_t reg, uint32_t val)
{
    return dpio_access(SB_WRITE, reg, &val);
}

struct dividers {
    uint32_t n, m1, m2, p1, p2;
};

/* The pixel clock (kHz) dividers give: 100 MHz x m1 x m2 / n / (p1 x p2 x
 * 5), each step rounded as the hardware's documentation and i915 do. */
static uint32_t dividers_khz(const struct dividers *d, uint32_t *vco)
{
    uint64_t p = (uint64_t)d->p1 * d->p2 * 5;
    *vco = d->n ? (uint32_t)((100000ull * d->m1 * d->m2 + d->n / 2) / d->n) : 0;
    return p ? (uint32_t)((*vco + p / 2) / p) : 0;
}

/* i915's search (vlv_find_best_dpll), the dividers for target kHz. */
static bool find_dividers(uint32_t target, struct dividers *best)
{
    uint32_t best_ppm = 1000000, best_p = 0;
    bool found = false;
    struct dividers d;
    for (d.n = 1; d.n <= 5; d.n++)                      /* 100 MHz / n at least 19.2 */
        for (d.p1 = 3; d.p1 >= 2; d.p1--)
            for (d.p2 = 20; d.p2 >= 2; d.p2 -= d.p2 > 10 ? 2 : 1)
                for (d.m1 = 2; d.m1 <= 3; d.m1++) {
                    uint32_t p = d.p1 * d.p2 * 5, vco;
                    uint64_t num = (uint64_t)target * p * d.n, den = 100000ull * d.m1;
                    d.m2 = (uint32_t)((num + den / 2) / den);
                    if (d.m2 < 11 || d.m2 > 156)
                        continue;
                    uint32_t khz = dividers_khz(&d, &vco);
                    if (vco < 4000000 || vco > 6000000 || khz < 25000 || khz > 270000)
                        continue;
                    uint32_t ppm = (uint32_t)(1000000ull * (khz > target ? khz - target
                                                                           : target - khz) / target);
                    if (ppm < 100 && p > best_p)
                        ppm = 0;                        /* a bigger p, nearly as close */
                    else if (ppm + 10 >= best_ppm)
                        continue;
                    *best = d;
                    best_ppm = ppm;
                    best_p = p;
                    found = true;
                }
    return found;
}

/* Wait (at most ms) until (register & mask) == want. */
static bool wait_for(uint32_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint64_t hz = arch_counter_hz(), end = arch_counter() + hz * ms / 1000;
    while ((rd(reg) & mask) != want)
        if (arch_counter() >= end)
            return false;
        else
            arch_cpu_relax();
    return true;
}

static void wait_us(uint32_t us)
{
    uint64_t hz = arch_counter_hz(), end = arch_counter() + hz * us / 1000000;
    while (arch_counter() < end)
        arch_cpu_relax();
}

/* The GTT entries from GPU address 0 for `bytes` of framebuffer: those not
 * valid are pointed at stolen memory at the same offset (the firmware's
 * framebuffer lies at the start of it). False if it would not fit. */
static bool gtt_cover(struct gpu_card *g, uint64_t bytes)
{
    uint64_t stolen = pci_read32(g->pci, 0x5C) & 0xFFF00000u;
    uint64_t stolen_size = (uint64_t)(pci_read16(g->pci, 0x50) >> 3 & 0x1F) * (32u << 20);
    if (bytes > stolen_size)
        return false;
    uint32_t flags = rd(GTT_OFFSET) & 0xF;            /* entry 0's: valid, writeable... */
    bool wrote = false;
    for (uint64_t page = 0; page < (bytes + 4095) / 4096; page++) {
        if (rd(GTT_OFFSET + (uint32_t)page * 4) & 1)
            continue;
        uint64_t phys = stolen + page * 4096;
        wr(GTT_OFFSET + (uint32_t)page * 4,
           (uint32_t)(phys & 0xFFFFF000u) | (uint32_t)(phys >> 28 & 0xFF0) | flags | 1);
        wrote = true;
    }
    if (wrote) {
        wr(GTT_FLUSH, 1);                               /* the GPU forgets its cached entries */
        (void)rd(GTT_FLUSH);
    }
    return true;
}

static uint32_t wm_need(uint32_t khz, uint32_t htotal, uint32_t width);
static uint32_t wm_reserve(void);

static bool set_mode(struct gpu_card *g, int i, const struct edid_detailed *t, struct gpu_screen *fb,
                     char *why, size_t why_size)
{
    uint32_t port_reg = ports[i].port, port = rd(port_reg);
    struct dividers d;
    uint32_t stride = (t->hactive * 4 + 63) & ~63u;
    if (strcmp(ports[i].kind, "hdmi")) {
        ksnprintf(why, why_size, "only HDMI outputs so far");
        return false;
    }
    if (!(port & PORT_ENABLE) || (port & PORT_PIPE_B) || !(rd(PIPE_CONF(0)) & PIPE_ON)) {
        ksnprintf(why, why_size, "only the output pipe A shows so far");
        return false;
    }
    uint32_t engine_khz = start_freq * 1000 * 90 / 100;     /* 90% of the display clock */
    if (ports[i].max_khz && t->clock_khz > ports[i].max_khz) {
        ksnprintf(why, why_size, "needs %u.%03u MHz, more than the %u MHz %s carries",
                  t->clock_khz / 1000, t->clock_khz % 1000, ports[i].max_khz / 1000,
                  ports[i].limit_from);
        return false;
    }
    if (engine_khz && t->clock_khz > engine_khz) {
        ksnprintf(why, why_size, "needs %u.%03u MHz, more than the display engine's %u MHz "
                  "(90%% of its %u MHz clock)", t->clock_khz / 1000, t->clock_khz % 1000,
                  engine_khz / 1000, start_freq);
        return false;
    }
    /* (66b-7) The watermarks stay the firmware's: the mode's need (i915's
     * method) must fit in the reserve they keep. */
    uint32_t need = wm_need(t->clock_khz, t->htotal, t->hactive), reserve = wm_reserve();
    if (need > reserve) {
        ksnprintf(why, why_size, "needs %u lines of the display FIFO in reserve, the firmware's "
                  "watermarks keep %u", need, reserve);
        return false;
    }
    if (!find_dividers(t->clock_khz, &d)) {
        ksnprintf(why, why_size, "no PLL setting for %u.%03u MHz", t->clock_khz / 1000,
                  t->clock_khz % 1000);
        return false;
    }
    if (!gtt_cover(g, (uint64_t)stride * t->vactive)) {
        ksnprintf(why, why_size, "the framebuffer does not fit in stolen memory");
        return false;
    }
    /* Off: plane, port, pipe (until its state follows), PLL to its reference. */
    uint32_t cntr = rd(PLANE_CNTR(0)), conf = rd(PIPE_CONF(0));
    wr(PLANE_CNTR(0), cntr & ~PLANE_ON);
    wr(PLANE_SURF(0), 0);
    timer_sleep_ms(25);                                 /* the plane goes at the next frame */
    wr(port_reg, port & ~PORT_ENABLE);
    wr(PIPE_CONF(0), conf & ~PIPE_ON);
    bool stopped = wait_for(PIPE_CONF(0), PIPE_STATE, 0, 100);
    uint32_t dpll = DPLL_INTEGRATED_REF | DPLL_REF_CLK | DPLL_VGA_MODE_DIS;
    wr(DPLL(0), dpll);
    /* The timings: active - 1 and total - 1 (blanking from the end of the
     * active area to the total), sync start - 1 and end - 1. */
    wr(PIPE_TIMINGS(0) + 0, (t->hactive - 1) | (t->htotal - 1) << 16);
    wr(PIPE_TIMINGS(0) + 4, (t->hactive - 1) | (t->htotal - 1) << 16);
    wr(PIPE_TIMINGS(0) + 8, (t->hsync_start - 1) | (t->hsync_end - 1) << 16);
    wr(PIPE_TIMINGS(0) + 12, (t->vactive - 1) | (t->vtotal - 1) << 16);
    wr(PIPE_TIMINGS(0) + 16, (t->vactive - 1) | (t->vtotal - 1) << 16);
    wr(PIPE_TIMINGS(0) + 20, (t->vsync_start - 1) | (t->vsync_end - 1) << 16);
    wr(PIPE_SRC(0), (t->hactive - 1) << 16 | (t->vactive - 1));
    /* The PLL (i915's vlv_prepare_pll for HDMI on pipe A, channel 0). */
    uint32_t v = 0, dw3 = d.m1 << 8 | d.m2 | d.p1 << 21 | d.p2 << 16 | d.n << 12 | 1u << 24 |
                         1u << 28;              /* K 1; S1 5 (HDMI and DP: "the DAC's is unstable") */
    bool sb = dpio_write(DPIO_PCS_DW17_BCAST, 0x0100000F) && dpio_read(DPIO_PLL_DW16(0), &v) &&
              dpio_write(DPIO_PLL_DW16(0), v & 0x00FFFFFF) && dpio_write(DPIO_CMN_DW0, 0x610) &&
              dpio_write(DPIO_PLL_DW3(0), dw3) && dpio_write(DPIO_PLL_DW3(0), dw3 | DPIO_CALIBRATE) &&
              dpio_write(DPIO_PLL_DW18(0), 0x009F0003) && dpio_write(DPIO_PLL_DW5(0), 0x0DF70000) &&
              dpio_read(DPIO_PLL_DW7(0), &v) &&
              dpio_write(DPIO_PLL_DW7(0), (v & 0x0000FF00) | 0x01C00000) &&
              dpio_write(DPIO_PLL_DW19(0), 0x87871000);
    wr(DPLL(0), dpll | DPLL_VCO_ON | DPLL_EXT_BUFFER);
    wait_us(150);
    bool locked = wait_for(DPLL(0), DPLL_LOCKED, DPLL_LOCKED, 10);
    wr(DPLL_MD(0), 0);                                  /* no pixel multiplier */
    /* On: the port's sync directions, the pipe, the plane, the port. */
    port = (port & ~(SYNC_H_HIGH | SYNC_V_HIGH)) | (t->hsync_high ? SYNC_H_HIGH : 0) |
           (t->vsync_high ? SYNC_V_HIGH : 0);
    wr(port_reg, port & ~PORT_ENABLE);
    wr(PIPE_CONF(0), conf | PIPE_ON);
    bool running = wait_for(PIPE_CONF(0), PIPE_STATE, PIPE_STATE, 100);
    wr(PLANE_STRIDE(0), stride);
    buffer_bytes = 0;                                   /* (66d-1) made again at the next flip */
    buffer_shown = 0;
    wr(PLANE_CNTR(0), (cntr & ~PLANE_TILED) | PLANE_ON);
    wr(PLANE_SURF(0), 0);
    wr(port_reg, port | PORT_ENABLE);
    kprintf("  intelgpu:      pipe A %ux%u, PLL m1 %u m2 %u n %u p1 %u p2 %u%s%s%s%s\n",
            t->hactive, t->vactive, d.m1, d.m2, d.n, d.p1, d.p2, stopped ? "" : "; the pipe did not stop",
            sb ? "" : "; the sideband did not answer", locked ? "" : "; the PLL did not lock",
            running ? "" : "; the pipe did not start");
    const struct pci_bar *aperture = &g->pci->bars[2];
    *fb = (struct gpu_screen){ (volatile uint32_t *)phys_to_virt(aperture->address), t->hactive,
                                t->vactive, stride / 4, 16, 8, 0 };
    return true;
}

/* The physical address a GTT entry points to (0: not valid). */
static uint64_t gtt_page(uint32_t gpu_address)
{
    uint32_t e = rd(GTT_OFFSET + (gpu_address >> 12) * 4);
    if (!(e & 1))
        return 0;
    return (uint64_t)(e & 0xFFFFF000u) | (uint64_t)(e & 0xFF0u) << 28;
}

/* Pipe p as the hardware has it: its timings, PLL, plane and port. */
static void report_pipe(struct gpu_card *g, int p)
{
    char name = (char)('A' + p);
    uint32_t conf = rd(PIPE_CONF(p));
    if (!(conf & PIPE_ON)) {
        kprintf("  pipe %c: off\n", name);
        return;
    }
    uint32_t t[6];                      /* HTOTAL HBLANK HSYNC VTOTAL VBLANK VSYNC */
    for (int k = 0; k < 6; k++)
        t[k] = rd(PIPE_TIMINGS(p) + k * 4);
    uint32_t src = rd(PIPE_SRC(p));
    uint32_t hactive = (t[0] & 0x1FFF) + 1, htotal = (t[0] >> 16 & 0x1FFF) + 1;
    uint32_t vactive = (t[3] & 0x1FFF) + 1, vtotal = (t[3] >> 16 & 0x1FFF) + 1;
    kprintf("  pipe %c: %ux%u (source %ux%u), total %ux%u, sync %u-%u / %u-%u", name, hactive,
            vactive, (src >> 16 & 0xFFF) + 1, (src & 0xFFF) + 1, htotal, vtotal,
            (t[2] & 0x1FFF) + 1, (t[2] >> 16 & 0x1FFF) + 1, (t[5] & 0x1FFF) + 1,
            (t[5] >> 16 & 0x1FFF) + 1);
    for (int i = 0; i < port_count; i++) {
        uint32_t v = rd(ports[i].port);
        if ((v & PORT_ENABLE) && (int)!!(v & PORT_PIPE_B) == p)
            kprintf("; drives %s", gpu_output_name(g, i));
    }
    kprintf("\n");
    uint32_t dpll = rd(DPLL(p)), dw3;
    kprintf("    PLL: %s%s", dpll & DPLL_VCO_ON ? "on" : "off", dpll & DPLL_LOCKED ? ", locked" : "");
    if (!dpio_read(DPIO_PLL_DW3(p), &dw3)) {
        kprintf("; its dividers: the sideband did not answer\n");
    } else {
        uint32_t m1 = dw3 >> 8 & 7, m2 = dw3 & 0xFF, n = dw3 >> 12 & 0xF;
        uint32_t p1 = dw3 >> 21 & 7, p2 = dw3 >> 16 & 0x1F;
        uint64_t div = (uint64_t)n * p1 * p2 * 5;
        uint64_t khz = div ? (100000ull * m1 * m2 + div / 2) / div : 0;
        kprintf(", m1 %u m2 %u n %u p1 %u p2 %u: pixel clock %lu.%03lu MHz", m1, m2, n, p1, p2,
                khz / 1000, khz % 1000);
        uint64_t frame = (uint64_t)htotal * vtotal;
        if (khz && frame) {
            uint64_t mhz = (khz * 1000000 + frame / 2) / frame;        /* millihertz */
            kprintf(", %lu.%03lu Hz", mhz / 1000, mhz % 1000);
        }
        kprintf("\n");
    }
    uint32_t cntr = rd(PLANE_CNTR(p)), surf = rd(PLANE_SURF(p)) & 0xFFFFF000u;
    if (!(cntr & PLANE_ON)) {
        kprintf("    plane %c: off\n", name);
        return;
    }
    const struct pci_bar *aperture = &g->pci->bars[2];
    kprintf("    plane %c: format %u%s, %s, stride %u bytes, at GPU address 0x%x (the aperture's "
            "0x%lx; memory 0x%lx)\n", name, cntr >> 26 & 0xF,
            (cntr >> 26 & 0xF) == 6 ? " (32-bit BGRX)" : "", cntr & PLANE_TILED ? "tiled" : "linear",
            rd(PLANE_STRIDE(p)), surf, aperture->address + surf, gtt_page(surf));
}

/* (66b-7) The watermarks i915's method gives pipe A showing one plane
 * (the primary, 32-bit pixels) of width at khz with htotal: the plane gets
 * the whole FIFO (511 lines); the watermark is method 2's ("large
 * buffer"): ((latency x pixel rate / line time) + 1) lines of pixels, in
 * 64-byte lines; the registers hold FIFO size - watermark; self-refresh's
 * the same against both pipes' FIFOs (1023); the cursor's 63 of 63; drain
 * latency "high precision, 2". */
struct watermarks {
    uint32_t fifo, plane, sr, cursor, sr_cursor, ddl;
};

/* What a mode needs in reserve (method 2), in the FIFO's 64-byte lines. */
static uint32_t wm_need(uint32_t khz, uint32_t htotal, uint32_t width)
{
    uint64_t lines = (uint64_t)WM_LATENCY_US * 10 * khz / ((uint64_t)htotal * 10000) + 1;
    return (uint32_t)((lines * width * 4 + 63) / 64);
}

/* What the watermarks set now keep in reserve for plane A: its part of the
 * FIFO less the register's value (66b-7a: the J1900's firmware keeps 241
 * of 256 for every mode; a mode needs ~100-160). */
static uint32_t wm_reserve(void)
{
    uint32_t fifo = (rd(DSPARB) & 0xFF) | (rd(DSPARB2) & 1) << 8;
    uint32_t level = (rd(DSPFW1) & 0xFF) | (rd(DSPHOWM) & 1) << 8;
    return level <= fifo ? fifo - level : 0;
}

static void compute_wm(uint32_t khz, uint32_t htotal, uint32_t width, struct watermarks *w)
{
    uint32_t raw = wm_need(khz, htotal, width);
    w->fifo = FIFO_LINES;
    w->plane = raw <= FIFO_LINES ? FIFO_LINES - raw : 0xFFFF;
    w->sr = raw <= 2 * 512 - 1 ? 2 * 512 - 1 - raw : 0xFFFF;
    w->cursor = 0;
    w->sr_cursor = 0;
    w->ddl = 0x80 | 2;
}

static void read_wm(struct watermarks *w, bool *self_refresh)
{
    uint32_t arb = rd(DSPARB), arb2 = rd(DSPARB2), fw1 = rd(DSPFW1), fw2 = rd(DSPFW2);
    uint32_t howm = rd(DSPHOWM);
    w->fifo = (arb & 0xFF) | (arb2 & 1) << 8;           /* where sprite A's part begins */
    w->plane = (fw1 & 0xFF) | (howm & 1) << 8;
    w->sr = (fw1 >> 23 & 0x1FF) | (howm >> 24 & 1) << 9;
    w->cursor = fw2 >> 8 & 0x3F;
    w->sr_cursor = rd(DSPFW3) >> 24 & 0x3F;
    w->ddl = rd(VLV_DDL(0)) & 0xFF;
    *self_refresh = rd(FW_BLC_SELF) & (1u << 15);
}

static void report_wm(void)
{
    struct watermarks now, want;
    bool self_refresh;
    read_wm(&now, &self_refresh);
    uint32_t arb = rd(DSPARB), arb2 = rd(DSPARB2);
    uint32_t sprite_b = (arb >> 8 & 0xFF) | (arb2 >> 4 & 1) << 8;
    kprintf("  watermarks: FIFO plane A %u, sprites A %u and B %u; plane A %u, self-refresh %u "
            "(%s), cursor %u and %u, drain latency 0x%x\n", now.fifo, sprite_b - now.fifo,
            FIFO_LINES - sprite_b, now.plane, now.sr, self_refresh ? "on" : "off", now.cursor,
            now.sr_cursor, now.ddl);
    uint32_t dw3, vco, t0 = rd(PIPE_TIMINGS(0));
    if (!(rd(PIPE_CONF(0)) & PIPE_ON) || !dpio_read(DPIO_PLL_DW3(0), &dw3))
        return;
    struct dividers d = { dw3 >> 12 & 0xF, dw3 >> 8 & 7, dw3 & 0xFF, dw3 >> 21 & 7,
                          dw3 >> 16 & 0x1F };
    compute_wm(dividers_khz(&d, &vco), (t0 >> 16 & 0x1FFF) + 1, (rd(PIPE_SRC(0)) >> 16 & 0xFFF) + 1,
               &want);
    uint32_t khz = dividers_khz(&d, &vco);
    kprintf("    this mode needs %u lines in reserve, they keep %u; i915's own for it: FIFO plane "
            "A %u, sprites 0 and 0; plane A %u, self-refresh %u, cursor %u and %u, drain latency "
            "0x%x\n", wm_need(khz, (t0 >> 16 & 0x1FFF) + 1, (rd(PIPE_SRC(0)) >> 16 & 0xFFF) + 1),
            wm_reserve(), want.fifo, want.plane, want.sr, want.cursor, want.sr_cursor, want.ddl);
}

static void report(struct gpu_card *g)
{
    kprintf("  GMBUS: clock register 0x%x; as the firmware left it GMBUS0 0x%x, GMBUS2 0x%x\n",
            start_freq, start_gmbus0, start_gmbus2);
    for (int i = 0; i < port_count; i++) {
        kprintf("  %-6s pins %u: %lu reads, %lu whole, %lu no answer, %lu out of time, %lu never idle",
                gpu_output_name(g, i), ports[i].pin, tally[i].reads, tally[i].whole, tally[i].nak,
                tally[i].late, tally[i].stuck);
        if (tally[i].nak + tally[i].late)
            kprintf(" (the last: GMBUS2 0x%x at byte %u)", tally[i].last_status, tally[i].last_at);
        kprintf("\n");
    }
    if (vbt.version) {
        kprintf("  VBT: version %u, in the OpRegion (version %u) at 0x%x; %u child devices:",
                vbt.version, vbt.opregion_version, vbt.asls, vbt.children);
        for (uint32_t k = 0; k < vbt.children && k < 8; k++)
            kprintf(" [port %u, type 0x%04x, DDC %u, AUX 0x%x]", vbt.child[k].port,
                    vbt.child[k].type, vbt.child[k].ddc, vbt.child[k].aux);
        kprintf("; VGA %s (DDC %u)\n", vbt.crt ? "on the board" : "not on the board", vbt.crt_pin);
    }
    kprintf("  outputs from %s; display clock %u MHz (the most a mode may need: %u MHz)\n",
            outputs_from, start_freq, start_freq * 90 / 100);
    uint32_t live = rd(HOTPLUG_STAT);
    kprintf("  hotplug live status: port B %u, port C %u\n", live >> 29 & 1, live >> 28 & 1);
    for (int p = 0; p < 2; p++)
        report_pipe(g, p);
    report_wm();
    if (vblank_irq)
        kprintf("  vertical blanks: %lu (%lu interrupts); pipe A's frame %u\n", vblanks,
                interrupts, rd(FRAME_COUNT(0)));
    else
        kprintf("  vertical blanks: no interrupt (no MSI)\n");
    kprintf("  standby: the signal stopped %lu times, back %lu times with the screen%s\n",
            standby_offs, standby_ons, standby ? "; stopped now" : "");
    if (buffer_bytes)                           /* (67f: the else above was this if's) */
        kprintf("  screen buffers: 2 of %lu KB (GPU addresses 0x%x and 0x%x); showing buffer %d; "
                "%lu flips\n", buffer_bytes >> 10, buffer_at[0], buffer_at[1], buffer_shown, flips);
    uint16_t ggc = pci_read16(g->pci, 0x50);   /* GMS, bits 7-3: the stolen memory in 32 MiB */
    uint32_t bsm = pci_read32(g->pci, 0x5C) & 0xFFF00000u;
    kprintf("  stolen memory: 0x%x, %u MiB; GTT %lu MiB of addresses\n", bsm, (ggc >> 3 & 0x1F) * 32,
            ((uint64_t)(2u << 20) / 4) * 4096 >> 20);
}

/* (66b-4) Ask the board: its VBT, in the OpRegion the firmware leaves in
 * memory (PCI config 0xFC; "IntelGraphicsMem"; the VBT in mailbox 4 at
 * 0x400). Its blocks: 1 general features (byte 4 bit 0: VGA on the board),
 * 2 general definitions (byte 0 VGA's DDC pins, byte 4 a child device's
 * size, then the child devices: the type at 2, the port at 16, DDC pins at
 * 19, the AUX channel at 25, from version 204 HDMI's highest rate in byte 7
 * bits 7-5). On this chip a child's DDC pins are GMBUS pins. Fills ports[]
 * from it, or from the fallback. */
static void read_vbt(struct pci_device *d)
{
    port_count = 0;
    uint32_t asls = pci_read32(d, ASLS);
    const uint8_t *op = asls ? phys_to_virt(asls) : 0;
    const uint8_t *v = op ? op + 0x400 : 0;
    uint32_t room = op && (*(const uint32_t *)(op + 88) & (1u << 4)) ? 0x1C00 - 0x400
                                                                       : 0x2000 - 0x400;
    if (!op || memcmp(op, "IntelGraphicsMem", 16) || memcmp(v, "$VBT", 4)) {
        memcpy(ports, fallback, sizeof(fallback));
        port_count = (int)(sizeof(fallback) / sizeof(fallback[0]));
        return;
    }
    uint32_t size = *(const uint16_t *)(v + 24), bdb = *(const uint32_t *)(v + 28);
    if (size > room || bdb + 22 > size || bdb + *(const uint16_t *)(v + bdb + 20) > size) {
        memcpy(ports, fallback, sizeof(fallback));
        port_count = (int)(sizeof(fallback) / sizeof(fallback[0]));
        outputs_from = "the driver's table (the VBT's sizes are wrong)";
        return;
    }
    const uint8_t *b = v + bdb;
    uint32_t version = *(const uint16_t *)(b + 16), total = *(const uint16_t *)(b + 20);
    const uint8_t *features = 0, *defs = 0;
    uint32_t defs_size = 0;
    for (uint32_t at = *(const uint16_t *)(b + 18); at + 3 <= total;) {
        uint32_t id = b[at], n = *(const uint16_t *)(b + at + 1);
        if (id == 53 && at + 8 <= total && b[at + 3] >= 3)
            n = *(const uint32_t *)(b + at + 4);    /* MIPI sequences v3+: a size of its own */
        if (at + 3 + n > total)
            break;
        if (id == 1)
            features = b + at + 3;
        if (id == 2) {
            defs = b + at + 3;
            defs_size = n;
        }
        at += 3 + n;
    }
    vbt.asls = asls;
    vbt.opregion_version = op[23];
    vbt.version = version;
    vbt.crt = features && (features[4] & 1);
    if (defs && defs_size >= 5) {
        uint32_t csize = defs[4];
        vbt.crt_pin = defs[0];
        if (vbt.crt)
            ports[port_count++] = (struct output_hw){ "vga", "the VGA port",
                                                      defs[0] ? defs[0] : 2, PORT_ADPA, 0, "" };
        for (uint32_t at = 5; csize >= 26 && at + csize <= defs_size; at += csize) {
            const uint8_t *c = defs + at;
            uint16_t type = *(const uint16_t *)(c + 2);
            if (!type)
                continue;
            if (vbt.children < 8)
                vbt.child[vbt.children] = (struct vbt_child){ type, c[16], c[19], c[25] };
            vbt.children++;
            uint32_t port = c[16];
            bool b_port = port == 1 || port == 7, c_port = port == 2 || port == 8;
            if ((!b_port && !c_port) || port_count == GPU_OUTPUTS)
                continue;                           /* (D is Cherry Trail's; CRT came above) */
            bool hdmi = (type & (1u << 4)) && !(type & (1u << 11));   /* TMDS, not "not HDMI" */
            uint32_t rate = version >= 204 ? (uint32_t)(c[7] >> 5) : 0;
            static const uint32_t rates[] = { 0, 297000, 165000, 594000, 340000, 300000 };
            uint32_t max = rate < 6 ? rates[rate] : 0;
            ports[port_count++] = (struct output_hw){
                hdmi ? "hdmi" : "dp", b_port ? "port B" : "port C", c[19],
                hdmi ? (b_port ? PORT_HDMIB : PORT_HDMIC) : (b_port ? PORT_DPB : PORT_DPC),
                max ? max : HDMI_MAX_KHZ, max ? "the VBT says this output" : "the chip's HDMI" };
        }
    }
    outputs_from = "the VBT";
    if (!port_count) {
        memcpy(ports, fallback, sizeof(fallback));
        port_count = (int)(sizeof(fallback) / sizeof(fallback[0]));
        outputs_from = "the driver's table (the VBT lists no outputs)";
    }
}

/* The interrupt (i915's order for this chip): the master enable off and
 * the display's enables held, the pipe's status read and cleared, the
 * display's raised bits cleared last (they follow PIPESTAT's level), then
 * all on again. Interrupt context. */
static void vblank_interrupt(void *context)
{
    (void)context;
    wr(MASTER_IER, 0);
    uint32_t ier = rd(VLV_IER);
    wr(VLV_IER, 0);
    uint32_t iir = rd(VLV_IIR);
    uint32_t gt = rd(GTIIR), pm = rd(PMIIR);
    if (gt)
        wr(GTIIR, gt);                              /* (kept off: cleared if raised anyway) */
    if (pm)
        wr(PMIIR, pm);
    uint32_t stat = rd(PIPESTAT(0));
    wr(PIPESTAT(0), (stat & 0x7FFF0000u) | (stat & 0xFFFFu));     /* its raised statuses cleared */
    if (iir)
        wr(VLV_IIR, iir);
    wr(VLV_IER, ier);
    wr(MASTER_IER, 1u << 31);
    interrupts++;
    if (stat & START_VBLANK) {
        vblanks++;
        wait_queue_wake_all(&vblank_waiters);
    }
}

/* Switch pipe A's vblank interrupt on (the core's and power management's
 * stay off). False without MSI. */
static bool vblank_on(struct pci_device *d)
{
    uint64_t address;
    uint32_t data;
    pci_enable_dma(d);                              /* (a message is a write by the device) */
    wr(GTIER, 0);
    wr(GTIMR, ~0u);
    wr(GTIIR, ~0u);
    wr(PMIER, 0);
    wr(PMIMR, ~0u);
    wr(PMIIR, ~0u);
    if (!pci_msi_enable(d, vblank_interrupt, 0, &address, &data))
        return false;
    wr(PIPESTAT(0), (rd(PIPESTAT(0)) & 0x7FFF0000u) | START_VBLANK_ON | 0xFFFFu);
    wr(VLV_IIR, PIPE_A_EVENT);
    wr(VLV_IMR, rd(VLV_IMR) & ~PIPE_A_EVENT);
    wr(VLV_IER, rd(VLV_IER) | PIPE_A_EVENT);
    wr(MASTER_IER, 1u << 31);
    return true;
}

static bool wait_vblank(struct gpu_card *g, int i, uint64_t *frame, uint32_t timeout_ms)
{
    (void)g;
    if (!vblank_irq || (rd(ports[i].port) & PORT_PIPE_B))
        return false;                               /* (pipe A's only, so far) */
    uint64_t seen = vblanks;
    uint64_t deadline = timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000;
    bool came = wait_event_until(&vblank_waiters, vblanks != seen ||
                                 thread_stop_requested(), deadline) && vblanks != seen;
    *frame = rd(FRAME_COUNT(0));
    return came;
}

/* (66d-1) The screen buffers: 0 the firmware's framebuffer (GPU address
 * 0, where the console draws), 1 in stolen memory too, both the plane's
 * size and stride now. (66d-2) In fixed slots of BUFFER_SLOT (32 MiB: 4K
 * fits), so a program's mapping of them stays right across mode changes.
 * Buffer 1 is made (cleared to black) when first asked for, again after
 * a mode change. */
#define BUFFER_SLOT (32ull << 20)

static bool buffers_ready(struct gpu_card *g)
{
    uint64_t bytes = (uint64_t)rd(PLANE_STRIDE(0)) * ((rd(PIPE_SRC(0)) & 0xFFF) + 1);
    if (buffer_bytes == bytes)
        return true;
    if (bytes > BUFFER_SLOT || !gtt_cover(g, 2 * BUFFER_SLOT))
        return false;
    uint64_t phys = g->pci->bars[2].address;
    paging_write_combining(phys, 2 * BUFFER_SLOT);
    memset((void *)phys_to_virt(phys + BUFFER_SLOT), 0, bytes);     /* black */
    buffer_at[0] = 0;
    buffer_at[1] = (uint32_t)BUFFER_SLOT;
    buffer_bytes = bytes;
    return true;
}

/* Output i's port on, or off only for standby (37e-2d: it still shows). */
static bool port_on(int i)
{
    return ports[i].port && ((rd(ports[i].port) & PORT_ENABLE) || (standby >> i & 1));
}

/* (37e-2d) An HDMI port's signal off (its monitor to standby) or back.
 * Register writes only: a crash calls it, with no lock. */
static bool backlight(struct gpu_card *g, int i, bool on)
{
    (void)g;
    if (i < 0 || i >= port_count || i >= 8 || strcmp(ports[i].kind, "hdmi") || !ports[i].port)
        return false;
    if (!on && !(standby >> i & 1)) {
        uint32_t v = rd(ports[i].port);
        if (!(v & PORT_ENABLE))
            return false;                           /* (no signal to stop) */
        standby_port[i] = v;
        standby |= 1u << i;
        wr(ports[i].port, v & ~PORT_ENABLE);
        standby_offs++;
    } else if (on && (standby >> i & 1)) {
        wr(ports[i].port, standby_port[i]);
        standby &= ~(1u << i);
        standby_ons++;
    } else if (on)
        return false;                               /* (it was not stopped) */
    return true;
}

static bool buffers(struct gpu_card *g, int i, uint64_t *phys, uint64_t *slot, int *count,
                    uint32_t *stride)
{
    if (!vblank_irq || !port_on(i) || (rd(ports[i].port) & PORT_PIPE_B) ||
        !buffers_ready(g))
        return false;
    *phys = g->pci->bars[2].address;
    *slot = BUFFER_SLOT;
    *count = 2;
    *stride = rd(PLANE_STRIDE(0));
    return true;
}

static bool flip(struct gpu_card *g, int i, int buffer, uint64_t *frame)
{
    if (!vblank_irq || !port_on(i) || (rd(ports[i].port) & PORT_PIPE_B) ||
        !buffers_ready(g))
        return false;
    uint32_t at = buffer_at[buffer];
    wr(PLANE_SURF(0), at);                          /* taken at the next vertical blank */
    for (int k = 0; k < 6; k++) {                   /* (a 60 Hz frame is 16.7 ms: ~100 ms) */
        if (rd(PLANE_SURFLIVE(0)) == at) {
            *frame = rd(FRAME_COUNT(0));
            flips++;
            buffer_shown = buffer;
            return true;
        }
        wait_vblank(g, i, frame, 20);
    }
    return rd(PLANE_SURFLIVE(0)) == at;
}

static bool showing(struct gpu_card *g, int i)
{
    (void)g;
    return port_on(i);
}

/* (67f) The module's start, for the chip in its table. */
static bool start(struct pci_device *d)
{
    if (bay_trail.pci)
        return false;                               /* (one chip: its state is static) */
    const struct pci_bar *bar = &d->bars[0];
    if (bar->is_io || bar->size < (4u << 20)) {
        kprintf("  intelgpu:      %02x:%02x.%u: BAR 0 is not the registers (%lu bytes)\n", d->bus,
                d->device, d->function, bar->size);
        return false;
    }
    regs = paging_map_mmio(bar->address, 4u << 20);     /* the registers, then the GTT */
    if (!regs)
        return false;
    bay_trail.model = "Intel Bay Trail HD Graphics";
    bay_trail.pci = d;
    bay_trail.read_edid = read_edid;
    bay_trail.showing = showing;
    bay_trail.report = report;
    bay_trail.set_mode = set_mode;
    bay_trail.wait_vblank = wait_vblank;
    bay_trail.flip = flip;
    bay_trail.buffers = buffers;
    bay_trail.backlight = backlight;                /* (37e-2d) HDMI: the signal */
    start_gmbus0 = rd(GMBUS0);
    start_gmbus2 = rd(GMBUS2);
    start_freq = rd(GMBUS_FREQ_VLV);
    read_vbt(d);
    vblank_irq = vblank_on(d);
    bay_trail.output_count = port_count;
    for (int i = 0; i < port_count; i++) {
        bay_trail.outputs[i].kind = ports[i].kind;
        bay_trail.outputs[i].where = ports[i].where;
    }
    gpu_register(&bay_trail);
    return true;
}

/* Bay Trail's display: 8086:0f30-0f33. */
static const struct module_pci ids[] = {
    { 0x8086, 0x0F30 }, { 0x8086, 0x0F31 }, { 0x8086, 0x0F32 }, { 0x8086, 0x0F33 },
};

MODULE(intelgpu, start, 0, 0, ids)
