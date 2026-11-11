/*
 * (66a) The graphics layer: see gpu.h. Each device has a thread (named
 * after it) that looks at every output every 2 s: the EDID's first 16
 * bytes (its header and the monitor's maker, product and serial number);
 * a monitor that answers anew, or with other bytes, is read whole (the base
 * block and one extension: 256 bytes) and logged; one that stops answering
 * is gone.
 *
 * (66b-2) Setting a mode: the mode must be one the monitor lists; its whole
 * timing comes from the monitor's detailed timings, else from the
 * standard tables below (VESA DMT, CEA-861's 720p and 1080p), for the
 * modes a monitor lists by size and rate only. The driver programs it; the
 * console then draws on the new framebuffer.
 */
#include "gpu.h"
#include "arch.h"
#include "console.h"
#include "display.h"
#include "kprintf.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "timer.h"
#include "heap.h"
#include "memory.h"
#include "mouse.h"
#include "mounts.h"
#include "vfs.h"
#include "paging.h"

#define GPUS     4
#define WATCH_MS 2000

static struct gpu *gpus[GPUS];
static int gpu_count;
static struct spinlock lock = SPINLOCK_INIT("gpu");      /* the outputs' state: watch and report */
static uint64_t looks, reads, failed;   /* passes of the watch, EDIDs read whole, reads that failed */
static uint32_t set_mhz;                /* (66b-2) the rate of the mode set last, 0: the firmware's */
static const char *set_by;              /* (66b-6) who set it: "displays set" or the settings file */
static int shown_buffer;                /* (66d-1) the screen buffer shown (0: the console's) */
/* (37g-5d) From the start of a sleep until the drivers are back nothing
 * here calls a driver: as Linux, whose processes are frozen and whose
 * console waits until the graphics driver's resume ends. The watcher's
 * pass, the backlight's and brightness's changes and a mode set hold
 * `quiet` while they call one; the screen owner's calls look at the flag. */
static volatile bool asleep;
static struct mutex quiet = MUTEX_INIT("gpu sleep");

static void log_monitor(const struct gpu *g, const struct gpu_output *o, const char *what)
{
    const struct edid_info *e = &o->info;
    if (!e->valid) {
        kprintf("  %-14s %s %s: an EDID of %u bytes that is not valid\n", g->name, o->name, what,
                o->edid_size);
        return;
    }
    kprintf("  %s:%*s %s %s: '%s', native %ux%u at %u.%03u Hz, %d modes listed\n", g->name,
            (int)(13 - strlen(g->name)), "", o->name, what, e->name[0] ? e->name : "(no name)",
            e->native.width, e->native.height, e->native.refresh_mhz / 1000,
            e->native.refresh_mhz % 1000, e->count);
}

/* One look at output i: who answers there now. */
static void look(struct gpu *g, int i)
{
    struct gpu_output *o = &g->outputs[i];
    uint8_t ident[16];
    bool answers = g->read_edid(g, i, 0, ident, sizeof(ident)) && ident[0] == 0 &&
                   ident[1] == 0xFF && ident[7] == 0;
    if (!answers) {
        uint64_t s = spin_lock(&lock);
        bool was = o->connected;
        o->connected = false;
        spin_unlock(&lock, s);
        if (was)
            kprintf("  %s:%*s %s disconnected\n", g->name, (int)(13 - strlen(g->name)), "", o->name);
        return;
    }
    if (o->connected && !memcmp(ident, o->ident, sizeof(ident)))
        return;                                 /* the same monitor */
    static uint8_t edid[256];                   /* (one look at a time: the threads' turns
                                                 * are kept apart by `reading`) */
    static struct mutex reading = MUTEX_INIT("gpu reading");
    mutex_lock(&reading);
    uint32_t size = 128;
    if (!g->read_edid(g, i, 0, edid, 128)) {
        failed++;
        mutex_unlock(&reading);
        return;                                 /* (looked at again in 2 s) */
    }
    if (edid[126] >= 1 && g->read_edid(g, i, 128, edid + 128, 128))
        size = 256;
    reads++;
    struct edid_info info;
    edid_parse(edid, size, &info);
    bool was = o->connected;
    uint64_t s = spin_lock(&lock);
    memcpy(o->ident, ident, sizeof(ident));
    memcpy(o->edid, edid, size);
    o->edid_size = size;
    o->info = info;
    o->connected = true;
    spin_unlock(&lock, s);
    mutex_unlock(&reading);
    log_monitor(g, o, was ? "a new monitor" : "connected");
}

