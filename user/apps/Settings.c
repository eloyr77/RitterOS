/*
 * settings (69c, roadmap item 47, the user's): the system's and the user's
 * settings in a window, a page each, chosen from a list on the left
 * (libwidget's widget_pages beside a widget_list). Every page applies what
 * it saves at once where it can.
 *
 * (69d) The System pages, /system/config's, through the program that owns
 * each setting (its output under the page's button) or the file's lines
 * (settings_set) where its reader looks again by itself:
 *
 *   Display           displays' outputs; displays set OUTPUT MODE, and
 *                     the line in /system/config/displays
 *   Sound             volume (soundd's master volume and mute)
 *   Memory            swap: on, off, make SIZE
 *   Date & time       time's zone; date -s
 *   Elves             elf start, stop, restart NAME
 *   Service names     services' NAME = PORT lines
 *   Power             power's lines, then power settings
 *   Processor         cpu's clock, then power clock
 *   (69e) Network cards  a card's file in /system/config/network, then
 *                     net up (and elf restart dhclient for DHCP <-> fixed)
 *   Routing           net forward, masquerade, default
 *   Virtual cards     net tap add/remove, net bridge add/remove
 *   Card names        network/names' lines; net forget NAME
 *   Wi-Fi             wifi country; the radio's powersave, then elf
 *                     restart wifid; saved networks: wifi join, forget
 *   Firewall          guard's rules: guard add, remove N; in, out,
 *                     forward drop|allow
 *
 * They change the machine: root's (Sound: soundd's rule, root and the
 * console's user). (69f, the user's) Anyone else sees them read only until
 * Unlock: the user's own password, checked by `sudo -S true` (users in
 * /system/config/sudo), then kept while the app runs and every change made
 * through `sudo -S` with it: a program run so, a file's line by `settings
 * set FILE KEY [VALUE]` (this program's one-line writer as a command, for
 * files in /system/config only).
 *
 *   (69f) Users       add a user (useradd, passwd -S), set a user's
 *                     password, sudo's users and nopassword lines
 *
 * The User pages, the user's own files in ~/.config:
 *
 *   Appearance        theme, accent, text_size (~/.config/windows)
 *   Mouse & touchpad  double_click, click_steady (~/.config/windows)
 *   Window system     startup, refresh (~/.config/windows)
 *   File types        ENDING = PROGRAM lines (~/.config/filetypes)
 *   (69f) Account     your own password (passwd -S: setuid, asks the
 *                     current one)
 *   At login          ~/.config/login: Console or Window system, and the
 *                     items run at a login at the machine, in order
 *
 * The window settings are written a line at a time (settings_set, the
 * user's rule: a program changes only its lines), then windowd is asked to
 * read them again (window_settings_reload, 69b): the desktop and every open
 * window follow at once. ~/.config/login is a script: with Window system
 * its first lines are `windowd &` and `wsend wait` (the user's, 69c), so
 * windowd runs first and every other item at once after it, a windowed
 * app given `&` (always after windowd answers); its comments are kept.
 */
#include <dirent.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"
#include "widget.h"
#include "window.h"
#include "../bin/settings_file.h"

PROGRAM_HELP("Settings [set FILE KEY [VALUE]]", "the system's and your settings, in a window");

#ifdef __x86_64__
#define ARCH_NAME "x86_64"
#else
#define ARCH_NAME "aarch64"
#endif

static struct window *win;
static int run_in(char *const argv[], const char *input, char *out, size_t size);   /* (below) */
static char windows_file[320], filetypes_file[320], login_file[320];

/* A window setting as windowd reads it: the user's file, else the
 * system's, else dflt. */
static void windows_value(const char *key, char *value, size_t size, const char *dflt)
{
    if (config_get(windows_file, key, value, size) == 1 ||
        config_get("/system/config/windows", key, value, size) == 1)
        return;
    snprintf(value, size, "%s", dflt);
}

static bool saved(struct widget *status, int failed, const char *what)
{
    char text[200];
    if (failed)
        snprintf(text, sizeof(text), "Not saved: %s: %s", what, strerror(errno));
    else
        snprintf(text, sizeof(text), "Saved.");
    widget_set_text(status, text);
    return !failed;
}

/* (69h, the user's: choices from a list) The installed apps (bundles in
 * apps: NAME.program) and, unless apps_only, the command-line programs of
 * bin, each a line, apps first, by name. Choosing one fills a field the
 * user may still type in. */
static void programs_text(char *rows, size_t size, bool apps_only)
{
    static const char *const dirs[2] = { "/system/" ARCH_NAME "/apps", "/system/" ARCH_NAME "/bin" };
    size_t n = 0;
    rows[0] = 0;
    for (int k = 0; k < (apps_only ? 1 : 2); k++) {
        char names[160][40];
        int count = 0;
        DIR *d = opendir(dirs[k]);
        struct dirent *e;
        while (d && (e = readdir(d)) && count < 160) {
            char name[40];
            size_t len = strlen(e->d_name);
            if (!((e->d_name[0] >= 'a' && e->d_name[0] <= 'z') || (e->d_name[0] >= 'A' && e->d_name[0] <= 'Z')) ||
                len >= sizeof(name))
                continue;                       /* (names: not `[`, test's other name) */
            if (k == 0) {
                if (len < 9 || strcmp(e->d_name + len - 8, ".program"))
                    continue;
                len -= 8;
            }
            snprintf(name, sizeof(name), "%.*s", (int)len, e->d_name);
            int at = count++;
            while (at > 0 && strcmp(names[at - 1], name) > 0) {
                memcpy(names[at], names[at - 1], sizeof(name));
                at--;
            }
            memcpy(names[at], name, sizeof(name));
        }
        if (d)
            closedir(d);
        for (int i = 0; i < count && n < size - 42; i++)
            n += (size_t)snprintf(rows + n, size - n, "%s%s", n ? "\n" : "", names[i]);
    }
}

/* A list's chosen row into the editor given (data). */
static void fill_from(struct widget *w, void *data)
{
    if (widget_value(w) >= 0)
        widget_set_text((struct widget *)data, widget_text(w));
}

/* --- Appearance ------------------------------------------------------------ */

static struct widget *dark, *light, *accent, *text_size, *size_shown, *look_status;

static void size_moved(struct widget *w, void *data)
{
    (void)data;
    char text[32];
    snprintf(text, sizeof(text), "%d pixels", widget_value(w));
    widget_set_text(size_shown, text);
}

static bool hex6(const char *s)
{
    if (strlen(s) != 6)
        return false;
    for (int i = 0; i < 6; i++)
        if (!strchr("0123456789abcdefABCDEF", s[i]))
            return false;
    return true;
}

static void look_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    const char *a = widget_text(accent);
    if (*a && !hex6(a)) {
        widget_message(win, "Accent", "The accent is six hexadecimal digits, RRGGBB "
                       "(as 5E81AC), or empty for the theme's.");
        return;
    }
    char size[16];
    snprintf(size, sizeof(size), "%d", widget_value(text_size));
    int failed = settings_set(windows_file, "theme", widget_value(light) ? "light" : "dark") < 0 ||
                 settings_set(windows_file, "accent", *a ? a : 0) < 0 ||
                 settings_set(windows_file, "text_size", size) < 0;
    if (saved(look_status, failed, windows_file))
        window_settings_reload(win);
}

static struct widget *appearance_page(void)
{
    char theme[16], acc[16], size[16];
    windows_value("theme", theme, sizeof(theme), "dark");
    windows_value("accent", acc, sizeof(acc), "");
    windows_value("text_size", size, sizeof(size), "15");
    int s = atoi(size);
    s = s >= 8 && s <= 64 ? s : 15;
    struct widget *p = widget_column(), *themes = widget_row(), *sizes = widget_row();
    widget_add(p, widget_label("Theme: the colours of windows, menus and programs"));
    dark = widget_radio("Dark", 0, strcmp(theme, "light") != 0);
    light = widget_radio("Light", dark, strcmp(theme, "light") == 0);
    widget_add(themes, dark);
    widget_add(themes, light);
    widget_add(p, themes);
    widget_add(p, widget_label("Accent: the colour of what is in front and chosen, RRGGBB\n"
                               "(empty: the theme's, 5E81AC)"));
    accent = widget_editor(acc, 1);
    widget_add(p, accent);
    widget_add(p, widget_label("Text size, in programs that draw scalable text"));
    text_size = widget_slider(8, 64, s);
    size_shown = widget_label("");
    widget_on(text_size, size_moved, 0);
    size_moved(text_size, 0);
    widget_stretch(text_size, 1);
    widget_add(sizes, text_size);
    widget_add(sizes, size_shown);
    widget_add(p, sizes);
    struct widget *save = widget_button("Save");
    widget_on(save, look_save, 0);
    widget_add(p, save);
    look_status = widget_label("");
    widget_add(p, look_status);
    widget_set_text(p, "Appearance");
    return p;
}

/* --- Mouse & touchpad ------------------------------------------------------ */

static struct widget *click_ms, *click_shown, *steady, *mouse_status;

static void click_moved(struct widget *w, void *data)
{
    (void)data;
    char text[32];
    snprintf(text, sizeof(text), "%d ms", widget_value(w));
    widget_set_text(click_shown, text);
}

static void mouse_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    char ms[16];
    snprintf(ms, sizeof(ms), "%d", widget_value(click_ms));
    int failed = settings_set(windows_file, "double_click", ms) < 0 ||
                 settings_set(windows_file, "click_steady", widget_value(steady) ? "on" : "off") < 0;
    if (saved(mouse_status, failed, windows_file))
        window_settings_reload(win);
}

static struct widget *mouse_page(void)
{
    char ms[16], st[16];
    windows_value("double_click", ms, sizeof(ms), "500");
    windows_value("click_steady", st, sizeof(st), "on");
    int v = atoi(ms);
    v = v >= 100 && v <= 5000 ? v : 500;
    struct widget *p = widget_column(), *clicks = widget_row();
    widget_add(p, widget_label("Double click: two presses within"));
    click_ms = widget_slider(100, 5000, v);
    click_shown = widget_label("");
    widget_on(click_ms, click_moved, 0);
    click_moved(click_ms, 0);
    widget_stretch(click_ms, 1);
    widget_add(clicks, click_ms);
    widget_add(clicks, click_shown);
    widget_add(p, clicks);
    steady = widget_check("Steady clicks: a touchpad's slow creep around a click is held",
                          strcmp(st, "off") != 0);
    widget_add(p, steady);
    struct widget *save = widget_button("Save");
    widget_on(save, mouse_save, 0);
    widget_add(p, save);
    mouse_status = widget_label("");
    widget_add(p, mouse_status);
    widget_set_text(p, "Mouse & touchpad");
    return p;
}

/* --- Window system ----------------------------------------------------------- */

static struct widget *startup, *rate_auto, *rate, *winsys_status;

