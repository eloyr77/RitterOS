/*
 * Test client for the menu system (step 19a). It opens a window with a menu
 * bar (one menu, "File", with "Open" = id 1 and "Quit" = id 2) and prints the
 * id of each menu item the user chooses. The test clicks the menu and an item
 * and checks the printed id.
 */
#include <stdio.h>
#include <unistd.h>
#include "window.h"

int main(void)
{
    struct window *win = window_open(200, 150, "Menus");
    if (!win) {
        printf("wmenu: cannot open a window (is windowd running?)\n");
        return 1;
    }
    window_fill(win, 0, 0, 200, 150, window_rgb(win, 40, 44, 52));
    window_flush(win);

    static const struct window_menu_item file_items[] = { { 1, "Open" }, { 2, "Quit" } };
    static const struct window_menu menus[] = { { "File", file_items, 2 } };
    window_set_menu(win, menus, 1);
    printf("wmenu: window created\n");

    static const struct window_menu_item ctx[] = { { 3, "Copy" }, { 4, "Paste" } };
    struct window_event ev;
    unsigned prev = 0;
    while (window_wait(win, &ev)) {
        if (ev.type == WINDOW_MENU) {
            printf("wmenu: menu %d\n", ev.menu);
            if (ev.menu == 2)               /* Quit */
                break;
        } else if (ev.type == WINDOW_POINTER) {
            if ((ev.buttons & 2) && !(prev & 2))    /* right button pressed: a context menu */
                window_popup_menu(win, ev.x, ev.y, ctx, 2);
            prev = ev.buttons;
        }
    }
    window_close(win);
    return 0;
}
