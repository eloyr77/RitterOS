/*
 * Test: the display handoff (step 15d). Take over the screen from the text
 * console, fill it with a solid color so a screenshot can confirm it, wait
 * for the test to capture it, then give the screen back (the console
 * redraws). Proves SYS_DISPLAY: acquire, info, flush and release.
 * (65e-34) `display modes` prints DISPLAY_MODES instead: the screen's mode
 * and refresh, the monitor and its native mode, the firmware's modes, and
 * the EDID's size and first bytes. (66c-3) `display vblank` waits for 60
 * vertical blanks (DISPLAY_VBLANK) and says how long they took and the
 * frame numbers, or why there are none. (66d-1) `display flip` fills the
 * screen red, shows screen buffer 1 (black) for a second and buffer 0
 * again, timing each flip (DISPLAY_FLIP), then gives the screen back.
 * (66d-2) `display buffers` maps the screen buffers (DISPLAY_BUFFERS),
 * fills buffer 1 green itself (timed: write-combining memory), shows it
 * for a second and buffer 0 again; `display buffers exit` ends while it
 * still has them and the screen (the kernel takes both back).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

static uint32_t row[8192];              /* one screen row of pixels */

static void hz(const struct sys_display_mode *m)
{
    if (m->refresh_mhz)
        printf("%ux%u at %u.%03u Hz", m->width, m->height, m->refresh_mhz / 1000,
               m->refresh_mhz % 1000);
    else
        printf("%ux%u", m->width, m->height);
}

static int modes(void)
{
    static struct sys_display_modes d;
    if (ritteros_display_modes(&d) < 0) {
        printf("display: modes: %s\n", strerror(errno));
        return 1;
    }
    printf("display: now ");
    hz(&d.now);
    if (d.edid_size) {
        printf("; monitor '%s', native ", d.name);
        hz(&d.native);
        printf("; EDID %u bytes, begins", d.edid_size);
        for (int i = 0; i < 8; i++)
            printf(" %02x", d.edid[i]);
    } else {
        printf("; no EDID");
    }
    printf("\ndisplay: %u modes:", d.mode_count);
    for (uint32_t i = 0; i < d.mode_count; i++) {
        printf(" ");
        hz(&d.modes[i]);
    }
    printf("\n");
    return 0;
}

