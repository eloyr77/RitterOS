/*
 * Test (65e): how fast a TCP stream comes in, on any card.
 *
 *   netrate HOST PORT SECONDS [-p] [REQUEST]
 *       connects, sends REQUEST once (if given: \r and \n in it are CR and
 *       LF, and a line break ends it unless it ends with one), then
 *       reads until SECONDS have passed or the other side closes. A line
 *       per second with its MB/s (10^6 bytes), then the total. With -p
 *       the stream must be the tests' pattern (byte i = i % 251,
 *       scripts/testnet.py's tcp-send): the bytes that are not are counted.
 *   netrate iiod HOST SECONDS [RATE]
 *       the PlutoSDR's iiod (port 30431), as libiio's programs read it: the
 *       receiver's buffer opened (I and Q), then READBUF after READBUF.
 *       With RATE the sample rate is set first (higher than the link can
 *       carry, so the link is what is measured) and put back after. Only
 *       one client at a time may have the buffer: close plutosa first.
 *   netrate send HOST PORT BYTES [REQUEST]
 *       (65e) the other way: connects, sends REQUEST (as above: an HTTP
 *       POST's header with its Content-Length), then BYTES of the pattern,
 *       a line per second with what went into the connection, ends its
 *       sending (shutdown) and waits for the other side's answer or close:
 *       the total is to that, so it counts what arrived, not what the send
 *       buffer took.
 *   netrate listen PORT [-p]
 *       (65e) the receiving end for netrate send between two machines:
 *       waits for one connection on PORT, reads to its end (-p: checks
 *       the pattern), the same lines, then answers how much came and
 *       closes (the sender's clock stops at the answer). The firewall must
 *       let PORT in (guard).
 *   netrate serve PORT
 *       (36e) the other end for measuring against a machine on the network,
 *       one connection after the other until it is ended: a connection
 *       whose first bytes are the request "send" gets the tests' pattern
 *       until it closes (netrate HOST PORT SECONDS -p send downloads it);
 *       any other is read to its end and answered as listen does (netrate
 *       send HOST PORT BYTES uploads). A line for each connection. The
 *       firewall must let PORT in (guard).
 *   netrate read PATH SECONDS
 *       a file or a device (/dev/usbdisk1) read from its start in 64 KiB
 *       pieces, the same lines: the same measure without a network (a USB
 *       stick: the controller and the bulk transfers alone).
 *
 * The reading itself is a few instructions per 64 KiB: what is measured
 * is the network and the kernel's way to the program.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHUNK       65536
#define IIOD_PORT   "30431"
#define IIOD_DEVICE "cf-ad9361-lpc"         /* the receiver's samples */
#define IIOD_PHY    "ad9361-phy"            /* its settings */
#define IIOD_SAMPLES 65536                  /* the buffer, in samples (I and Q, 4 bytes) */
#define IIOD_READ   (4 * IIOD_SAMPLES)

static uint8_t buffer[CHUNK];
static uint8_t run[CHUNK + 251];            /* (65e-26) the pattern from 0, a chunk and a period */

/* (65e-26) The pattern made once, then copied and compared a chunk at a
 * time: made a byte at a time (with a % each) it took the sending core's
 * time from the system the program measures. */
static void make_run(void)
{
    for (size_t i = 0; i < sizeof(run); i++)
        run[i] = (uint8_t)(i % 251);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *list;
    int e = getaddrinfo(host, port, &hints, &list);
    if (e) {
        printf("netrate: %s: %s\n", host, gai_strerror(e));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *a = list; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) < 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(list);
    if (fd < 0)
        printf("netrate: connect to %s port %s: %s\n", host, port, strerror(errno));
    else
        printf("netrate: connected to %s port %s\n", host, port);
    return fd;
}

/* Counting what comes, a line each second. */
static struct {
    double start, second_start, deadline;
    uint64_t total, second_bytes, wrong;
    int seconds;
    bool check;
} m;

static void count(const uint8_t *p, size_t n)
{
    if (m.check)
        for (size_t done = 0; done < n;) {          /* (65e-26) a chunk at a time */
            size_t k = n - done < CHUNK ? n - done : CHUNK;
            const uint8_t *want = run + (m.total + done) % 251;
            if (memcmp(p + done, want, k))
                for (size_t i = 0; i < k; i++)
                    m.wrong += p[done + i] != want[i];
            done += k;
        }
    m.total += n;
    m.second_bytes += n;
    double t = now_s();
    while (t - m.second_start >= 1.0) {
        printf("netrate: second %d: %.2f MB/s\n", ++m.seconds, (double)m.second_bytes / 1e6);
        m.second_bytes = 0;
        m.second_start += 1.0;
    }
}