static void winsys_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    const char *r = widget_text(rate);
    int hz = atoi(r);
    if (!widget_value(rate_auto) && (hz < 10 || hz > 1000 || strspn(r, "0123456789") != strlen(r))) {
        widget_message(win, "Frames a second", "The frames a second are a number from 10 to 1000, "
                       "or the screen's own rate.");
        return;
    }
    int failed = settings_set(windows_file, "startup", widget_text(startup)) < 0 ||
                 settings_set(windows_file, "refresh", widget_value(rate_auto) ? "auto" : r) < 0;
    if (saved(winsys_status, failed, windows_file))
        window_settings_reload(win);
}

static struct widget *winsys_page(void)
{
    char st[128], rv[16];
    windows_value("startup", st, sizeof(st), "Owl");
    windows_value("refresh", rv, sizeof(rv), "auto");
    struct widget *p = widget_column(), *row = widget_row(), *col = widget_column();
    widget_add(p, widget_label("Opened when the window system starts: a program and its\n"
                               "arguments (empty: nothing; counts from its next start)"));
    startup = widget_editor(st, 1);
    char apps[2048];
    programs_text(apps, sizeof(apps), true);    /* (69h: the apps, to choose) */
    struct widget *choices = widget_list(apps);
    widget_on(choices, fill_from, startup);
    widget_add(col, startup);
    widget_add(row, col);
    widget_add(row, choices);
    widget_add(p, row);
    widget_add(p, widget_label("Frames a second the window system draws at most"));
    rate_auto = widget_check("The screen's own rate (auto)", strcmp(rv, "auto") == 0);
    widget_add(p, rate_auto);
    rate = widget_editor(strcmp(rv, "auto") ? rv : "60", 1);
    widget_add(p, rate);
    struct widget *save = widget_button("Save");
    widget_on(save, winsys_save, 0);
    widget_add(p, save);
    winsys_status = widget_label("");
    widget_add(p, winsys_status);
    widget_set_text(p, "Window system");
    return p;
}

/* --- File types --------------------------------------------------------------- */

static struct widget *types, *ending, *program, *types_status;

/* The table's rows: "Ending\tProgram", then each ENDING = PROGRAM line. */
static void types_fill(void)
{
    size_t size = 64, n = 0;
    char *rows = malloc(size);
    if (!rows)
        return;
    n = (size_t)snprintf(rows, size, "Ending\tProgram");
    FILE *f = fopen(filetypes_file, "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f)) {
        char *k = line + strspn(line, " \t"), *eq = strchr(k, '=');
        if (*k == '#' || !eq)
            continue;
        char *ke = eq;
        while (ke > k && strchr(" \t", ke[-1]))
            ke--;
        char *v = eq + 1 + strspn(eq + 1, " \t");
        v[strcspn(v, "\r\n")] = 0;
        size_t need = n + (size_t)(ke - k) + strlen(v) + 3;
        if (need > size) {
            char *more = realloc(rows, size = need * 2);
            if (!more)
                break;
            rows = more;
        }
        n += (size_t)snprintf(rows + n, size - n, "\n%.*s\t%s", (int)(ke - k), k, v);
    }
    if (f)
        fclose(f);
    widget_set_text(types, rows);
    free(rows);
}

static void type_chosen(struct widget *w, void *data)
{
    (void)data;
    const char *row = widget_text(w), *tab = strchr(row, '\t');
    if (!tab)
        return;
    char e[64];
    snprintf(e, sizeof(e), "%.*s", (int)(tab - row), row);
    widget_set_text(ending, e);
    widget_set_text(program, tab + 1);
}

static void type_set(struct widget *w, void *data)
{
    bool remove = data != 0;
    (void)w;
    const char *e = widget_text(ending), *p = widget_text(program);
    if (!*e || strpbrk(e, " \t=#/.") || (!remove && (!*p || strpbrk(p, "=#")))) {
        widget_message(win, "File types", "An ending is letters and digits without the dot "
                       "(as png); a program is its name (as imageview).");
        return;
    }
    saved(types_status, settings_set(filetypes_file, e, remove ? 0 : p) < 0, filetypes_file);
    types_fill();
}

static struct widget *types_page(void)
{
    settings_make(filetypes_file, SETTINGS_USER_FILETYPES_TEXT);   /* (its reader writes it) */
    struct widget *p = widget_column(), *fields = widget_row();
    widget_add(p, widget_label("The program a double click in owl (and open FILE) starts,\n"
                               "by the file's ending"));
    struct widget *tr = widget_row();
    types = widget_table(2, "Ending\tProgram");
    widget_on(types, type_chosen, 0);
    types_fill();
    widget_add(tr, types);
    ending = widget_editor("", 1);
    program = widget_editor("", 1);
    static char progs[8192];                    /* (69h: the apps and programs, to choose) */
    programs_text(progs, sizeof(progs), false);
    struct widget *choices = widget_list(progs);
    widget_on(choices, fill_from, program);
    widget_add(tr, choices);
    widget_add(p, tr);
    widget_stretch(program, 1);
    widget_add(fields, widget_label("Ending"));
    widget_add(fields, ending);
    widget_add(fields, widget_label("Program"));
    widget_add(fields, program);
    widget_add(p, fields);
    struct widget *buttons = widget_row(), *set = widget_button("Set"), *remove = widget_button("Remove");
    widget_on(set, type_set, 0);
    widget_on(remove, type_set, (void *)1);
    widget_add(buttons, set);
    widget_add(buttons, remove);
    widget_add(p, buttons);
    types_status = widget_label("");
    widget_add(p, types_status);
    widget_set_text(p, "File types");
    return p;
}

/* --- (69f) Account --------------------------------------------------------- */

static struct widget *acct_current, *acct_new, *acct_again, *acct_status;

static void account_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    const char *c = widget_text(acct_current), *a = widget_text(acct_new), *b = widget_text(acct_again);
    if ((getuid() != 0 && !*c) || !*a || strcmp(a, b)) {
        widget_message(win, "Account", "Your password now, and the new one twice, the same.");
        return;
    }
    char input[400], out[512];
    if (getuid() == 0)                          /* (root is not asked its present one) */
        snprintf(input, sizeof(input), "%s\n%s\n", a, b);
    else
        snprintf(input, sizeof(input), "%s\n%s\n%s\n", c, a, b);
    char *argv[] = { "passwd", "-S", 0 };       /* (setuid: your own, after the present one) */
    run_in(argv, input, out, sizeof(out));
    memset(input, 0, sizeof(input));
    widget_set_text(acct_current, "");
    widget_set_text(acct_new, "");
    widget_set_text(acct_again, "");
    widget_set_text(acct_status, out);
}

static struct widget *account_page(void)
{
    struct widget *p = widget_column(), *grid = widget_grid(2);
    widget_add(p, widget_label("Your password (it shows while typed)"));
    acct_current = widget_editor("", 1);
    acct_new = widget_editor("", 1);
    acct_again = widget_editor("", 1);
    if (getuid() != 0) {
        widget_add(grid, widget_label("Now"));
        widget_add(grid, acct_current);
    }
    widget_add(grid, widget_label("New"));
    widget_add(grid, acct_new);
    widget_add(grid, widget_label("Again"));
    widget_add(grid, acct_again);
    widget_add(p, grid);
    struct widget *save = widget_button("Change");
    widget_on(save, account_save, 0);
    widget_add(p, save);
    acct_status = widget_label("");
    widget_add(p, acct_status);
    widget_set_text(p, "Account");
    return p;
}

/* --- At login -------------------------------------------------------------- */

#define ITEMS 32
static char comments[2048];             /* the file's comment lines, kept in order */
static char windowd_line[160];          /* "windowd &" or the user's own windowd line */
static char items[ITEMS][160];
static int nitems;
static struct widget *console, *windowed, *item_list, *item_entry, *login_status;

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && strchr(" \t\r\n", s[n - 1]))
        s[--n] = 0;
    size_t lead = strspn(s, " \t");
    memmove(s, s + lead, strlen(s + lead) + 1);
}

/* ~/.config/login read: comments, the windowd line (Window system), the
 * items in order (`wsend wait` is the app's own). */
static void login_read(void)
{
    comments[0] = windowd_line[0] = 0;
    nitems = 0;
    FILE *f = fopen(login_file, "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#') {
            size_t n = strlen(comments);
            snprintf(comments + n, sizeof(comments) - n, "%s\n", line);
        } else if (!strncmp(line, "windowd", 7) && (!line[7] || line[7] == ' ' || line[7] == '&')) {
            snprintf(windowd_line, sizeof(windowd_line), "%s", line);
        } else if (strcmp(line, "wsend wait") && nitems < ITEMS) {
            snprintf(items[nitems++], sizeof(items[0]), "%s", line);
        }
    }
    if (f)
        fclose(f);
}

static void items_show(int chosen)
{
    char text[ITEMS * 162] = "";
    size_t n = 0;
    for (int i = 0; i < nitems; i++)
        n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%s", i ? "\n" : "", items[i]);
    widget_set_text(item_list, text);
    widget_set_value(item_list, chosen);
}

/* A windowed app: the command's first word is an app (a bundle in apps). */
static bool windowed_app(const char *command)
{
    char name[64], path[160];
    snprintf(name, sizeof(name), "%.*s", (int)strcspn(command, " \t&"), command);
    snprintf(path, sizeof(path), "/system/" ARCH_NAME "/apps/%s.program/%s", name, name);
    return name[0] && access(path, X_OK) == 0;
}

static void login_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    char tmp[340];
    snprintf(tmp, sizeof(tmp), "%s.new", login_file);
    FILE *f = fopen(tmp, "w");
    bool ok = f != 0;
    if (f) {
        fputs(comments, f);
        if (widget_value(windowed)) {           /* windowd first, every other item after it answers */
            const char *wl = windowd_line[0] ? windowd_line : "windowd &";
            fprintf(f, "%s%s\nwsend wait\n", wl, strchr(wl, '&') ? "" : " &");
        }
        for (int i = 0; i < nitems; i++) {
            bool app = windowed_app(items[i]) && !strchr(items[i], '&');
            fprintf(f, "%s%s\n", items[i], app ? " &" : "");
        }
        ok = fclose(f) == 0;
    }
    if (ok && rename(tmp, login_file) < 0)
        ok = false;
    saved(login_status, !ok, login_file);
    if (ok) {
        login_read();                           /* (as written: a windowed app now has its &) */
        items_show(widget_value(item_list));
    }
}

static void item_add(struct widget *w, void *data)
{
    (void)w, (void)data;
    char text[160];
    snprintf(text, sizeof(text), "%s", widget_text(item_entry));
    trim(text);
    if (!text[0] || nitems == ITEMS)
        return;
    if (!strncmp(text, "windowd", 7)) {
        widget_message(win, "At login", "The window system is chosen above: Window system.");
        return;
    }
    snprintf(items[nitems++], sizeof(items[0]), "%s", text);
    widget_set_text(item_entry, "");
    items_show(nitems - 1);
}