static int vblanks(void)
{
    uint64_t first = 0, frame = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 61; i++) {
        int r = ritteros_display_vblank(&frame, 200);
        if (r < 0) {
            printf("display: no vertical blank here (%s)\n", strerror(errno));
            return 1;
        }
        if (r == 0) {
            printf("display: vertical blank %d did not come within 200 ms\n", i);
            return 1;
        }
        if (i == 0) {
            first = frame;
            clock_gettime(CLOCK_MONOTONIC, &t0);    /* timed from the first one */
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long us = (t1.tv_sec - t0.tv_sec) * 1000000LL + (t1.tv_nsec - t0.tv_nsec) / 1000;
    printf("display: 60 vertical blanks in %lld.%03lld ms (frames %llu to %llu)\n", us / 1000,
           us % 1000, (unsigned long long)first, (unsigned long long)frame);
    return 0;
}

static int flips(void)
{
    if (ritteros_display_acquire() < 0) {
        printf("display: acquire: %s\n", strerror(errno));
        return 1;
    }
    struct sys_display d;
    if (ritteros_display_info(&d) < 0) {
        ritteros_display_release();
        return 1;
    }
    uint32_t width = d.width < 8192 ? d.width : 8192;
    for (uint32_t i = 0; i < width; i++)
        row[i] = (uint32_t)255 << d.red_shift;
    ritteros_display_flush(row, 0, 0, 0, width, d.height);  /* buffer 0 red */
    int status = 0;
    for (int b = 1; b >= 0; b--) {                  /* buffer 1 (black), then 0 again */
        struct timespec t0, t1;
        uint64_t frame = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = ritteros_display_flip(b, &frame);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long long us = (t1.tv_sec - t0.tv_sec) * 1000000LL + (t1.tv_nsec - t0.tv_nsec) / 1000;
        if (r < 0) {
            printf("display: no flipping here (%s)\n", strerror(errno));
            status = 1;
            break;
        }
        if (r == 0) {
            printf("display: buffer %d did not show\n", b);
            status = 1;
            break;
        }
        printf("display: buffer %d shown after %lld.%03lld ms (frame %llu)\n", b, us / 1000,
               us % 1000, (unsigned long long)frame);
        if (b)
            sleep(1);
    }
    ritteros_display_release();
    return status;
}

static long long since_us(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000000LL + (t1.tv_nsec - t0->tv_nsec) / 1000;
}

static int buffers(int just_exit)
{
    if (ritteros_display_acquire() < 0) {
        printf("display: acquire: %s\n", strerror(errno));
        return 1;
    }
    struct sys_display info;
    struct sys_display_buffers b;
    if (ritteros_display_info(&info) < 0 || ritteros_display_buffers(&b) < 0) {
        printf("display: no screen buffers here (%s)\n", strerror(errno));
        ritteros_display_release();
        return 1;
    }
    printf("display: %u screen buffers of %ux%u, stride %u, at 0x%llx and 0x%llx\n", b.count,
           b.width, b.height, b.stride, (unsigned long long)b.address[0],
           (unsigned long long)b.address[1]);
    uint32_t green = (uint32_t)255 << info.green_shift;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t y = 0; y < b.height; y++) {
        uint32_t *line = (uint32_t *)(uintptr_t)(b.address[1] + (uint64_t)y * b.stride);
        for (uint32_t x = 0; x < b.width; x++)
            line[x] = green;
    }
    long long us = since_us(&t0);
    uint64_t bytes = (uint64_t)b.width * b.height * 4, frame;
    printf("display: buffer 1 filled green in %lld.%03lld ms (%llu MB/s)\n", us / 1000, us % 1000,
           (unsigned long long)(us ? bytes / (uint64_t)us : 0));
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = ritteros_display_flip(1, &frame);
    printf("display: buffer 1 %s after %lld ms\n", r > 0 ? "shown" : "not shown", since_us(&t0) / 1000);
    sleep(1);
    if (just_exit)
        _exit(r > 0 ? 0 : 1);                       /* still holding the screen and buffers */
    ritteros_display_flip(0, &frame);
    ritteros_display_release();
    return r > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "buffers"))
        return buffers(argc == 3 && !strcmp(argv[2], "exit"));
    if (argc == 2 && !strcmp(argv[1], "flip"))
        return flips();
    if (argc == 2 && !strcmp(argv[1], "modes"))
        return modes();
    if (argc == 2 && !strcmp(argv[1], "vblank"))
        return vblanks();
    if (ritteros_display_acquire() < 0) {
        printf("display: acquire: %s\n", strerror(errno));
        return 1;
    }
    struct sys_display d;
    if (ritteros_display_info(&d) < 0) {
        printf("display: info: %s\n", strerror(errno));
        ritteros_display_release();
        return 1;
    }
    uint32_t width = d.width < 8192 ? d.width : 8192;
    uint32_t color = (uint32_t)255 << d.red_shift;      /* pure red, in the native format */
    for (uint32_t i = 0; i < width; i++)
        row[i] = color;
    /* A stride of 0 makes the kernel copy this one row to every screen row. */
    if (ritteros_display_flush(row, 0, 0, 0, width, d.height) < 0) {
        printf("display: flush: %s\n", strerror(errno));
        ritteros_display_release();
        return 1;
    }
    printf("display: %ux%u filled red\n", d.width, d.height);
    char c;
    read(0, &c, 1);                     /* wait until the test has captured the screen */
    ritteros_display_release();
    printf("display: released\n");
    return 0;
}
