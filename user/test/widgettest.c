/*
 * Test: libwidget (41e-1) and focus events (41f-1). With no argument a
 * window holds a column: a label, check boxes Wi-Fi (on) and Bluetooth, a
 * row of radio buttons Small (chosen) and Large, and a button Big that
 * stretches to the column's end; each callback prints what changed, Big
 * ends the run (widget_quit), then the values are printed. The test drives
 * it with Tab, Shift+Tab, Space and Enter through QEMU, clicks Big, and
 * looks at screenshots. `widgettest focus` opens a plain window and prints
 * its WINDOW_FOCUS events (three, or 30 seconds). (41e-2) `widgettest more`:
 * a 200x100 window (too small: it grows) with tabs Sound (a slider 0-100 at
 * 30) and Disk (a progress bar 4 of 10, a button Fill that fills it) and a
 * button Done; each change printed, then the values and whether it grew.
 * (41f-2) `widgettest limits`: a plain 300x200 window kept between 250x150
 * and 400x300 (window_set_limits) prints the sizes windowd gives it.
 * (41e-3) `widgettest rows`: a list of 20 items and a tree beside it, a
 * table under them, a button Done; each row chosen printed (tabs as |),
 * then the values. (41f-3) `widgettest hover`: a plain window prints its
 * WINDOW_ENTER and WINDOW_LEAVE events (four, or 10 seconds). (41e-4)
 * `widgettest editor`: a five-line editor, a one-line entry, Done; their
 * texts (newlines as |) and the entry's cursor printed at the end.
 * (41f-4) `widgettest text`: a plain window prints its WINDOW_TEXT
 * characters (three, or 10 seconds). (41e-5) `widgettest dialog`: a window
 * with buttons Ask (a question: Delete or Cancel), Tell (a message) and
 * Quit; each answer printed. (41f-6) `widgettest screen`: a plain window
 * that the typed f takes to full screen and back, m minimises and q ends;
 * each size it is given and its focus printed. `widgettest drop`: a window
 * at full screen prints the first drag let go on it (where, and its text).
 */
#include <stdio.h>
#include <string.h>
#include "widget.h"
#include "window.h"

static void changed(struct widget *w, void *data)
{
    (void)data;
    printf("widgettest: %s %d\n", widget_text(w), widget_value(w));
}

static void big(struct widget *w, void *root)
{
    printf("widgettest: %s pressed\n", widget_text(w));
    widget_quit(root);
}

static int focus_events(void)
{
    struct window *win = window_open(300, 200, "focus");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    window_fill(win, 0, 0, 300, 200, window_rgb(win, 0x30, 0x30, 0x60));
    window_flush(win);
    struct window_event ev;
    int seen = 0;
    for (int waited = 0; seen < 3 && waited < 30000; waited += 100) {
        int r = window_wait_timeout(win, &ev, 100);
        if (r < 0)
            break;
        if (r == 1 && ev.type == WINDOW_FOCUS) {
            printf("widgettest: focus %d\n", ev.key);
            seen++;
        }
    }
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}

static void moved(struct widget *w, void *name)
{
    printf("widgettest: %s %d\n", (const char *)name, widget_value(w));
}

static void fill(struct widget *w, void *bar)
{
    (void)w;
    widget_set_value(bar, 10);
    printf("widgettest: progress %d\n", widget_value(bar));
}

static int more(void)
{
    struct window *win = window_open(200, 100, "more");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    struct widget *root = widget_column(), *tabs = widget_tabs(), *sound = widget_column(), *disk = widget_column();
    struct widget *slider = widget_slider(0, 100, 30), *bar = widget_progress(10, 4);
    struct widget *fillb = widget_button("Fill"), *done = widget_button("Done");
    widget_set_text(sound, "Sound");
    widget_set_text(disk, "Disk");
    widget_add(sound, widget_label("Volume"));
    widget_add(sound, slider);
    widget_add(disk, bar);
    widget_add(disk, fillb);
    widget_add(tabs, sound);
    widget_add(tabs, disk);
    widget_add(root, tabs);
    widget_add(root, done);
    widget_on(slider, moved, "slider");
    widget_on(tabs, moved, "tab");
    widget_on(fillb, fill, bar);
    widget_on(done, big, root);
    printf("widgettest: ready\n");
    int r = widget_run(win, root);
    printf("widgettest: run %d, slider %d, tab %d, progress %d, grew %d\n", r, widget_value(slider),
           widget_value(tabs), widget_value(bar), window_width(win) > 200 || window_height(win) > 100);
    widget_free(root);
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}