static void item_move(struct widget *w, void *data)
{
    (void)w;
    int how = (int)(long)data, i = widget_value(item_list);    /* 0 remove, -1 up, 1 down */
    if (i < 0 || i >= nitems)
        return;
    if (how == 0) {
        memmove(items[i], items[i + 1], (size_t)(nitems - i - 1) * sizeof(items[0]));
        nitems--;
        items_show(i < nitems ? i : nitems - 1);
        return;
    }
    int j = i + how;
    if (j < 0 || j >= nitems)
        return;
    char t[160];
    memcpy(t, items[i], sizeof(t));
    memcpy(items[i], items[j], sizeof(t));
    memcpy(items[j], t, sizeof(t));
    items_show(j);
}

static struct widget *login_page(void)
{
    login_read();
    struct widget *p = widget_column(), *start = widget_row();
    widget_add(p, widget_label("Start in, when you log in at this machine (not over SSH)"));
    console = widget_radio("Console", 0, windowd_line[0] == 0);
    windowed = widget_radio("Window system", console, windowd_line[0] != 0);
    widget_add(start, console);
    widget_add(start, windowed);
    widget_add(p, start);
    widget_add(p, widget_label("Run at login, in order (a windowed app after the window\n"
                               "system is up)"));
    struct widget *lr = widget_row(), *lc = widget_column();   /* (a container: at the top) */
    item_list = widget_list("");
    items_show(nitems ? 0 : -1);
    widget_add(lc, item_list);
    widget_add(lr, lc);
    item_entry = widget_editor("", 1);
    static char progs[8192];                    /* (69h: the apps and programs, to choose) */
    programs_text(progs, sizeof(progs), false);
    struct widget *choices = widget_list(progs);
    widget_on(choices, fill_from, item_entry);
    widget_add(lr, choices);
    widget_add(p, lr);
    struct widget *add_row = widget_row(), *add = widget_button("Add");
    widget_stretch(item_entry, 1);
    widget_on(add, item_add, 0);
    widget_add(add_row, item_entry);
    widget_add(add_row, add);
    widget_add(p, add_row);
    struct widget *buttons = widget_row(), *up = widget_button("Up"), *down = widget_button("Down"),
                  *remove = widget_button("Remove"), *save = widget_button("Save");
    widget_on(up, item_move, (void *)-1L);
    widget_on(down, item_move, (void *)1L);
    widget_on(remove, item_move, 0);
    widget_on(save, login_save, 0);
    widget_add(buttons, up);
    widget_add(buttons, down);
    widget_add(buttons, remove);
    widget_add(buttons, save);
    widget_add(p, buttons);
    login_status = widget_label("");
    widget_add(p, login_status);
    widget_set_text(p, "At login");
    return p;
}

/* --- System (69d) --------------------------------------------------------------- */

/* Run argv (a program of the system), (69f) input on its standard input
 * when not 0 (else none), its output (both streams) into out, cut to size;
 * 0 when it ended with 0. */
static int run_in(char *const argv[], const char *input, char *out, size_t size)
{
    int p[2], in[2];
    out[0] = 0;
    if (pipe(p) < 0)
        return -1;
    if (pipe(in) < 0) {
        close(p[0]), close(p[1]);
        return -1;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_adddup2(&fa, p[1], 2);
    posix_spawn_file_actions_addclose(&fa, in[0]);
    posix_spawn_file_actions_addclose(&fa, in[1]);
    posix_spawn_file_actions_addclose(&fa, p[0]);
    posix_spawn_file_actions_addclose(&fa, p[1]);
    pid_t pid;
    int e = posix_spawnp(&pid, argv[0], &fa, 0, argv, 0);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    if (!e && input)                            /* (a few lines: the pipe holds them) */
        (void)!write(in[1], input, strlen(input));
    close(in[1]);
    close(p[1]);
    size_t n = 0;
    ssize_t r;
    char junk[256];
    while ((r = read(p[0], n < size - 1 ? out + n : junk, n < size - 1 ? size - 1 - n : sizeof(junk))) > 0)
        if (n < size - 1)
            n += (size_t)r;
    out[n] = 0;
    while (n && out[n - 1] == '\n')
        out[--n] = 0;
    close(p[0]);
    if (e) {
        snprintf(out, size, "%s: %s", argv[0], strerror(e));
        return -1;
    }
    int st;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run(char *const argv[], char *out, size_t size)
{
    return run_in(argv, 0, out, size);
}

/* (69f) Unlocked: the user's password, kept while the app runs (cleared at
 * its end), for sudo -S. */
static char unlocked[128];
static bool is_unlocked;

/* A change of the machine's: argv as root, through `sudo -S` with the
 * kept password unless this is root's own app. input follows the
 * password on the program's standard input. */
static int run_root_in(char *const argv[], const char *input, char *out, size_t size)
{
    if (getuid() == 0 || !is_unlocked)
        return run_in(argv, input, out, size);
    char *with[24] = { "sudo", "-S" }, text[640];
    int n = 0;
    while (argv[n] && n < 21) {
        with[n + 2] = argv[n];
        n++;
    }
    with[n + 2] = 0;
    snprintf(text, sizeof(text), "%s\n%s", unlocked, input ? input : "");
    int r = run_in(with, text, out, size);
    memset(text, 0, sizeof(text));
    return r;
}

static int run_root(char *const argv[], char *out, size_t size)
{
    return run_root_in(argv, 0, out, size);
}

/* A line of a system's settings file: settings_set for root, else (69f)
 * `sudo -S Settings set FILE KEY [VALUE]`. 0, or -1 with errno. */
static int system_set(const char *path, const char *key, const char *value)
{
    if (getuid() == 0)
        return settings_set(path, key, value);
    char *argv[] = { "Settings", "set", (char *)path, (char *)key, (char *)value, 0 }, out[256];
    if (run_root(argv, out, sizeof(out)) == 0)
        return 0;
    errno = EACCES;
    return -1;
}

/* The System pages change the machine: root's. Anyone else is told so. */
static bool may_change(void)
{
    if (getuid() == 0 || is_unlocked)
        return true;
    widget_message(win, "System", "Only root may change the system's settings: Unlock them "
                   "with your password first (a user of sudo).");
    return false;
}

/* A program's lines as a list's rows ("" stays one empty row), each line's
 * runs of spaces made one (its columns are for a terminal's fixed width;
 * a list's font is not) and its leading ones dropped. (69e) Of guard's,
 * only the numbered rules, without their packet counts. */
static void lines_text(char *const argv[], char *rows, size_t size)
{
    char out[4096];
    run(argv, out, sizeof(out));
    bool rules = !strcmp(argv[0], "guard");
    size_t n = 0;
    for (char *line = strtok(out, "\n"); line; line = strtok(0, "\n")) {
        line += strspn(line, " ");
        if (rules) {
            if (!(*line >= '0' && *line <= '9'))
                continue;
            char *count = strstr(line, " packet(s)");
            if (count) {                            /* (the count before it too) */
                while (count > line && count[-1] != ' ')
                    count--;
                *count = 0;
            }
        }
        if (n && n < size - 2)
            rows[n++] = '\n';
        for (char *c = line; *c && n < size - 2; c++)
            if (*c != ' ' || (n && rows[n - 1] != ' ' && rows[n - 1] != '\n'))
                rows[n++] = *c;
        while (n && rows[n - 1] == ' ')
            n--;
    }
    rows[n] = 0;
}

static void lines_into(struct widget *list, char *const argv[])
{
    char rows[4096];
    lines_text(argv, rows, sizeof(rows));
    widget_set_text(list, rows);
}

/* -- Display -- */

/* (69h, the user's: choices from a list) displays' outputs, each with the
 * modes its monitor lists, to choose from. */
#define OUTPUTS 8
static char output_name[OUTPUTS][16], output_modes[OUTPUTS][1024];
static int outputs;
static struct widget *display_screen, *display_outputs, *display_modes, *display_status;

/* displays' report read: the screen's line, every output (a line "  NAME
 * ..." whose NAME is letters then digits, each once: the drivers' other
 * lines name them again) and the modes its monitor lists (the "modes:"
 * line after it). */
static void display_fill(void)
{
    static char out[16384];
    char rows[2048] = "", *argv[] = { "displays", 0 };
    run(argv, out, sizeof(out));
    outputs = 0;
    size_t n = 0;
    widget_set_text(display_screen, "");
    for (char *line = strtok(out, "\n"); line; line = strtok(0, "\n")) {
        if (!strncmp(line, "screen:", 7)) {
            widget_set_text(display_screen, line);
            continue;
        }
        char *m = strstr(line, "modes: ");
        if (m && line[0] == ' ' && outputs) {
            snprintf(output_modes[outputs - 1], sizeof(output_modes[0]), "%s", m + 7);
            continue;
        }
        if (line[0] != ' ' || line[1] != ' ' || line[2] == ' ')
            continue;
        char *name = line + 2;
        size_t letters = strspn(name, "abcdefghijklmnopqrstuvwxyz"), digits = strspn(name + letters, "0123456789");
        if (!letters || !digits || name[letters + digits] != ' ' || outputs == OUTPUTS)
            continue;
        bool seen = false;
        for (int i = 0; i < outputs; i++)
            seen |= !strncmp(output_name[i], name, letters + digits) && !output_name[i][letters + digits];
        if (seen)
            continue;
        snprintf(output_name[outputs], sizeof(output_name[0]), "%.*s", (int)(letters + digits), name);
        output_modes[outputs][0] = 0;
        char *rest = name + letters + digits;
        rest += strspn(rest, " ");
        n += (size_t)snprintf(rows + n, sizeof(rows) - n, "%s%s  %.30s", n ? "\n" : "",
                              output_name[outputs], rest);
        outputs++;
    }
    widget_set_text(display_outputs, rows);
    widget_set_text(display_modes, "");
    if (!outputs)
        widget_set_text(display_status, "No outputs: no graphics driver for this machine's "
                        "graphics; the firmware's mode stays.");
}

static void output_chosen(struct widget *w, void *data)
{
    (void)data;
    int i = widget_value(w);
    char rows[1100];
    snprintf(rows, sizeof(rows), "%s", i >= 0 && i < outputs ? output_modes[i] : "");
    for (char *c = rows; *c; c++)
        if (*c == ' ')
            *c = '\n';
    widget_set_text(display_modes, rows);
    if (i >= 0 && i < outputs && !output_modes[i][0])
        widget_set_text(display_status, "Its monitor lists no modes (none there, or no EDID).");
}

static void display_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    if (!may_change())
        return;
    int i = widget_value(display_outputs);
    char mode[32], out[512];
    snprintf(mode, sizeof(mode), "%s", widget_text(display_modes));
    if (i < 0 || i >= outputs || !mode[0]) {
        widget_message(win, "Display", "Choose an output, then one of its modes.");
        return;
    }
    char *argv[] = { "displays", "set", output_name[i], mode, 0 };
    if (run_root(argv, out, sizeof(out)) == 0 && system_set(SETTINGS_DISPLAYS, output_name[i], mode) < 0)
        snprintf(out, sizeof(out), "Not saved: %s: %s", SETTINGS_DISPLAYS, strerror(errno));
    widget_set_text(display_status, out);
}

static struct widget *display_page(void)
{
    struct widget *p = widget_column(), *lists = widget_row(), *left = widget_column(), *right = widget_column();
    display_screen = widget_label("");
    widget_add(p, display_screen);
    display_outputs = widget_list("");
    display_modes = widget_list("");
    display_status = widget_label("");
    widget_on(display_outputs, output_chosen, 0);
    widget_add(left, widget_label("Outputs"));
    widget_add(left, display_outputs);
    widget_add(right, widget_label("Modes its monitor lists"));
    widget_add(right, display_modes);
    widget_add(lists, left);
    widget_add(lists, right);
    widget_add(p, lists);
    display_fill();
    widget_add(p, widget_label("Set at once and at every start (/system/config/displays)"));
    struct widget *save = widget_button("Save");
    widget_on(save, display_save, 0);
    widget_add(p, save);
    widget_add(p, display_status);
    widget_set_text(p, "Display");
    return p;
}

/* -- Sound -- */

static struct widget *sound_volume, *sound_shown, *sound_mute, *sound_status;

static void sound_moved(struct widget *w, void *data)
{
    (void)data;
    char text[16];
    snprintf(text, sizeof(text), "%d", widget_value(w));
    widget_set_text(sound_shown, text);
}

static void sound_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    char v[16], out[256], more[256];
    snprintf(v, sizeof(v), "%d", widget_value(sound_volume));
    char *set[] = { "volume", v, 0 };
    char *mute[] = { "volume", widget_value(sound_mute) ? "mute" : "unmute", 0 };
    if (run(set, out, sizeof(out)) == 0 && run(mute, more, sizeof(more)) == 0)
        snprintf(out, sizeof(out), "%s", more);  /* (the last says both) */
    widget_set_text(sound_status, out);
}

