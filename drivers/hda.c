/*
 * Intel High Definition Audio (HDA / Azalia) driver, a module (67g): the
 * standard interface both Intel and AMD PCs expose. Brings up each HDA
 * controller (PCI class 04.03),
 * reads each codec's description -- its pins' configuration defaults say what
 * the laptop or board maker wired them to: speakers, a headphone jack, a line
 * out, HDMI, or nothing -- and chooses the outputs: speakers and headphones
 * first, then line outs, digital (HDMI) last. From each chosen pin it follows
 * the codec's connection lists to a converter (DAC), through mixers and
 * selectors, and powers, selects and unmutes every node on the way (0 dB),
 * the speaker amplifier (EAPD) too. It plays 48 kHz / 16-bit / stereo PCM
 * through a cyclic DMA ring linked by a two-entry buffer descriptor list;
 * codec commands go through the CORB/RIRB rings (polled). Each controller is
 * a sound card of the sound layer (driver_sound.h), which numbers them
 * /dev/pcm0, pcm1, ... by their outputs' kind (26a). Written from the HDA
 * specification; developed against QEMU's intel-hda.
 */
#include "driver.h"
#include "driver_sound.h"

/* Controller registers (byte offsets). */
#define GCAP        0x00
#define GCTL        0x08
#define STATESTS    0x0E
#define CORBLBASE   0x40
#define CORBUBASE   0x44
#define CORBWP      0x48
#define CORBRP      0x4A
#define CORBCTL     0x4C
#define CORBSIZE    0x4E
#define RIRBLBASE   0x50
#define RIRBUBASE   0x54
#define RIRBWP      0x58
#define RINTCNT     0x5A
#define RIRBCTL     0x5C
#define RIRBSTS     0x5D
#define RIRBSIZE    0x5E

/* A stream descriptor's registers (relative to its base). */
#define SD_CTL      0x00
#define SD_STS      0x03
#define SD_LPIB     0x04
#define SD_CBL      0x08
#define SD_LVI      0x0C
#define SD_FMT      0x12
#define SD_BDPL     0x18
#define SD_BDPU     0x1C

#define RING_PAGES  16
#define RING_SIZE   (RING_PAGES * 4096)                 /* 64 KiB cyclic buffer */
#define FMT_48_16_2 0x0011                              /* 48 kHz, 16-bit, 2 channels */
#define STREAM_TAG  1

#define MAX_NODES   96                                  /* widgets per codec looked at */
#define MAX_CONN    16                                  /* a widget's connections looked at */
#define MAX_PATH    6                                   /* pin ... DAC */
#define MAX_OUTS    4                                   /* pins played at once */

/* Widget types (parameter 0x09, bits 23:20). */
enum { W_OUT = 0, W_IN = 1, W_MIXER = 2, W_SELECTOR = 3, W_PIN = 4 };

/* What a pin is for, best first: the sound card's kind too. */
enum { RANK_SPEAKER = SOUND_SPEAKERS, RANK_LINE = SOUND_LINE_OUT, RANK_DIGITAL = SOUND_DIGITAL,
       RANK_NONE };

struct bdl_entry {
    uint64_t address;
    uint32_t length;
    uint32_t ioc;
} __attribute__((packed));

struct node {
    uint8_t type, nconn;
    uint8_t conn[MAX_CONN];
    uint32_t caps, pincaps, config;
};

/* A path from a pin to a converter: nids[0] is the pin, nids[n-1] the DAC;
 * pick[i] which of nids[i]'s connections leads to nids[i+1]. */
struct path {
    int n;
    uint8_t nids[MAX_PATH], pick[MAX_PATH];
};

