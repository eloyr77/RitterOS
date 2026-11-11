/*
 * reel [FILE]: play a video (RVID so far) in a window -- the frames at the
 * video's rate, and its audio track (if any) through /dev/pcm0 on a thread.
 * Without a file it opens an empty window; File > Open... picks one with the
 * shared dialog (window_choose_file), which also stops what is playing and
 * plays the new one. The window keeps the last frame when a video ends. 'q',
 * File > Close or the close box quits. Frames are scaled to fit the window
 * with their proportions kept, so the window may be resized (windowd also
 * makes one for a large video fit the screen). A windowd client; decoding is
 * libvideo (frames from libimage-style RGBA, audio from libaudio-style PCM).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <thread.h>
#include "audio.h"
#include "image.h"
#include "ritteros.h"
#include "video.h"
#include "window.h"

PROGRAM_HELP("reel [FILE]", "play a video (RVID) in a window");

#define M_OPEN  1
#define M_CLOSE 2

#define RATE 48000

static struct audio_buffer track;       /* the decoded audio, for the player thread */
static volatile int stop_audio;
static volatile int quit;
static struct window *g_win;
static char next_path[512];             /* a video chosen to play next (the event thread's) */
static volatile int next_ready;         /* next_path is set: stop the current one */
static char folder[512] = "/";          /* where the dialog starts */

/* Watch for the menu, the close box or 'q' (the frames play meanwhile). */
/* A new size from windowd, taken by the thread that draws (resizing swaps
 * the window's buffer, which must not happen under a frame being drawn). */
static int want_w, want_h, want_resize;

static int event_thread(void *arg)
{
    (void)arg;
    struct window_event ev;
    while (window_wait(g_win, &ev)) {
        if (ev.type == WINDOW_RESIZE) {
            want_w = ev.x, want_h = ev.y;
            __atomic_store_n(&want_resize, 1, __ATOMIC_RELEASE);
        }
        if (ev.type == WINDOW_CLOSED ||
            (ev.type == WINDOW_KEY && (ev.key == 'q' || ev.key == 'Q')) ||
            (ev.type == WINDOW_MENU && ev.menu == M_CLOSE)) {
            quit = 1;
            break;
        }
        if (ev.type == WINDOW_MENU && ev.menu == M_OPEN && !next_ready) {
            char path[512];
            if (window_choose_file(g_win, "Open a video", folder, path, sizeof(path)) == 1) {
                snprintf(next_path, sizeof(next_path), "%s", path);
                __atomic_store_n(&next_ready, 1, __ATOMIC_RELEASE);
            }
        }
    }
    quit = 1;                               /* the window closed */
    return 0;
}

/* Stream the audio track to /dev/pcm0 (48 kHz stereo). */
static int audio_thread(void *arg)
{
    (void)arg;
    struct audio_buffer a = track;
    struct audio_buffer r;
    if (a.rate != RATE && audio_resample(&a, RATE, &r) == 0)
        a = r;
    int fd = open("/dev/pcm0", O_WRONLY);
    if (fd < 0)
        return 0;
    int16_t *st = a.channels == 2 ? a.samples : malloc((size_t)a.frames * 4);
    if (a.channels == 1 && st) {
        const int16_t *m = a.samples;
        for (long i = 0; i < a.frames; i++)
            st[i * 2] = st[i * 2 + 1] = m[i];
    }
    long total = a.frames * 4, off = 0;
    while (off < total && !stop_audio) {                /* write in small pieces so we can stop */
        long n = total - off < 8192 ? total - off : 8192;
        ssize_t w = write(fd, (uint8_t *)st + off, (size_t)n);
        if (w <= 0)
            break;
        off += w;
    }
    close(fd);                                          /* drains the rest */
    if (a.channels == 1)
        free(st);
    return 0;
}

static struct image last;               /* the frame shown, drawn again after a resize */
static int have_last;

/* Draw f scaled to fit the window with its proportions kept, centred, the
 * rest the background (as imageview does). */