static struct widget *sound_page(void)
{
    char out[128];
    char *ask[] = { "volume", 0 };
    int v = 100, muted = 0;
    if (run(ask, out, sizeof(out)) == 0) {      /* "volume 80" or "volume 80 (muted)" */
        sscanf(out, "volume %d", &v);
        muted = strstr(out, "(muted)") != 0;
    }
    struct widget *p = widget_column(), *row = widget_row();
    widget_add(p, widget_label("The master volume of everything the sound server plays"));
    sound_volume = widget_slider(0, 100, v);
    sound_shown = widget_label("");
    widget_on(sound_volume, sound_moved, 0);
    sound_moved(sound_volume, 0);
    widget_stretch(sound_volume, 1);
    widget_add(row, sound_volume);
    widget_add(row, sound_shown);
    widget_add(p, row);
    sound_mute = widget_check("Mute", muted);
    widget_add(p, sound_mute);
    struct widget *save = widget_button("Save");
    widget_on(save, sound_save, 0);
    widget_add(p, save);
    sound_status = widget_label(run(ask, out, sizeof(out)) == 0 ? "" : out);
    widget_add(p, sound_status);
    widget_set_text(p, "Sound");
    return p;
}

/* -- Memory -- */

static struct widget *swap_info, *swap_size, *swap_status;

static void swap_do(struct widget *w, void *data)
{
    (void)w;
    if (!may_change())
        return;
    const char *what = data;
    char size[32], out[512];
    snprintf(size, sizeof(size), "%s", widget_text(swap_size));
    if (!strcmp(what, "make") && (!size[0] || strspn(size, "0123456789KMGkmg") != strlen(size))) {
        widget_message(win, "Swap", "A size is a number with K, M or G (4G).");
        return;
    }
    char *argv[] = { "swap", (char *)what, size, 0 };
    if (strcmp(what, "make"))
        argv[2] = 0;
    run_root(argv, out, sizeof(out));
    widget_set_text(swap_status, out);
    char *show[] = { "swap", 0 };
    lines_into(swap_info, show);
}

static struct widget *memory_page(void)
{
    struct widget *p = widget_column(), *buttons = widget_row(), *make = widget_row();
    widget_add(p, widget_label("Swap: the file /system/swap holds programs' memory, encrypted,\n"
                               "when memory runs short"));
    swap_info = widget_list("");
    char *show[] = { "swap", 0 };
    lines_into(swap_info, show);
    widget_add(p, swap_info);
    struct widget *on = widget_button("On"), *off = widget_button("Off"), *mk = widget_button("Make");
    widget_on(on, swap_do, "on");
    widget_on(off, swap_do, "off");
    widget_on(mk, swap_do, "make");
    widget_add(buttons, on);
    widget_add(buttons, off);
    widget_add(p, buttons);
    swap_size = widget_editor("", 1);
    widget_add(make, widget_label("Make the file, size"));
    widget_add(make, swap_size);
    widget_add(make, mk);
    widget_add(p, make);
    swap_status = widget_label("");
    widget_add(p, swap_status);
    widget_set_text(p, "Memory");
    return p;
}

/* -- Date & time -- */

static struct widget *time_now, *time_zone, *time_clock, *time_status;

static void time_show(void)
{
    char out[128];
    char *argv[] = { "date", 0 };
    run(argv, out, sizeof(out));
    widget_set_text(time_now, out);
}

/* +HH:MM, -HH:MM or 0. */
static bool zone_ok(const char *z)
{
    return !strcmp(z, "0") || (strlen(z) == 6 && (z[0] == '+' || z[0] == '-') &&
                               strspn(z + 1, "0123456789") == 2 && z[3] == ':' &&
                               strspn(z + 4, "0123456789") == 2);
}

static void zone_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    if (!may_change())
        return;
    char z[16];                                 /* (69h: a row of the list; "0 (UTC)" is 0) */
    snprintf(z, sizeof(z), "%.*s", (int)strcspn(widget_text(time_zone), " "), widget_text(time_zone));
    if (!zone_ok(z)) {
        widget_message(win, "Time zone", "Choose the offset from UTC in the list.");
        return;
    }
    saved(time_status, system_set(SETTINGS_TIME, "zone", z) < 0, SETTINGS_TIME);
    time_show();
}

static void clock_set(struct widget *w, void *data)
{
    (void)w, (void)data;
    if (!may_change())
        return;
    char t[40], out[256];
    snprintf(t, sizeof(t), "%s", widget_text(time_clock));
    char *argv[] = { "date", "-s", t, 0 };
    run_root(argv, out, sizeof(out));
    widget_set_text(time_status, out);
    time_show();
}

static struct widget *time_page(void)
{
    char zone[16];
    if (config_get(SETTINGS_TIME, "zone", zone, sizeof(zone)) != 1)
        snprintf(zone, sizeof(zone), "0");
    struct widget *p = widget_column(), *zr = widget_row(), *cr = widget_row();
    time_now = widget_label("");
    time_show();
    widget_add(p, time_now);
    widget_add(p, widget_label("Time zone: the offset from UTC (no daylight saving rules)"));
    /* (69h) the offsets in use somewhere, to choose from */
    static const char *const zones[] = {
        "-12:00", "-11:00", "-10:00", "-09:30", "-09:00", "-08:00", "-07:00", "-06:00", "-05:00",
        "-04:00", "-03:30", "-03:00", "-02:00", "-01:00", "0 (UTC)", "+01:00", "+02:00", "+03:00",
        "+03:30", "+04:00", "+04:30", "+05:00", "+05:30", "+05:45", "+06:00", "+06:30", "+07:00",
        "+08:00", "+08:45", "+09:00", "+09:30", "+10:00", "+10:30", "+11:00", "+12:00", "+12:45",
        "+13:00", "+14:00",
    };
    char rows[400] = "";
    size_t n = 0;
    int chosen = -1, count = (int)(sizeof(zones) / sizeof(zones[0]));
    for (int i = 0; i < count; i++) {
        n += (size_t)snprintf(rows + n, sizeof(rows) - n, "%s%s", i ? "\n" : "", zones[i]);
        if (!strncmp(zones[i], zone, strlen(zone)) && (zones[i][strlen(zone)] == 0 || zones[i][strlen(zone)] == ' '))
            chosen = i;
    }
    time_zone = widget_list(rows);
    widget_set_value(time_zone, chosen);
    struct widget *zs = widget_button("Save");
    widget_on(zs, zone_save, 0);
    widget_add(zr, time_zone);
    widget_add(zr, zs);
    widget_add(p, zr);
    widget_add(p, widget_label("Set the clock: YYYY-MM-DD HH:MM[:SS], local time"));
    time_clock = widget_editor("", 1);
    struct widget *cs = widget_button("Set");
    widget_on(cs, clock_set, 0);
    widget_add(cr, time_clock);
    widget_add(cr, cs);
    widget_add(p, cr);
    time_status = widget_label("");
    widget_add(p, time_status);
    widget_set_text(p, "Date & time");
    return p;
}

/* -- Elves -- */

static struct widget *elves, *elves_status;

static void elf_do(struct widget *w, void *data)
{
    (void)w;
    if (!may_change())
        return;
    char name[64], out[256];
    const char *row = widget_text(elves);
    snprintf(name, sizeof(name), "%.*s", (int)strcspn(row, " \t:"), row);
    if (!name[0] || !strcmp(name, "elvesd")) {
        widget_set_text(elves_status, "Choose an elf in the list first.");
        return;
    }
    char *argv[] = { "elf", (char *)data, name, 0 };
    run_root(argv, out, sizeof(out));
    widget_set_text(elves_status, out);
    char *list[] = { "elf", 0 };
    lines_into(elves, list);
}

static struct widget *elves_page(void)
{
    struct widget *p = widget_column(), *buttons = widget_row();
    widget_add(p, widget_label("Elves: programs kept running (choose one)"));
    elves = widget_list("");
    char *list[] = { "elf", 0 };
    lines_into(elves, list);
    widget_add(p, elves);
    static const char *what[][2] = { { "Start", "start" }, { "Stop", "stop" }, { "Restart", "restart" } };
    for (int i = 0; i < 3; i++) {
        struct widget *b = widget_button(what[i][0]);
        widget_on(b, elf_do, (void *)what[i][1]);
        widget_add(buttons, b);
    }
    widget_add(p, buttons);
    elves_status = widget_label("");
    widget_add(p, elves_status);
    widget_set_text(p, "Elves");
    return p;
}

/* -- Service names -- */

static struct widget *services, *service_name, *service_port, *services_status;

static void services_fill(void)
{
    char rows[8192];
    size_t n = (size_t)snprintf(rows, sizeof(rows), "Name\tPort");
    FILE *f = fopen(SETTINGS_SERVICES, "r");
    char line[128];
    while (f && fgets(line, sizeof(line), f)) {
        char name[64], port[16];
        if (line[strspn(line, " \t")] != '#' && sscanf(line, " %63[^ \t=] = %15s", name, port) == 2 &&
            n < sizeof(rows) - 100)
            n += (size_t)snprintf(rows + n, sizeof(rows) - n, "\n%s\t%s", name, port);
    }
    if (f)
        fclose(f);
    widget_set_text(services, rows);
}