static void watch(void *arg)
{
    struct gpu *g = arg;
    for (;;) {
        timer_sleep_ms(WATCH_MS);
        mutex_lock(&quiet);
        if (!asleep) {
            for (int i = 0; i < g->output_count; i++)
                look(g, i);
            looks++;
        }
        mutex_unlock(&quiet);
    }
}

void gpu_sleep(bool going)
{
    mutex_lock(&quiet);
    asleep = going;
    mutex_unlock(&quiet);
}

void gpu_register(struct gpu *g)
{
    if (gpu_count == GPUS)
        return;
    ksnprintf(g->name, sizeof(g->name), "gpu%d", gpu_count);
    for (int i = 0; i < g->output_count; i++) {     /* kind + its number across every device */
        int n = 0;
        for (int k = 0; k < gpu_count; k++)
            for (int j = 0; j < gpus[k]->output_count; j++)
                n += !strcmp(gpus[k]->outputs[j].kind, g->outputs[i].kind);
        for (int j = 0; j < i; j++)
            n += !strcmp(g->outputs[j].kind, g->outputs[i].kind);
        ksnprintf(g->outputs[i].name, sizeof(g->outputs[i].name), "%s%d", g->outputs[i].kind, n);
    }
    gpus[gpu_count++] = g;
    char list[128];
    size_t at = 0;
    for (int i = 0; i < g->output_count; i++)
        at += (size_t)ksnprintf(list + at, sizeof(list) - at, "%s%s (%s)", i ? ", " : "",
                                g->outputs[i].name, g->outputs[i].where);
    kprintf("  %s:%*s %02x:%02x.%u %s (%04x:%04x); outputs %s\n", g->name,
            (int)(13 - strlen(g->name)), "", g->pci->bus, g->pci->device, g->pci->function,
            g->model, g->pci->vendor_id, g->pci->device_id, list);
    if (!g->read_edid)
        return;                                     /* (66e-1: a driver that cannot read yet) */
    for (int i = 0; i < g->output_count; i++)       /* who is there at the boot */
        look(g, i);
    if (!thread_create(g->name, watch, g))
        kprintf("  %s: no watching thread: plugging is not noticed\n", g->name);
}

/* The standard timings (VESA DMT; CEA-861 for 1280x720 and 1920x1080): pixel
 * clock (kHz), then active, sync start, sync end and total, horizontally and
 * vertically, and the sync directions. 1280x800, 1680x1050 and 1920x1200 at
 * 60 Hz are DMT's reduced-blanking timings (CVT RB): less bandwidth, and
 * flat panels, which list them, all take them. */
static const struct edid_detailed standard[] = {
    { 25175, 640, 656, 752, 800, 480, 490, 492, 525, false, false, 0 },
    { 31500, 640, 656, 720, 840, 480, 481, 484, 500, false, false, 0 },
    { 28322, 720, 738, 846, 900, 400, 412, 414, 449, false, true, 0 },
    { 40000, 800, 840, 968, 1056, 600, 601, 605, 628, true, true, 0 },
    { 49500, 800, 816, 896, 1056, 600, 601, 604, 625, true, true, 0 },
    { 65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, false, false, 0 },
    { 78750, 1024, 1040, 1136, 1312, 768, 769, 772, 800, true, true, 0 },
    { 108000, 1152, 1216, 1344, 1600, 864, 865, 868, 900, true, true, 0 },
    { 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, true, true, 0 },
    { 71000, 1280, 1328, 1360, 1440, 800, 803, 809, 823, true, false, 0 },
    { 108000, 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066, true, true, 0 },
    { 135000, 1280, 1296, 1440, 1688, 1024, 1025, 1028, 1066, true, true, 0 },
    { 162000, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, true, true, 0 },
    { 119000, 1680, 1728, 1760, 1840, 1050, 1053, 1059, 1080, true, false, 0 },
    { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, true, true, 0 },
    { 154000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235, true, false, 0 },
    { 162000, 2048, 2074, 2154, 2250, 1152, 1153, 1156, 1200, true, true, 0 },
};