struct hda {
    struct sound_card card;         /* (card.priv is this) */
    struct pci_device *pci;
    volatile uint8_t *regs;
    uint32_t sd;                    /* the first output stream descriptor's offset */
    uint32_t *corb;                 /* command ring (256 x 4) */
    uint64_t *rirb;                 /* response ring (256 x 8) */
    uint16_t corb_wp, rirb_rp;
    struct bdl_entry *bdl;
    uint8_t *ring;                  /* the PCM ring (RING_SIZE), contiguous */
    uint64_t ring_phys, bdl_phys, rings_phys;
    int codec, afg, rank;
    struct node nodes[MAX_NODES];
    int start, count;               /* the widgets' node numbers */
    struct path outs[MAX_OUTS];
    int nouts;
    bool present, running;
    uint32_t wpos;                  /* logical write offset into the ring */
    struct mutex lock;
};

#define CARDS 8
static struct hda *cards[CARDS];           /* (37g-4) for sleep: by device */
static int ncards;

static uint32_t rd32(struct hda *h, uint32_t o) { return mmio_read32(h->regs + o); }
static uint16_t rd16(struct hda *h, uint32_t o) { return mmio_read16(h->regs + o); }
static void wr32(struct hda *h, uint32_t o, uint32_t v) { mmio_write32(h->regs + o, v); }
static void wr16(struct hda *h, uint32_t o, uint16_t v) { mmio_write16(h->regs + o, v); }
static void wr8(struct hda *h, uint32_t o, uint8_t v) { mmio_write8(h->regs + o, v); }

/* One codec command; returns the response (0 on timeout). */
static uint32_t cmd(struct hda *h, int nid, uint32_t verb)
{
    uint32_t c = (uint32_t)h->codec << 28 | (uint32_t)nid << 20 | verb;
    h->corb_wp = (uint16_t)((h->corb_wp + 1) & 0xFF);
    h->corb[h->corb_wp] = c;
    arch_write_barrier();
    wr16(h, CORBWP, h->corb_wp);
    /* A codec answers within microseconds: wait for it without sleeping
     * first (a sleep is a whole timer tick, 10 ms: reading the laptop's two
     * codecs took 7 s of its boot), then in ticks up to a second. */
    uint64_t hz = arch_counter_hz(), until = arch_counter() + (hz ? hz / 1000 : 0);
    while ((rd16(h, RIRBWP) & 0xFF) == h->rirb_rp && hz && arch_counter() < until)
        arch_cpu_relax();
    for (int i = 0; i < 100 && (rd16(h, RIRBWP) & 0xFF) == h->rirb_rp; i++)
        timer_sleep_ms(10);
    if ((rd16(h, RIRBWP) & 0xFF) == h->rirb_rp)
        return 0;
    h->rirb_rp = (uint16_t)((h->rirb_rp + 1) & 0xFF);
    arch_read_barrier();
    return (uint32_t)h->rirb[h->rirb_rp];       /* the response is the low 32 bits */
}

#define PARAM(h, nid, p)    cmd(h, nid, 0xF0000 | (p))     /* GET_PARAMETER */

static struct node *node(struct hda *h, int nid)
{
    return nid >= h->start && nid < h->start + h->count ? &h->nodes[nid - h->start] : 0;
}

/* A widget's connection list (short form: four 8-bit entries a reply; long
 * form: two 16-bit ones; an entry with the range bit stands for all from the
 * one before it). */
static void read_connections(struct hda *h, int nid, struct node *w)
{
    uint32_t len = PARAM(h, nid, 0x0E);
    bool longform = len & 0x80;
    int total = (int)(len & 0x7F), per = longform ? 2 : 4, prev = 0;
    w->nconn = 0;
    for (int i = 0; i < total && w->nconn < MAX_CONN; i += per) {
        uint32_t r = cmd(h, nid, 0xF0200 | (uint32_t)i);
        for (int k = 0; k < per && i + k < total && w->nconn < MAX_CONN; k++) {
            uint32_t e = longform ? r >> (16 * k) & 0xFFFF : r >> (8 * k) & 0xFF;
            bool range = longform ? e & 0x8000 : e & 0x80;
            int id = (int)(e & (longform ? 0x7FFF : 0x7F));
            if (range && prev)
                for (int r2 = prev + 1; r2 <= id && w->nconn < MAX_CONN; r2++)
                    w->conn[w->nconn++] = (uint8_t)r2;
            else
                w->conn[w->nconn++] = (uint8_t)id;
            prev = id;
        }
    }
}

