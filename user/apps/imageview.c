/*
 * imageview [FILE]: show an image (PPM, BMP, PNG or JPEG) in a window. The image
 * is scaled to fit the window keeping its proportions (centred), so resizing
 * the window rescales it. Without a
 * file it opens an empty window; File > Open... picks one with the shared
 * dialog (window_choose_file), and the window takes the image's size. 'q',
 * File > Close or the close box quits. A windowd client (libwindow);
 * decoding is libimage.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "image.h"
#include "ritteros.h"
#include "window.h"

PROGRAM_HELP("imageview [FILE]", "show an image (PPM, BMP, PNG, JPEG) in a window");

#define M_OPEN  1
#define M_CLOSE 2

static struct image source;             /* the decoded image, kept for rescaling */
static int have_image;
static char folder[512] = "/";        /* where the dialog starts: the last file's */

/* Scale the source to fit the window keeping its proportions (centred, the
 * rest the background), convert RGBA to the window's native pixels, and draw
 * it. */
static void render(struct window *win)
{
    int ww = window_width(win), wh = window_height(win);
    uint32_t background = window_rgb(win, 0x36, 0x3D, 0x4A);
    if (!have_image) {                          /* empty: a hint */
        window_fill(win, 0, 0, ww, wh, background);
        window_text(win, 12, 12, "File > Open... to show an image", window_rgb(win, 0xD8, 0xDE, 0xE9),
                    WINDOW_TRANSPARENT);
        window_flush(win);
        return;
    }
    int w = ww, h = (int)((long)source.h * ww / source.w);
    if (h > wh)                                 /* (taller than the window: fit the height) */
        h = wh, w = (int)((long)source.w * wh / source.h);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    int x = (ww - w) / 2, y = (wh - h) / 2;
    struct image scaled;
    if (image_scale(&source, w, h, &scaled) < 0)
        return;
    uint32_t *native = malloc((size_t)w * h * 4);
    if (native) {
        int rs, gs, bs;
        window_shifts(win, &rs, &gs, &bs);
        const uint8_t *p = scaled.pixels;
        for (long i = 0; i < (long)w * h; i++, p += 4)
            native[i] = (uint32_t)p[0] << rs | (uint32_t)p[1] << gs | (uint32_t)p[2] << bs;
        if (x > 0) {                            /* the bars beside or above and below it */
            window_fill(win, 0, 0, x, wh, background);
            window_fill(win, x + w, 0, ww - x - w, wh, background);
        }
        if (y > 0) {
            window_fill(win, 0, 0, ww, y, background);
            window_fill(win, 0, y + h, ww, wh - y - h, background);
        }
        window_blit(win, x, y, w, h, native);
        free(native);
        window_flush(win);
    }
    image_free(&scaled);
}

/* A size for an image of w x h that fits the screen. */
static void fit(int *w, int *h)
{
    struct sys_display d;
    int sw = 1280, sh = 800;
    if (ritteros_display_info(&d) == 0)
        sw = (int)d.width, sh = (int)d.height;
    if (*w > sw - 80) { *h = (int)((long)*h * (sw - 80) / *w); *w = sw - 80; }
    if (*h > sh - 80) { *w = (int)((long)*w * (sh - 80) / *h); *h = sh - 80; }
    if (*w < 16) *w = 16;
    if (*h < 16) *h = 16;
}

/* Load path in place of the image shown (0, or -1 with the old one kept). */
static int load(const char *path)
{
    struct image next;
    if (image_load(path, &next) < 0) {
        printf("imageview: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (have_image)
        image_free(&source);
    source = next;
    have_image = 1;
    snprintf(folder, sizeof(folder), "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash && slash != folder)
        *slash = 0;
    else
        snprintf(folder, sizeof(folder), "/");
    printf("imageview: showing %s (%dx%d)\n", path, source.w, source.h);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        dprintf(2, "usage: imageview [FILE]\n");
        return 2;
    }
    if (argc == 2 && load(argv[1]) < 0)
        return 1;
    if (argc < 2 && getcwd(folder, sizeof(folder)) == 0)
        snprintf(folder, sizeof(folder), "/");
    int w = 480, h = 320;                       /* empty; else the image's size, within the screen */
    if (have_image) {
        w = source.w;
        h = source.h;
        fit(&w, &h);
    }
    struct window *win = window_open(w, h, argc == 2 ? argv[1] : "imageview");
    if (!win) {
        if (errno == EACCES)                    /* (another user's windowd, 15k) */
            printf("imageview: cannot open a window: the screen is another user's\n");
        else
            printf("imageview: cannot open a window (is windowd running?)\n");
        return 1;
    }
    static const struct window_menu_item file_items[] = { { M_OPEN, "Open..." }, { M_CLOSE, "Close" } };
    static const struct window_menu menubar[] = { { "File", file_items, 2 } };
    window_set_menu(win, menubar, 1);
    if (!have_image)
        printf("imageview: started, no image\n");
    render(win);
    struct window_event ev;
    while (window_wait(win, &ev)) {
        if (ev.type == WINDOW_CLOSED)
            break;
        if (ev.type == WINDOW_KEY && (ev.key == 'q' || ev.key == 'Q'))
            break;
        if (ev.type == WINDOW_MENU && ev.menu == M_CLOSE)
            break;
        if (ev.type == WINDOW_MENU && ev.menu == M_OPEN) {
            char path[512];
            if (window_choose_file(win, "Open an image", folder, path, sizeof(path)) == 1 &&
                load(path) == 0) {
                int nw = source.w, nh = source.h;
                fit(&nw, &nh);
                window_resize(win, nw, nh);     /* the window takes the image's size */
            }
            render(win);
        }
        if (ev.type == WINDOW_RESIZE) {
            window_resize(win, ev.x, ev.y);
            render(win);
        }
    }
    window_close(win);
    if (have_image)
        image_free(&source);
    return 0;
}