static void report(const char *why)
{
    double took = now_s() - m.start;
    printf("netrate: %llu bytes in %.2f s: %.2f MB/s (%s)", (unsigned long long)m.total, took,
           took > 0 ? (double)m.total / 1e6 / took : 0.0, why);
    if (m.check)
        printf(", %llu wrong", (unsigned long long)m.wrong);
    printf("\n");
}

static void begin(double seconds)
{
    m.start = m.second_start = now_s();
    m.deadline = m.start + seconds;
}

/* Wait until fd has something, at most until the deadline: false at the
 * deadline. */
static bool readable(int fd)
{
    for (;;) {
        double left = m.deadline - now_s();
        if (left <= 0)
            return false;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, (int)(left * 1000) + 1);
        if (r > 0)
            return true;
        if (r < 0 && errno != EINTR)
            return false;
    }
}

static size_t request_of(const char *text, char *line, size_t size);

static int stream(int argc, char **argv)
{
    int fd = connect_to(argv[1], argv[2]);
    if (fd < 0)
        return 1;
    int at = 4;
    if (at < argc && !strcmp(argv[at], "-p")) {
        m.check = true;
        at++;
    }
    if (at < argc) {
        char line[512];
        size_t n = request_of(argv[at], line, sizeof(line));   /* (an HTTP request's lines) */
        if (write(fd, line, n) != (ssize_t)n) {
            printf("netrate: the request could not be sent\n");
            return 1;
        }
    }
    begin(atof(argv[3]));
    const char *why = "time up";
    while (readable(fd)) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n == 0) {
            why = "the other side closed";
            break;
        }
        if (n < 0) {
            printf("netrate: read: %s\n", strerror(errno));
            why = "a read failed";
            break;
        }
        count(buffer, (size_t)n);
    }
    report(why);
    close(fd);
    return m.check && m.wrong ? 1 : 0;
}

/* --- iiod ------------------------------------------------------------------ */

static int iiod_fd;

static bool send_all(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(iiod_fd, b, n);
        if (w <= 0)
            return false;
        b += w;
        n -= (size_t)w;
    }
    return true;
}

static bool read_all(void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        ssize_t r = read(iiod_fd, b, n);
        if (r <= 0)
            return false;
        b += r;
        n -= (size_t)r;
    }
    return true;
}

/* A line from iiod (an answer's number, a mask), without its break. */
static bool read_line(char *out, size_t size)
{
    size_t n = 0;
    for (;;) {
        char c;
        if (read(iiod_fd, &c, 1) != 1)
            return false;
        if (c == '\n')
            break;
        if (n + 1 < size)
            out[n++] = c;
    }
    out[n] = 0;
    return true;
}

/* A command whose answer is a number (negative: an errno). */
static long command(const char *text)
{
    char line[64];
    if (!send_all(text, strlen(text)) || !read_line(line, sizeof(line)))
        return -EIO;
    return strtol(line, 0, 10);
}

static bool read_rate(char *out, size_t size)
{
    long n = command("READ " IIOD_PHY " INPUT voltage0 sampling_frequency\r\n");
    if (n <= 0 || (size_t)n >= size || !read_all(out, (size_t)n))
        return false;
    out[n] = 0;
    char end;
    read_all(&end, 1);                      /* (the value's line break) */
    return true;
}

static bool write_rate(const char *value)
{
    char text[128];
    snprintf(text, sizeof(text), "WRITE " IIOD_PHY " INPUT voltage0 sampling_frequency %zu\r\n",
             strlen(value));
    if (!send_all(text, strlen(text)) || !send_all(value, strlen(value)))
        return false;
    char line[64];
    return read_line(line, sizeof(line)) && strtol(line, 0, 10) >= 0;
}