static uint32_t refresh_of(const struct edid_detailed *t)
{
    uint64_t frame = (uint64_t)t->htotal * t->vtotal;
    return frame ? (uint32_t)(((uint64_t)t->clock_khz * 1000000 + frame / 2) / frame) : 0;
}

/* Rates compared as a menu shows them: whole hertz (59.940 is 60). */
static bool same_rate(uint32_t a_mhz, uint32_t b_mhz)
{
    return (a_mhz + 500) / 1000 == (b_mhz + 500) / 1000;
}

/* The whole timing of w x h at about mhz: the monitor's own detailed timing,
 * else the standard one. False if neither has it. */
static bool timing_for(const struct edid_info *e, uint32_t w, uint32_t h, uint32_t mhz,
                       struct edid_detailed *out)
{
    for (int k = 0; k < e->detailed_count; k++)
        if (e->detailed[k].hactive == w && e->detailed[k].vactive == h &&
            same_rate(e->detailed[k].refresh_mhz, mhz)) {
            *out = e->detailed[k];
            return true;
        }
    for (size_t k = 0; k < sizeof(standard) / sizeof(standard[0]); k++)
        if (standard[k].hactive == w && standard[k].vactive == h &&
            same_rate(refresh_of(&standard[k]), mhz)) {
            *out = standard[k];
            out->refresh_mhz = refresh_of(out);
            return true;
        }
    return false;
}

/* gpu_set_mode's work; `from` (66b-6) names the settings file a mode at the
 * start came from, for the log, or is 0 (displays set). */
