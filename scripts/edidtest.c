/*
 * (65e-34) The kernel's EDID parser (kernel/edid.c) on the Mac, under ASan
 * and UBSan: EDIDs built here byte by byte from known timings, checked
 * against values worked out by hand; damaged ones refused; random ones
 * read without a fault. Prints "edidtest: N checks passed" or the first
 * difference, and exits 1 on any.
 *
 *   clang -fsanitize=address,undefined -I kernel scripts/edidtest.c kernel/edid.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "edid.h"

static int checks, failures;

static void expect(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("edidtest: FAILED: %s\n", what);
    }
}

static void checksum(uint8_t *block)
{
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++)
        sum = (uint8_t)(sum + block[i]);
    block[127] = (uint8_t)(256 - sum);
}

/* An 18-byte detailed timing: clock in 10 kHz units, active and blanking
 * sizes, flags byte 17 (0x80: interlaced). */
static void dtd(uint8_t *d, unsigned clock, unsigned w, unsigned hblank, unsigned h,
                unsigned vblank, uint8_t flags)
{
    memset(d, 0, 18);
    d[0] = (uint8_t)clock, d[1] = (uint8_t)(clock >> 8);
    d[2] = (uint8_t)w, d[3] = (uint8_t)hblank, d[4] = (uint8_t)((w >> 8) << 4 | (hblank >> 8));
    d[5] = (uint8_t)h, d[6] = (uint8_t)vblank, d[7] = (uint8_t)((h >> 8) << 4 | (vblank >> 8));
    d[17] = flags;
}

/* (66b-2) A detailed timing's front porches and sync widths. */
static void sync(uint8_t *d, unsigned hfp, unsigned hsw, unsigned vfp, unsigned vsw)
{
    d[8] = (uint8_t)hfp, d[9] = (uint8_t)hsw, d[10] = (uint8_t)((vfp & 15) << 4 | (vsw & 15));
    d[11] = (uint8_t)((hfp >> 8) << 6 | (hsw >> 8) << 4 | (vfp >> 4) << 2 | (vsw >> 4));
}

static int detailed(const struct edid_info *e, int i, unsigned khz, unsigned w, unsigned hs,
                    unsigned he, unsigned ht, unsigned h, unsigned vs, unsigned ve, unsigned vt,
                    int hhigh, int vhigh)
{
    if (i >= e->detailed_count)
        return 0;
    const struct edid_detailed *t = &e->detailed[i];
    return t->clock_khz == khz && t->hactive == w && t->hsync_start == hs && t->hsync_end == he &&
           t->htotal == ht && t->vactive == h && t->vsync_start == vs && t->vsync_end == ve &&
           t->vtotal == vt && t->hsync_high == hhigh && t->vsync_high == vhigh;
}

static void base(uint8_t *e, uint8_t revision)
{
    static const uint8_t header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memset(e, 0, 128);
    memcpy(e, header, 8);
    e[18] = 1, e[19] = revision;
    for (int i = 38; i < 54; i++)
        e[i] = 1;                               /* standard timings unused */
}

static int has(const struct edid_info *e, uint32_t w, uint32_t h, uint32_t mhz)
{
    for (int i = 0; i < e->count; i++)
        if (e->timings[i].width == w && e->timings[i].height == h &&
            e->timings[i].refresh_mhz == mhz)
            return 1;
    return 0;
}