static int iiod(int argc, char **argv)
{
    iiod_fd = connect_to(argv[2], IIOD_PORT);
    if (iiod_fd < 0)
        return 1;
    char old[64] = "";
    if (argc > 4) {
        if (!read_rate(old, sizeof(old)) || !write_rate(argv[4])) {
            printf("netrate: iiod: the sample rate could not be set\n");
            return 1;
        }
        char now[64] = "";
        read_rate(now, sizeof(now));
        printf("netrate: iiod: sample rate %s (was %s)\n", now, old);
    }
    char open_cmd[128];
    snprintf(open_cmd, sizeof(open_cmd), "OPEN " IIOD_DEVICE " %d 00000003\r\n", IIOD_SAMPLES);
    long r = command(open_cmd);
    if (r < 0) {
        printf("netrate: iiod: OPEN: %s\n", strerror((int)-r));
        if (old[0])
            write_rate(old);
        return 1;
    }
    char read_cmd[96];
    snprintf(read_cmd, sizeof(read_cmd), "READBUF " IIOD_DEVICE " %d\r\n", IIOD_READ);
    begin(atof(argv[3]));
    const char *why = "time up";
    while (now_s() < m.deadline) {
        if (!send_all(read_cmd, strlen(read_cmd))) {
            why = "the request could not be sent";
            break;
        }
        /* The answer in pieces: each a length line and its bytes, the
         * channel mask once, after the first length (libiio's client). */
        long left = IIOD_READ;
        bool mask = true, failed = false;
        while (left > 0) {
            char line[64];
            if (!read_line(line, sizeof(line))) {
                failed = true;
                break;
            }
            long n = strtol(line, 0, 10);
            if (n <= 0) {
                if (n < 0)
                    printf("netrate: iiod: READBUF: %s\n", strerror((int)-n));
                failed = n < 0;
                break;
            }
            if (mask) {
                if (!read_line(line, sizeof(line))) {
                    failed = true;
                    break;
                }
                mask = false;
            }
            while (n > 0) {
                size_t take = n < CHUNK ? (size_t)n : CHUNK;
                if (!read_all(buffer, take)) {
                    failed = true;
                    break;
                }
                count(buffer, take);
                n -= (long)take;
                left -= (long)take;
            }
            if (failed)
                break;
        }
        if (failed) {
            why = "the answer stopped";
            break;
        }
    }
    report(why);
    command("CLOSE " IIOD_DEVICE "\r\n");
    if (old[0] && !write_rate(old))
        printf("netrate: iiod: the old sample rate %s could not be set again\n", old);
    close(iiod_fd);
    return 0;
}

/* The REQUEST argument with \r and \n made CR and LF, a line break at its
 * end unless it ends with one; its length. */
static size_t request_of(const char *text, char *line, size_t size)
{
    size_t n = 0;
    for (const char *p = text; *p && n + 2 < size; p++) {
        if (p[0] == '\\' && (p[1] == 'n' || p[1] == 'r')) {
            line[n++] = p[1] == 'n' ? '\n' : '\r';
            p++;
        } else {
            line[n++] = *p;
        }
    }
    if (!n || line[n - 1] != '\n')
        line[n++] = '\n';
    return n;
}

static int upload(int argc, char **argv)
{
    int fd = connect_to(argv[2], argv[3]);
    if (fd < 0)
        return 1;
    long long bytes = atoll(argv[4]);
    if (argc > 5) {
        char line[512];
        size_t n = request_of(argv[5], line, sizeof(line));
        if (write(fd, line, n) != (ssize_t)n) {
            printf("netrate: the request could not be sent\n");
            return 1;
        }
    }
    begin(1e9);
    long long sent = 0;
    while (sent < bytes) {
        size_t n = bytes - sent < (long long)sizeof(buffer) ? (size_t)(bytes - sent) : sizeof(buffer);
        memcpy(buffer, run + sent % 251, n);       /* (65e-26) */
        ssize_t w = write(fd, buffer, n);
        if (w <= 0) {
            printf("netrate: write: %s\n", strerror(errno));
            break;
        }
        count(buffer, (size_t)w);
        sent += w;
    }
    shutdown(fd, SHUT_WR);                  /* (a plain receiver sees the end) */
    char answer[256];
    ssize_t got = read(fd, answer, sizeof(answer) - 1);   /* its answer: all arrived */
    double took = now_s() - m.start;
    answer[got > 0 ? got : 0] = 0;
    char *eol = strpbrk(answer, "\r\n");
    if (eol)
        *eol = 0;
    printf("netrate: sent %lld bytes in %.2f s to the answer: %.2f MB/s (answer: '%s')\n", sent,
           took, took > 0 ? (double)sent / 1e6 / took : 0.0, got > 0 ? answer : "none");
    close(fd);
    return sent == bytes ? 0 : 1;
}

