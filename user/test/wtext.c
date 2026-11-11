/*
 * Test app for libwindow (step 15f): open a titled window and draw with the
 * toolkit -- a colored background, a filled box, and a line of text. It then
 * holds the window so the test can screenshot it. Proves window_open, the
 * title (drawn by windowd with the shared font), window_fill and window_text.
 */
#include <stdio.h>
#include "window.h"

int main(void)
{
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
