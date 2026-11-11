/*
 * wshot FILE.png: the window system's whole screen, as windowd composed it
 * (the pointer too), saved as a picture (41h-2): WMSG_SHOT on a connection
 * of its own to the windowd at $WINDOW_SOCKET (a headless one's) or
 * /run/windowd; windowd answers with a copy of its screen in a shared
 * buffer. The file's ending chooses the format (libimage's image_save:
 * .png, .bmp, else PPM).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "image.h"
#include "ritteros.h"
#include "windowproto.h"

PROGRAM_HELP("wshot FILE.png", "save the window system's screen as a picture (needs windowd)");

static int read_full(int fd, void *buf, size_t n)
{
    for (size_t done = 0; done < n;) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0)
            return -1;
        done += (size_t)r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        dprintf(2, "usage: wshot FILE.png\n");
        return 2;
    }
    const char *at = getenv("WINDOW_SOCKET");
    if (!at || !*at)
        at = WINDOW_SOCKET;
    int sock = ritteros_local_connect(at);
    if (sock >= 0) {                                /* (15k) its user's or root's */
        int uid = ritteros_peer_uid(sock);
        if (uid != (int)getuid() && uid != 0) {
            close(sock);
            sock = -1;
            errno = EACCES;
        }
    }
    if (sock < 0) {
        dprintf(2, "wshot: no window server at %s: %s\n", at, strerror(errno));
        return 1;
    }
    struct wmsg m = { WMSG_SHOT, 0, 0, 0 };
    int bufd = -1;
    if (write(sock, &m, sizeof(m)) == (ssize_t)sizeof(m) && read_full(sock, &m, sizeof(m)) == 0 &&
        m.type == WMSG_SHOT)
        for (int i = 0; i < 100 && bufd < 0; i++) {    /* (sent just after the answer) */
            bufd = ritteros_share_recv(sock);
            if (bufd < 0 && errno == EAGAIN)
                usleep(10000);
            else
                break;
        }
    close(sock);
    size_t n = (size_t)m.a * m.b * 4;
    const uint32_t *pix = bufd < 0 ? 0 : ritteros_share_map(bufd);
    if (!pix || ritteros_share_size(bufd) < (long)n) {
        dprintf(2, "wshot: windowd sent no picture\n");
        return 1;
    }
    struct image img = { (int)m.a, (int)m.b, (int)m.a * 4, PIXEL_RGBA8888, malloc(n) };
    if (!img.pixels) {
        dprintf(2, "wshot: %s\n", strerror(ENOMEM));
        return 1;
    }
    int rs = m.c & 0xFF, gs = m.c >> 8 & 0xFF, bs = m.c >> 16 & 0xFF;
    for (size_t i = 0; i < (size_t)m.a * m.b; i++) {   /* the screen's pixels to RGBA */
        img.pixels[i * 4] = (uint8_t)(pix[i] >> rs);
        img.pixels[i * 4 + 1] = (uint8_t)(pix[i] >> gs);
        img.pixels[i * 4 + 2] = (uint8_t)(pix[i] >> bs);
        img.pixels[i * 4 + 3] = 0xFF;
    }
    munmap((void *)pix, n);
    close(bufd);
    if (image_save(argv[1], &img) < 0) {
        dprintf(2, "wshot: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    printf("wshot: %s (%ux%u)\n", argv[1], m.a, m.b);
    image_free(&img);
    return 0;
}