static void service_chosen(struct widget *w, void *data)
{
    (void)data;
    const char *row = widget_text(w), *tab = strchr(row, '\t');
    if (!tab)
        return;
    char name[64];
    snprintf(name, sizeof(name), "%.*s", (int)(tab - row), row);
    widget_set_text(service_name, name);
    widget_set_text(service_port, tab + 1);
}

static void service_set(struct widget *w, void *data)
{
    (void)w;
    if (!may_change())
        return;
    bool remove = data != 0;
    const char *name = widget_text(service_name), *port = widget_text(service_port);
    int n = atoi(port);
    if (!*name || strpbrk(name, " \t=#") ||
        (!remove && (n < 1 || n > 65535 || strspn(port, "0123456789") != strlen(port)))) {
        widget_message(win, "Service names", "A name (as http) and its port, 1-65535.");
        return;
    }
    saved(services_status, system_set(SETTINGS_SERVICES, name, remove ? 0 : port) < 0, SETTINGS_SERVICES);
    services_fill();
}

static struct widget *services_page(void)
{
    struct widget *p = widget_column(), *fields = widget_row(), *buttons = widget_row();
    widget_add(p, widget_label("Service names for programs (getaddrinfo): the same port for\n"
                               "TCP and UDP"));
    services = widget_table(2, "Name\tPort");
    widget_on(services, service_chosen, 0);
    services_fill();
    widget_add(p, services);
    service_name = widget_editor("", 1);
    service_port = widget_editor("", 1);
    widget_add(fields, widget_label("Name"));
    widget_add(fields, service_name);
    widget_add(fields, widget_label("Port"));
    widget_add(fields, service_port);
    widget_add(p, fields);
    struct widget *set = widget_button("Set"), *remove = widget_button("Remove");
    widget_on(set, service_set, 0);
    widget_on(remove, service_set, (void *)1);
    widget_add(buttons, set);
    widget_add(buttons, remove);
    widget_add(p, buttons);
    services_status = widget_label("");
    widget_add(p, services_status);
    widget_set_text(p, "Service names");
    return p;
}

/* -- Power -- */

static struct widget *button_off, *button_nothing, *button_sleep, *lid_nothing, *lid_sleep;
static struct widget *screen_off, *screen_dim, *sleep_after, *power_status;

static void power_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    if (!may_change())
        return;
    const char *m[3] = { widget_text(screen_off), widget_text(screen_dim), widget_text(sleep_after) };
    for (int i = 0; i < 3; i++)
        if (!*m[i] || strspn(m[i], "0123456789") != strlen(m[i]) || strlen(m[i]) > 5) {
            widget_message(win, "Power", "The times are whole minutes (0: never).");
            return;
        }
    const char *b = widget_value(button_sleep) ? "sleep" : widget_value(button_nothing) ? "nothing" : "off";
    int failed = system_set(SETTINGS_POWER, "power_button", b) < 0 ||
                 system_set(SETTINGS_POWER, "lid", widget_value(lid_sleep) ? "sleep" : "nothing") < 0 ||
                 system_set(SETTINGS_POWER, "screen_off", m[0]) < 0 ||
                 system_set(SETTINGS_POWER, "screen_dim", m[1]) < 0 ||
                 system_set(SETTINGS_POWER, "sleep_after", m[2]) < 0;
    if (!saved(power_status, failed, SETTINGS_POWER))
        return;
    char out[1024];
    char *argv[] = { "power", "settings", 0 };   /* (69a: applied at once) */
    if (run_root(argv, out, sizeof(out)) < 0)
        widget_set_text(power_status, out);
    else
        widget_set_text(power_status, "Saved, and applied.");
}

static struct widget *power_page(void)
{
    char b[16], l[16], so[16], sd[16], sa[16];
    if (config_get(SETTINGS_POWER, "power_button", b, sizeof(b)) != 1)
        snprintf(b, sizeof(b), "off");
    if (config_get(SETTINGS_POWER, "lid", l, sizeof(l)) != 1)
        snprintf(l, sizeof(l), "nothing");
    if (config_get(SETTINGS_POWER, "screen_off", so, sizeof(so)) != 1)
        snprintf(so, sizeof(so), "5");
    if (config_get(SETTINGS_POWER, "screen_dim", sd, sizeof(sd)) != 1)
        snprintf(sd, sizeof(sd), "2");
    if (config_get(SETTINGS_POWER, "sleep_after", sa, sizeof(sa)) != 1)
        snprintf(sa, sizeof(sa), "0");
    struct widget *p = widget_column(), *br = widget_row(), *lr = widget_row(), *times = widget_grid(2);
    widget_add(p, widget_label("The power button"));
    button_off = widget_radio("Switch off", 0, !strcmp(b, "off") || (strcmp(b, "nothing") && strcmp(b, "sleep")));
    button_nothing = widget_radio("Nothing", button_off, !strcmp(b, "nothing"));
    button_sleep = widget_radio("Sleep", button_off, !strcmp(b, "sleep"));
    widget_add(br, button_off);
    widget_add(br, button_nothing);
    widget_add(br, button_sleep);
    widget_add(p, br);
    widget_add(p, widget_label("Closing the lid"));
    lid_nothing = widget_radio("Nothing", 0, strcmp(l, "sleep") != 0);
    lid_sleep = widget_radio("Sleep", lid_nothing, !strcmp(l, "sleep"));
    widget_add(lr, lid_nothing);
    widget_add(lr, lid_sleep);
    widget_add(p, lr);
    widget_add(p, widget_label("Minutes without input until (0: never)"));
    screen_off = widget_editor(so, 1);
    screen_dim = widget_editor(sd, 1);
    sleep_after = widget_editor(sa, 1);
    widget_add(times, widget_label("the screen goes off"));
    widget_add(times, screen_off);
    widget_add(times, widget_label("a panel dims"));
    widget_add(times, screen_dim);
    widget_add(times, widget_label("the machine sleeps"));
    widget_add(times, sleep_after);
    widget_add(p, times);
    struct widget *save = widget_button("Save");
    widget_on(save, power_save, 0);
    widget_add(p, save);
    power_status = widget_label("");
    widget_add(p, power_status);
    widget_set_text(p, "Power");
    return p;
}

/* -- Processor -- */

static const char *const clock_modes[] = { "performance", "balanced", "powersave", "top", "firmware" };
static struct widget *clock_radio[5], *clock_status;

static void clock_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    if (!may_change())
        return;
    const char *mode = "performance";
    for (int i = 0; i < 5; i++)
        if (widget_value(clock_radio[i]))
            mode = clock_modes[i];
    if (!saved(clock_status, system_set(SETTINGS_CPU, "clock", mode) < 0, SETTINGS_CPU))
        return;
    char out[512];
    char *argv[] = { "power", "clock", 0 };      /* (69a: on every core at once) */
    run_root(argv, out, sizeof(out));
    widget_set_text(clock_status, out);
}

static struct widget *processor_page(void)
{
    char c[16];
    if (config_get(SETTINGS_CPU, "clock", c, sizeof(c)) != 1)
        snprintf(c, sizeof(c), "performance");
    static const char *const labels[] = {
        "Performance: it chooses, favouring speed", "Balanced: it chooses, balancing speed and power",
        "Power saving: it chooses, favouring low power", "Top: its highest clock whenever it runs",
        "Firmware: the firmware's own settings (from the next start)",
    };
    struct widget *p = widget_column();
    widget_add(p, widget_label("The processor's clock (AMD processors with CPPC)"));
    int chosen = 0;
    for (int i = 0; i < 5; i++)
        if (!strcmp(c, clock_modes[i]))
            chosen = i;
    for (int i = 0; i < 5; i++) {
        clock_radio[i] = widget_radio(labels[i], i ? clock_radio[0] : 0, i == chosen);
        widget_add(p, clock_radio[i]);
    }
    struct widget *save = widget_button("Save");
    widget_on(save, clock_save, 0);
    widget_add(p, save);
    clock_status = widget_label("");
    widget_add(p, clock_status);
    widget_set_text(p, "Processor");
    return p;
}

/* -- (69e) the network -- */

/* text split at spaces into argv after its first `skip` places (at most
 * max - 1 words, then 0); text is cut up in place. */
static void words(char *text, char **argv, int skip, int max)
{
    int n = skip;
    for (char *t = strtok(text, " \t"); t && n < max - 1; t = strtok(0, " \t"))
        argv[n++] = t;
    argv[n] = 0;
}

/* Run argv (root's), its output shown on status; the list given (when
 * not 0) filled again from refill. */
static void act(char **argv, struct widget *status, struct widget *list, char **refill)
{
    char out[1024];
    run_root(argv, out, sizeof(out));
    widget_set_text(status, out[0] ? out : "Done.");
    if (list && refill)
        lines_into(list, refill);
}

/* (69h) The cards there are settings for (their files in
 * /system/config/network), in name order: count, at most max. */
#define CARDS 8
static int card_names(char names[][32], int max)
{
    int n = 0;
    DIR *d = opendir(CONFIG_DIR "/network");
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), CONFIG_DIR "/network/%s", e->d_name);
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "names") || !strcmp(e->d_name, "virtual") ||
            stat(path, &st) < 0 || !S_ISREG(st.st_mode) || strlen(e->d_name) >= 32)
            continue;
        int at = n++;                           /* (in order: readdir's is the folder's) */
        while (at > 0 && strcmp(names[at - 1], e->d_name) > 0) {
            memcpy(names[at], names[at - 1], 32);
            at--;
        }
        snprintf(names[at], 32, "%s", e->d_name);
    }
    if (d)
        closedir(d);
    return n;
}

/* Is name a word of the list (words apart by spaces or commas)? */
static bool listed_in(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *c = list; *c;) {
        c += strspn(c, " ,\t");
        size_t k = strcspn(c, " ,\t");
        if (k == n && !strncmp(c, name, n))
            return true;
        c += k;
    }
    return false;
}

/* The checked cards' names, apart by spaces, into out. */
static void checked_cards(struct widget *const *box, char names[][32], int n, char *out, size_t size)
{
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++)
        if (widget_value(box[i]))
            at += (size_t)snprintf(out + at, size - at, "%s%s", at ? " " : "", names[i]);
}

/* Network cards */

static struct widget *cards, *card_field[6], *cards_status;
static const char *const card_keys[6] = { "address", "gateway", "dns", "address6", "gateway6", "dns6" };
static char card_now[32];

