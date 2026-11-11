#include "edid.h"

/* The established timings (bytes 35-36, bit 7 first, then byte 37 bit 7). */
static const struct edid_timing established[17] = {
    { 720, 400, 70000 }, { 720, 400, 88000 }, { 640, 480, 60000 }, { 640, 480, 67000 },
    { 640, 480, 72000 }, { 640, 480, 75000 }, { 800, 600, 56000 }, { 800, 600, 60000 },
    { 800, 600, 72000 }, { 800, 600, 75000 }, { 832, 624, 75000 }, { 1024, 768, 0 },
    { 1024, 768, 60000 }, { 1024, 768, 70000 }, { 1024, 768, 75000 }, { 1280, 1024, 75000 },
    { 1152, 870, 75000 },
};

static void add(struct edid_info *e, uint32_t w, uint32_t h, uint32_t mhz)
{
    if (!w || !h || !mhz || e->count == EDID_TIMINGS)
        return;
    for (int i = 0; i < e->count; i++)
        if (e->timings[i].width == w && e->timings[i].height == h &&
            e->timings[i].refresh_mhz == mhz)
            return;
    e->timings[e->count++] = (struct edid_timing){ w, h, mhz };
}

/* An 18-byte detailed timing descriptor: a mode (pixel clock != 0), or a
 * display descriptor (the monitor's name: tag 0xFC). */
static void descriptor(struct edid_info *e, const uint8_t *d)
{
    uint32_t clock = (uint32_t)d[0] | (uint32_t)d[1] << 8;      /* 10 kHz units */
    if (!clock) {
        if (d[3] == 0xFC && !e->name[0]) {
            int n = 0;
            while (n < 13 && d[5 + n] != 0x0A && d[5 + n] != 0)
                n++;
            while (n > 0 && d[5 + n - 1] == ' ')
                n--;
            for (int i = 0; i < n; i++)
                e->name[i] = (char)(d[5 + i] >= 0x20 && d[5 + i] < 0x7F ? d[5 + i] : '?');
            e->name[n] = 0;
        }
        return;
    }
    uint32_t w = d[2] | (uint32_t)(d[4] >> 4) << 8, hblank = d[3] | (uint32_t)(d[4] & 15) << 8;
    uint32_t h = d[5] | (uint32_t)(d[7] >> 4) << 8, vblank = d[6] | (uint32_t)(d[7] & 15) << 8;
    if (d[17] & 0x80)
        return;                                         /* interlaced: not a mode we show */
    uint64_t total = (uint64_t)(w + hblank) * (h + vblank);
    if (!total)
        return;
    uint32_t mhz = (uint32_t)(((uint64_t)clock * 10000 * 1000 + total / 2) / total);
    if (!e->native.width)
        e->native = (struct edid_timing){ w, h, mhz };
    add(e, w, h, mhz);
    /* (66b-2) The whole timing: the front porch and sync width (bytes 8-11,
     * their top bits in 11), the sync directions when they are separate
     * digital signals (byte 17 bits 4-3 = 11: bit 2 vertical, bit 1
     * horizontal positive); other kinds are taken as negative. */
    uint32_t hfp = d[8] | (uint32_t)(d[11] >> 6 & 3) << 8;
    uint32_t hsw = d[9] | (uint32_t)(d[11] >> 4 & 3) << 8;
    uint32_t vfp = (uint32_t)(d[10] >> 4) | (uint32_t)(d[11] >> 2 & 3) << 4;
    uint32_t vsw = (uint32_t)(d[10] & 15) | (uint32_t)(d[11] & 3) << 4;
    if (e->detailed_count == EDID_DETAILED || hfp + hsw > hblank || vfp + vsw > vblank)
        return;
    bool separate = (d[17] & 0x18) == 0x18;
    e->detailed[e->detailed_count++] = (struct edid_detailed){
        clock * 10, w, w + hfp, w + hfp + hsw, w + hblank, h, h + vfp, h + vfp + vsw, h + vblank,
        separate && (d[17] & 2), separate && (d[17] & 4), mhz };
}

bool edid_parse(const uint8_t *edid, uint32_t size, struct edid_info *out)
{
    static const uint8_t header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    *out = (struct edid_info){ 0 };
    if (size < 128)
        return false;
    for (int i = 0; i < 8; i++)
        if (edid[i] != header[i])
            return false;
    uint32_t blocks = 1 + edid[126];
    if (blocks > size / 128)
        blocks = size / 128;                            /* (what the firmware passed on) */
    for (uint32_t b = 0; b < blocks; b++) {
        uint8_t sum = 0;
        for (int i = 0; i < 128; i++)
            sum = (uint8_t)(sum + edid[b * 128 + (uint32_t)i]);
        if (sum)
            return false;
    }
    out->blocks = blocks;
    for (int i = 0; i < 4; i++)                         /* the four descriptors */
        descriptor(out, edid + 54 + 18 * i);
    for (int bit = 0; bit < 17; bit++) {                /* the established timings */
        int byte = 35 + bit / 8, mask = 0x80 >> (bit % 8);
        if (edid[byte] & mask)
            add(out, established[bit].width, established[bit].height,
                established[bit].refresh_mhz);
    }
    for (int i = 0; i < 8; i++) {                       /* the standard timings */
        const uint8_t *t = edid + 38 + 2 * i;
        if ((t[0] == 1 && t[1] == 1) || t[0] == 0)
            continue;
        uint32_t w = ((uint32_t)t[0] + 31) * 8, h;
        switch (t[1] >> 6) {
        case 0: h = edid[18] == 1 && edid[19] < 3 ? w : w * 10 / 16; break;  /* 1:1 before 1.3 */
        case 1: h = w * 3 / 4; break;
        case 2: h = w * 4 / 5; break;
        default: h = w * 9 / 16; break;
        }
        add(out, w, h, ((uint32_t)(t[1] & 0x3F) + 60) * 1000);
    }
    for (uint32_t b = 1; b < blocks; b++) {             /* CEA-861 extensions: their DTDs */
        const uint8_t *x = edid + b * 128;
        if (x[0] != 0x02 || x[2] < 4)
            continue;
        for (uint32_t at = x[2]; at + 18 <= 127; at += 18) {
            if (!x[at] && !x[at + 1])
                break;
            descriptor(out, x + at);
        }
    }
    out->valid = true;
    return true;
}

uint32_t edid_refresh(const struct edid_info *e, uint32_t width, uint32_t height)
{
    if (!e->valid)
        return 0;
    if (e->native.width == width && e->native.height == height)
        return e->native.refresh_mhz;
    uint32_t lowest = 0;
    for (int i = 0; i < e->count; i++) {
        const struct edid_timing *t = &e->timings[i];
        if (t->width != width || t->height != height)
            continue;
        if (t->refresh_mhz == 60000)
            return 60000;
        if (!lowest || t->refresh_mhz < lowest)
            lowest = t->refresh_mhz;
    }
    return lowest;
}