static int limits(void)
{
    struct window *win = window_open(300, 200, "limits");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    window_set_limits(win, 250, 150, 400, 300);
    window_fill(win, 0, 0, 300, 200, window_rgb(win, 0x30, 0x60, 0x30));
    window_flush(win);
    printf("widgettest: limits ready\n");
    struct window_event ev;
    int seen = 0;
    for (int waited = 0; seen < 2 && waited < 30000; waited += 100) {
        int r = window_wait_timeout(win, &ev, 100);
        if (r < 0)
            break;
        if (r == 1 && ev.type == WINDOW_RESIZE) {
            printf("widgettest: resize %d x %d\n", ev.x, ev.y);
            window_resize(win, ev.x, ev.y);
            window_fill(win, 0, 0, ev.x, ev.y, window_rgb(win, 0x30, 0x60, 0x30));
            window_flush(win);
            seen++;
        }
    }
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}

static void row_chosen(struct widget *w, void *name)
{
    char text[128];
    snprintf(text, sizeof(text), "%s", widget_text(w));
    for (char *c = text; *c; c++)
        if (*c == '\t')
            *c = '|';
    printf("widgettest: %s %d %s\n", (const char *)name, widget_value(w), text);
}

static int rows(void)
{
    struct window *win = window_open(400, 300, "rows");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    char items[512] = "";
    for (int i = 0; i < 20; i++)
        snprintf(items + strlen(items), sizeof(items) - strlen(items), "%sItem %d", i ? "\n" : "", i);
    struct widget *root = widget_column(), *pair = widget_row();
    struct widget *list = widget_list(items);
    struct widget *tree = widget_tree("Fruit\n\tApples\n\tPears\nVegetables\n\tCarrots\n\t\tOrange carrots");
    struct widget *table = widget_table(3, "Name\tSize\tKind\napple\t3\tfruit\npear\t5\tfruit\nleek\t9\tvegetable");
    struct widget *done = widget_button("Done");
    widget_add(pair, list);
    widget_add(pair, tree);
    widget_add(root, pair);
    widget_add(root, table);
    widget_add(root, done);
    widget_on(list, row_chosen, "list");
    widget_on(tree, row_chosen, "tree");
    widget_on(table, row_chosen, "table");
    widget_on(done, big, root);
    printf("widgettest: ready\n");
    int r = widget_run(win, root);
    printf("widgettest: run %d, list %d, tree %d, table %d\n", r, widget_value(list), widget_value(tree),
           widget_value(table));
    widget_free(root);
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}

static int hover_events(void)
{
    struct window *win = window_open(300, 200, "hover");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    window_fill(win, 0, 0, 300, 200, window_rgb(win, 0x30, 0x30, 0x60));
    window_flush(win);
    printf("widgettest: hover ready\n");
    struct window_event ev;
    int seen = 0;
    for (int waited = 0; seen < 4 && waited < 10000; waited += 100) {
        int r = window_wait_timeout(win, &ev, 100);
        if (r < 0)
            break;
        if (r == 1 && (ev.type == WINDOW_ENTER || ev.type == WINDOW_LEAVE)) {
            printf("widgettest: %s\n", ev.type == WINDOW_ENTER ? "enter" : "leave");
            seen++;
        }
    }
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}

static int editors(void)
{
    struct window *win = window_open(360, 240, "editor");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    struct widget *root = widget_column(), *ed = widget_editor("", 5), *entry = widget_editor("", 1);
    struct widget *done = widget_button("Done");
    widget_add(root, ed);
    widget_add(root, entry);
    widget_add(root, done);
    widget_on(done, big, root);
    printf("widgettest: ready\n");
    int r = widget_run(win, root);
    char text[256];
    snprintf(text, sizeof(text), "%s", widget_text(ed));
    for (char *c = text; *c; c++)
        if (*c == '\n')
            *c = '|';
    printf("widgettest: run %d, editor %s, entry %s, cursor %d\n", r, text, widget_text(entry), widget_value(entry));
    widget_free(root);
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}

static int text_events(void)
{
    struct window *win = window_open(300, 200, "text");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    window_fill(win, 0, 0, 300, 200, window_rgb(win, 0x30, 0x30, 0x60));
    window_flush(win);
    printf("widgettest: text ready\n");
    struct window_event ev;
    int seen = 0;
    for (int waited = 0; seen < 3 && waited < 10000; waited += 100) {
        int r = window_wait_timeout(win, &ev, 100);
        if (r < 0)
            break;
        if (r == 1 && ev.type == WINDOW_TEXT) {
            printf("widgettest: text %x\n", ev.key);
            seen++;
        }
    }
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}

static void ask(struct widget *w, void *win)
{
    (void)w;
    printf("widgettest: question %d\n", widget_question(win, "Delete", "Delete the file?", "Delete", "Cancel"));
}

static void tell(struct widget *w, void *win)
{
    (void)w;
    widget_message(win, "Done", "It is done.");
    printf("widgettest: message closed\n");
}

