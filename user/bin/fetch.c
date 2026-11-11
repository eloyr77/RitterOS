/*
 * fetch [-i] [-o FILE] URL: get a web page or file over HTTP or HTTPS and
 * write it to standard output (or FILE).
 *
 * HTTP/1.1 over a TCP connection: the request is a line "GET /path
 * HTTP/1.1" and headers ("Host: ..."), ended by an empty line; the
 * answer is a status line ("HTTP/1.1 200 OK"), headers, an empty line and
 * the body. Its length is given by Content-Length, or it comes in chunks
 * (each one's size in hex on a line of its own, 0 at the end), or it ends
 * when the server closes the connection. Redirections (301, 302, 303,
 * 307, 308 with a Location) are followed, up to 5, also from http:// to
 * https:// and back. https:// (step 23e) is the same over TLS 1.3 (libtls):
 * the server's certificate must chain to a root of
 * /system/certs/cacert.pem and name the host. Whether a body is complete
 * is HTTP's to say (its length, its last chunk), so a server closing
 * without TLS's close_notify is accepted, as other HTTP clients do.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "ritteros.h"
#include "tls.h"

PROGRAM_HELP("fetch [-i] [-o FILE] URL", "get a web page or file over HTTP or HTTPS (-i: with the headers)");

#define REDIRECTS 5

static char in[16384];                  /* what came from the server, not used yet */
static size_t in_at, in_len;
static int connection = -1;
static struct tls *secure;              /* the connection's TLS, for https:// */
static char broken[200];                /* why receiving failed (not just the end) */
static char line[2048];

struct url {
    bool https;
    char host[256];
    bool bracketed;                     /* (64d) [an IPv6 address] */
    uint16_t port;
    char path[1024];
};

static int fail(const char *what, const char *why)
{
    dprintf(STDERR_FILENO, "fetch: %s: %s\n", what, why);
    return 1;
}

static bool same_text(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
        char y = b[i] >= 'A' && b[i] <= 'Z' ? (char)(b[i] + 32) : b[i];
        if (x != y)
            return false;
        if (!x)
            return true;
    }
    return true;
}

/* A TCP connection to host's port (64d): each of getaddrinfo's addresses
 * in its order (IPv6 first where the machine has it) until one answers; a
 * descriptor, or -1 with errno (ENONAME and the like for the name, the
 * last connect's otherwise). */
static int connect_to(const char *host, uint16_t port)
{
    struct addrinfo hints, *list;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    char service[8];
    snprintf(service, sizeof(service), "%u", port);
    errno = 0;
    int r = getaddrinfo(host, service, &hints, &list);
    if (r) {
        if (!errno)
            errno = r == EAI_NONAME ? ENONAME : EINVAL;
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *a = list; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) < 0) {
            int e = errno;
            close(fd);
            fd = -1;
            errno = e;
        }
    }
    freeaddrinfo(list);
    return fd;
}

/* http[s]://HOST[:PORT][/PATH]; false (and why) if it is not one. HOST
 * may be [an IPv6 address]. */
static bool parse_url(const char *text, struct url *u, const char **why)
{
    u->https = same_text(text, "https://", 8);
    if (u->https)
        text += 8;
    else if (same_text(text, "http://", 7))
        text += 7;
    else if (strstr(text, "://")) {
        *why = "not an http:// or https:// address";
        return false;
    }
    size_t n = strcspn(text, ":/");
    u->bracketed = text[0] == '[';                  /* http://[2001:db8::5]:8080/ (64d) */
    if (u->bracketed) {
        const char *end = strchr(text, ']');
        if (!end || (end[1] && end[1] != ':' && end[1] != '/')) {
            *why = "no ] after the IPv6 address";
            return false;
        }
        text++;
        n = (size_t)(end - text);
    }
    if (n == 0 || n >= sizeof(u->host)) {
        *why = "no host name in it";
        return false;
    }
    memcpy(u->host, text, n);
    u->host[n] = 0;
    text += n + (u->bracketed ? 1 : 0);
    u->port = u->https ? 443 : 80;
    if (*text == ':') {
        long port = 0;
        for (text++; *text >= '0' && *text <= '9' && port <= 65535; text++)
            port = port * 10 + (*text - '0');
        if (port < 1 || port > 65535 || (*text && *text != '/')) {
            *why = "not a port number";
            return false;
        }
        u->port = (uint16_t)port;
    }
    if (strlen(text) >= sizeof(u->path)) {
        *why = "the path is too long";
        return false;
    }
    snprintf(u->path, sizeof(u->path), "%s", *text ? text : "/");
    return true;
}