/* What an output pin is for, from its configuration default and caps. */
static int pin_rank(const struct node *w)
{
    if (!(w->pincaps & 0x10) || (w->config >> 30) == 1)
        return RANK_NONE;                           /* not an output, or wired to nothing */
    int device = (int)(w->config >> 20 & 0xF);
    if (device == 0x5 || device == 0x6 || (w->pincaps & (1u << 7 | 1u << 24)))
        return RANK_DIGITAL;                        /* S/PDIF, HDMI, DisplayPort */
    if (device == 0x1 || device == 0x2)
        return RANK_SPEAKER;                        /* speakers, headphones */
    if (device == 0x0)
        return RANK_LINE;
    return RANK_NONE;                               /* (inputs: mics, line in, ...) */
}

static const char *pin_kind(const struct node *w)
{
    static const char *const kinds[16] = {
        "line out", "speaker", "headphones", "CD", "S/PDIF out", "digital out",
        "modem line", "modem handset", "line in", "aux", "microphone", "telephony",
        "S/PDIF in", "digital in", "reserved", "other" };
    return kinds[w->config >> 20 & 0xF];
}

/* Depth-first from nid towards an output converter; true with p filled. */
static bool find_path(struct hda *h, int nid, struct path *p, int depth)
{
    struct node *w = node(h, nid);
    if (!w || depth >= MAX_PATH)
        return false;
    for (int i = 0; i < depth; i++)
        if (p->nids[i] == nid)
            return false;                           /* (a loop) */
    p->nids[depth] = (uint8_t)nid;
    if (w->type == W_OUT) {
        p->n = depth + 1;
        return true;
    }
    if (depth > 0 && w->type != W_MIXER && w->type != W_SELECTOR)
        return false;
    for (int i = 0; i < w->nconn; i++) {
        p->pick[depth] = (uint8_t)i;
        if (find_path(h, w->conn[i], p, depth + 1))
            return true;
    }
    return false;
}

/* The 0 dB setting of an amplifier, from its caps (offset; at most the
 * number of steps). */
static uint32_t zero_db(uint32_t ampcaps)
{
    uint32_t offset = ampcaps & 0x7F, steps = ampcaps >> 8 & 0x7F;
    return offset <= steps ? offset : steps;
}

/* Power, select and unmute the path; the pin's output (and headphone
 * drive), its amplifier switch (EAPD); the converter's format and stream. */
static void open_path(struct hda *h, const struct path *p)
{
    uint32_t afg_out = PARAM(h, h->afg, 0x12), afg_in = PARAM(h, h->afg, 0x0D);
    for (int i = 0; i < p->n; i++) {
        int nid = p->nids[i];
        struct node *w = node(h, nid);
        cmd(h, nid, 0x70500);                       /* power state D0 */
        if (i + 1 < p->n && (w->type == W_PIN || w->type == W_SELECTOR) && w->nconn > 1)
            cmd(h, nid, 0x70100 | p->pick[i]);      /* connection select */
        bool own = w->caps & 0x08;                  /* (its own amp caps, else the group's) */
        if (w->caps & 0x04) {                       /* an output amplifier: unmute, 0 dB */
            uint32_t caps = own ? PARAM(h, nid, 0x12) : afg_out;
            cmd(h, nid, 0x30000 | 0xB000 | zero_db(caps));
        }
        if ((w->caps & 0x02) && w->type == W_MIXER && i + 1 < p->n) {
            uint32_t caps = own ? PARAM(h, nid, 0x0D) : afg_in;   /* the mixer's input */
            cmd(h, nid, 0x30000 | 0x7000 | (uint32_t)p->pick[i] << 8 | zero_db(caps));
        }
    }
    struct node *pin = node(h, p->nids[0]);
    int device = (int)(pin->config >> 20 & 0xF);
    cmd(h, p->nids[0], 0x70700 | 0x40 | (device == 0x2 ? 0x80 : 0));  /* out (+ headphone drive) */
    if (pin->pincaps & (1u << 16))
        cmd(h, p->nids[0], 0x70C00 | 0x02);         /* EAPD: the external amplifier on */
    int dac = p->nids[p->n - 1];
    cmd(h, dac, 0x20000 | FMT_48_16_2);             /* converter format */
    cmd(h, dac, 0x70600 | (STREAM_TAG << 4));       /* stream tag, channel 0 */
}

