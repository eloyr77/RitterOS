/*
 * notify TEXT...: a notification on the window system's screen (41g-4):
 * windowd shows the words, a space between them, in a box at the top
 * right for 5 seconds (window_notify without a window). It returns at
 * once; it needs windowd running.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "ritteros.h"
#include "window.h"
#include "windowproto.h"

PROGRAM_HELP("notify TEXT...", "show a notification on the window system's screen (needs windowd)");

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: notify TEXT...\n");
        return 2;
    }
    char text[WINDOW_NOTIFY_MAX];
    size_t n = 0;
    for (int i = 1; i < argc && n < sizeof(text) - 1; i++)
        n += (size_t)snprintf(text + n, sizeof(text) - n, i > 1 ? " %s" : "%s", argv[i]);
    if (window_notify(0, 0, text) < 0) {
        dprintf(2, "notify: no window server: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
