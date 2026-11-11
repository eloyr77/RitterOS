/*
 * notes: a note kept in ~/notes.txt, a whole windowed RitterOS app (41h-4)
 * using the window system's parts together: libwidget's controls (an
 * editor and buttons), a question dialog before clearing, a notification
 * when saved, the user's colours (libwidget takes them), and an app file
 * (notes.info, installed as its bundle's info) for the desktop menu. test.sh beside it tests it without a
 * screen: a headless windowd, input from wsend, a picture from wshot.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "widget.h"
#include "window.h"

#define NOTE_MAX 4096

struct notes {
    struct window *win;
    struct widget *root, *editor;
    char path[256];
};

static void save(struct widget *w, void *data)
{
    struct notes *n = data;
    (void)w;
    const char *text = widget_text(n->editor);
    FILE *f = fopen(n->path, "w");
    if (!f || fputs(text, f) < 0 || fclose(f) != 0) {
        widget_message(n->win, "Not saved", strerror(errno));
        return;
    }
    char line[64];
    snprintf(line, sizeof(line), "%zu bytes in ~/notes.txt", strlen(text));
    window_notify(n->win, "Saved", line);          /* (a box at the top right for 5 s) */
    printf("notes: saved %zu bytes\n", strlen(text));
}

static void clear(struct widget *w, void *data)
{
    struct notes *n = data;
    (void)w;
    if (widget_question(n->win, "Clear", "Clear the note?", "Clear", "Keep")) {
        widget_set_text(n->editor, "");
        printf("notes: cleared\n");
    }
}

int main(void)
{
    static struct notes n;
    const char *home = getenv("HOME");
    snprintf(n.path, sizeof(n.path), "%s/notes.txt", home ? home : "");
    static char text[NOTE_MAX];                     /* the note so far, if there is one */
    FILE *f = fopen(n.path, "r");
    if (f) {
        text[fread(text, 1, sizeof(text) - 1, f)] = 0;
        fclose(f);
    }
    n.win = window_open(360, 260, "notes");
    if (!n.win) {
        printf("notes: cannot open a window (is windowd running?)\n");
        return 1;
    }
    n.root = widget_column();
    n.editor = widget_editor(text, 6);
    struct widget *buttons = widget_row(), *s = widget_button("Save"), *c = widget_button("Clear");
    widget_add(n.root, widget_label("A note, kept in ~/notes.txt"));
    widget_add(n.root, n.editor);
    widget_stretch(n.editor, 1);                    /* (the editor takes the spare height) */
    widget_add(buttons, s);
    widget_add(buttons, c);
    widget_add(n.root, buttons);
    widget_on(s, save, &n);
    widget_on(c, clear, &n);
    printf("notes: started\n");
    widget_run(n.win, n.root);                      /* (until the window's X) */
    widget_free(n.root);
    window_close(n.win);
    return 0;
}
