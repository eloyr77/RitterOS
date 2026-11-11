/*
 * owl: a graphical file browser (step 17b, the user's; named for a keen-sighted
 * animal). It opens a window with libwindow and lists a directory: directories
 * first and marked, then files with their size. The arrow keys (or a click)
 * move the selection; Backspace or ".." goes up; Enter or a double click
 * opens it (windowd decides what is one). Opening by the rule the user set, told apart by where
 * the file lives:
 *   - a directory  -> go into it;
 *   - (42b) an app's bundle, a folder NAME.program holding its program NAME
 *     (anywhere) -> run NAME (it draws its own window);
 *   - any other executable -> open a terminal running it (term <path>);
 *   - a data file by its type: (41g-2) the program the user's file types
 *     (42a: ~/.config/filetypes, window_app_for) give its ending: .ppm
 *     .png... in imageview, .rvid in reel, .wav in tune in a terminal;
 *     anything else in the editor (term edit <path>).
 * It starts in the running user's home directory (or a directory named on the
 * command line: owl [DIR]). (41f-6) An entry pressed and moved is dragged
 * out: its full path is what the pointer carries to another window.
 * (41g-6) Names starting with a dot are not listed (Unix's hidden files).
 * (42b) A bundle is one item: its icon.png left of NAME, among the files;
 * Edit > Show contents enters it (a folder with an icon moves every name
 * right, to keep them aligned). (42c) File > Open with... (and the
 * right-click menu's) chooses an app for the selected file in libwindow's
 * dialog (window_choose), from /system/<arch>/apps or anywhere: only the
 * bundles whose info lists the file's ending (window_app_opens) can be
 * chosen.
 */
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "image.h"
#include "ritteros.h"
#include "window.h"

PROGRAM_HELP("owl [DIR]",
             "a graphical file browser (needs windowd)");

#if defined(__x86_64__)
#define ARCH "x86_64"
#else
#define ARCH "aarch64"
#endif

#define CW          9                   /* the font cell, 9x15 */
#define CH          15
#define WIN_W       (80 * CW)           /* 720 */
#define WIN_H       (28 * CH)           /* 420: one path line + 27 entries */
#define BTN_LEFT    1
#define BTN_RIGHT   2
#define MAX_ENTRIES 512

/* Menu item ids (menu bar and context menu). */
#define M_OPEN      1
#define M_UP        2
#define M_REFRESH   3
#define M_QUIT      4
#define F_NEW       10          /* File: New folder */
#define E_RENAME    11          /* Edit: Rename, Delete, Copy, Paste */
#define E_DELETE    12
#define E_COPY      13
#define E_PASTE     14
#define E_SHOW      15          /* (42b) Show contents: into an app's bundle */
#define F_WITH      16          /* (42c) Open with...: an app chosen for the file */
#define G_HOME      20          /* Go: Home, Apps, Root */
#define G_APPS      21
#define G_ROOT      22

/* A prompt along the top for a name to type, or a yes/no delete confirm. */
#define PR_NONE     0
#define PR_NEW      1
#define PR_RENAME   2
#define PR_DELETE   3

struct entry {
    char name[128];
    int is_dir;
    int is_exec;
    long size;
    int icon;                           /* (41g-6) its app's icon in icons[], or -1 */
    int is_app;                         /* (42b) an app's bundle NAME.program (shown as NAME) */
};

/* (41g-6) Apps' icons, as windowd shows them: 32x32 halved to 16x16 (each
 * pixel the alpha-weighted mean of four), drawn in a row's 15 pixels. */
#define ICON        16
#define ICON_ROOM   (ICON + 4)          /* what a folder with icons moves its names right */
struct icon {
    uint8_t rgba[ICON * ICON * 4];
};
static struct icon icons[32];
static int nicons;
static bool has_icons;                  /* the folder shown has an app with one */

