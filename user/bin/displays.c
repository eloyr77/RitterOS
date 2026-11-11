/*
 * displays (66a, roadmap item 33): the screen as it is now (the firmware's
 * mode), every graphics device on PCI, and with a driver its outputs (vga0,
 * hdmi0...), the monitor on each (its EDID: name, native mode, the modes it
 * lists) and which one shows the picture. The kernel's report
 * (REPORT_DISPLAYS), as cpus and net print theirs.
 *
 * (66b-2) `displays set OUTPUT WxH[@HZ]` shows that output's monitor in
 * another mode it lists (DISPLAY_SET_MODE): root or the user at the
 * machine's screen; windowd follows (66b-5). Without @HZ, the
 * monitor's usual rate for that size.
 *
 * (37g-5c-2) `displays sleep`: what the graphics drivers kept over the last
 * sleep, the registers before it and after it (REPORT_DISPLAY_SLEEP).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("displays [set OUTPUT WxH[@HZ] | sleep]",
             "list the screen, graphics devices, outputs and monitors; set a mode; what the "
             "last sleep changed");

static char text[64 * 1024];                    /* (the kernel's most for `displays sleep`) */

/* "1920x1080" or "1920x1080@60": false if it is neither. */
static int parse_mode(const char *s, uint32_t *w, uint32_t *h, uint32_t *hz)
{
    char *end;
    unsigned long a = strtoul(s, &end, 10);
    if (end == s || *end != 'x')
        return 0;
    const char *b = end + 1;
    unsigned long c = strtoul(b, &end, 10);
    if (end == b || a == 0 || c == 0 || a > 16384 || c > 16384)
        return 0;
    unsigned long r = 0;
    if (*end == '@') {
        const char *f = end + 1;
        r = strtoul(f, &end, 10);
        if (end == f || r == 0 || r > 1000)
            return 0;
    }
    if (*end)
        return 0;
    *w = (uint32_t)a, *h = (uint32_t)c, *hz = (uint32_t)r;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "sleep"))) {
        int64_t n = ritteros_report(argc == 1 ? REPORT_DISPLAYS : REPORT_DISPLAY_SLEEP, text,
                                    sizeof(text));
        if (n < 0)
            return 1;
        write(STDOUT_FILENO, text, n < (int64_t)sizeof(text) ? (size_t)n : sizeof(text) - 1);
        return 0;
    }
    uint32_t w, h, hz;
    if (argc != 4 || strcmp(argv[1], "set") || !parse_mode(argv[3], &w, &h, &hz)) {
        fprintf(stderr, "usage: displays [set OUTPUT WxH[@HZ] | sleep]\n");
        return 2;
    }
    char said[128];
    if (ritteros_display_set_mode(argv[2], w, h, hz * 1000, said, sizeof(said)) < 0) {
        if (said[0])
            fprintf(stderr, "displays: %s\n", said);
        else
            fprintf(stderr, "displays: %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    printf("%s\n", said);
    return 0;
}
