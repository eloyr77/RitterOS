/*
 * Test: the text field and the clipboard (15w).
 *   wfield         a window with a field at (10, 10), 380 pixels wide; Enter
 *                  prints "wfield: enter 'TEXT'" and ends
 *   wfield paste   prints what the clipboard holds and ends (another program
 *                  than the one that copied: the clipboard is windowd's)
 */
#include <stdio.h>
#include <string.h>
#include "window.h"

int main(int argc, char **argv)
{
    struct window *win = window_open(400, 60, "wfield");
    if (!win) {
        printf("wfield: cannot open a window\n");
        return 1;
    }
    window_fill(win, 0, 0, 400, 60, window_rgb(win, 0xE5, 0xE9, 0xF0));
    if (argc > 1 && !strcmp(argv[1], "paste")) {
        static char text[1024];
        int n = window_clipboard_get(win, text, sizeof(text));
        printf("wfield: clipboard '%s' (%d bytes)\n", n >= 0 ? text : "?", n);
        window_close(win);
        return 0;
    }
    struct window_field *f = window_field_new(win, 10, 10, 380);
    window_field_draw(f);
    window_flush(win);
    printf("wfield: ready\n");
    struct window_event ev;
    while (window_wait(win, &ev))
        if (window_field_event(f, &ev) == 1) {
            printf("wfield: enter '%s'\n", window_field_text(f));
            break;
        }
    window_field_free(f);
    window_close(win);
    return 0;
}