/* Read the codec at h->codec: its audio function group, its widgets, and
 * the paths of its best outputs. The rank of those (RANK_NONE: none). */
static int read_codec(struct hda *h, bool show)
{
    uint32_t sub = PARAM(h, 0, 0x04);               /* root: function groups */
    int fg_start = (int)(sub >> 16 & 0xFF), fg_count = (int)(sub & 0xFF);
    h->afg = 0;
    for (int fg = fg_start; fg < fg_start + fg_count && !h->afg; fg++)
        if ((PARAM(h, fg, 0x05) & 0x7F) == 0x01)    /* an audio function group */
            h->afg = fg;
    if (!h->afg)
        return RANK_NONE;
    cmd(h, h->afg, 0x70500);                        /* the group powered (D0) */
    uint32_t ws = PARAM(h, h->afg, 0x04);
    h->start = (int)(ws >> 16 & 0xFF);
    h->count = (int)(ws & 0xFF) < MAX_NODES ? (int)(ws & 0xFF) : MAX_NODES;
    if (h->start + h->count > 256)                  /* (49l: node numbers are 8 bits in */
        h->count = 256 - h->start;                  /* a verb; more would hit another codec) */
    for (int i = 0; i < h->count; i++) {
        int nid = h->start + i;
        struct node *w = &h->nodes[i];
        memset(w, 0, sizeof(*w));
        w->caps = PARAM(h, nid, 0x09);
        w->type = (uint8_t)(w->caps >> 20 & 0xF);
        if (w->caps & 0x100)                        /* a connection list */
            read_connections(h, nid, w);
        if (w->type == W_PIN) {
            w->pincaps = PARAM(h, nid, 0x0C);
            w->config = cmd(h, nid, 0xF1C00);       /* configuration default */
        }
    }
    int best = RANK_NONE;
    for (int i = 0; i < h->count; i++)
        if (h->nodes[i].type == W_PIN && pin_rank(&h->nodes[i]) < best)
            best = pin_rank(&h->nodes[i]);
    h->nouts = 0;
    for (int i = 0; i < h->count; i++) {
        struct node *w = &h->nodes[i];
        if (w->type != W_PIN || (w->config >> 30) == 1)
            continue;                               /* (unconnected pins say nothing) */
        int rank = pin_rank(w);
        struct path p = { 0 };
        bool reach = rank != RANK_NONE && find_path(h, h->start + i, &p, 0);
        if (show) {
            char route[48] = "";
            for (int k = 1; reach && k < p.n; k++) {
                size_t at = strlen(route);
                ksnprintf(route + at, sizeof(route) - at, " -> %u", p.nids[k]);
            }
            kprintf("                 pin %d: %s, %s%s\n", h->start + i, pin_kind(w),
                    (w->config >> 30) == 2 ? "built in" : "jack", route);
        }
        /* The best kind plays (speakers and headphones both); digital
         * outputs only the first. */
        if (reach && rank == best && h->nouts < MAX_OUTS &&
            !(rank == RANK_DIGITAL && h->nouts))
            h->outs[h->nouts++] = p;
    }
    return h->nouts ? best : RANK_NONE;
}

/* Reset a 16-bit ring pointer via its RST bit, then clear it. */
static void reset_ptr(struct hda *h, uint32_t reg)
{
    wr16(h, reg, 0x8000);
    for (int i = 0; i < 100 && !(rd16(h, reg) & 0x8000); i++)
        timer_sleep_ms(1);
    wr16(h, reg, 0x0000);
    for (int i = 0; i < 100 && (rd16(h, reg) & 0x8000); i++)
        timer_sleep_ms(1);
}