static int dialogs(void)
{
    struct window *win = window_open(300, 300, "dialogs");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    struct widget *root = widget_column(), *row = widget_row();
    struct widget *a = widget_button("Ask"), *t = widget_button("Tell"), *q = widget_button("Quit");
    widget_add(row, a);
    widget_add(row, t);
    widget_add(row, q);
    widget_add(root, row);
    widget_on(a, ask, win);
    widget_on(t, tell, win);
    widget_on(q, big, root);
    printf("widgettest: ready\n");
    int r = widget_run(win, root);
    printf("widgettest: run %d\n", r);
    widget_free(root);
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}

static void paint(struct window *win, int w, int h)
{
    window_resize(win, w, h);
    window_fill(win, 0, 0, w, h, window_rgb(win, 0x30, 0x60, 0x30));
    window_flush(win);
}

static int screen_modes(void)
{
    struct window *win = window_open(300, 200, "screen");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    paint(win, 300, 200);
    printf("widgettest: screen ready\n");
    struct window_event ev;
    int full = 0;
    while (window_wait(win, &ev)) {
        if (ev.type == WINDOW_TEXT && ev.key == 'f')
            window_fullscreen(win, full = !full);
        else if (ev.type == WINDOW_TEXT && ev.key == 'm')
            window_minimize(win);
        else if (ev.type == WINDOW_TEXT && ev.key == 'q')
            break;
        else if (ev.type == WINDOW_RESIZE) {
            paint(win, ev.x, ev.y);
            printf("widgettest: size %d x %d\n", ev.x, ev.y);
        } else if (ev.type == WINDOW_FOCUS)
            printf("widgettest: screen focus %d\n", ev.key);
    }
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}

static int drops(void)
{
    struct window *win = window_open(300, 200, "drop");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    paint(win, 300, 200);
    window_fullscreen(win, 1);
    struct window_event ev;
    for (int waited = 0; waited < 30000; waited += 100) {
        int r = window_wait_timeout(win, &ev, 100);
        if (r < 0)
            break;
        if (r == 1 && ev.type == WINDOW_RESIZE) {
            paint(win, ev.x, ev.y);
            printf("widgettest: drop ready\n");
        } else if (r == 1 && ev.type == WINDOW_DROP) {
            printf("widgettest: drop %d %d %s\n", ev.x, ev.y, window_drop_text(win));
            break;
        }
    }
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}

/* (41g-4) A window's notification: a title and a text long enough to
 * wrap in the box; the window stays until q (or 30 s). */
static int notifies(void)
{
    struct window *win = window_open(300, 200, "notify");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    paint(win, 300, 200);
    int r = window_notify(win, "From a window",
                          "widgettest says hello with a text that wraps onto a second line");
    printf("widgettest: notified %d\n", r);
    struct window_event ev;
    for (int waited = 0; waited < 30000; waited += 100) {
        int got = window_wait_timeout(win, &ev, 100);
        if (got < 0 || (got == 1 && ev.type == WINDOW_TEXT && ev.key == 'q'))
            break;
    }
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "notify"))
        return notifies();
    if (argc > 1 && !strcmp(argv[1], "screen"))
        return screen_modes();
    if (argc > 1 && !strcmp(argv[1], "drop"))
        return drops();
    if (argc > 1 && !strcmp(argv[1], "dialog"))
        return dialogs();
    if (argc > 1 && !strcmp(argv[1], "editor"))
        return editors();
    if (argc > 1 && !strcmp(argv[1], "text"))
        return text_events();
    if (argc > 1 && !strcmp(argv[1], "rows"))
        return rows();
    if (argc > 1 && !strcmp(argv[1], "hover"))
        return hover_events();
    if (argc > 1 && !strcmp(argv[1], "more"))
        return more();
    if (argc > 1 && !strcmp(argv[1], "limits"))
        return limits();
    if (argc > 1 && !strcmp(argv[1], "focus"))
        return focus_events();
    struct window *win = window_open(300, 240, "widgettest");
    if (!win) {
        printf("widgettest: cannot open a window\n");
        return 1;
    }
    struct widget *root = widget_column(), *row = widget_row();
    struct widget *wifi = widget_check("Wi-Fi", 1), *bt = widget_check("Bluetooth", 0);
    struct widget *small = widget_radio("Small", 0, 1), *large = widget_radio("Large", small, 0);
    struct widget *b = widget_button("Big");
    widget_add(root, widget_label("Settings"));
    widget_add(root, wifi);
    widget_add(root, bt);
    widget_add(row, small);
    widget_add(row, large);
    widget_add(root, row);
    widget_add(root, b);
    widget_stretch(b, 1);
    widget_on(wifi, changed, 0);
    widget_on(bt, changed, 0);
    widget_on(small, changed, 0);
    widget_on(large, changed, 0);
    widget_on(b, big, root);
    printf("widgettest: ready\n");
    int r = widget_run(win, root);
    printf("widgettest: run %d, Wi-Fi %d, Bluetooth %d, Small %d, Large %d\n", r, widget_value(wifi),
           widget_value(bt), widget_value(small), widget_value(large));
    widget_free(root);
    window_close(win);
    printf("widgettest: done\n");
    return 0;
}
