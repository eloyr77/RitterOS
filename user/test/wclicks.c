/*
 * Test: where clicks land (15q, steady clicks). Opens a 300x200 window and
 * prints each press and release of the left button with its place in the
 * window, until COUNT presses came (default 3) or the window is closed.
 */
#include <stdio.h>
#include <stdlib.h>
#include "window.h"

int main(int argc, char **argv)
{
    int want = argc > 1 ? atoi(argv[1]) : 3;
    struct window *win = window_open(300, 200, "wclicks");
    if (!win) {
        printf("wclicks: cannot open a window\n");
        return 1;
    }
    window_fill(win, 0, 0, 300, 200, window_rgb(win, 0x40, 0x60, 0x40));
    window_flush(win);
    printf("wclicks: ready\n");
    struct window_event ev;
    unsigned held = 0;
    int presses = 0;
    while (window_wait(win, &ev)) {
        if (ev.type != WINDOW_POINTER)
            continue;
        if ((ev.buttons & 1) && !(held & 1)) {
            printf("wclicks: press %d,%d\n", ev.x, ev.y);
            presses++;
        } else if (!(ev.buttons & 1) && (held & 1)) {
            printf("wclicks: release %d,%d\n", ev.x, ev.y);
            if (presses >= want)
                break;
        }
        held = ev.buttons;
    }
    window_close(win);
    printf("wclicks: done\n");
    return 0;
}