/* The controller reset (its codecs ask for addresses again), then the
 * command rings in their memory, cleared and running: the codecs that
 * answered (STATESTS), 0 for none. The boot's, and (37g-4) after a sleep. */
static uint16_t reset_and_rings(struct hda *h)
{
    wr32(h, GCTL, 0);                               /* reset */
    for (int i = 0; i < 100 && (rd32(h, GCTL) & 1); i++)
        timer_sleep_ms(1);
    wr32(h, GCTL, 1);                               /* leave reset */
    for (int i = 0; i < 100 && !(rd32(h, GCTL) & 1); i++)
        timer_sleep_ms(1);
    timer_sleep_ms(2);                              /* codecs settle (they ask for an address) */
    uint16_t states = rd16(h, STATESTS);
    if (!states)
        return 0;
    uint64_t rings = h->rings_phys;
    memset(h->corb, 0, 4096);
    memset(h->ring, 0, RING_SIZE);
    wr8(h, CORBCTL, 0);                             /* stop the rings while set up */
    wr8(h, RIRBCTL, 0);
    wr8(h, CORBSIZE, 0x02);                         /* 256 entries */
    wr8(h, RIRBSIZE, 0x02);
    wr32(h, CORBLBASE, (uint32_t)rings);
    wr32(h, CORBUBASE, (uint32_t)(rings >> 32));
    wr32(h, RIRBLBASE, (uint32_t)(rings + 2048));
    wr32(h, RIRBUBASE, (uint32_t)((rings + 2048) >> 32));
    reset_ptr(h, CORBRP);
    h->corb_wp = 0;
    wr16(h, CORBWP, 0);
    wr16(h, RIRBWP, 0x8000);                        /* reset the write pointer */
    h->rirb_rp = 0;
    wr16(h, RINTCNT, 0xFF);
    wr8(h, CORBCTL, 0x02);                          /* run */
    wr8(h, RIRBCTL, 0x02);                          /* DMA enable (no interrupt) */
    return states;
}

static void stream_setup(struct hda *h);

/* Bring up one controller: reset, the command rings, its codecs (the best
 * one's outputs), the output stream. */
static bool controller_up(struct hda *h)
{
    struct pci_device *pci = h->pci;
    h->lock = (struct mutex)MUTEX_INIT("hda");
    pci_enable_dma(pci);
    struct pci_bar *bar = &pci->bars[0];
    if (!bar->address)
        return false;
    h->regs = paging_map_mmio(bar->address, bar->size);
    kprintf("  hda:           %02x:%02x.%x (%04x:%04x)\n", pci->bus, pci->device, pci->function,
            pci->vendor_id, pci->device_id);

    /* CORB/RIRB in one page, the BDL in another; the ring contiguous. */
    h->rings_phys = pmm_alloc_page();
    h->bdl_phys = pmm_alloc_page();
    h->ring_phys = pmm_alloc_pages(RING_PAGES);
    if (!h->rings_phys || !h->bdl_phys || !h->ring_phys)
        return false;
    h->corb = phys_to_virt(h->rings_phys);
    h->rirb = (uint64_t *)((uint8_t *)phys_to_virt(h->rings_phys) + 2048);
    h->bdl = phys_to_virt(h->bdl_phys);
    h->ring = phys_to_virt(h->ring_phys);
    uint16_t states = reset_and_rings(h);
    if (!states) {
        kprintf("                 no codec\n");
        return false;
    }

    /* Every codec described; the one with the best outputs is used. */
    int best = RANK_NONE, best_codec = -1, last_read = -1;
    for (int c = 0; c < 15; c++) {
        if (!(states & (1u << c)))
            continue;
        h->codec = last_read = c;
        uint32_t id = PARAM(h, 0, 0x00);
        kprintf("                 codec %d: %04x:%04x\n", c, id >> 16, id & 0xFFFF);
        int rank = read_codec(h, true);
        if (rank < best)
            best = rank, best_codec = c;
    }
    if (best_codec < 0) {
        kprintf("                 no output\n");
        return false;
    }
    h->codec = best_codec;
    h->rank = best;
    if (best_codec != last_read)                    /* (its widgets and paths, read again) */
        read_codec(h, false);

    /* The output stream descriptor: after the input streams (GCAP bits 8-11). */
    int iss = (rd16(h, GCAP) >> 8) & 0xF;
    h->sd = 0x80 + (uint32_t)iss * 0x20;
    stream_setup(h);
    for (int i = 0; i < h->nouts; i++)
        open_path(h, &h->outs[i]);
    h->present = true;
    return true;
}