static void cards_fill(void)
{
    char text[1024] = "";
    size_t n = 0;
    DIR *d = opendir(CONFIG_DIR "/network");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), CONFIG_DIR "/network/%s", e->d_name);
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "names") || !strcmp(e->d_name, "virtual") ||
            stat(path, &st) < 0 || !S_ISREG(st.st_mode) || n > sizeof(text) - 40)
            continue;
        n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%s", n ? "\n" : "", e->d_name);
    }
    if (d)
        closedir(d);
    widget_set_text(cards, text);
}

static void card_chosen(struct widget *w, void *data)
{
    (void)data;
    snprintf(card_now, sizeof(card_now), "%s", widget_text(w));
    char path[300], value[128];
    snprintf(path, sizeof(path), CONFIG_DIR "/network/%s", card_now);
    for (int i = 0; i < 6; i++)
        widget_set_text(card_field[i], config_get(path, card_keys[i], value, sizeof(value)) == 1 ? value : "");
}

static void card_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    if (!may_change())
        return;
    if (!card_now[0]) {
        widget_set_text(cards_status, "Choose a card first.");
        return;
    }
    char path[300], before[128] = "";
    snprintf(path, sizeof(path), CONFIG_DIR "/network/%s", card_now);
    config_get(path, "address", before, sizeof(before));
    for (int i = 0; i < 6; i++) {
        const char *v = widget_text(card_field[i]);
        if (strpbrk(v, " \t=#")) {
            widget_message(win, "Network cards", "An address is one word: dhcp, 10.0.2.15/24, "
                           "2001:db8::5/64, auto, off.");
            return;
        }
    }
    for (int i = 0; i < 6; i++) {
        const char *v = widget_text(card_field[i]);
        if (system_set(path, card_keys[i], *v ? v : 0) < 0) {
            saved(cards_status, 1, path);
            return;
        }
    }
    char *up[] = { "net", "up", 0 };
    act(up, cards_status, 0, 0);
    bool was = !strcmp(before, "dhcp") || !before[0], now = !strcmp(widget_text(card_field[0]), "dhcp");
    if (was != now) {                           /* (dhclient takes a card's way at its start) */
        char *restart[] = { "elf", "restart", "dhclient", 0 };
        char out[256];
        run_root(restart, out, sizeof(out));
    }
}

static struct widget *cards_page(void)
{
    static const char *const labels[6] = { "Address", "Gateway", "DNS server", "IPv6 address",
                                           "IPv6 gateway", "IPv6 DNS server" };
    struct widget *p = widget_column(), *row = widget_row(), *grid = widget_grid(2);
    widget_add(p, widget_label("A network card's settings (choose one), applied by net up\n"
                               "(dhcp or ADDRESS/N; IPv6 auto, dhcp, off or ADDRESS/N)"));
    cards = widget_list("");
    cards_fill();
    widget_on(cards, card_chosen, 0);
    struct widget *left = widget_column();      /* (a container: at the top) */
    widget_add(left, cards);
    widget_add(row, left);
    for (int i = 0; i < 6; i++) {
        card_field[i] = widget_editor("", 1);
        widget_add(grid, widget_label(labels[i]));
        widget_add(grid, card_field[i]);
    }
    widget_add(row, grid);
    widget_add(p, row);
    struct widget *save = widget_button("Save");
    widget_on(save, card_save, 0);
    widget_add(p, save);
    cards_status = widget_label("");
    widget_add(p, cards_status);
    widget_set_text(p, "Network cards");
    return p;
}

/* Routing */

static struct widget *forward_on, *masq[CARDS], *dflt[CARDS], *routing_status;
static char routing_cards[CARDS][32];
static int routing_n;

static void routing_save(struct widget *w, void *data)
{
    (void)w, (void)data;
    if (!may_change())
        return;
    char m[300], d[300], out[512], all[1024];
    checked_cards(masq, routing_cards, routing_n, m, sizeof(m));
    checked_cards(dflt, routing_cards, routing_n, d, sizeof(d));
    char *fw[] = { "net", "forward", widget_value(forward_on) ? "on" : "off", 0 };
    run_root(fw, out, sizeof(out));
    snprintf(all, sizeof(all), "%s", out);
    char *ma[16] = { "net", "masquerade" };
    words(m, ma, 2, 16);
    if (!ma[2])
        ma[2] = "off", ma[3] = 0;
    run_root(ma, out, sizeof(out));
    snprintf(all + strlen(all), sizeof(all) - strlen(all), "\n%s", out);
    char *de[16] = { "net", "default" };
    words(d, de, 2, 16);
    if (!de[2])
        de[2] = "auto", de[3] = 0;
    run_root(de, out, sizeof(out));
    snprintf(all + strlen(all), sizeof(all) - strlen(all), "\n%s", out);
    widget_set_text(routing_status, all);
}

static struct widget *routing_page(void)
{
    char f[16] = "", m[128] = "", d[128] = "";
    config_get(SETTINGS_ROUTING, "forward", f, sizeof(f));
    config_get(SETTINGS_ROUTING, "masquerade", m, sizeof(m));
    config_get(SETTINGS_ROUTING, "default", d, sizeof(d));
    struct widget *p = widget_column(), *mr = widget_row(), *dr = widget_row();
    forward_on = widget_check("Pass packets on between the cards (a router)", !strcmp(f, "on"));
    widget_add(p, forward_on);
    routing_n = card_names(routing_cards, CARDS);       /* (69h: the cards there are) */
    widget_add(p, widget_label("Private cards: their packets go out with this machine's\n"
                               "address (masquerade; none checked: none)"));
    for (int i = 0; i < routing_n; i++) {
        masq[i] = widget_check(routing_cards[i], listed_in(m, routing_cards[i]));
        widget_add(mr, masq[i]);
    }
    widget_add(p, mr);
    widget_add(p, widget_label("The cards the default route prefers, in the cards' order\n"
                               "(none checked: the first with a gateway)"));
    for (int i = 0; i < routing_n; i++) {
        dflt[i] = widget_check(routing_cards[i], strcmp(d, "auto") && listed_in(d, routing_cards[i]));
        widget_add(dr, dflt[i]);
    }
    widget_add(p, dr);
    struct widget *save = widget_button("Save");
    widget_on(save, routing_save, 0);
    widget_add(p, save);
    routing_status = widget_label("");
    widget_add(p, routing_status);
    widget_set_text(p, "Routing");
    return p;
}

/* Virtual cards */

static struct widget *virtuals, *tap_user, *bridge_ports[CARDS], *virtual_status;
static char port_cards[CARDS][32];
static int port_n;

static void virtual_fill(void)
{
    char text[2048] = "";
    size_t n = 0;
    FILE *f = fopen(SETTINGS_VIRTUAL, "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] && line[0] != '#' && n < sizeof(text) - 260)
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%s", n ? "\n" : "", line);
    }
    if (f)
        fclose(f);
    widget_set_text(virtuals, text);
}

static void virtual_do(struct widget *w, void *data)
{
    (void)w;
    if (!may_change())
        return;
    int what = (int)(long)data;                 /* 0 a tap, 1 a bridge, 2 remove the chosen */
    char text[160], *argv[16] = { "net" };
    if (what == 0) {
        snprintf(text, sizeof(text), "%s", widget_text(tap_user));
        argv[1] = "tap", argv[2] = "add", argv[3] = 0;
        if (text[0])
            argv[3] = "user", argv[4] = text, argv[5] = 0;
    } else if (what == 1) {
        checked_cards(bridge_ports, port_cards, port_n, text, sizeof(text));
        argv[1] = "bridge", argv[2] = "add";
        words(text, argv, 3, 16);
        if (!argv[3]) {
            widget_message(win, "Virtual cards", "A bridge needs its ports: check them.");
            return;
        }
    } else {
        snprintf(text, sizeof(text), "%.*s", (int)strcspn(widget_text(virtuals), " \t="), widget_text(virtuals));
        if (!text[0]) {
            widget_set_text(virtual_status, "Choose a card first.");
            return;
        }
        argv[1] = !strncmp(text, "br", 2) ? "bridge" : "tap", argv[2] = "remove", argv[3] = text, argv[4] = 0;
    }
    act(argv, virtual_status, 0, 0);
    virtual_fill();
}

static struct widget *virtual_page(void)
{
    struct widget *p = widget_column(), *tr = widget_row(), *br = widget_row();
    widget_add(p, widget_label("Virtual cards (tapN) and bridges (brN), made at every start"));
    virtuals = widget_list("");
    virtual_fill();
    widget_add(p, virtuals);
    struct widget *remove = widget_button("Remove the chosen");
    widget_on(remove, virtual_do, (void *)2L);
    widget_add(p, remove);
    tap_user = widget_editor("", 1);
    struct widget *tap = widget_button("Add a tap");
    widget_on(tap, virtual_do, 0);
    widget_add(tr, widget_label("Its user (empty: root)"));
    widget_add(tr, tap_user);
    widget_add(tr, tap);
    widget_add(p, tr);
    struct widget *bridge = widget_button("Add a bridge");
    widget_on(bridge, virtual_do, (void *)1L);
    widget_add(br, widget_label("Its ports"));
    char all[CARDS][32];                        /* (69h: the wired cards and taps there are) */
    int n = card_names(all, CARDS);
    port_n = 0;
    for (int i = 0; i < n; i++)
        if (strncmp(all[i], "br", 2) && strncmp(all[i], "wlan", 4)) {    /* (no bridge in a bridge, no Wi-Fi) */
            memcpy(port_cards[port_n], all[i], 32);
            bridge_ports[port_n] = widget_check(port_cards[port_n], 0);
            widget_add(br, bridge_ports[port_n++]);
        }
    widget_add(br, bridge);
    widget_add(p, br);
    virtual_status = widget_label("");
    widget_add(p, virtual_status);
    widget_set_text(p, "Virtual cards");
    return p;
}

/* Card names */

static struct widget *names, *names_status;

static void names_fill(void)
{
    char text[2048] = "";
    size_t n = 0;
    FILE *f = fopen(CONFIG_DIR "/network/names", "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] && line[0] != '#' && n < sizeof(text) - 260)
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%s", n ? "\n" : "", line);
    }
    if (f)
        fclose(f);
    widget_set_text(names, text);
}

static void name_forget(struct widget *w, void *data)
{
    (void)w, (void)data;
    if (!may_change())
        return;
    char name[32];
    snprintf(name, sizeof(name), "%.*s", (int)strcspn(widget_text(names), " \t="), widget_text(names));
    if (!name[0]) {
        widget_set_text(names_status, "Choose a card first.");
        return;
    }
    char *argv[] = { "net", "forget", name, 0 };
    act(argv, names_status, 0, 0);
    names_fill();
}

static struct widget *names_page(void)
{
    struct widget *p = widget_column();
    widget_add(p, widget_label("The cards' fixed names, by their MAC address (netnamesd adds\n"
                               "new cards); a forgotten name is given again"));
    names = widget_list("");
    names_fill();
    widget_add(p, names);
    struct widget *forget = widget_button("Forget the chosen");
    widget_on(forget, name_forget, 0);
    widget_add(p, forget);
    names_status = widget_label("");
    widget_add(p, names_status);
    widget_set_text(p, "Card names");
    return p;
}

