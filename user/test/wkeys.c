/*
 * Test: physical keys in a window (15p). Opens a window and prints each
 * WINDOW_KEY_DOWN and WINDOW_KEY_UP it gets (the key's USB HID usage ID), and
 * the WINDOW_KEY bytes beside them, until COUNT key events came (default 8)
 * or the window is closed. The test presses keys through QEMU and checks the
 * order: modifiers included, releases as well as presses.
 */
#include <stdio.h>
#include <stdlib.h>
#include "ritteros.h"
#include "window.h"

int main(int argc, char **argv)
{
    int want = argc > 1 ? atoi(argv[1]) : 8;
    struct window *win = window_open(200, 100, "wkeys");
    if (!win) {
        printf("wkeys: cannot open a window\n");
        return 1;
    }
    window_fill(win, 0, 0, 200, 100, window_rgb(win, 0x30, 0x30, 0x60));
    window_flush(win);
    printf("wkeys: ready\n");
    struct window_event ev;
    int seen = 0;
    while (seen < want && window_wait(win, &ev)) {
        if (ev.type == WINDOW_KEY_DOWN || ev.type == WINDOW_KEY_UP) {
            printf("wkeys: %s 0x%02x\n", ev.type == WINDOW_KEY_DOWN ? "down" : "up", ev.key);
            seen++;
        } else if (ev.type == WINDOW_KEY) {
            printf("wkeys: byte 0x%02x\n", ev.key);
        }
    }
    window_close(win);
    printf("wkeys: done\n");
    return 0;
}