static int change_mode(const char *output, uint32_t width, uint32_t height, uint32_t refresh_mhz,
                       char *said, size_t said_size, const char *from)
{
    static struct mutex changing = MUTEX_INIT("gpu modes");
    struct gpu *g = 0;
    int i = -1;
    for (int k = 0; k < gpu_count && !g; k++)
        for (int j = 0; j < gpus[k]->output_count; j++)
            if (!strcmp(gpus[k]->outputs[j].name, output)) {
                g = gpus[k];
                i = j;
                break;
            }
    if (!g) {
        ksnprintf(said, said_size, "%s: no such output", output);
        return -ERR_NOENT;
    }
    struct edid_info *e = kmalloc(sizeof(*e));
    if (!e)
        return -ERR_NOMEM;
    uint64_t s = spin_lock(&lock);
    bool connected = g->outputs[i].connected;
    *e = g->outputs[i].info;
    spin_unlock(&lock, s);
    int result = -ERR_INVAL;
    struct edid_detailed t;
    if (!connected || !e->valid) {
        ksnprintf(said, said_size, "%s: no monitor (or none that says what it can show)", output);
        result = -ERR_NODEV;
    } else if (!refresh_mhz && !(refresh_mhz = edid_refresh(e, width, height))) {
        ksnprintf(said, said_size, "%s: the monitor does not list %ux%u", output, width, height);
    } else {
        bool listed = false;
        for (int k = 0; k < e->count && !listed; k++)
            listed = e->timings[k].width == width && e->timings[k].height == height &&
                     same_rate(e->timings[k].refresh_mhz, refresh_mhz);
        if (!listed)
            ksnprintf(said, said_size, "%s: the monitor does not list %ux%u at %u Hz", output,
                      width, height, (refresh_mhz + 500) / 1000);
        else if (!timing_for(e, width, height, refresh_mhz, &t))
            ksnprintf(said, said_size, "%s: no timing known for %ux%u at %u Hz", output, width,
                      height, (refresh_mhz + 500) / 1000);
        else if (!g->set_mode || !g->showing || !g->showing(g, i))
            ksnprintf(said, said_size, "%s: %s", output, !g->set_mode
                      ? "its driver cannot set modes" : "the picture is not on this output");
        else
            result = 1;                                 /* go on */
    }
    kfree(e);
    if (result != 1) {
        kprintf("  %s:%*s %s\n", g->name, (int)(13 - strlen(g->name)), "", said);
        return result;
    }
    mutex_lock(&changing);
    struct framebuffer fb;
    char why[96] = "the machine is going to sleep or waking";
    mutex_lock(&quiet);
    bool set = !asleep && g->set_mode(g, i, &t, &fb, why, sizeof(why));
    mutex_unlock(&quiet);
    if (!set) {
        mutex_unlock(&changing);
        ksnprintf(said, said_size, "%s: %ux%u at %u.%03u Hz: %s", output, width, height,
                  t.refresh_mhz / 1000, t.refresh_mhz % 1000, why);
        kprintf("  %s:%*s %s\n", g->name, (int)(13 - strlen(g->name)), "", said);
        return -ERR_INVAL;
    }
    /* (66b-3) A framebuffer larger than the firmware's: write-combining
     * beyond its old end too (uncached, the J1900 wrote 69 MB/s there,
     * 2,376 within). */
    bool combining = paging_write_combining(virt_to_phys((const void *)fb.base),
                                            (uint64_t)fb.stride * fb.height * 4);
    bool drawn = console_set_screen(&fb);
    mouse_screen(fb.width, fb.height);                  /* (66b-5) the pointer reaches it all */
    set_mhz = t.refresh_mhz;
    set_by = from ? from : "displays set";
    shown_buffer = 0;                                   /* (66d-1) a mode shows buffer 0 */
    mutex_unlock(&changing);
    if (from)
        ksnprintf(said, said_size, "%s now %ux%u at %u.%03u Hz (%s)%s", output, width, height,
                  t.refresh_mhz / 1000, t.refresh_mhz % 1000, from,
                  !drawn ? "; the console could not follow"
                  : combining ? "" : "; not write-combining");
    else
        ksnprintf(said, said_size, "%s now %ux%u at %u.%03u Hz (pixel clock %u.%03u MHz)%s",
                  output, width, height, t.refresh_mhz / 1000, t.refresh_mhz % 1000,
                  t.clock_khz / 1000, t.clock_khz % 1000, !drawn ? "; the console could not follow"
                  : combining ? "" : "; not write-combining");
    kprintf("  %s:%*s %s\n", g->name, (int)(13 - strlen(g->name)), "", said);
    return 0;
}

int gpu_set_mode(const char *output, uint32_t width, uint32_t height, uint32_t refresh_mhz,
                 char *said, size_t said_size)
{
    return change_mode(output, width, height, refresh_mhz, said, said_size, 0);
}

#define DISPLAYS_FILE     "/system/config/displays"
#define DISPLAYS_FILE_MAX 4096

static bool blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* "1920x1080" or "1920x1080@60" (whole hertz), as displays set takes it. */
static bool parse_mode(const char *s, size_t n, uint32_t *w, uint32_t *h, uint32_t *mhz)
{
    uint32_t v[3] = { 0, 0, 0 };
    int part = 0;
    size_t digits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (v[part] > 100000)
                return false;
            v[part] = v[part] * 10 + (uint32_t)(c - '0');
            digits++;
        } else if ((c == 'x' && part == 0) || (c == '@' && part == 1)) {
            if (!digits)
                return false;
            part++;
            digits = 0;
        } else {
            return false;
        }
    }
    if (part < 1 || !digits || !v[0] || !v[1] || v[0] > 16384 || v[1] > 16384 ||
        (part == 2 && (!v[2] || v[2] > 1000)))
        return false;
    *w = v[0], *h = v[1], *mhz = v[2] * 1000;
    return true;
}

