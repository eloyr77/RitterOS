/*
 * Test: sockets (steps 37 and 38). Every mode prints what it saw, for the
 * test to compare:
 *   sock udp HOST PORT TEXT      send TEXT, print the answer and who sent it
 *   sock tcp HOST PORT TEXT      connect, send TEXT, print all that comes back
 *   sock get HOST PORT BYTES     connect, read until the end; the bytes must
 *                                be the pattern (byte i = i % 251)
 *   sock slowget HOST PORT BYTES the same, reading slowly (2 KiB every 5 ms:
 *                                the window closes and opens again)
 *   sock put HOST PORT BYTES     connect, send BYTES of the pattern, close
 *   sock serve PORT              accept one connection, answer its text in
 *                                capitals until it ends
 *   sock loop BYTES              TCP over the loopback: a thread sends BYTES
 *                                of the pattern to 127.0.0.1, this one reads
 *   sock udploop                 UDP over the loopback (and its errors)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <unistd.h>
#include "ritteros.h"

static char buffer[16384];

static int fail(const char *what)
{
    printf("sock: %s: %s\n", what, strerror(errno));
    return 1;
}

static uint32_t host(const char *name)
{
    uint32_t ip = 0;
    if (ritteros_resolve(name, &ip) < 0) {
        printf("sock: %s: %s\n", name, strerror(errno));
        exit(1);
    }
    return ip;
}

static int connected(const char *name, const char *port)
{
    int fd = ritteros_socket(SOCKET_TCP);
    if (fd < 0)
        exit(fail("socket"));
    if (ritteros_connect(fd, host(name), (uint16_t)atoi(port)) < 0)
        exit(fail("connect"));
    return fd;
}

/* Send n bytes of the pattern, from offset 0. A buffer of its own: in
 * "loop" another thread reads into the other one meanwhile. */
static int send_pattern(int fd, long n)
{
    static char out[16384];
    for (long at = 0; at < n;) {
        size_t chunk = n - at < (long)sizeof(out) ? (size_t)(n - at) : sizeof(out);
        for (size_t i = 0; i < chunk; i++)
            out[i] = (char)((at + (long)i) % 251);
        for (size_t done = 0; done < chunk;) {
            ssize_t w = write(fd, out + done, chunk - done);
            if (w <= 0)
                return -1;
            done += (size_t)w;
        }
        at += (long)chunk;
    }
    return 0;
}

/* Read to the end and check the pattern; prints the result. Slowly: 2 KiB
 * at a time, 5 ms apart. */
static int receive_pattern(int fd, long want, bool slowly)
{
    long total = 0, wrong = -1;
    ssize_t r;
    while ((r = read(fd, buffer, slowly ? 2048 : sizeof(buffer))) > 0) {
        if (slowly)
            usleep(5000);
        for (ssize_t i = 0; i < r; i++)
            if (wrong < 0 && (unsigned char)buffer[i] != (total + i) % 251)
                wrong = total + i;
        total += r;
    }
    if (r < 0)
        return fail("read");
    if (wrong >= 0)
        printf("sock: received %ld bytes, wrong at byte %ld\n", total, wrong);
    else
        printf("sock: received %ld bytes, pattern %s\n", total, total == want ? "ok" : "short");
    return wrong < 0 && total == want ? 0 : 1;
}

static int loop_port;
static long loop_bytes;

static int loop_sender(void *arg)
{
    (void)arg;
    int fd = ritteros_socket(SOCKET_TCP);
    if (fd < 0 || ritteros_connect(fd, 0x7F000001, (uint16_t)loop_port) < 0)
        return fail("connect");
    int r = send_pattern(fd, loop_bytes);
    close(fd);
    return r;
}