/* Wi-Fi */

static struct widget *country, *powersave, *networks, *ssid, *passphrase, *wifi_open, *wifi_status;

static void networks_fill(void)
{
    char text[2048] = "";
    size_t n = 0;
    DIR *d = opendir(CONFIG_DIR "/wifi/networks");
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.' && n < sizeof(text) - 260)
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%s", n ? "\n" : "", e->d_name);
    if (d)
        closedir(d);
    widget_set_text(networks, text);
}

static void wifi_do(struct widget *w, void *data)
{
    (void)w;
    if (!may_change())
        return;
    int what = (int)(long)data;                 /* 0 the radio, 1 join, 2 forget */
    char a[64], b[128], out[512];
    if (what == 0) {
        snprintf(a, sizeof(a), "%s", widget_text(country));
        if (strlen(a) != 2 || strspn(a, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != 2) {
            widget_message(win, "Wi-Fi", "A country is two letters (US, DE), or 00 for the world.");
            return;
        }
        char *cc[] = { "wifi", "country", a, 0 };
        run_root(cc, out, sizeof(out));
        if (system_set(SETTINGS_WIFI_RADIO, "powersave", widget_value(powersave) ? "on" : "off") < 0)
            snprintf(out, sizeof(out), "Not saved: %s: %s", SETTINGS_WIFI_RADIO, strerror(errno));
        else {
            char *restart[] = { "elf", "restart", "wifid", 0 };    /* (powersave: at its start) */
            char more[256];
            run_root(restart, more, sizeof(more));
        }
        widget_set_text(wifi_status, out);
    } else if (what == 1) {
        snprintf(a, sizeof(a), "%s", widget_text(ssid));
        snprintf(b, sizeof(b), "%s", widget_text(passphrase));
        if (!a[0] || (!b[0] && !widget_value(wifi_open))) {
            widget_message(win, "Wi-Fi", "A network's name, and its passphrase (or Open: a network "
                           "without one).");
            return;
        }
        char *join[] = { "wifi", "join", a, widget_value(wifi_open) ? 0 : b, 0 };   /* (no passphrase: open) */
        act(join, wifi_status, 0, 0);
        widget_set_text(passphrase, "");
        networks_fill();
    } else {
        snprintf(a, sizeof(a), "%s", widget_text(networks));
        if (!a[0]) {
            widget_set_text(wifi_status, "Choose a network first.");
            return;
        }
        char *forget[] = { "wifi", "forget", a, 0 };
        act(forget, wifi_status, 0, 0);
        networks_fill();
    }
}

/* (69h, the user's: choices from a list) The networks in range, from
 * `wifi scan` ("wifi: NAME   wpa2 channel 6   -39 dBm"; a name may hold
 * spaces: it ends where the security word does); choosing one fills the
 * network's name and Open. */
#define SCANNED 32
static char scan_ssid[SCANNED][33];
static bool scan_open[SCANNED];
static int scanned;
static struct widget *in_range;

static void wifi_scan(struct widget *w, void *data)
{
    (void)w, (void)data;
    char out[4096], rows[4096] = "", *argv[] = { "wifi", "scan", 0 };
    run(argv, out, sizeof(out));
    scanned = 0;
    size_t n = 0;
    char last[256] = "";
    for (char *line = strtok(out, "\n"); line; line = strtok(0, "\n")) {
        snprintf(last, sizeof(last), "%s", line);
        if (strncmp(line, "wifi: ", 6) || scanned == SCANNED)
            continue;
        char *name = line + 6, *sec = 0;
        static const char *const kinds[] = { " open channel", " wpa2 channel", " wpa3 channel" };
        for (int k = 0; k < 3; k++)
            for (char *at = strstr(name, kinds[k]); at; at = strstr(at + 1, kinds[k]))
                sec = at > sec ? at : sec;
        if (!sec)
            continue;
        size_t len = (size_t)(sec - name);
        while (len && name[len - 1] == ' ')
            len--;
        if (!len || len > 32)
            continue;
        snprintf(scan_ssid[scanned], sizeof(scan_ssid[0]), "%.*s", (int)len, name);
        scan_open[scanned] = !strncmp(sec, " open", 5);
        char *rest = sec + 1, info[80];
        size_t k = 0;
        for (char *c = rest; *c && k < sizeof(info) - 1; c++)      /* (its spaces made one) */
            if (*c != ' ' || (k && info[k - 1] != ' '))
                info[k++] = *c;
        info[k] = 0;
        n += (size_t)snprintf(rows + n, sizeof(rows) - n, "%s%s  (%s)", n ? "\n" : "", scan_ssid[scanned], info);
        scanned++;
    }
    widget_set_text(in_range, rows);
    widget_set_text(wifi_status, scanned ? "" : last);    /* (no card, joined: wifi says so) */
}

static void scan_chosen(struct widget *w, void *data)
{
    (void)data;
    int i = widget_value(w);
    if (i < 0 || i >= scanned)
        return;
    widget_set_text(ssid, scan_ssid[i]);
    widget_set_value(wifi_open, scan_open[i]);
}

static struct widget *wifi_page(void)
{
    char cc[8] = "00", ps[8] = "off";
    config_get(SETTINGS_WIFI_RADIO, "country", cc, sizeof(cc));
    config_get(SETTINGS_WIFI_RADIO, "powersave", ps, sizeof(ps));
    struct widget *p = widget_column(), *cr = widget_row(), *jr = widget_grid(2), *jr2 = widget_row();
    country = widget_editor(cc, 1);
    widget_add(cr, widget_label("Country"));
    widget_add(cr, country);
    powersave = widget_check("Power saving", !strcmp(ps, "on"));
    widget_add(cr, powersave);
    struct widget *radio = widget_button("Save");
    widget_on(radio, wifi_do, 0);
    widget_add(cr, radio);
    widget_add(p, cr);
    struct widget *lists = widget_row(), *saved_col = widget_column(), *range_col = widget_column();
    widget_add(saved_col, widget_label("Saved (wifid joins them)"));
    networks = widget_list("");
    networks_fill();
    widget_add(saved_col, networks);
    struct widget *forget = widget_button("Forget the chosen");
    widget_on(forget, wifi_do, (void *)2L);
    widget_add(saved_col, forget);
    widget_add(range_col, widget_label("In range (choose one)"));
    in_range = widget_list("");
    widget_on(in_range, scan_chosen, 0);
    widget_add(range_col, in_range);
    struct widget *scan = widget_button("Scan");
    widget_on(scan, wifi_scan, 0);
    widget_add(range_col, scan);
    widget_add(lists, saved_col);
    widget_add(lists, range_col);
    widget_add(p, lists);
    ssid = widget_editor("", 1);
    passphrase = widget_editor("", 1);
    widget_add(jr, widget_label("Network"));
    widget_add(jr, ssid);
    widget_add(jr, widget_label("Passphrase"));
    widget_add(jr, passphrase);
    widget_add(p, jr);
    wifi_open = widget_check("Open (no passphrase)", 0);
    struct widget *join = widget_button("Join");
    widget_on(join, wifi_do, (void *)1L);
    widget_add(jr2, wifi_open);
    widget_add(jr2, join);
    widget_add(p, jr2);
    wifi_status = widget_label("");
    widget_add(p, wifi_status);
    widget_set_text(p, "Wi-Fi");
    return p;
}

/* Firewall */

static struct widget *rules, *rule, *in_drop, *out_drop, *fwd_drop, *guard_status;
static char *const guard_show[] = { "guard", 0 };

/* The rules shown again once guardd loaded the change (it looks every 2
 * s; `guard` lists what it loaded): 3 s at most. */
static void rules_follow(const char *before)
{
    char now[4096];
    for (int i = 0; i < 30; i++) {
        usleep(100000);
        lines_text((char **)guard_show, now, sizeof(now));
        if (strcmp(now, before))
            break;
    }
    widget_set_text(rules, now);
}

static void guard_do(struct widget *w, void *data)
{
    (void)w;
    if (!may_change())
        return;
    int what = (int)(long)data;                 /* 0 add, 1 remove the chosen, 2 the defaults */
    char text[200], out[512], *argv[24] = { "guard" }, before[4096];
    lines_text((char **)guard_show, before, sizeof(before));
    if (what == 0) {
        snprintf(text, sizeof(text), "%s", widget_text(rule));
        argv[1] = "add";
        words(text, argv, 2, 24);
        if (!argv[2]) {
            widget_message(win, "Firewall", "A rule: allow in tcp 8080 from 192.168.0.0/24.");
            return;
        }
        act(argv, guard_status, 0, 0);
        rules_follow(before);
        widget_set_text(rule, "");
    } else if (what == 1) {
        const char *row = widget_text(rules);
        snprintf(text, sizeof(text), "%.*s", (int)strspn(row + strspn(row, " "), "0123456789"),
                 row + strspn(row, " "));
        if (!text[0]) {
            widget_set_text(guard_status, "Choose a numbered rule first.");
            return;
        }
        argv[1] = "remove", argv[2] = text, argv[3] = 0;
        act(argv, guard_status, 0, 0);
        rules_follow(before);
    } else {
        static const char *const way[3] = { "in", "out", "forward" };
        struct widget *drop[3] = { in_drop, out_drop, fwd_drop };
        size_t n = 0;
        for (int i = 0; i < 3; i++) {
            char *d[] = { "guard", (char *)way[i], widget_value(drop[i]) ? "drop" : "allow", 0 };
            run_root(d, out, sizeof(out));
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%s", n ? "\n" : "", out);
        }
        widget_set_text(guard_status, text);
        lines_into(rules, (char **)guard_show);
    }
}

static struct widget *firewall_page(void)
{
    char out[256];
    run((char **)guard_show, out, sizeof(out));         /* "firewall: on; in = drop, out = allow, ..." */
    struct widget *p = widget_column(), *ar = widget_row(), *dr = widget_row();
    widget_add(p, widget_label("The firewall's rules: the first that matches decides"));
    rules = widget_list("");
    lines_into(rules, (char **)guard_show);
    widget_add(p, rules);
    struct widget *remove = widget_button("Remove the chosen");
    widget_on(remove, guard_do, (void *)1L);
    widget_add(p, remove);
    rule = widget_editor("", 1);
    widget_stretch(rule, 1);
    struct widget *add = widget_button("Add");
    widget_on(add, guard_do, 0);
    widget_add(ar, rule);
    widget_add(ar, add);
    widget_add(p, ar);
    widget_add(p, widget_label("Without a matching rule: drop (checked) or allow"));
    in_drop = widget_check("in", strstr(out, "in = drop") != 0);
    out_drop = widget_check("out", strstr(out, "out = drop") != 0);
    fwd_drop = widget_check("forward", strstr(out, "forward = drop") != 0);
    struct widget *defaults = widget_button("Save");
    widget_on(defaults, guard_do, (void *)2L);
    widget_add(dr, in_drop);
    widget_add(dr, out_drop);
    widget_add(dr, fwd_drop);
    widget_add(dr, defaults);
    widget_add(p, dr);
    guard_status = widget_label("");
    widget_add(p, guard_status);
    widget_set_text(p, "Firewall");
    return p;
}

/* -- (69f) Users -- */

static struct widget *users, *user_name, *user_pw, *user_pw2, *sudo_users, *sudo_nopw, *users_status;

static void users_fill(void)
{
    char text[2048] = "";
    size_t n = 0;
    FILE *f = fopen(CONFIG_DIR "/users", "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || !strchr(line, ':') || n > sizeof(text) - 40)
            continue;
        n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%.*s", n ? "\n" : "",
                              (int)strcspn(line, ":"), line);
    }
    if (f)
        fclose(f);
    widget_set_text(users, text);
}