/* Some bytes from the server: their count, 0 at the end, -1. */
static ssize_t receive(void *buf, size_t n)
{
    ssize_t r = secure ? tls_read(secure, buf, n) : read(connection, buf, n);
    if (r < 0 && !broken[0])
        snprintf(broken, sizeof(broken), "%s", secure ? tls_error(secure) : strerror(errno));
    return r;
}

/* The next byte from the server, or -1 at the end (or an error). */
static int next_byte(void)
{
    if (in_at == in_len) {
        ssize_t n = receive(in, sizeof(in));
        if (n <= 0)
            return -1;
        in_at = 0, in_len = (size_t)n;
    }
    return (unsigned char)in[in_at++];
}

/* A line without its CR LF; false at the end of the stream. */
static bool read_line(void)
{
    size_t n = 0;
    int c;
    while ((c = next_byte()) >= 0 && c != '\n')
        if (n < sizeof(line) - 1)
            line[n++] = (char)c;
    if (n && line[n - 1] == '\r')
        n--;
    line[n] = 0;
    return c >= 0;
}

static bool write_all(int fd, const char *data, size_t n)
{
    while (n) {
        ssize_t w = write(fd, data, n);
        if (w <= 0)
            return false;
        data += w, n -= (size_t)w;
    }
    return true;
}

/* Copy up to n bytes of the body (all of it: n < 0) to out; the count. */
static long copy_body(int out, long n, bool *write_failed)
{
    long done = 0;
    while (n < 0 || done < n) {
        if (in_at == in_len) {
            ssize_t r = receive(in, sizeof(in));
            if (r <= 0)
                break;
            in_at = 0, in_len = (size_t)r;
        }
        size_t chunk = in_len - in_at;
        if (n >= 0 && (long)chunk > n - done)
            chunk = (size_t)(n - done);
        if (!write_all(out, in + in_at, chunk)) {
            *write_failed = true;
            break;
        }
        in_at += chunk;
        done += (long)chunk;
    }
    return done;
}

/* The connection closed (TLS's close_notify sent, its complaint about a
 * missing one ignored: see above). */
static void hang_up(void)
{
    if (secure) {
        tls_close(secure);
        tls_free(secure);
        secure = 0;
    }
    close(connection);
}

