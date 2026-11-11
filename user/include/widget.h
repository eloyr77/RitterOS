/*
 * libwidget (41e): controls for windowed programs, the same in every one.
 * A program builds a tree of widgets -- rows, columns and grids holding
 * labels, buttons, check boxes, radio buttons... -- and hands it with its
 * window to widget_run, which lays it out to the window (again after every
 * resize), draws it (libdraw on the window's own pixels, FreeSans at the
 * user's text_size), and runs the window's events: a click or Space/Enter
 * presses the control with the keyboard, Tab and Shift+Tab move the
 * keyboard between controls, and each change calls the function given with
 * widget_on. Strings are copied. Not thread-safe: one thread builds and
 * runs a tree (its callbacks may change it).
 */
#ifndef RITTEROS_WIDGET_H
#define RITTEROS_WIDGET_H

struct widget;
struct window;

/* Layouts. A row puts its children left to right, a column top to bottom,
 * a grid in `columns` columns (row by row), each child at the size it
 * needs and 8 pixels apart (the window's outermost one 8 from its edges);
 * a row's or column's spare length goes to the children with a stretch
 * weight (widget_stretch), shared by their weights. Children are added in
 * order with widget_add (to a row, column, grid or tabs; others ignore it). */
struct widget *widget_row(void);
struct widget *widget_column(void);
struct widget *widget_grid(int columns);

/* (41e-2) Tabs: its children are pages, one shown at a time below a row
 * of titles (each page's text, widget_set_text); its value is the open
 * page (0 the first), changed by a click on a title or by Left/Right
 * while it has the keyboard; it is as large as its largest page. */
struct widget *widget_tabs(void);
void widget_add(struct widget *parent, struct widget *child);
void widget_stretch(struct widget *w, int weight);

/* A label: text (wrapped at 400 pixels, and at newlines); a button: text
 * on a face, pressed by a click or by Space/Enter; a check box (value 0
 * or 1); a radio button (value 1 when it is its group's chosen one): `group`
 * 0 starts a new group, else the new one joins group's (choosing one
 * clears the others; on 1 makes it the chosen one). 0 without memory. */
struct widget *widget_label(const char *utf8);
struct widget *widget_button(const char *utf8);
struct widget *widget_check(const char *utf8, int on);
struct widget *widget_radio(const char *utf8, struct widget *group, int on);

/* (41e-2) A slider: a value from min to max, dragged with the pointer or
 * moved by the arrow keys (Page Up/Down a tenth, Home/End the ends); a
 * progress bar: value of max shown filled (no input). */
struct widget *widget_slider(int min, int max, int value);
struct widget *widget_progress(int max, int value);

/* (41e-3) Rows to choose from, given as text: a list, a line a row; a
 * table of `columns` columns, a line a row and a tab between cells, its
 * first line the header; a tree, a line a node, its leading tabs its level
 * (a node with deeper lines after it is a branch, closed at first: its
 * triangle, Right/Left or Space/Enter open and close it). widget_value is
 * the chosen row (a table's counted under its header, a tree's line
 * among all of them; -1: none) and widget_text its text (a table's with
 * its tabs); widget_set_text gives new rows (none chosen). A click or
 * Up/Down, Page Up/Down, Home/End choose; the wheel and the scroll bar
 * scroll; the row under the pointer is highlighted. */
struct widget *widget_list(const char *lines);
struct widget *widget_table(int columns, const char *rows);
struct widget *widget_tree(const char *lines);

/* (41e-4) An editor: text over `lines` lines (wrapped at its width,
 * scrolled with a scroll bar and the wheel), or with 1 a one-line entry
 * (scrolled sideways; newlines become spaces). Typing (WINDOW_TEXT) and
 * the arrows, Home/End (Ctrl: the text's ends), Page Up/Down, Backspace,
 * Delete and Enter edit; Shift and a drag select, a double click selects
 * a word; Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V go through windowd's clipboard;
 * the pointer is an I-beam over it, its cursor blinks. widget_text is its
 * text, widget_value its cursor (a byte of the text); widget_set_text puts
 * the cursor at the end; its function is called at every change. */
struct widget *widget_editor(const char *utf8, int lines);

/* (41e-5) Dialogs over a program's window (window_dialog_open: the window
 * takes no input meanwhile), sized to their text, run until answered (they
 * may be called from a callback): a message with an OK button; a question
 * with the buttons yes and no (0: "Yes", "No"), 1 for yes, 0 for no,
 * Escape or the dialog closed. The first button has the keyboard (Enter
 * presses it); Escape presses the last. */
void widget_message(struct window *parent, const char *title, const char *text);
int widget_question(struct window *parent, const char *title, const char *text, const char *yes, const char *no);

/* What to call when a widget is used: a button pressed, a check box
 * changed, a radio button chosen, a slider moved, another tab opened,
 * another row chosen, an editor's text changed (fn(w, data)). */
void widget_on(struct widget *w, void (*fn)(struct widget *w, void *data), void *data);

/* A widget's value (a check box's 0/1, a radio button's: chosen or not,
 * a slider's, a progress bar's, the open tab; kept within their range)
 * and text, read and changed (a change is drawn; it calls no function). */
int widget_value(const struct widget *w);
void widget_set_value(struct widget *w, int value);
const char *widget_text(const struct widget *w);
void widget_set_text(struct widget *w, const char *utf8);

/* Run the window's events for the tree root until the window is closed (0)
 * or widget_quit(root) is called, from a callback (1). The window stays
 * open; the tree stays the program's (widget_free gives back all of it).
 * (41f-2) The window cannot be resized below what the tree needs, and one
 * opened smaller grows to it. */
int widget_run(struct window *win, struct widget *root);
void widget_quit(struct widget *root);
void widget_free(struct widget *root);

#endif