void gpu_load_settings(void)
{
    static char text[DISPLAYS_FILE_MAX];
    struct vfs *fs = mounts_root();
    uint64_t id;
    struct rfs_inode inode;
    int64_t got = fs && vfs_lookup(fs, DISPLAYS_FILE, &id, &inode)
                  ? vfs_read(fs, id, &inode, 0, text, sizeof(text)) : -1;
    if (got <= 0)
        return;                                     /* no file: the firmware's mode */
    size_t n = (size_t)got;
    int line = 0;
    for (size_t at = 0; at < n; line++) {
        size_t end = at;
        while (end < n && text[end] != '\n')
            end++;
        size_t i = at, stop = end;
        at = end + 1;
        for (size_t k = i; k < stop; k++)
            if (text[k] == '#')
                stop = k;                           /* a comment to the line's end */
        while (i < stop && blank(text[i]))
            i++;
        while (stop > i && blank(text[stop - 1]))
            stop--;
        if (i == stop)
            continue;                               /* empty, or only a comment */
        size_t key = i;
        while (i < stop && !blank(text[i]) && text[i] != '=')
            i++;
        char output[8];
        size_t key_len = i - key;
        while (i < stop && blank(text[i]))
            i++;
        uint32_t w, h, mhz;
        if (key_len == 0 || key_len >= sizeof(output) || i == stop || text[i] != '=') {
            kprintf("  %-15sline %d of %s: not OUTPUT = WxH[@HZ]\n", "displays:", line + 1,
                    DISPLAYS_FILE);
            continue;
        }
        i++;
        while (i < stop && blank(text[i]))
            i++;
        if (!parse_mode(text + i, stop - i, &w, &h, &mhz)) {
            kprintf("  %-15sline %d of %s: not OUTPUT = WxH[@HZ]\n", "displays:", line + 1,
                    DISPLAYS_FILE);
            continue;
        }
        memcpy(output, text + key, key_len);
        output[key_len] = 0;
        char said[128];
        if (change_mode(output, w, h, mhz, said, sizeof(said), DISPLAYS_FILE) == -ERR_NOENT)
            kprintf("  %-15s%s in %s: no such output\n", "displays:", output, DISPLAYS_FILE);
    }
}

int gpu_wait_vblank(uint64_t *frame, uint32_t timeout_ms)
{
    for (int k = 0; !asleep && k < gpu_count; k++) {
        struct gpu *g = gpus[k];
        if (!g->wait_vblank || !g->showing)
            continue;
        for (int i = 0; i < g->output_count; i++)
            if (g->showing(g, i))
                return g->wait_vblank(g, i, frame, timeout_ms) ? 1 : 0;
    }
    return -ERR_NODEV;
}

int gpu_flip(int buffer, uint64_t *frame)
{
    if (buffer < 0 || buffer > 1)
        return -ERR_INVAL;
    for (int k = 0; !asleep && k < gpu_count; k++) {
        struct gpu *g = gpus[k];
        if (!g->flip || !g->showing)
            continue;
        for (int i = 0; i < g->output_count; i++)
            if (g->showing(g, i)) {
                if (!g->flip(g, i, buffer, frame))
                    return 0;
                shown_buffer = buffer;
                return 1;
            }
    }
    return -ERR_NODEV;
}