int main(void)
{
    static uint8_t a[256], b[128], r[512];
    struct edid_info e;

    /* A: EDID 1.4 with a CEA extension.
     * - native 1920x1080: 148.5 MHz / (2200 x 1125) = 60.000 Hz
     * - 1600x1200: 162 MHz / (2160 x 1250) = 60.000 Hz
     * - the name "TEST MONITOR" (13 bytes with its line feed)
     * - established 640x480@60 (byte 35 bit 5), 800x600@60 (35 bit 0),
     *   1024x768@60 (36 bit 3) and @75 (36 bit 1)
     * - standard 1280x1024@60 (5:4) and 1680x1050@60 (16:10)
     * - the extension: 1280x720 at 74.25 MHz / (1650 x 750) = 60.000 Hz, and
     *   1920x1080 interlaced (left out) */
    base(a, 4);
    a[35] = 0x21, a[36] = 0x0A;
    a[38] = 1280 / 8 - 31, a[39] = 0x80;
    a[40] = 1680 / 8 - 31, a[41] = 0x00;
    dtd(a + 54, 14850, 1920, 280, 1080, 45, 0x1E);
    sync(a + 54, 88, 44, 4, 5);                 /* CEA's 1080p: both syncs positive */
    memset(a + 72, 0, 18);
    a[75] = 0xFC;
    memcpy(a + 77, "TEST MONITOR\n", 13);
    dtd(a + 90, 16200, 1600, 560, 1200, 50, 0x1A);
    sync(a + 90, 64, 192, 1, 3);                /* VESA's; vertical sync made negative */
    memset(a + 108, 0, 18);
    a[111] = 0xFD;                              /* range limits: no mode */
    a[126] = 1;
    checksum(a);
    uint8_t *x = a + 128;
    x[0] = 0x02, x[1] = 3, x[2] = 4;
    dtd(x + 4, 7425, 1280, 370, 720, 30, 0x0E);
    sync(x + 4, 110, 40, 5, 5);                 /* not separate digital: both negative */
    dtd(x + 22, 7425, 1920, 280, 540, 22, 0x9E);
    checksum(x);

    expect(edid_parse(a, sizeof(a), &e) && e.valid, "A is valid");
    expect(e.blocks == 2, "A: two blocks");
    expect(!strcmp(e.name, "TEST MONITOR"), "A: the name");
    expect(e.native.width == 1920 && e.native.height == 1080 && e.native.refresh_mhz == 60000,
           "A: native 1920x1080 at 60.000 Hz");
    expect(has(&e, 1600, 1200, 60000), "A: 1600x1200 at 60 Hz (a detailed timing)");
    expect(has(&e, 1280, 720, 60000), "A: 1280x720 at 60 Hz (the extension's)");
    expect(has(&e, 1280, 1024, 60000), "A: 1280x1024 at 60 Hz (standard, 5:4)");
    expect(has(&e, 1680, 1050, 60000), "A: 1680x1050 at 60 Hz (standard, 16:10)");
    expect(has(&e, 640, 480, 60000) && has(&e, 800, 600, 60000), "A: established 640x480, 800x600");
    expect(has(&e, 1024, 768, 60000) && has(&e, 1024, 768, 75000), "A: 1024x768 at 60 and 75");
    expect(!has(&e, 1920, 540, 60000) && !has(&e, 1920, 540, 120000), "A: the interlaced one left out");
    expect(e.count == 9, "A: nine modes");
    /* (66b-2) The detailed timings whole, in their order. */
    expect(e.detailed_count == 3, "A: three detailed timings (the interlaced one left out)");
    expect(detailed(&e, 0, 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 1, 1),
           "A: 1920x1080's whole timing, syncs positive");
    expect(detailed(&e, 1, 162000, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, 1, 0),
           "A: 1600x1200's whole timing, vertical sync negative");
    expect(detailed(&e, 2, 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, 0, 0),
           "A: 1280x720's whole timing, analog syncs taken as negative");
    expect(edid_refresh(&e, 1920, 1080) == 60000, "A: refresh of the native mode");
    expect(edid_refresh(&e, 1024, 768) == 60000, "A: 60 Hz preferred for 1024x768");
    expect(edid_refresh(&e, 1920, 1200) == 0, "A: an unlisted size is 0");

    /* Only the base block given (the extension's count says one more). */
    expect(edid_parse(a, 128, &e) && e.blocks == 1 && !has(&e, 1280, 720, 60000),
           "A's base block alone: one block, no extension modes");

    /* B: EDID 1.3, native 1920x1080 at 148.35 MHz: 148350000 / 2475000 =
     * 59.939 Hz (59939 mHz, rounded); 1024x768 at 75 Hz only; no name. */
    base(b, 3);
    b[36] = 0x02;
    dtd(b + 54, 14835, 1920, 280, 1080, 45, 0x1E);
    checksum(b);
    expect(edid_parse(b, sizeof(b), &e) && e.native.refresh_mhz == 59939, "B: 59.939 Hz");
    expect(!e.name[0], "B: no name");
    expect(edid_refresh(&e, 1024, 768) == 75000, "B: 1024x768 only at 75 Hz");

    /* Damaged ones are refused. */
    uint8_t bad[128];
    memcpy(bad, b, 128);
    bad[60] ^= 1;
    expect(!edid_parse(bad, 128, &e) && !e.valid, "a wrong checksum");
    memcpy(bad, b, 128);
    bad[0] = 1;
    checksum(bad);
    expect(!edid_parse(bad, 128, &e), "a wrong header");
    expect(!edid_parse(b, 127, &e), "too short");
    uint8_t two[256];                           /* says one extension, which is damaged */
    memcpy(two, a, 256);
    two[200] ^= 0x55;
    expect(!edid_parse(two, 256, &e), "a damaged extension block");

    /* Random contents with a right header and checksums: read, no fault. */
    srand(65034);
    for (int round = 0; round < 200000; round++) {
        for (int i = 0; i < 512; i++)
            r[i] = (uint8_t)rand();
        memcpy(r, a, 8);
        r[126] = (uint8_t)(rand() % 6);
        for (int k = 0; k < 4; k++)
            checksum(r + 128 * k);
        uint32_t size = 128 + (uint32_t)(rand() % 385);
        edid_parse(r, size, &e);
        (void)edid_refresh(&e, 1920, 1080);
    }
    checks++;

    if (failures) {
        printf("edidtest: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("edidtest: %d checks passed\n", checks);
    return 0;
}
