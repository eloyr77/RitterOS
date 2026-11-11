/*
 * hellowin: the starting point of a windowed RitterOS program. It opens a
 * window, draws into it, and answers the keyboard, clicks and resizes. What
 * happens is also printed on its standard output, which `make run` brings
 * back to the computer it was started from.
 */
#include <stdio.h>
#include "ritteros.h"
#include "window.h"

static struct window *win;
static uint32_t bg, fg, accent;
static int keys, clicks;

static void draw(void)
{
    int w = window_width(win), h = window_height(win);
    char line[80];
    window_fill(win, 0, 0, w, h, bg);
    window_text(win, 16, 16, "Hello from a RitterOS window!", fg, WINDOW_TRANSPARENT);
    window_fill(win, 16, 40, w > 32 ? w - 32 : 0, 3, accent);
    snprintf(line, sizeof(line), "%d keys, %d clicks, %dx%d", keys, clicks, w, h);
    window_text(win, 16, 56, line, fg, WINDOW_TRANSPARENT);
    window_flush(win);
}

int main(void)
{
    win = window_open(360, 100, "hellowin");
    if (!win) {
        printf("hellowin: cannot open a window (is windowd running?)\n");
        return 1;
    }
    bg = window_rgb(win, 0x2B, 0x3A, 0x55);
    fg = window_rgb(win, 0xEC, 0xEF, 0xF4);
    accent = window_rgb(win, 0x88, 0xC0, 0xD0);
    draw();
    printf("hellowin: started\n");

    struct window_event ev;
    unsigned buttons = 0;
    while (window_wait(win, &ev) && ev.type != WINDOW_CLOSED) {
        if (ev.type == WINDOW_KEY) {
            keys++;
            printf("hellowin: key %d\n", ev.key);
            draw();
        } else if (ev.type == WINDOW_POINTER) {
            if ((ev.buttons & MOUSE_LEFT) && !(buttons & MOUSE_LEFT)) {
                clicks++;                               /* a press, not a held button */
                printf("hellowin: click at %d,%d\n", ev.x, ev.y);
                draw();
            }
            buttons = ev.buttons;
        } else if (ev.type == WINDOW_RESIZE) {
            if (window_resize(win, ev.x, ev.y) == 0)    /* the new pixels are blank */
                draw();
        }
    }
    window_close(win);
    printf("hellowin: closed\n");
    return 0;
}