/* The output stream: the ring's two halves in the BDL, the descriptor
 * reset, then its ring, format and BDL; not running. */
static void stream_setup(struct hda *h)
{
    for (int i = 0; i < 2; i++) {                   /* two halves of the ring */
        h->bdl[i].address = h->ring_phys + (uint64_t)i * (RING_SIZE / 2);
        h->bdl[i].length = RING_SIZE / 2;
        h->bdl[i].ioc = 0;
    }
    wr32(h, h->sd + SD_CTL, 0x01);                  /* SRST */
    for (int i = 0; i < 100 && !(rd32(h, h->sd + SD_CTL) & 1); i++)
        timer_sleep_ms(1);
    wr32(h, h->sd + SD_CTL, 0x00);
    for (int i = 0; i < 100 && (rd32(h, h->sd + SD_CTL) & 1); i++)
        timer_sleep_ms(1);
    wr32(h, h->sd + SD_CBL, RING_SIZE);
    wr16(h, h->sd + SD_LVI, 1);
    wr16(h, h->sd + SD_FMT, FMT_48_16_2);
    wr32(h, h->sd + SD_BDPL, (uint32_t)h->bdl_phys);
    wr32(h, h->sd + SD_BDPU, (uint32_t)(h->bdl_phys >> 32));
    wr32(h, h->sd + SD_CTL, (uint32_t)STREAM_TAG << 20);   /* stream number, not running yet */
}

static long hda_write(struct sound_card *card, const void *buf, size_t n);
static void hda_drain(struct sound_card *card);

/* A controller (the module's PCI table matches the class; the class is
 * checked again here): up, its outputs described, registered. A controller
 * that did not come up keeps its memory (its rings may be running). */
static bool hda_start(struct pci_device *pci)
{
    if (pci->class_code != 0x04 || pci->subclass != 0x03)
        return false;
    struct hda *h = kzalloc(sizeof(*h));
    if (!h)
        return false;
    h->pci = pci;
    if (!controller_up(h))
        return false;
    struct sound_card *card = &h->card;
    card->driver = "hda";
    card->pci = pci;
    card->kind = h->rank;
    card->priv = h;
    card->write = hda_write;
    card->drain = hda_drain;
    ksnprintf(card->outputs, sizeof(card->outputs), "codec %d: ", h->codec);
    for (int i = 0; i < h->nouts; i++) {
        size_t at = strlen(card->outputs);
        const struct path *p = &h->outs[i];
        ksnprintf(card->outputs + at, sizeof(card->outputs) - at, "%s%s pin %u (DAC %u)",
                  i ? ", " : "", pin_kind(node(h, p->nids[0])), p->nids[0], p->nids[p->n - 1]);
    }
    sound_register(card);
    if (ncards < CARDS)
        cards[ncards++] = h;                        /* (37g-4) */
    return true;
}

/* (37g-4) Sleep: the stream stopped, the lock kept over the sleep
 * (nothing plays); after it the controller reset with its rings, the codec's
 * audio group powered and its paths opened again (known: not read again),
 * the stream set up, empty. */
static struct hda *card_of(const struct pci_device *pci)
{
    for (int i = 0; i < ncards; i++)
        if (cards[i]->pci == pci)
            return cards[i];
    return 0;
}

static void stream_run(struct hda *h, bool on);

static bool hda_suspend(struct pci_device *pci)
{
    struct hda *h = card_of(pci);
    if (!h)
        return true;
    mutex_lock(&h->lock);
    stream_run(h, false);
    return true;
}

