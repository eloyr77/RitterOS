/*
 * Test: windowd reaps the per-connection threads (overnight review fix). Open
 * and close many windows one after another; each is a client connection whose
 * thread ends. Without reaping, windowd runs out of thread numbers after ~14
 * and can open no more; with it, all succeed.
 */
#include <stdio.h>
#include <unistd.h>
#include "window.h"

int main(void)
{
    for (int i = 0; i < 20; i++) {
        struct window *w = window_open(60, 40, "x");
        if (!w) {
            printf("wmany: window %d failed to open\n", i);
            return 1;
        }
        window_close(w);
        usleep(60000);                  /* let windowd notice the close and reap the thread */
    }
    struct window *w = window_open(60, 40, "x");    /* the 21st, well past the ~14 limit */
    if (!w) {
        printf("wmany: the final window failed to open\n");
        return 1;
    }
    printf("wmany: ok, 21 windows opened and closed in turn\n");
    window_close(w);
    return 0;
}
