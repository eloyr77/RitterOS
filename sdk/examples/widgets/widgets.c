/*
 * widgets: a small settings window made of libwidget's controls (41e). The
 * window is a column: a label, two check boxes, a row of radio buttons and
 * a row of buttons at the bottom (a stretching space above them pushes them
 * down). Each change prints a line; Apply prints the choices, Close ends
 * the run. Tab and Shift+Tab move between the controls, Space and Enter
 * press the one with the keyboard.
 */
#include <stdio.h>
#include "widget.h"
#include "window.h"

struct form {
    struct widget *root, *sound, *updates, *small, *large;
};

static void changed(struct widget *w, void *data)
{
    (void)data;
    printf("widgets: %s is now %s\n", widget_text(w), widget_value(w) ? "on" : "off");
}

static void apply(struct widget *w, void *data)
{
    struct form *f = data;
    (void)w;
    printf("widgets: sound %d, updates %d, text %s\n", widget_value(f->sound), widget_value(f->updates),
           widget_value(f->large) ? "large" : "small");
}

static void close_window(struct widget *w, void *data)
{
    (void)w;
    widget_quit(((struct form *)data)->root);
}

int main(void)
{
    struct window *win = window_open(320, 230, "widgets");
    if (!win) {
        printf("widgets: cannot open a window (is windowd running?)\n");
        return 1;
    }
    struct form f;
    f.root = widget_column();
    f.sound = widget_check("Play sounds", 1);
    f.updates = widget_check("Look for updates", 0);
    f.small = widget_radio("Small text", 0, 1);
    f.large = widget_radio("Large text", f.small, 0);
    struct widget *sizes = widget_row(), *buttons = widget_row(), *space = widget_column();
    struct widget *ok = widget_button("Apply"), *close = widget_button("Close");

    widget_add(f.root, widget_label("Choose how this program behaves."));
    widget_add(f.root, f.sound);
    widget_add(f.root, f.updates);
    widget_add(sizes, f.small);
    widget_add(sizes, f.large);
    widget_add(f.root, sizes);
    widget_add(f.root, space);                  /* (an empty column takes the spare height) */
    widget_stretch(space, 1);
    widget_add(buttons, ok);
    widget_add(buttons, close);
    widget_add(f.root, buttons);

    widget_on(f.sound, changed, 0);
    widget_on(f.updates, changed, 0);
    widget_on(f.large, changed, 0);
    widget_on(f.small, changed, 0);
    widget_on(ok, apply, &f);
    widget_on(close, close_window, &f);
    printf("widgets: started\n");
    widget_run(win, f.root);                    /* (until Close, or the window's X) */
    widget_free(f.root);
    window_close(win);
    return 0;
}
