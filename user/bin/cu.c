/*
 * cu DEVICE (50e, after Unix's "call up"): a terminal on a serial line. The
 * keys typed go to DEVICE (/dev/usbserial0, or just usbserial0) as they
 * are, Ctrl-C included; what it sends comes to the screen. ~. at the start
 * of a line leaves, ~~ sends one ~. It ends by itself when the device is
 * unplugged, or when its input ends (not a terminal).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("cu [-l] DEVICE",
             "a terminal on a serial line (~. leaves)");

static struct termios saved;
static bool raw;

static void restore(void)
{
    if (raw)
        tcsetattr(0, TCSANOW, &saved);
    raw = false;
}

/* All of n bytes to fd; false if it refused. */
static bool put(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-l"))     /* Unix cu's form: cu -l DEVICE */
        i++;
    if (argc != i + 1 || !argv[i][0]) {
        fprintf(stderr, "usage: cu DEVICE\n");
        return 2;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s%s", strchr(argv[i], '/') ? "" : "/dev/", argv[i]);
    int dev = open(path, O_RDWR);
    if (dev < 0) {
        fprintf(stderr, "cu: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (isatty(0) && tcgetattr(0, &saved) == 0) {
        struct termios t = saved;
        cfmakeraw(&t);
        if (tcsetattr(0, TCSANOW, &t) == 0)
            raw = true;
    }
    printf("Connected to %s (~. leaves).\r\n", path);

    bool line_start = true, tilde = false;
    const char *why = 0;
    char buf[4096];
    while (!why) {
        struct pollfd fds[2] = { { dev, POLLIN, 0 }, { 0, POLLIN, 0 } };
        int n = poll(fds, 2, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            why = strerror(errno);
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t r = read(dev, buf, sizeof(buf));
            if (r == 0) {
                why = "the device is gone";
                break;
            }
            if (r < 0 && errno != EINTR && errno != EAGAIN) {
                why = strerror(errno);
                break;
            }
            if (r > 0)
                put(1, buf, (size_t)r);
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t r = read(0, buf, sizeof(buf));
            if (r == 0) {
                why = "the input ended";
                break;
            }
            if (r < 0)
                continue;
            /* The escape: ~ at the start of a line, then . (leave) or ~
             * (one ~); anything else sends both. */
            char out[2 * sizeof(buf)];
            size_t k = 0;
            for (ssize_t j = 0; j < r && !why; j++) {
                char c = buf[j];
                if (tilde) {
                    tilde = false;
                    if (c == '.') {
                        why = "";
                        break;
                    }
                    if (c != '~')
                        out[k++] = '~';
                    out[k++] = c;
                } else if (c == '~' && line_start) {
                    tilde = true;
                    continue;
                } else {
                    out[k++] = c;
                }
                line_start = c == '\r' || c == '\n';
            }
            if (k && !put(dev, out, k) && !why)
                why = "the device refused what was typed";
        }
    }
    restore();
    printf("\r\nDisconnected%s%s.\n", why && *why ? ": " : "", why ? why : "");
    close(dev);
    return why && *why && strcmp(why, "the input ended") ? 1 : 0;
}