/* A name as useradd takes one: small letters and digits, a letter first. */
static bool user_name_ok(const char *name)
{
    return name[0] >= 'a' && name[0] <= 'z' && strlen(name) <= USER_NAME_MAX &&
           strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_-") == strlen(name);
}

static void user_do(struct widget *w, void *data)
{
    (void)w;
    if (!may_change())
        return;
    int what = (int)(long)data;                 /* 0 add, 1 the chosen one's password, 2 sudo's lists */
    char out[512], name[USER_NAME_MAX + 1], input[300];
    if (what == 2) {
        const char *u = widget_text(sudo_users), *np = widget_text(sudo_nopw);
        if (strpbrk(u, "=#") || strpbrk(np, "=#")) {
            widget_message(win, "Users", "Names apart by commas: anna, bob.");
            return;
        }
        saved(users_status, system_set(SETTINGS_SUDO, "users", u) < 0 ||
                            system_set(SETTINGS_SUDO, "nopassword", *np ? np : 0) < 0, SETTINGS_SUDO);
        return;
    }
    snprintf(name, sizeof(name), "%s", what == 0 ? widget_text(user_name) : widget_text(users));
    const char *a = widget_text(user_pw), *b = widget_text(user_pw2);
    if (!user_name_ok(name) || !*a || strcmp(a, b) || strchr(a, '\n')) {
        widget_message(win, "Users", what == 0 ? "A name (small letters and digits, a letter first) "
                       "and its password twice, the same." : "Choose a user, and give the "
                       "password twice, the same.");
        return;
    }
    if (what == 0) {
        char *add[] = { "useradd", name, 0 };
        if (run_root(add, out, sizeof(out)) < 0) {
            widget_set_text(users_status, out);
            return;
        }
    }
    snprintf(input, sizeof(input), "%s\n%s\n", a, b);
    char *pw[] = { "passwd", "-S", name, 0 };
    run_root_in(pw, input, out, sizeof(out));
    memset(input, 0, sizeof(input));
    widget_set_text(user_pw, "");
    widget_set_text(user_pw2, "");
    widget_set_text(users_status, out);
    users_fill();
}

static struct widget *users_page(void)
{
    char u[256] = "", np[256] = "";
    config_get(SETTINGS_SUDO, "users", u, sizeof(u));
    config_get(SETTINGS_SUDO, "nopassword", np, sizeof(np));
    struct widget *p = widget_column(), *grid = widget_grid(2), *buttons = widget_row(), *sg = widget_grid(2);
    widget_add(p, widget_label("The users (choose one to set its password)"));
    users = widget_list("");
    users_fill();
    widget_add(p, users);
    user_name = widget_editor("", 1);
    user_pw = widget_editor("", 1);
    user_pw2 = widget_editor("", 1);
    widget_add(grid, widget_label("New user"));
    widget_add(grid, user_name);
    widget_add(grid, widget_label("Password"));
    widget_add(grid, user_pw);
    widget_add(grid, widget_label("Again"));
    widget_add(grid, user_pw2);
    widget_add(p, grid);
    struct widget *add = widget_button("Add the user"), *set = widget_button("Set the chosen's password");
    widget_on(add, user_do, 0);
    widget_on(set, user_do, (void *)1L);
    widget_add(buttons, add);
    widget_add(buttons, set);
    widget_add(p, buttons);
    sudo_users = widget_editor(u, 1);
    sudo_nopw = widget_editor(np, 1);
    widget_add(sg, widget_label("May use sudo"));
    widget_add(sg, sudo_users);
    widget_add(sg, widget_label("sudo never asks"));
    widget_add(sg, sudo_nopw);
    widget_add(p, sg);
    struct widget *save = widget_button("Save");
    widget_on(save, user_do, (void *)2L);
    widget_add(p, save);
    users_status = widget_label("");
    widget_add(p, users_status);
    widget_set_text(p, "Users");
    return p;
}

/* -- (69f) Unlock -- */

static struct widget *unlock_pw, *unlock_note;

static void unlock(struct widget *w, void *data)
{
    (void)w, (void)data;
    char out[256], input[140];
    snprintf(input, sizeof(input), "%s\n", widget_text(unlock_pw));
    char *argv[] = { "sudo", "-S", "true", 0 };
    int r = run_in(argv, input, out, sizeof(out));
    memset(input, 0, sizeof(input));
    if (r < 0) {
        widget_set_text(unlock_pw, "");
        widget_message(win, "Unlock", out[0] ? out : "sudo said no.");
        return;
    }
    snprintf(unlocked, sizeof(unlocked), "%s", widget_text(unlock_pw));
    is_unlocked = true;
    widget_set_text(unlock_pw, "");
    widget_set_text(unlock_note, "Unlocked");
}

/* --- the window ---------------------------------------------------------------- */

static void page_chosen(struct widget *w, void *data)
{
    if (widget_value(w) >= 0)
        widget_set_value((struct widget *)data, widget_value(w));
}

/* (69f) `settings set FILE KEY [VALUE]`: one line of a file in
 * /system/config (the System pages' writes after Unlock, through sudo). */
static int set_command(int argc, char **argv)
{
    if (argc != 4 && argc != 5) {
        dprintf(2, "usage: Settings [set FILE KEY [VALUE]]\n");
        return 2;
    }
    const char *path = argv[2], *key = argv[3], *value = argc == 5 ? argv[4] : 0;
    if (strncmp(path, CONFIG_DIR "/", strlen(CONFIG_DIR) + 1) || strstr(path, "..") ||
        !key[0] || strpbrk(key, " \t=#\n") || (value && strchr(value, '\n'))) {
        dprintf(2, "settings: set: a file of %s, a key and its value (one line)\n", CONFIG_DIR);
        return 2;
    }
    if (settings_set(path, key, value) < 0) {
        dprintf(2, "settings: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (value)
        printf("settings: %s: %s = %s\n", path, key, value);
    else
        printf("settings: %s: %s's line taken out\n", path, key);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "set"))
        return set_command(argc, argv);
    if (argc != 1) {
        dprintf(2, "usage: Settings [set FILE KEY [VALUE]]\n");
        return 2;
    }
    struct passwd *pw = getpwuid(getuid());
    if (!pw) {
        printf("settings: who are you? (no user %d)\n", (int)getuid());
        return 1;
    }
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/.config", pw->pw_dir);
    mkdir(dir, 0700);
    snprintf(windows_file, sizeof(windows_file), "%s/" SETTINGS_USER_WINDOWS, pw->pw_dir);
    snprintf(filetypes_file, sizeof(filetypes_file), "%s/" SETTINGS_USER_FILETYPES, pw->pw_dir);
    snprintf(login_file, sizeof(login_file), "%s/.config/login", pw->pw_dir);
    win = window_open(640, 440, "Settings");
    if (!win) {
        if (errno == EACCES)                    /* (another user's windowd, 15k) */
            printf("settings: cannot open a window: the screen is another user's\n");
        else
            printf("settings: cannot open a window (is windowd running?)\n");
        return 1;
    }
    struct widget *root = widget_column(), *tabs = widget_tabs();
    /* (69d) System: the machine's settings */
    struct widget *sys = widget_row(), *sys_pages = widget_pages();
    struct widget *sys_list = widget_list("Display\nSound\nMemory\nDate & time\nElves\nService names\n"
                                          "Power\nProcessor\nNetwork cards\nRouting\nVirtual cards\n"
                                          "Card names\nWi-Fi\nFirewall\nUsers");
    widget_add(sys_pages, display_page());
    widget_add(sys_pages, sound_page());
    widget_add(sys_pages, memory_page());
    widget_add(sys_pages, time_page());
    widget_add(sys_pages, elves_page());
    widget_add(sys_pages, services_page());
    widget_add(sys_pages, power_page());
    widget_add(sys_pages, processor_page());
    widget_add(sys_pages, cards_page());        /* (69e) */
    widget_add(sys_pages, routing_page());
    widget_add(sys_pages, virtual_page());
    widget_add(sys_pages, names_page());
    widget_add(sys_pages, wifi_page());
    widget_add(sys_pages, firewall_page());
    widget_add(sys_pages, users_page());        /* (69f) */
    widget_set_value(sys_list, 0);
    widget_on(sys_list, page_chosen, sys_pages);
    struct widget *sys_left = widget_column();
    widget_add(sys_left, sys_list);
    if (getuid() != 0) {                        /* (69f) read only until Unlock */
        unlock_note = widget_label("Read only");    /* (one line: a label with a newline is 400 wide) */
        unlock_pw = widget_editor("", 1);
        struct widget *button = widget_button("Unlock");
        widget_on(button, unlock, 0);
        widget_add(sys_left, unlock_note);
        widget_add(sys_left, unlock_pw);
        widget_add(sys_left, button);
    }
    widget_add(sys, sys_left);
    widget_stretch(sys_pages, 1);
    widget_add(sys, sys_pages);
    widget_set_text(sys, "System");
    widget_add(tabs, sys);
    /* User: the user's own */
    struct widget *user = widget_row(), *pages = widget_pages();
    struct widget *list = widget_list("Appearance\nMouse & touchpad\nWindow system\nFile types\nAccount\n"
                                      "At login");
    widget_add(pages, appearance_page());
    widget_add(pages, mouse_page());
    widget_add(pages, winsys_page());
    widget_add(pages, types_page());
    widget_add(pages, account_page());          /* (69f) */
    widget_add(pages, login_page());
    widget_set_value(list, 0);
    widget_on(list, page_chosen, pages);
    struct widget *left = widget_column();      /* (a container: at the top, not centred) */
    widget_add(left, list);
    widget_add(user, left);
    widget_stretch(pages, 1);
    widget_add(user, pages);
    widget_set_text(user, "User");
    widget_add(tabs, user);
    widget_stretch(tabs, 1);
    widget_add(root, tabs);
    printf("settings: started\n");
    widget_run(win, root);
    memset(unlocked, 0, sizeof(unlocked));      /* (69f) */
    window_close(win);
    widget_free(root);
    return 0;
}