static int udploop(void)
{
    int a = ritteros_socket(SOCKET_UDP), b = ritteros_socket(SOCKET_UDP);
    int pa = ritteros_bind(a, 0), pb = ritteros_bind(b, 4000);
    if (a < 0 || b < 0 || pa < 0 || pb < 0)
        return fail("bind");
    int c = ritteros_socket(SOCKET_UDP);
    if (ritteros_bind(c, 4000) < 0)
        printf("sock: port 4000 again: %s\n", strerror(errno));
    close(c);
    for (int i = 1; i <= 3; i++) {
        char text[32];
        int n = snprintf(text, sizeof(text), "datagram %d", i);
        if (ritteros_sendto(a, text, (size_t)n, 0x7F000001, 4000) != n)
            return fail("sendto");
    }
    for (int i = 1; i <= 3; i++) {
        uint32_t ip;
        uint16_t port;
        ssize_t r = ritteros_recvfrom(b, buffer, sizeof(buffer) - 1, &ip, &port, 1000);
        if (r < 0)
            return fail("recvfrom");
        buffer[r] = 0;
        char from[16];
        ip_format(ip, from, sizeof(from));
        printf("sock: '%s' from %s, %s port\n", buffer, from, port == pa ? "the sender's" : "another");
    }
    if (ritteros_recvfrom(b, buffer, sizeof(buffer), 0, 0, 300) < 0)
        printf("sock: nothing more: %s\n", strerror(errno));
    close(a), close(b);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "udp") == 0) {
        int fd = ritteros_socket(SOCKET_UDP);
        if (fd < 0 || ritteros_connect(fd, host(argv[2]), (uint16_t)atoi(argv[3])) < 0)
            return fail("connect");
        if (write(fd, argv[4], strlen(argv[4])) < 0)
            return fail("write");
        uint32_t ip;
        uint16_t port;
        ssize_t r = ritteros_recvfrom(fd, buffer, sizeof(buffer) - 1, &ip, &port, 3000);
        if (r < 0)
            return fail("recvfrom");
        buffer[r] = 0;
        char from[16];
        ip_format(ip, from, sizeof(from));
        printf("sock: answer from %s:%u: '%s'\n", from, port, buffer);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "tcp") == 0) {
        int fd = connected(argv[2], argv[3]);
        if (write(fd, argv[4], strlen(argv[4])) < 0)
            return fail("write");
        size_t n = 0;
        ssize_t r;
        while (n < sizeof(buffer) - 1 && (r = read(fd, buffer + n, sizeof(buffer) - 1 - n)) > 0)
            n += (size_t)r;
        buffer[n] = 0;
        printf("sock: answer: '%s'\n", buffer);
        close(fd);
        return 0;
    }
    if (argc == 5 && (strcmp(argv[1], "get") == 0 || strcmp(argv[1], "slowget") == 0)) {
        int fd = connected(argv[2], argv[3]);
        int r = receive_pattern(fd, atol(argv[4]), argv[1][0] == 's');
        close(fd);
        return r;
    }
    if (argc == 5 && strcmp(argv[1], "put") == 0) {
        int fd = connected(argv[2], argv[3]);
        if (send_pattern(fd, atol(argv[4])) < 0)
            return fail("write");
        close(fd);
        printf("sock: sent %ld bytes\n", atol(argv[4]));
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "serve") == 0) {
        int fd = ritteros_socket(SOCKET_TCP);
        if (fd < 0 || ritteros_bind(fd, (uint16_t)atoi(argv[2])) < 0 || ritteros_listen(fd, 1) < 0)
            return fail("listen");
        printf("sock: listening on port %s\n", argv[2]);
        uint32_t ip;
        uint16_t port;
        int c = ritteros_accept(fd, &ip, &port);
        if (c < 0)
            return fail("accept");
        char from[16];
        ip_format(ip, from, sizeof(from));
        printf("sock: connection from %s\n", from);
        ssize_t r;
        while ((r = read(c, buffer, sizeof(buffer))) > 0) {
            for (ssize_t i = 0; i < r; i++)
                if (buffer[i] >= 'a' && buffer[i] <= 'z')
                    buffer[i] = (char)(buffer[i] - 32);
            if (write(c, buffer, (size_t)r) != r)
                return fail("write");
        }
        printf("sock: connection ended\n");
        close(c), close(fd);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "loop") == 0) {
        loop_bytes = atol(argv[2]);
        int fd = ritteros_socket(SOCKET_TCP);
        if (fd < 0 || (loop_port = ritteros_bind(fd, 0)) < 0 || ritteros_listen(fd, 1) < 0)
            return fail("listen");
        int t = thread_create(loop_sender, 0);
        int c = ritteros_accept(fd, 0, 0);
        if (t < 0 || c < 0)
            return fail("accept");
        int r = receive_pattern(c, loop_bytes, false);
        int sent = 1;
        thread_join(t, &sent);
        close(c), close(fd);
        return r || sent;
    }
    if (argc == 2 && strcmp(argv[1], "udploop") == 0)
        return udploop();
    printf("usage: sock udp|tcp|get|slowget|put HOST PORT ARG, serve PORT, loop BYTES, udploop\n");
    return 2;
}