static void blit_frame(struct window *win, const struct image *f)
{
    int ww = window_width(win), wh = window_height(win);
    uint32_t background = window_rgb(win, 0x36, 0x3D, 0x4A);
    int w = ww, h = (int)((long)f->h * ww / f->w);
    if (h > wh)
        h = wh, w = (int)((long)f->w * wh / f->h);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    int x = (ww - w) / 2, y = (wh - h) / 2;
    struct image scaled;
    const struct image *s = f;
    if ((w != f->w || h != f->h) && image_scale(f, w, h, &scaled) == 0)
        s = &scaled;
    else if (w != f->w || h != f->h)
        return;
    uint32_t *native = malloc((size_t)w * h * 4);
    if (native) {
        int rs, gs, bs;
        window_shifts(win, &rs, &gs, &bs);
        const uint8_t *p = s->pixels;
        for (long i = 0; i < (long)w * h; i++, p += 4)
            native[i] = (uint32_t)p[0] << rs | (uint32_t)p[1] << gs | (uint32_t)p[2] << bs;
        if (x > 0) {                                    /* the bars beside or above and below */
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
    if (s == &scaled)
        image_free(&scaled);
}

/* Take a size windowd asked for, and draw what was shown again. */
static void take_resize(struct window *win)
{
    if (!__atomic_load_n(&want_resize, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&want_resize, 0, __ATOMIC_RELEASE);
    if (window_resize(win, want_w, want_h) < 0)
        return;
    if (have_last)
        blit_frame(win, &last);
    else {
        window_fill(win, 0, 0, want_w, want_h, window_rgb(win, 0x36, 0x3D, 0x4A));
        window_flush(win);
    }
}

/* Show a frame and keep it as the one shown. */
static void show_frame(struct window *win, struct image *f)
{
    take_resize(win);
    blit_frame(win, f);
    if (have_last)
        image_free(&last);
    last = *f;                                          /* (it is ours now) */
    have_last = 1;
}

/* Play path in the window (resized to the video): until it ends, another is
 * chosen or the program quits. 0, or -1 if it cannot be opened. */
static int play(struct window *win, const char *path)
{
    struct video *v;
    if (video_open(path, &v) < 0) {
        printf("reel: %s: %s\n", path, strerror(errno));
        return -1;
    }
    snprintf(folder, sizeof(folder), "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash && slash != folder)
        *slash = 0;
    else
        snprintf(folder, sizeof(folder), "/");
    int w, h, fps, has_audio;
    video_info(v, &w, &h, &fps, &has_audio);
    if (window_width(win) != w || window_height(win) != h)
        window_resize(win, w, h);               /* the window takes the video's size */
    printf("reel: playing %s (%dx%d, %d fps%s)\n", path, w, h, fps,
           has_audio ? ", with audio" : "");
    int at = -1;
    stop_audio = 0;
    if (has_audio && video_audio(v, &track) == 0)
        at = thread_create(audio_thread, 0);
    struct image frame;
    int64_t pts;
    while (!quit && !next_ready && video_read_frame(v, &frame, &pts) == 1) {
        show_frame(win, &frame);
        usleep((unsigned)(1000000 / fps));      /* hold the frame */
    }
    stop_audio = 1;
    if (at >= 0)
        thread_join(at, 0);
    if (has_audio)
        audio_free(&track);
    video_close(v);
    printf("reel: done\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        dprintf(2, "usage: reel [FILE]\n");
        return 2;
    }
    int w = 480, h = 320;                       /* empty until a video plays */
    if (argc == 2) {
        struct video *v;
        if (video_open(argv[1], &v) < 0) {
            printf("reel: %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
        int fps, has_audio;
        video_info(v, &w, &h, &fps, &has_audio);
        video_close(v);
    } else if (getcwd(folder, sizeof(folder)) == 0) {
        snprintf(folder, sizeof(folder), "/");
    }
    struct window *win = window_open(w, h, argc == 2 ? argv[1] : "reel");
    if (!win) {
        if (errno == EACCES)                    /* (another user's windowd, 15k) */
            printf("reel: cannot open a window: the screen is another user's\n");
        else
            printf("reel: cannot open a window (is windowd running?)\n");
        return 1;
    }
    static const struct window_menu_item file_items[] = { { M_OPEN, "Open..." }, { M_CLOSE, "Close" } };
    static const struct window_menu menubar[] = { { "File", file_items, 2 } };
    window_set_menu(win, menubar, 1);
    g_win = win;
    if (argc < 2) {                             /* empty: a hint */
        window_fill(win, 0, 0, w, h, window_rgb(win, 0x36, 0x3D, 0x4A));
        window_text(win, 12, 12, "File > Open... to play a video", window_rgb(win, 0xD8, 0xDE, 0xE9),
                    WINDOW_TRANSPARENT);
        window_flush(win);
        printf("reel: started, no video\n");
    }
    int et = thread_create(event_thread, 0);
    if (argc == 2)
        play(win, argv[1]);
    while (!quit) {                             /* wait for a choice from the menu */
        if (__atomic_load_n(&next_ready, __ATOMIC_ACQUIRE)) {
            char path[512];
            snprintf(path, sizeof(path), "%s", next_path);
            __atomic_store_n(&next_ready, 0, __ATOMIC_RELEASE);
            play(win, path);
        } else {
            take_resize(win);                   /* (between videos too) */
            usleep(50000);
        }
    }
    window_close(win);                          /* ends the event thread's window_wait */
    (void)et;
    return 0;
}