int main(int argc, char **argv)
{
    const char *output = 0, *address = 0;
    bool headers = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0)
            headers = true;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (!address && argv[i][0] != '-')
            address = argv[i];
        else
            address = 0, i = argc;
    }
    if (!address) {
        dprintf(STDERR_FILENO, "usage: fetch [-i] [-o FILE] URL\n");
        return 2;
    }
    struct url u;
    const char *why;
    if (!parse_url(address, &u, &why))
        return fail(address, why);

    for (int redirects = 0;; redirects++) {
        if ((connection = connect_to(u.host, u.port)) < 0)
            return fail(u.host, strerror(errno));
        if (u.https) {
            secure = tls_client();
            if (!secure || tls_configure(secure, 0) < 0 ||
                tls_connect_socket(secure, connection, u.host) < 0 || tls_handshake(secure) < 0)
                return fail(u.host, secure ? tls_error(secure) : "out of memory");
        }
        char host[272], request[1600];
        const char *left = u.bracketed ? "[" : "", *right = u.bracketed ? "]" : "";
        if (u.port == (u.https ? 443 : 80))
            snprintf(host, sizeof(host), "%s%s%s", left, u.host, right);
        else
            snprintf(host, sizeof(host), "%s%s%s:%u", left, u.host, right, u.port);
        int n = snprintf(request, sizeof(request),
                         "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: RitterOS-fetch/1\r\n"
                         "Accept: */*\r\nConnection: close\r\n\r\n", u.path, host);
        if (secure ? tls_write(secure, request, (size_t)n) != n
                   : !write_all(connection, request, (size_t)n))
            return fail(u.host, secure ? tls_error(secure) : strerror(errno));
        in_at = in_len = 0;

        /* The status line and the headers. */
        if (!read_line() || strncmp(line, "HTTP/1.", 7) != 0 || strlen(line) < 12)
            return fail(u.host, broken[0] ? broken : "not an HTTP answer");
        int status = atoi(line + 9);
        char status_line[128];
        snprintf(status_line, sizeof(status_line), "%s", line + 9);
        if (headers)
            printf("%s\n", line);
        long length = -1;
        bool chunked = false;
        char location[1024] = "";
        while (read_line() && line[0]) {
            if (headers)
                printf("%s\n", line);
            char *value = strchr(line, ':');
            if (!value)
                continue;
            *value++ = 0;
            while (*value == ' ' || *value == '\t')
                value++;
            if (same_text(line, "content-length", 15))
                length = atol(value);
            else if (same_text(line, "transfer-encoding", 18) && same_text(value, "chunked", 7))
                chunked = true;
            else if (same_text(line, "location", 9))
                snprintf(location, sizeof(location), "%s", value);
        }
        if (headers)
            printf("\n");

        if (status >= 300 && status < 400 && status != 304 && location[0]) {
            hang_up();
            if (redirects == REDIRECTS)
                return fail(address, "too many redirections");
            dprintf(STDERR_FILENO, "fetch: %s: redirected to %s\n", status_line, location);
            if (location[0] == '/') {
                snprintf(u.path, sizeof(u.path), "%s", location);
            } else if (!parse_url(location, &u, &why)) {
                return fail(location, why);
            }
            continue;
        }
        if (status < 200 || status >= 300) {
            hang_up();
            return fail(address, status_line);
        }

        int out = STDOUT_FILENO;
        if (output && (out = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
            return fail(output, strerror(errno));
        bool write_failed = false;
        long got = 0;
        if (chunked) {
            for (;;) {
                if (!read_line())
                    break;
                long size = 0;
                for (const char *c = line; *c; c++) {
                    int d = *c >= '0' && *c <= '9' ? *c - '0'
                          : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10
                          : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
                    if (d < 0)
                        break;                      /* ";extensions" */
                    size = size * 16 + d;
                }
                if (size == 0) {
                    length = got;                   /* the last chunk: complete */
                    break;
                }
                long copied = copy_body(out, size, &write_failed);
                got += copied;
                if (copied < size || write_failed || !read_line())
                    break;                          /* the CR LF after the chunk */
            }
            if (length < 0)
                length = got + 1;                   /* it ended inside: short */
        } else {
            got = copy_body(out, length, &write_failed);
        }
        hang_up();
        if (write_failed)
            return fail(output ? output : "standard output", strerror(errno));
        if (broken[0])
            return fail(address, broken);
        if (output && close(out) < 0)
            return fail(output, strerror(errno));
        if (length >= 0 && got < length) {
            dprintf(STDERR_FILENO, "fetch: %s: the connection ended early (%ld of %ld bytes)\n",
                    address, got, length);
            return 1;
        }
        if (output)
            dprintf(STDERR_FILENO, "fetch: %s, %ld bytes saved to %s\n", status_line, got, output);
        return 0;
    }
}
