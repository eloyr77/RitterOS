/*
 * Test app for libwindow (step 15f): open a titled window and draw with the
 * toolkit -- a colored background, a filled box, and a line of text. It then
 * holds the window so the test can screenshot it. Proves window_open, the
 * title (drawn by windowd with the shared font), window_fill and window_text.
 *
 * (68s) `wtext rect`: a 300x120 window drawn blue and shown, then drawn red
 * all over but shown only through window_flush_rect: a rectangle inside, one
 * over the top-left corner, one over the bottom-right, one wholly outside
 * ("rect shown"), each in a frame of its own (windowd joins one frame's
 * rectangles into the box around them). Once /tmp/wtext-go exists the whole window is shown with
 * window_flush ("whole shown"). The test's shots find red only where a
 * rectangle was, then everywhere.
 *
 * (69b) `wtext settings`: a small window takes window_color's background
 * and the text height of the user's text size, says "wtext: settings
 * ready", and once /tmp/wtext-go exists (the test changed the settings
 * meanwhile) asks windowd to read them again (window_settings_reload) and
 * waits for WINDOW_SETTINGS (5 s at most); it prints both before and after
 * ("wtext: settings: before 2E3440 19, after ECEFF4 24").
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "wfont.h"
#include "window.h"

static int settings_mode(void)
{
    struct window *w = window_open(120, 40, "settings");
    if (!w) {
        printf("wtext: cannot open a window\n");
        return 1;
    }
    const struct font *f = wfont_open("FreeSans.ttf", 0);
    uint32_t back = window_color(WINDOW_COLOR_BACKGROUND);
    int height = f ? wfont_height(f) : 0;
    printf("wtext: settings ready\n");
    while (access("/tmp/wtext-go", F_OK) != 0)
        usleep(50000);
    window_settings_reload(w);
    struct window_event ev;
    int got = 0;
    while (!got && window_wait_timeout(w, &ev, 5000) > 0)
        got = ev.type == WINDOW_SETTINGS;
    f = wfont_open("FreeSans.ttf", 0);
    printf("wtext: settings: before %06X %d, after %06X %d%s\n", (unsigned)back, height,
           (unsigned)window_color(WINDOW_COLOR_BACKGROUND), f ? wfont_height(f) : 0,
           got ? "" : " (no WINDOW_SETTINGS)");
    window_close(w);
    return got ? 0 : 1;
}

static int rect_mode(void)
{
    struct window *w = window_open(300, 120, "rect");
    if (!w) {
        printf("wtext: cannot open a window\n");
        return 1;
    }
    window_fill(w, 0, 0, 300, 120, window_rgb(w, 0, 0, 200));
    window_flush(w);
    sleep(1);
    window_fill(w, 0, 0, 300, 120, window_rgb(w, 200, 0, 0));
    window_flush_rect(w, 20, 30, 40, 20);       /* inside: 20-59 x 30-49 */
    usleep(300000);                             /* (a frame each: one frame's are joined) */
    window_flush_rect(w, -10, -10, 25, 25);     /* the corner's part: 0-14 x 0-14 */
    usleep(300000);                             /* (a frame each: one frame's are joined) */
    window_flush_rect(w, 280, 100, 50, 50);     /* to the window's edge: 280-299 x 100-119 */
    usleep(300000);                             /* (a frame each: one frame's are joined) */
    window_flush_rect(w, 300, 0, 10, 10);       /* outside it: nothing */
    printf("wtext: rect shown\n");
    while (access("/tmp/wtext-go", F_OK) != 0)
        usleep(50000);
    window_flush(w);
    printf("wtext: whole shown\n");
    struct window_event ev;
    while (window_wait(w, &ev))
        ;
    window_close(w);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "rect"))
        return rect_mode();
    if (argc > 1 && !strcmp(argv[1], "settings"))
        return settings_mode();
    struct window *w = window_open(300, 120, "RitterOS");
    if (!w) {
        printf("wtext: cannot open a window\n");
        return 1;
    }
    uint32_t blue = window_rgb(w, 0, 0, 200);
    uint32_t green = window_rgb(w, 0, 200, 0);
    uint32_t white = window_rgb(w, 255, 255, 255);
    window_fill(w, 0, 0, 300, 120, blue);               /* background */
    window_fill(w, 20, 40, 60, 40, green);              /* a box */
    window_text(w, 10, 10, "Hello", white, WINDOW_TRANSPARENT);
    window_flush(w);
    printf("wtext: drawn\n");
    struct window_event ev;
    while (window_wait(w, &ev))                          /* hold the window open */
        ;
    window_close(w);
    return 0;
}
