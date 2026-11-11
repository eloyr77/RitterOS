/*
 * Test client for windowd (step 15e): make a 200x150 green window, hand its
 * pixels to windowd, and print the first pointer event windowd forwards. The
 * test screenshots the composited window and moves the mouse to drive the
 * event.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"
#include "windowproto.h"

#define W   200
#define H   150

static int read_full(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0)
            return -1;
        done += (size_t)r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int cr = 0, cg = 200, cb = 80;              /* green by default */
    if (argc == 4) {
        cr = atoi(argv[1]);
        cg = atoi(argv[2]);
        cb = atoi(argv[3]);
    }
    struct sys_display d;
    if (ritteros_display_info(&d) < 0) {
        printf("wclient: no display info: %s\n", strerror(errno));
        return 1;
    }
    int bufd = ritteros_share_create((size_t)W * H * 4);
    uint32_t *pix = bufd < 0 ? 0 : ritteros_share_map(bufd);
    if (!pix) {
        printf("wclient: no buffer: %s\n", strerror(errno));
        return 1;
    }
    uint32_t color = ((uint32_t)cr << d.red_shift) | ((uint32_t)cg << d.green_shift) |
                     ((uint32_t)cb << d.blue_shift);
    for (int i = 0; i < W * H; i++)
        pix[i] = color;

    int sock = -1;
    for (int i = 0; i < 100 && sock < 0; i++) {     /* wait for windowd to come up */
        sock = ritteros_local_connect(WINDOW_SOCKET);
        if (sock < 0 && errno == ECONNREFUSED)
            usleep(50000);
        else
            break;
    }
    if (sock < 0) {
        printf("wclient: cannot reach windowd: %s\n", strerror(errno));
        return 1;
    }
    if (ritteros_share_send(sock, bufd) < 0) {
        printf("wclient: send buffer: %s\n", strerror(errno));
        return 1;
    }
    struct wmsg m = { WMSG_CREATE, W, H, 0 };
    if (write(sock, &m, sizeof(m)) < 0 || read_full(sock, &m, sizeof(m)) < 0 ||
        m.type != WMSG_CREATED) {
        printf("wclient: create failed\n");
        return 1;
    }
    printf("wclient: window created, id %u\n", m.a);
    int said = 0;
    while (read_full(sock, &m, sizeof(m)) == 0) {       /* hold the window, report once */
        if (m.type == WMSG_POINTER && !said) {
            printf("wclient: pointer %u,%u\n", m.a, m.b);
            said = 1;
        }
        if (m.type == WMSG_CLOSE) {                      /* the close box was clicked */
            printf("wclient: closed\n");
            break;
        }
    }
    close(sock);
    return 0;
}