static bool hda_resume(struct pci_device *pci)
{
    struct hda *h = card_of(pci);
    if (!h)
        return true;
    bool ok = reset_and_rings(h) & (1u << h->codec);
    if (ok) {
        cmd(h, h->afg, 0x70500);                    /* the group powered (D0) */
        stream_setup(h);
        for (int i = 0; i < h->nouts; i++)
            open_path(h, &h->outs[i]);
    } else {
        kprintf("  hda:           %02x:%02x.%x: codec %d did not answer after the sleep\n",
                pci->bus, pci->device, pci->function, h->codec);
    }
    h->wpos = 0;
    h->running = false;
    mutex_unlock(&h->lock);
    return ok;
}

static void stream_run(struct hda *h, bool on)
{
    uint32_t c = rd32(h, h->sd + SD_CTL);
    if (on)
        c |= 0x02;
    else
        c &= ~0x02u;
    wr32(h, h->sd + SD_CTL, c);
    h->running = on;
}

/* Stop the stream and reset (silence); the next write starts it again. */
static void stop(struct hda *h)
{
    mutex_lock(&h->lock);
    stream_run(h, false);
    memset(h->ring, 0, RING_SIZE);
    h->wpos = 0;
    wr32(h, h->sd + SD_CTL, (uint32_t)STREAM_TAG << 20);   /* keep the stream tag */
    mutex_unlock(&h->lock);
}

static void hda_drain(struct sound_card *card)
{
    struct hda *h = card->priv;
    if (!h->running)
        return;
    /* Silence after the last sample, so that playing past it (between two
     * looks) plays nothing; then wait until playback reaches it -- or has
     * passed it: what is left then grows again instead of reaching 0. (It
     * waited for exactly 0, and missed it: the ring's old content played
     * again, for up to two seconds, the user's "more than two seconds".) */
    mutex_lock(&h->lock);
    uint32_t used = (h->wpos - rd32(h, h->sd + SD_LPIB)) & (RING_SIZE - 1);
    for (uint32_t k = 0, at = h->wpos; k + 1 < RING_SIZE - used; k++, at = (at + 1) & (RING_SIZE - 1))
        h->ring[at] = 0;
    arch_write_barrier();
    mutex_unlock(&h->lock);
    uint32_t left = used;
    for (int i = 0; i < 1000; i++) {
        uint32_t now = (h->wpos - rd32(h, h->sd + SD_LPIB)) & (RING_SIZE - 1);
        if (now == 0 || now > left)
            break;
        left = now;
        timer_sleep_ms(2);
    }
    timer_sleep_ms(80);                             /* and the host's output latency */
    stop(h);
}

static long hda_write(struct sound_card *card, const void *buf, size_t n)
{
    struct hda *h = card->priv;
    const uint8_t *p = buf;
    size_t done = 0;
    mutex_lock(&h->lock);
    if (!h->running) {
        h->wpos = 0;
        stream_run(h, true);
    }
    while (done < n) {
        uint32_t lpib = rd32(h, h->sd + SD_LPIB);   /* bytes the hardware has played */
        uint32_t used = (h->wpos - lpib) & (RING_SIZE - 1);
        uint32_t freeb = RING_SIZE - used - 1;      /* keep one byte so wpos != lpib means full */
        if (freeb == 0) {
            mutex_unlock(&h->lock);
            timer_sleep_ms(2);                      /* the ring is full; let it drain */
            mutex_lock(&h->lock);
            continue;
        }
        uint32_t chunk = (uint32_t)(n - done);
        if (chunk > freeb)
            chunk = freeb;
        uint32_t to_end = RING_SIZE - h->wpos;
        if (chunk > to_end)
            chunk = to_end;
        memcpy(h->ring + h->wpos, p + done, chunk);
        arch_write_barrier();
        h->wpos = (h->wpos + chunk) & (RING_SIZE - 1);
        done += chunk;
    }
    mutex_unlock(&h->lock);
    return (long)done;
}

static const struct module_pci ids[] = {
    MODULE_CLASS(0x04, 0x03),                       /* HD Audio, from any maker */
};

MODULE(hda, hda_start, hda_suspend, hda_resume, ids)