static struct entry entries[MAX_ENTRIES];
static int nentries;
static int sel;                         /* the selected entry */
static int top;                         /* the first entry shown (scrolling) */
static char cwd[1024];                  /* the directory being shown */
static struct window *win;
static uint32_t c_bg, c_fg, c_dir, c_sel, c_path, c_prompt;
static uint8_t bg_rgb[3], sel_rgb[3];   /* (41g-6) the two a row's icon is blended over */
static uint8_t prev_buttons;
static int prompt_action;               /* PR_*: a prompt is up along the top */
static char prompt_text[128];           /* the name being typed */
static int prompt_len;
static char clip[1024];                 /* a path copied for Paste */
static char status[160];                /* a message (an error, or what Copy holds) */
static char home_dir[1024] = "/";       /* the running user's home, for the Go menu */

static int rows_visible(void)
{
    int r = window_height(win) / CH - 1; /* minus the path line */
    return r < 1 ? 1 : r;
}

/* Scrolling (15o): the keys and clicks move the view to keep the selection
 * in it (follow); the wheel and the scroll bar move the view alone. */
static bool follow = true;
static bool dragging_bar;               /* the scroll bar's thumb held */
static int press_idx = -1;              /* (41f-6) the entry pressed: dragged out once it moves */
static int press_x, press_y;
#define DRAG_PX     5                   /* that far (pixels, across and down added) */

static void scroll_to(int first)
{
    int max = nentries - rows_visible();
    first = first > max ? max : first;
    first = first < 0 ? 0 : first;
    follow = false;
    if (first != top) {
        top = first;
        printf("owl: showing rows %d-%d of %d\n", top + 1,
               top + rows_visible() < nentries ? top + rows_visible() : nentries, nentries);
    }
}

static void join(char *out, size_t size, const char *dir, const char *name)
{
    if (!strcmp(dir, "/"))
        snprintf(out, size, "/%s", name);
    else
        snprintf(out, size, "%s/%s", dir, name);
}

/* (42b) Whether the folder `name` in cwd is an app's bundle: NAME.program
 * holding its program NAME. */