int gpu_buffers(struct sys_display_buffers *d, uint64_t *phys, uint64_t *slot)
{
    for (int k = 0; !asleep && k < gpu_count; k++) {
        struct gpu *g = gpus[k];
        if (!g->buffers || !g->showing)
            continue;
        for (int i = 0; i < g->output_count; i++) {
            int count;
            uint32_t stride, w, h, s;
            uint8_t rs, gs, bs;
            if (!g->showing(g, i))
                continue;
            if (!g->buffers(g, i, phys, slot, &count, &stride) ||
                !console_screen_format(&w, &h, &s, &rs, &gs, &bs))
                return -ERR_NODEV;
            d->count = (uint32_t)(count < DISPLAY_BUFFERS_MAX ? count : DISPLAY_BUFFERS_MAX);
            d->width = w;
            d->height = h;
            d->stride = stride;
            return 0;
        }
    }
    return -ERR_NODEV;
}

void gpu_screen_back(void)
{
    uint64_t frame;
    if (shown_buffer)
        gpu_flip(0, &frame);
}

void gpu_backlight(bool on, char *said, size_t size)
{
    size_t at = 0;
    said[0] = 0;
    mutex_lock(&quiet);
    for (int k = 0; !asleep && k < gpu_count; k++) {
        struct gpu *g = gpus[k];
        for (int i = 0; g->backlight && i < g->output_count; i++)
            if (g->backlight(g, i, on) && at < size)    /* (37e-2d) a monitor: its signal */
                at += (size_t)ksnprintf(said + at, size - at, "%sthe %s %s: %s", at ? "; " : "",
                                        strcmp(g->outputs[i].kind, "edp") ? "signal" : "backlight",
                                        on ? "on" : "off", g->outputs[i].name);
    }
    mutex_unlock(&quiet);
}

void gpu_dim(bool dim, char *said, size_t size)
{
    size_t at = 0;
    said[0] = 0;
    mutex_lock(&quiet);
    for (int k = 0; !asleep && k < gpu_count; k++) {
        struct gpu *g = gpus[k];
        if (!g->brightness || !g->brightness_now)
            continue;
        for (int i = 0; i < g->output_count; i++) {
            struct gpu_output *o = &g->outputs[i];
            uint32_t now, to;
            if (dim == o->dimmed || !g->brightness_now(g, i, &now))
                continue;
            to = dim ? now / 3 : o->undimmed;
            if (!g->brightness(g, i, to))
                continue;
            if (dim)
                o->undimmed = now;
            o->dimmed = dim;
            if (at < size)
                at += (size_t)ksnprintf(said + at, size - at, "%s%s 0x%04x to 0x%04x",
                                        at ? ", " : "", o->name, now, to);
        }
    }
    mutex_unlock(&quiet);
}

void gpu_backlight_emergency(void)
{
    for (int k = 0; k < gpu_count; k++)
        for (int i = 0; gpus[k]->backlight && i < gpus[k]->output_count; i++)
            gpus[k]->backlight(gpus[k], i, true);
}

static void print_mode(const char *label, const struct sys_display_mode *m)
{
    kprintf("%s%ux%u", label, m->width, m->height);
    if (m->refresh_mhz)
        kprintf(" at %u.%03u Hz", m->refresh_mhz / 1000, m->refresh_mhz % 1000);
}

static struct gpu *driver_of(const struct pci_device *d)
{
    for (int k = 0; k < gpu_count; k++)
        if (gpus[k]->pci == d)
            return gpus[k];
    return 0;
}

/* (37g-5c-2) The devices that keep something over a sleep, each with its
 * driver's lines (the device's model and place first). */
void gpu_print_sleep(void)
{
    int shown = 0;
    for (int k = 0; k < gpu_count; k++) {
        struct gpu *g = gpus[k];
        if (!g->report_sleep)
            continue;
        kprintf("%s: %s at %02x:%02x.%u\n", g->name, g->model, g->pci->bus, g->pci->device,
                g->pci->function);
        g->report_sleep(g);
        shown++;
    }
    if (!shown)
        kprintf("no graphics driver here keeps its registers over a sleep\n");
}

