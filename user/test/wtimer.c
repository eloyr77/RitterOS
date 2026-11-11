/*
 * Test: the event loop beside other work (41a). Opens a window and checks
 * window_wait_timeout (no wait, a wait that runs out), window_timer (two
 * timers, none early, one removed, a ninth refused) and window_fd in a poll
 * beside a pipe: the pipe written by another thread wakes the poll first,
 * then a pointer move (the test's, through QEMU) makes the window's
 * descriptor readable and the event comes out of window_wait_timeout.
 */
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "window.h"

static long long ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int pipe_fds[2];

static void *write_later(void *arg)
{
    (void)arg;
    usleep(100000);
    char c = 'x';
    write(pipe_fds[1], &c, 1);
    return 0;
}

/* Take timer events for `ms` milliseconds: count each id's, and how many
 * came before their period's multiple since `set` (the time they were set). */
static void run(struct window *win, int ms, long long set, const int *every, int *count, int *early)
{
    long long end = ms_now() + ms;
    struct window_event ev;
    for (long long now; (now = ms_now()) < end;) {
        int r = window_wait_timeout(win, &ev, (int)(end - now));
        if (r < 0)
            break;
        if (r == 1 && ev.type == WINDOW_TIMER && ev.key >= 1 && ev.key <= 2) {
            count[ev.key]++;
            if (ms_now() - set < (long long)count[ev.key] * every[ev.key])
                (*early)++;
        }
    }
}

int main(void)
{
    struct window *win = window_open(300, 200, "wtimer");
    if (!win) {
        printf("wtimer: cannot open a window\n");
        return 1;
    }
    window_fill(win, 0, 0, 300, 200, window_rgb(win, 0x30, 0x60, 0x30));
    window_flush(win);
    struct window_event ev;
    /* (whatever windowd sends at the start: its focus event (41f-1) comes
     * from the window's own thread up to 10 ms later, so wait for it) */
    for (int waited = 0; waited < 2000; waited += 10)
        if (window_wait_timeout(win, &ev, 10) > 0 && ev.type == WINDOW_FOCUS)
            break;
    while (window_wait_timeout(win, &ev, 0) > 0)
        ;
    printf("wtimer: ready\n");

    long long t0 = ms_now();
    int r = window_wait_timeout(win, &ev, 0);
    long long t1 = ms_now();
    printf("wtimer: at once %d, none %d, quick %d\n", r, ev.type == WINDOW_NONE, t1 - t0 < 50);
    t0 = ms_now();
    r = window_wait_timeout(win, &ev, 200);
    t1 = ms_now();
    printf("wtimer: 200 ms %d, none %d, waited enough %d\n", r, ev.type == WINDOW_NONE, t1 - t0 >= 200);

    int every[3] = { 0, 50, 120 }, count[3] = { 0, 0, 0 }, early = 0;
    long long set = ms_now();
    window_timer(win, every[1], 1);
    window_timer(win, every[2], 2);
    run(win, 600, set, every, count, &early);
    printf("wtimer: in 600 ms timer 1 %d, timer 2 %d, early %d\n", count[1], count[2], early);
    window_timer(win, 0, 1);
    count[1] = 0;
    int two = count[2];
    run(win, 300, set, every, count, &early);
    printf("wtimer: removed, timer 1 %d, timer 2 on %d\n", count[1], count[2] > two);
    window_timer(win, 0, 2);
    int took = 0;
    for (int id = 10; id < 18; id++)
        took += window_timer(win, 1000, id) == 0;
    printf("wtimer: 8 timers %d, a ninth refused %d\n", took, window_timer(win, 1000, 99) < 0);
    for (int id = 10; id < 18; id++)
        window_timer(win, 0, id);

    pipe(pipe_fds);
    pthread_t th;
    pthread_create(&th, 0, write_later, 0);
    struct pollfd p[2] = { { window_fd(win), POLLIN, 0 }, { pipe_fds[0], POLLIN, 0 } };
    int n = poll(p, 2, 5000);
    printf("wtimer: the pipe first %d, the window quiet %d\n", n == 1 && (p[1].revents & POLLIN),
           !(p[0].revents & POLLIN));
    pthread_join(th, 0);

    printf("wtimer: move the pointer\n");
    p[0].revents = 0;
    n = poll(p, 1, 20000);
    int pointer = 0;
    while (window_wait_timeout(win, &ev, 0) > 0)
        pointer += ev.type == WINDOW_POINTER;
    printf("wtimer: through window_fd %d, pointer events %d\n", n == 1 && (p[0].revents & POLLIN),
           pointer > 0);
    window_close(win);
    printf("wtimer: done\n");
    return 0;
}