static bool is_bundle(const char *name)
{
    size_t n = strlen(name);
    if (n <= 8 || strcmp(name + n - 8, ".program"))
        return false;
    char path[1024], inside[256];
    snprintf(inside, sizeof(inside), "%s/%.*s", name, (int)(n - 8), name);
    join(path, sizeof(path), cwd, inside);
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* (41g-6) The icon of the bundle `name` in cwd: (42b) its icon.png, loaded
 * and halved into icons[]; -1 if none. */
static int load_icon(const char *name)
{
    char inside[256], path[1024];
    snprintf(inside, sizeof(inside), "%s/icon.png", name);
    join(path, sizeof(path), cwd, inside);
    if (nicons == (int)(sizeof(icons) / sizeof(icons[0])))
        return -1;
    struct image img;
    if (image_load(path, &img) < 0)
        return -1;
    if (img.w != 2 * ICON || img.h != 2 * ICON) {
        struct image sized;
        int r = image_scale(&img, 2 * ICON, 2 * ICON, &sized);
        image_free(&img);
        if (r < 0)
            return -1;
        img = sized;
    }
    struct icon *ic = &icons[nicons];
    for (int y = 0; y < ICON; y++)
        for (int x = 0; x < ICON; x++) {
            unsigned r = 0, g = 0, b = 0, a = 0;
            for (int k = 0; k < 4; k++) {
                const uint8_t *p = img.pixels + (size_t)(2 * y + k / 2) * (size_t)img.stride +
                                   (size_t)(2 * x + k % 2) * 4;
                r += p[0] * p[3], g += p[1] * p[3], b += p[2] * p[3], a += p[3];
            }
            uint8_t *o = ic->rgba + (y * ICON + x) * 4;
            o[0] = (uint8_t)(a ? r / a : 0), o[1] = (uint8_t)(a ? g / a : 0);
            o[2] = (uint8_t)(a ? b / a : 0), o[3] = (uint8_t)(a / 4);
        }
    image_free(&img);
    return nicons++;
}

/* An icon at (x, y), blended over a row of colour back (its first CH rows:
 * the last is the icons' margin). */
static void draw_icon(int x, int y, const struct icon *ic, const uint8_t back[3])
{
    uint32_t *px = window_pixels(win);
    int w = window_width(win), h = window_height(win);
    for (int r = 0; r < CH && y + r < h; r++)
        for (int c = 0; c < ICON && x + c < w; c++) {
            const uint8_t *p = ic->rgba + (r * ICON + c) * 4;
            unsigned a = p[3];
            if (a)
                px[(y + r) * w + x + c] = window_rgb(win, (int)((p[0] * a + back[0] * (255 - a)) / 255),
                                                     (int)((p[1] * a + back[1] * (255 - a)) / 255),
                                                     (int)((p[2] * a + back[2] * (255 - a)) / 255));
        }
}

/* Read cwd into entries[]: ".." first (unless at the root), then the names,
 * directories before files and each group sorted. Resets the selection. */
static void list_dir(void)
{
    nentries = 0;
    nicons = 0;
    has_icons = false;
    sel = top = 0;
    follow = true;
    press_idx = -1;                              /* (a double click opened the pressed one) */
    if (strcmp(cwd, "/") != 0) {                 /* a way up, except at the root */
        snprintf(entries[0].name, sizeof(entries[0].name), "..");
        entries[0].is_dir = 1;
        entries[0].is_exec = 0;
        entries[0].size = 0;
        entries[0].icon = -1;
        entries[0].is_app = 0;
        nentries = 1;
    }
    int first = nentries;                        /* the real entries, to sort */
    DIR *d = opendir(cwd);
    struct dirent *e;
    while (d && nentries < MAX_ENTRIES && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;                            /* (41g-6) ".", "..", hidden files */
        struct entry *en = &entries[nentries];
        snprintf(en->name, sizeof(en->name), "%s", e->d_name);
        char path[1024];
        join(path, sizeof(path), cwd, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            en->is_dir = S_ISDIR(st.st_mode);
            en->is_exec = (st.st_mode & 0111) != 0;
            en->size = (long)st.st_size;
        } else {
            en->is_dir = en->is_exec = 0;
            en->size = 0;
        }
        en->is_app = en->is_dir && is_bundle(e->d_name);   /* (42b) one item, among the files */
        if (en->is_app)
            en->is_dir = 0;
        en->icon = en->is_app ? load_icon(e->d_name) : -1;
        has_icons |= en->icon >= 0;
        nentries++;
    }
    if (d)
        closedir(d);
    for (int i = first + 1; i < nentries; i++) { /* insertion sort: dirs first, then name */
        struct entry key = entries[i];
        int j = i - 1;
        while (j >= first && (entries[j].is_dir < key.is_dir ||
               (entries[j].is_dir == key.is_dir && strcmp(entries[j].name, key.name) > 0))) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

static void render(void)
{
    int w = window_width(win), h = window_height(win);
    window_fill(win, 0, 0, w, h, c_bg);
    if (prompt_action == PR_NONE) {
        window_text(win, 2, 0, cwd, c_path, WINDOW_TRANSPARENT); /* the path, at the top */
    } else {                                                    /* a prompt in its place */
        char line[300];
        if (prompt_action == PR_NEW)
            snprintf(line, sizeof(line), "New folder: %s_", prompt_text);
        else if (prompt_action == PR_RENAME)
            snprintf(line, sizeof(line), "Rename to: %s_", prompt_text);
        else
            snprintf(line, sizeof(line), "Delete %s? (y/n)",
                     sel >= 0 && sel < nentries ? entries[sel].name : "");
        window_text(win, 2, 0, line, c_prompt, WINDOW_TRANSPARENT);
    }
    int vis = rows_visible();
    if (follow && sel < top)
        top = sel;
    if (follow && sel >= top + vis)
        top = sel - vis + 1;
    if (top > nentries - vis)
        top = nentries - vis;
    if (top < 0)
        top = 0;
    int lw = w - WINDOW_SCROLLBAR_W;                             /* the list, then its bar */
    for (int i = 0; i < vis && top + i < nentries; i++) {
        int idx = top + i, y = (i + 1) * CH;
        struct entry *en = &entries[idx];
        if (idx == sel)
            window_fill(win, 0, y, lw, CH, c_sel);               /* the selection bar */
        if (en->icon >= 0)                                      /* (41g-6) an app's icon */
            draw_icon(2, y, &icons[en->icon], idx == sel ? sel_rgb : bg_rgb);
        char label[200];
        if (en->is_dir)
            snprintf(label, sizeof(label), "%s/", en->name);
        else if (en->is_app)                                    /* (42b) its name, no size */
            snprintf(label, sizeof(label), "%.*s", (int)strlen(en->name) - 8, en->name);
        else
            snprintf(label, sizeof(label), "%-40s %ld", en->name, en->size);
        window_text(win, has_icons ? 2 + ICON_ROOM : 2, y, label, en->is_dir ? c_dir : c_fg,
                    WINDOW_TRANSPARENT);
    }
    window_scrollbar(win, lw, CH, vis * CH, top, vis, nentries);
    if (status[0])                                              /* a message at the bottom */
        window_text(win, 2, h - CH, status, c_prompt, WINDOW_TRANSPARENT);
    window_flush(win);
}

static void navigate_to(const char *path)
{
    snprintf(cwd, sizeof(cwd), "%s", path);
    list_dir();
    render();
    printf("owl: at %s\n", cwd);        /* to the console/log */
}

static void go_up(void)
{
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", cwd);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        navigate_to("/");
    else {
        *slash = 0;
        navigate_to(parent);
    }
}

/* Open the selected entry (the user's rule, by where the file lives). */
static void open_entry(int idx)
{
    if (idx < 0 || idx >= nentries)
        return;
    struct entry *en = &entries[idx];
    if (en->is_dir) {
        if (!strcmp(en->name, ".."))
            go_up();
        else {
            char path[1024];
            join(path, sizeof(path), cwd, en->name);
            navigate_to(path);
        }
        return;
    }
    char path[1024];
    join(path, sizeof(path), cwd, en->name);
    char term[64];
    snprintf(term, sizeof(term), "/system/" ARCH "/apps/term.program/term");
    int fds[3] = { 0, 1, 2 };
    char name[64];
    int how = -1;
    if (en->is_app) {                                   /* (42b) a bundle: run its program */
        char program[1100];
        snprintf(name, sizeof(name), "%.*s", (int)strlen(en->name) - 8, en->name);
        snprintf(program, sizeof(program), "%s/%s", path, name);
        char *av[] = { name, 0 };
        ritteros_spawn(program, av, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
    } else if ((how = window_app_for(path, name, sizeof(name))) == 0) {   /* (41g-2) its app */
        char app[160];
        snprintf(app, sizeof(app), "/system/" ARCH "/apps/%s.program/%s", name, name);
        char *av[] = { name, path, 0 };
        ritteros_spawn(app, av, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
    } else if (how == 1) {                              /* one that runs in a terminal (tune) */
        char *av[] = { "term", name, path, 0 };
        ritteros_spawn(term, av, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
    } else if (en->is_exec && !strstr(path, ".program/")) {   /* a program: a terminal runs it
                                                         * (not a bundle's own files) */
        char *av[] = { "term", path, 0 };
        ritteros_spawn(term, av, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
    } else {                                            /* a data file: open it in the editor */
        char *av[] = { "term", "edit", path, 0 };
        ritteros_spawn(term, av, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
    }
}

/* Re-read the current directory, keeping the selection in range. */
static void refresh(void)
{
    int keep = sel;
    list_dir();
    sel = keep < nentries ? keep : nentries - 1;
    if (sel < 0)
        sel = 0;
    render();
}

static void set_status(const char *s) { snprintf(status, sizeof(status), "%s", s); }

/* The full path of the selected entry; 0 if none or "..". */
static int selected_path(char *out, size_t size)
{
    if (sel < 0 || sel >= nentries || !strcmp(entries[sel].name, ".."))
        return 0;
    join(out, size, cwd, entries[sel].name);
    return 1;
}

/* (42c) Open with...: the dialog lets only the apps that open the file be
 * chosen; the chosen bundle's program starts with it. */
static int app_opens_file(const char *bundle, void *file)
{
    return window_app_opens(bundle, file);
}

static void open_with(void)
{
    static char file[1024], app[1024];
    if (!selected_path(file, sizeof(file)) || entries[sel].is_dir || entries[sel].is_app)
        return;
    if (window_choose(win, "Open with", "/system/" ARCH "/apps", app_opens_file, file, app,
                      sizeof(app)) != 1)
        return;
    const char *base = strrchr(app, '/');
    base = base ? base + 1 : app;
    char name[64], program[1100];
    snprintf(name, sizeof(name), "%.*s", (int)strlen(base) - 8, base);
    snprintf(program, sizeof(program), "%s/%s", app, name);
    char *av[] = { name, file, 0 };
    int fds[3] = { 0, 1, 2 };
    printf("owl: open %s with %s\n", file, app);
    ritteros_spawn(program, av, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
}

static void prompt_start(int action, const char *initial)
{
    prompt_action = action;
    snprintf(prompt_text, sizeof(prompt_text), "%s", initial ? initial : "");
    prompt_len = (int)strlen(prompt_text);
    render();
}

/* A finished text prompt: make the folder, or rename the selected entry. */
static void prompt_done(void)
{
    if (prompt_len == 0) {
        prompt_action = PR_NONE;
        render();
        return;
    }
    char path[1024];
    if (prompt_action == PR_NEW) {
        join(path, sizeof(path), cwd, prompt_text);
        if (mkdir(path, 0755) < 0)
            set_status(strerror(errno));
        else {
            set_status("");
            printf("owl: new folder %s\n", path);      /* to the console/log */
        }
    } else if (prompt_action == PR_RENAME) {
        char from[1024];
        if (selected_path(from, sizeof(from))) {
            join(path, sizeof(path), cwd, prompt_text);
            set_status(rename(from, path) < 0 ? strerror(errno) : "");
        }
    }
    prompt_action = PR_NONE;
    refresh();
}

/* Delete the selected entry (a file, or an empty directory). */
static void do_delete(void)
{
    char path[1024];
    if (selected_path(path, sizeof(path)))
        set_status((entries[sel].is_dir ? rmdir(path) : unlink(path)) < 0 ? strerror(errno) : "");
    refresh();
}

/* Remember the selected entry's path for a later Paste. */
static void do_copy(void)
{
    if (selected_path(clip, sizeof(clip))) {
        char msg[200];
        snprintf(msg, sizeof(msg), "copied %s", clip);
        set_status(msg);
    }
    render();
}

/* Copy the remembered file/directory into the current directory. */
static void do_paste(void)
{
    if (!clip[0])
        return;
    const char *base = strrchr(clip, '/');
    base = base ? base + 1 : clip;
    char dest[1024], failed[256];
    join(dest, sizeof(dest), cwd, base);
    if (ritteros_copy(clip, dest) < 0 && ritteros_copy_bytes(clip, dest, failed, sizeof(failed)) < 0)
        set_status(strerror(errno));
    else
        set_status("");
    refresh();
}

/* A byte from the keyboard (arrow keys arrive as ESC [ A/B, like a terminal). */
static int esc;                         /* 0 normal, 1 after ESC, 2 after ESC [ */
static void key(int b)
{
    if (prompt_action) {                                /* typing a name, or a delete confirm */
        if (prompt_action == PR_DELETE) {
            prompt_action = PR_NONE;
            if (b == 'y' || b == 'Y')
                do_delete();
            else
                render();                               /* anything else cancels */
            return;
        }
        if (b == '\r' || b == '\n')
            prompt_done();
        else if (b == 0x1B) {
            prompt_action = PR_NONE;                    /* Escape: cancel */
            render();
        } else if (b == 0x7F || b == 0x08) {
            if (prompt_len > 0)
                prompt_text[--prompt_len] = 0;
            render();
        } else if (b >= 0x20 && b < 0x7F && prompt_len < (int)sizeof(prompt_text) - 1) {
            prompt_text[prompt_len++] = (char)b;
            prompt_text[prompt_len] = 0;
            render();
        }
        return;
    }
    if (esc == 1) {
        esc = b == '[' ? 2 : 0;
        return;
    }
    if (esc == 2) {
        esc = 0;
        if (b == 'A' && sel > 0) { sel--; follow = true; render(); }       /* up */
        else if (b == 'B' && sel < nentries - 1) { sel++; follow = true; render(); }   /* down */
        return;
    }
    if (b == 0x1B)
        esc = 1;
    else if (b == '\r' || b == '\n')
        open_entry(sel);
    else if (b == 0x7F || b == 0x08)
        go_up();
}

/* A double click (windowd's, the same rule for every program): open the
 * row under it. */
static void double_clicked(int x, int y)
{
    if (x >= window_width(win) - WINDOW_SCROLLBAR_W)
        return;                                         /* (on the scroll bar) */
    int row = y / CH, idx = top + row - 1;
    if (row >= 1 && idx >= 0 && idx < nentries)
        open_entry(idx);
}

static void pointer(int x, int y, unsigned buttons)
{
    int down = (buttons & BTN_LEFT) && !(prev_buttons & BTN_LEFT);
    int rdown = (buttons & BTN_RIGHT) && !(prev_buttons & BTN_RIGHT);
    prev_buttons = (uint8_t)buttons;
    int vis = rows_visible();
    if (press_idx >= 0) {                               /* (41f-6) a pressed entry moved: drag it */
        if (!(buttons & BTN_LEFT))
            press_idx = -1;
        else if (abs(x - press_x) + abs(y - press_y) >= DRAG_PX) {
            char path[1024];
            join(path, sizeof(path), cwd, entries[press_idx].name);
            window_drag(win, path, strlen(path));       /* (its full path: windowd carries it) */
            press_idx = -1;
        }
    }
    if (dragging_bar) {                                 /* the thumb held: follow the pointer */
        if (!(buttons & BTN_LEFT))
            dragging_bar = false;
        else {
            scroll_to(window_scrollbar_hit(CH, vis * CH, y, vis, nentries));
            render();
        }
        return;
    }
    if (down && x >= window_width(win) - WINDOW_SCROLLBAR_W && y >= CH) {
        dragging_bar = true;                            /* a press on the bar: jump, then drag */
        scroll_to(window_scrollbar_hit(CH, vis * CH, y, vis, nentries));
        render();
        return;
    }
    if (!down && !rdown)
        return;
    int row = y / CH;
    if (row < 1)                                        /* the path line */
        return;
    int idx = top + row - 1;
    if (idx < 0 || idx >= nentries)
        return;
    if (rdown) {                                        /* right-click: select, then a context menu */
        sel = idx;
        render();
        static const struct window_menu_item ctx[] = {
            { M_OPEN, "Open" }, { F_WITH, "Open with..." }, { E_SHOW, "Show contents" },
            { E_RENAME, "Rename" }, { E_DELETE, "Delete" }, { E_COPY, "Copy" },
        };
        window_popup_menu(win, x, y, ctx, 6);
    } else {                                            /* a click selects (a double click,
                                                         * windowd's, opens: double_clicked) */
        sel = idx;
        follow = true;
        render();
        if (strcmp(entries[idx].name, "..")) {          /* (41f-6) a drag may start here */
            press_idx = idx;
            press_x = x;
            press_y = y;
        }
    }
}

int main(int argc, char **argv)
{
    struct passwd *pw = getpwuid(getuid());             /* the running user's home */
    if (pw && pw->pw_dir)
        snprintf(home_dir, sizeof(home_dir), "%s", pw->pw_dir);
    if (argc > 1)
        snprintf(cwd, sizeof(cwd), "%s", argv[1]);
    else
        snprintf(cwd, sizeof(cwd), "%s", home_dir);
    struct stat st;
    if (stat(cwd, &st) != 0 || !S_ISDIR(st.st_mode))
        snprintf(cwd, sizeof(cwd), "/");

    win = window_open(WIN_W, WIN_H, "owl");
    if (!win) {
        if (errno == EACCES)                    /* (another user's windowd, 15k) */
            printf("owl: cannot open a window: the screen is another user's\n");
        else
            printf("owl: cannot open a window (is windowd running?)\n");
        return 1;
    }
    /* (41g-5) the user's theme: light when its background is */
    uint32_t back = window_color(WINDOW_COLOR_BACKGROUND), sel_color = window_color(WINDOW_COLOR_SELECTION);
    bool light = (back >> 16 & 0xFF) + (back >> 8 & 0xFF) + (back & 0xFF) > 3 * 128;
    c_sel = window_rgb(win, sel_color >> 16 & 0xFF, sel_color >> 8 & 0xFF, sel_color & 0xFF);  /* the selection bar */
    sel_rgb[0] = (uint8_t)(sel_color >> 16), sel_rgb[1] = (uint8_t)(sel_color >> 8), sel_rgb[2] = (uint8_t)sel_color;
    static const uint8_t light_bg[3] = { 0xE5, 0xE9, 0xF0 }, dark_bg[3] = { 0x36, 0x3D, 0x4A };
    memcpy(bg_rgb, light ? light_bg : dark_bg, 3);
    if (light) {
        c_bg = window_rgb(win, 0xE5, 0xE9, 0xF0);
        c_fg = window_rgb(win, 0x2E, 0x34, 0x40);
        c_dir = window_rgb(win, 0x4F, 0x6F, 0x3A);
        c_path = window_rgb(win, 0x8A, 0x63, 0x1C);
        c_prompt = window_rgb(win, 0x2E, 0x34, 0x40);
    } else {
        c_bg = window_rgb(win, 0x36, 0x3D, 0x4A);       /* a panel, distinct from the desktop */
        c_fg = window_rgb(win, 0xD8, 0xDE, 0xE9);
        c_dir = window_rgb(win, 0xA3, 0xBE, 0x8C);      /* directories, greenish */
        c_path = window_rgb(win, 0xEB, 0xCB, 0x8B);     /* the path, yellow */
        c_prompt = window_rgb(win, 0xEC, 0xEF, 0xF4);   /* a prompt / message, bright */
    }

    static const struct window_menu_item file_items[] = {
        { F_NEW, "New folder" }, { M_OPEN, "Open" }, { F_WITH, "Open with..." }, { M_UP, "Up" },
        { M_REFRESH, "Refresh" }, { M_QUIT, "Quit" },
    };
    static const struct window_menu_item edit_items[] = {
        { E_RENAME, "Rename" }, { E_DELETE, "Delete" }, { E_COPY, "Copy" }, { E_PASTE, "Paste" },
        { E_SHOW, "Show contents" },
    };
    static const struct window_menu_item go_items[] = {
        { G_HOME, "Home" }, { G_APPS, "Apps" }, { G_ROOT, "Root (/)" },
    };
    static const struct window_menu menubar[] = {
        { "File", file_items, 6 }, { "Edit", edit_items, 5 }, { "Go", go_items, 3 },
    };
    window_set_menu(win, menubar, 3);

    list_dir();
    render();
    printf("owl: started\n");

    struct window_event ev;
    while (window_wait(win, &ev)) {
        if (ev.type == WINDOW_KEY)
            key(ev.key);
        else if (ev.type == WINDOW_POINTER)
            pointer(ev.x, ev.y, ev.buttons);
        else if (ev.type == WINDOW_DOUBLE_CLICK)
            double_clicked(ev.x, ev.y);
        else if (ev.type == WINDOW_SCROLL) {             /* the wheel: 3 rows a step */
            scroll_to(top + 3 * ev.y);
            render();
        }
        else if (ev.type == WINDOW_RESIZE) {
            window_resize(win, ev.x, ev.y);
            render();
        } else if (ev.type == WINDOW_MENU) {
            char tmp[1024];
            switch (ev.menu) {
            case M_OPEN:    open_entry(sel); break;
            case F_WITH:    open_with(); render(); break;
            case M_UP:      go_up(); break;
            case M_REFRESH: refresh(); break;
            case M_QUIT:    window_close(win); return 0;
            case F_NEW:     prompt_start(PR_NEW, ""); break;
            case E_RENAME:  if (selected_path(tmp, sizeof(tmp)))
                                prompt_start(PR_RENAME, entries[sel].name);
                            break;
            case E_DELETE:  if (selected_path(tmp, sizeof(tmp)))
                                prompt_start(PR_DELETE, "");
                            break;
            case E_COPY:    do_copy(); break;
            case E_PASTE:   do_paste(); break;
            case E_SHOW:    if (selected_path(tmp, sizeof(tmp)) && entries[sel].is_app)
                                navigate_to(tmp);   /* (42b) inside the bundle */
                            break;
            case G_HOME:    navigate_to(home_dir); break;
            case G_APPS:    navigate_to("/system/" ARCH "/apps"); break;
            case G_ROOT:    navigate_to("/"); break;
            }
        }
    }
    window_close(win);
    return 0;
}