void gpu_print(void)
{
    static struct sys_display_modes fw;         /* (one report at a time: kprintf's capture) */
    static struct gpu_output copy[GPU_OUTPUTS];
    uint32_t w = 0, h = 0, stride;
    uint8_t rs, gs, bs;
    if (console_screen_format(&w, &h, &stride, &rs, &gs, &bs)) {
        display_modes(&fw, w, h);
        if (set_mhz) {
            fw.now.refresh_mhz = set_mhz;
            print_mode("screen: ", &fw.now);
            kprintf(", set %s %s", strcmp(set_by, "displays set") ? "from" : "with", set_by);
        } else {
            print_mode("screen: ", &fw.now);
            kprintf(", the firmware's mode");
        }
        if (fw.edid_size)
            kprintf("; its EDID: '%s'", fw.name);
        kprintf("\n");
    } else {
        kprintf("screen: none\n");
    }
    int found = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        if (d->class_code != 0x03)                  /* display controllers */
            continue;
        found++;
        struct gpu *g = driver_of(d);
        if (!g) {
            kprintf("%02x:%02x.%u %04x:%04x: no driver for these graphics yet\n", d->bus,
                    d->device, d->function, d->vendor_id, d->device_id);
            continue;
        }
        uint64_t s = spin_lock(&lock);
        memcpy(copy, g->outputs, sizeof(copy));
        spin_unlock(&lock, s);
        kprintf("%s: %s at %02x:%02x.%u; outputs looked at every %u s\n", g->name, g->model,
                d->bus, d->device, d->function, WATCH_MS / 1000);
        for (int k = 0; k < g->output_count; k++) {
            const struct gpu_output *o = &copy[k];
            bool showing = g->showing && g->showing(g, k);
            kprintf("  %-6s %-13s ", o->name, o->where);
            if (!g->read_edid) {
                kprintf("its monitor not read yet\n");
                continue;
            }
            if (!o->connected) {
                kprintf("no monitor%s\n", showing ? " (the port is on)" : "");
                continue;
            }
            const struct edid_info *e = &o->info;
            if (!e->valid) {
                kprintf("a monitor with an EDID that is not valid (%u bytes)\n", o->edid_size);
                continue;
            }
            kprintf("'%s', native %ux%u at %u.%03u Hz, EDID %u bytes%s\n",
                    e->name[0] ? e->name : "(no name)", e->native.width, e->native.height,
                    e->native.refresh_mhz / 1000, e->native.refresh_mhz % 1000, o->edid_size,
                    showing ? "; showing the picture" : "");
            kprintf("         modes:");
            for (int m = 0; m < e->count; m++)
                kprintf(" %ux%u@%u", e->timings[m].width, e->timings[m].height,
                        (e->timings[m].refresh_mhz + 500) / 1000);
            kprintf("\n");
        }
        if (g->report)
            g->report(g);
    }
    if (!found)
        kprintf("no graphics device on PCI\n");
    if (gpu_count)
        kprintf("(all devices: %lu looks, %lu EDIDs read, %lu reads failed)\n", looks, reads,
                failed);
    /* (66b-3) What writing to the screen costs, where it lands. */
    uint64_t fast, fast_ticks, slow, slow_ticks, hz = arch_counter_hz();
    console_screen_writes(&fast, &fast_ticks, &slow, &slow_ticks);
    if (gpu_count && hz && fast + slow) {          /* (where a driver can set a mode) */
        kprintf("screen writes: %lu KB in %lu ms within the firmware's framebuffer "
                "(write-combining)", fast >> 10, fast_ticks * 1000 / hz);
        if (fast_ticks)
            kprintf(", %lu MB/s", (fast >> 10) * (hz >> 10) / fast_ticks);
        kprintf("; %lu KB in %lu ms beyond it", slow >> 10, slow_ticks * 1000 / hz);
        if (slow_ticks)
            kprintf(", %lu MB/s", (slow >> 10) * (hz >> 10) / slow_ticks);
        kprintf("\n");
    }
}