static int listen_once(int argc, char **argv)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in at = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[2])) };
    if (s < 0 || bind(s, (struct sockaddr *)&at, sizeof(at)) < 0 || listen(s, 1) < 0) {
        printf("netrate: listen on port %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    m.check = argc > 3 && !strcmp(argv[3], "-p");
    printf("netrate: waiting on port %s\n", argv[2]);
    int fd = accept(s, 0, 0);
    close(s);
    if (fd < 0) {
        printf("netrate: accept: %s\n", strerror(errno));
        return 1;
    }
    printf("netrate: a connection\n");
    begin(1e9);
    const char *why = "the other side ended";
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n == 0)
            break;
        if (n < 0) {
            printf("netrate: read: %s\n", strerror(errno));
            why = "a read failed";
            break;
        }
        count(buffer, (size_t)n);
    }
    report(why);
    char answer[96];
    int k = snprintf(answer, sizeof(answer), "netrate: %llu bytes came\n", (unsigned long long)m.total);
    if (write(fd, answer, (size_t)k) != k)
        printf("netrate: the answer could not be sent\n");
    close(fd);
    return m.check && m.wrong ? 1 : 0;
}

static int file(char **argv)
{
    int fd = open(argv[2], O_RDONLY);
    if (fd < 0) {
        printf("netrate: %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    begin(atof(argv[3]));
    const char *why = "time up";
    while (now_s() < m.deadline) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n <= 0) {
            why = n ? "a read failed" : "the end";
            break;
        }
        count(buffer, (size_t)n);
    }
    report(why);
    close(fd);
    return 0;
}

/* (36e) One connection of serve: the pattern sent until the other side
 * closes, or what comes read and answered. */
static void serve_one(int fd)
{
    memset(&m, 0, sizeof(m));
    begin(1e9);
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n >= 4 && !memcmp(buffer, "send", 4)) {
        long long sent = 0;
        for (;;) {
            memcpy(buffer, run + sent % 251, sizeof(buffer));
            ssize_t w = write(fd, buffer, sizeof(buffer));
            if (w <= 0)
                break;                          /* (the other side closed: done) */
            sent += w;
        }
        double took = now_s() - m.start;
        printf("netrate: sent %lld bytes in %.2f s: %.2f MB/s\n", sent, took,
               took > 0 ? (double)sent / 1e6 / took : 0.0);
        return;
    }
    const char *why = "the other side ended";
    while (n > 0) {
        count(buffer, (size_t)n);
        n = read(fd, buffer, sizeof(buffer));
    }
    if (n < 0)
        why = "a read failed";
    report(why);
    char answer[96];
    int k = snprintf(answer, sizeof(answer), "netrate: %llu bytes came\n", (unsigned long long)m.total);
    if (write(fd, answer, (size_t)k) != k)
        printf("netrate: the answer could not be sent\n");
}

static int serve(char **argv)
{
    signal(SIGPIPE, SIG_IGN);                   /* (a client closing ends its connection only) */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in at = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[2])) };
    if (s < 0 || bind(s, (struct sockaddr *)&at, sizeof(at)) < 0 || listen(s, 4) < 0) {
        printf("netrate: serve on port %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    printf("netrate: serving on port %s\n", argv[2]);
    for (;;) {
        struct sockaddr_in from;
        socklen_t length = sizeof(from);
        int fd = accept(s, (struct sockaddr *)&from, &length);
        if (fd < 0) {
            printf("netrate: accept: %s\n", strerror(errno));
            continue;
        }
        uint32_t a = ntohl(from.sin_addr.s_addr);
        printf("netrate: a connection from %u.%u.%u.%u\n", a >> 24, (a >> 16) & 255,
               (a >> 8) & 255, a & 255);
        serve_one(fd);
        close(fd);
    }
}

int main(int argc, char **argv)
{
    make_run();
    if (argc >= 4 && !strcmp(argv[1], "iiod"))
        return iiod(argc, argv);
    if (argc == 4 && !strcmp(argv[1], "read"))
        return file(argv);
    if (argc >= 5 && !strcmp(argv[1], "send"))
        return upload(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "listen"))
        return listen_once(argc, argv);
    if (argc == 3 && !strcmp(argv[1], "serve"))
        return serve(argv);
    if (argc >= 4)
        return stream(argc, argv);
    printf("usage: netrate HOST PORT SECONDS [-p] [REQUEST]\n"
           "       netrate iiod HOST SECONDS [RATE]\n"
           "       netrate send HOST PORT BYTES [REQUEST]\n"
           "       netrate listen PORT [-p]\n"
           "       netrate serve PORT\n"
           "       netrate read PATH SECONDS\n");
    return 2;
}
