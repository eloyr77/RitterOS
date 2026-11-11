/*
 * Getting a page or a picture. HTTP/1.1 as fetch does it (user/bin/fetch.c):
 * a GET (or a form's POST, 27c) with Host and "Connection: close", the answer's status line and
 * headers, then the body by its Content-Length, in chunks, or until the
 * server closes. Redirections (301, 302, 303, 307, 308) are followed, up to
 * 5. https:// is the same over TLS 1.3 (libtls, the system's roots). A path
 * is a local file. The type comes from Content-Type, or for a file from its
 * name. Bodies are kept whole in memory, up to BODY_MAX. Since 27b answers
 * may come packed (Content-Encoding gzip, RFC 1952, or deflate: zlib, RFC
 * 1950, or raw DEFLATE as some servers send it); they are unpacked with the
 * inflater libimage uses for PNG (user/lib/inflate.c, linked into the
 * program: the library does not export it). Cookies (27q, RFC 6265): what
 * Set-Cookie and document.cookie set is kept in memory while the browser
 * runs (nothing on the disk) and sent back with the requests it belongs to.
 * A server that does not answer is given up after WAIT_MS: the connection,
 * and each wait for data (27q; the kernel's own limits are minutes, or none).
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "browser.h"
#include "ritteros.h"
#include "tls.h"
#include "../../lib/imagedec.h"

#define REDIRECTS 5
#define WAIT_MS 30000           /* a server that does not answer (the user's choice) */
#define COOKIES 300             /* kept at most (then the oldest go) */
#define COOKIE_SIZE 4096        /* a cookie's name and value together */
#define COOKIE_HEADER 8192      /* the Cookie header's value */

struct conn {
    int fd;
    struct tls *tls;
    char in[16384];
    size_t at, len;
    bool ended;
    bool silent;                /* a read ran out of time (WAIT_MS without data) */
};

static ssize_t conn_read(struct conn *c, void *buf, size_t n)
{
    errno = 0;
    ssize_t r = c->tls ? tls_read(c->tls, buf, n) : read(c->fd, buf, n);
    if (r < 0 && (errno == EAGAIN || errno == ETIMEDOUT))
        c->silent = true;                       /* (SO_RCVTIMEO: WAIT_MS without a byte) */
    return r < 0 ? -1 : r;
}

/* Connect fd to an address within WAIT_MS (a server that never answers
 * would keep the kernel's connect trying for minutes), then give its reads
 * and writes the same limit. -1 with errno (ETIMEDOUT) if not. */
static int connect_within(int fd, const struct addrinfo *a)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    if (connect(fd, a->ai_addr, a->ai_addrlen) < 0) {
        if (errno != EINPROGRESS)
            return -1;
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int ready = poll(&p, 1, WAIT_MS);
        if (ready <= 0) {
            errno = ready == 0 ? ETIMEDOUT : errno;
            return -1;
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
            return -1;
        if (error) {
            errno = error;
            return -1;
        }
    }
    if (fcntl(fd, F_SETFL, flags) < 0)
        return -1;
    struct timeval limit = { .tv_sec = WAIT_MS / 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
    return 0;
}

/* (64d) A TCP connection to host's port: each of getaddrinfo's addresses
 * in its order (IPv6 first where the machine has it) until one answers; a
 * descriptor, or -1 with errno (ENONAME and the like for the name, the
 * last connect's otherwise). A host in brackets is an IPv6 address. */
static int connect_name(const char *host, const char *service)
{
    char name[256];
    size_t n = strlen(host);
    if (n >= 2 && host[0] == '[' && host[n - 1] == ']' && n - 2 < sizeof(name)) {
        memcpy(name, host + 1, n - 2);
        name[n - 2] = 0;
        host = name;
    }
    struct addrinfo hints, *list;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
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
        if (fd >= 0 && connect_within(fd, a) < 0) {
            int e = errno;
            close(fd);
            fd = -1;
            errno = e;
        }
    }
    freeaddrinfo(list);
    return fd;
}

static int next_byte(struct conn *c)
{
    if (c->at == c->len) {
        if (c->ended)
            return -1;
        ssize_t r = conn_read(c, c->in, sizeof(c->in));
        if (r <= 0) {
            c->ended = true;
            return -1;
        }
        c->at = 0, c->len = (size_t)r;
    }
    return (unsigned char)c->in[c->at++];
}

/* One header line without its CR LF; false at the end. */
static bool read_line(struct conn *c, char *line, size_t size)
{
    size_t n = 0;
    for (;;) {
        int b = next_byte(c);
        if (b < 0)
            return n > 0 ? (line[n] = 0, true) : false;
        if (b == '\n')
            break;
        if (b != '\r' && n + 1 < size)
            line[n++] = (char)b;
    }
    line[n] = 0;
    return true;
}

/* Append up to n bytes of the body (n < 0: until the end) to r. */
static bool read_body(struct conn *c, struct response *r, long n)
{
    while (n != 0) {
        if (c->at == c->len) {
            if (c->ended)
                return n < 0;
            ssize_t got = conn_read(c, c->in, sizeof(c->in));
            if (got <= 0) {
                c->ended = true;
                return n < 0;
            }
            c->at = 0, c->len = (size_t)got;
        }
        size_t take = c->len - c->at;
        if (n >= 0 && (long)take > n)
            take = (size_t)n;
        if (r->length + (long)take > BODY_MAX) {
            snprintf(r->error, sizeof(r->error), "larger than %ld MiB", BODY_MAX >> 20);
            return false;
        }
        char *body = realloc(r->body, (size_t)r->length + take + 1);
        if (!body) {
            snprintf(r->error, sizeof(r->error), "out of memory");
            return false;
        }
        r->body = body;
        memcpy(body + r->length, c->in + c->at, take);
        r->length += (long)take;
        body[r->length] = 0;
        c->at += take;
        if (n > 0)
            n -= (long)take;
    }
    return true;
}

/* Unpack into a buffer that grows (DEFLATE does not say how much comes out):
 * from `guess` bytes, doubled up to BODY_MAX while it is too small. One of
 * gzip's members (start: after its header) or a zlib or raw stream. */
enum packing { PACK_RAW, PACK_ZLIB };

static long unpack(const uint8_t *in, size_t n, enum packing kind, size_t guess, uint8_t **out,
                   size_t *used)
{
    size_t cap = guess < 65536 ? 65536 : guess;
    for (;;) {
        if (cap > (size_t)BODY_MAX)
            cap = (size_t)BODY_MAX;
        uint8_t *buf = malloc(cap + 1);
        if (!buf)
            return -1;
        long got = kind == PACK_ZLIB ? inflate_zlib(in, n, buf, cap) : inflate_raw(in, n, buf, cap, used);
        if (got >= 0) {
            *out = buf;
            return got;
        }
        free(buf);
        if (cap == (size_t)BODY_MAX)
            return -1;                          /* (damaged, or larger than a page may be) */
        cap *= 2;
    }
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* gzip (RFC 1952): members one after another, each a header, DEFLATE, then
 * the CRC-32 and the length (mod 2^32) of what it held. */
static long gunzip(const uint8_t *in, size_t n, uint8_t **out)
{
    uint8_t *all = 0;
    size_t total = 0, at = 0;
    while (at < n) {
        const uint8_t *m = in + at;
        size_t left = n - at, h = 10;
        if (left < 18 || m[0] != 0x1F || m[1] != 0x8B || m[2] != 8 || (m[3] & 0xE0))
            break;                              /* not a member (trailing bytes: the end) */
        uint8_t flags = m[3];
        if (flags & 4) {                        /* FEXTRA: its length, then the data */
            size_t xlen = (size_t)m[10] | (size_t)m[11] << 8;
            if (xlen > left - 12)
                break;
            h = 12 + xlen;
        }
        for (int field = 8; field <= 16; field *= 2)    /* FNAME, FCOMMENT: NUL-ended */
            if (flags & field) {
                while (h < left && m[h])
                    h++;
                h++;
            }
        if (flags & 2)                          /* FHCRC */
            h += 2;
        if (h + 8 > left)
            break;
        uint8_t *part;
        size_t used = 0;
        long got = unpack(m + h, left - h, PACK_RAW, le32(in + n - 4), &part, &used);
        if (got < 0 || h + used + 8 > left || crc32(part, (size_t)got) != le32(m + h + used) ||
            (uint32_t)got != le32(m + h + used + 4) || total + (size_t)got > (size_t)BODY_MAX) {
            if (got >= 0)
                free(part);
            free(all);
            return -1;
        }
        uint8_t *grown = realloc(all, total + (size_t)got + 1);
        if (!grown) {
            free(part);
            free(all);
            return -1;
        }
        all = grown;
        memcpy(all + total, part, (size_t)got);
        free(part);
        total += (size_t)got;
        at += h + used + 8;
    }
    if (!all)
        return -1;                              /* (not even one member) */
    *out = all;
    return (long)total;
}

/* The body as it is meant to be read: unpacked if it came packed. */
static bool unpack_body(struct response *r)
{
    if (!r->length)
        r->encoding[0] = 0;                     /* (nothing came: nothing to unpack) */
    if (!r->encoding[0])
        return true;
    uint8_t *out = 0;
    long got = -1;
    const uint8_t *in = (const uint8_t *)r->body;
    size_t n = (size_t)r->length;
    if (!strcmp(r->encoding, "gzip")) {
        got = gunzip(in, n, &out);
    } else {                                    /* deflate: zlib as the standard says, or raw */
        got = unpack(in, n, PACK_ZLIB, 4 * n, &out, 0);
        size_t used;
        if (got < 0)
            got = unpack(in, n, PACK_RAW, 4 * n, &out, &used);
    }
    if (got < 0) {
        snprintf(r->error, sizeof(r->error), "the page's %s data is damaged", r->encoding);
        return false;
    }
    out[got] = 0;
    free(r->body);
    r->body = (char *)out;
    r->packed = r->length;
    r->length = got;
    return true;
}

static void hang_up(struct conn *c)
{
    if (c->tls) {
        tls_close(c->tls);
        tls_free(c->tls);
        c->tls = 0;
    }
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

static void type_from_name(const char *path, char *type, size_t size)
{
    const char *dot = strrchr(path, '.');
    const char *t = "application/octet-stream";
    if (dot && (same_word(dot, ".html") || same_word(dot, ".htm")))
        t = "text/html";
    else if (dot && same_word(dot, ".css"))
        t = "text/css";
    else if (dot && same_word(dot, ".txt"))
        t = "text/plain";
    else if (dot && same_word(dot, ".png"))
        t = "image/png";
    else if (dot && (same_word(dot, ".jpg") || same_word(dot, ".jpeg")))
        t = "image/jpeg";
    snprintf(type, size, "%s", t);
}

static int get_file(const char *path, struct response *r)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(r->error, sizeof(r->error), "%s", strerror(errno));
        return 0;
    }
    struct conn c = { .fd = fd };
    bool ok = read_body(&c, r, -1);
    close(fd);
    if (!ok)
        return 0;
    if (!r->body)
        r->body = calloc(1, 1);
    type_from_name(path, r->type, sizeof(r->type));
    return 1;
}

/* --- cookies (27q) --------------------------------------------------------- */

struct cookie {
    char *name, *value;
    char domain[256];                   /* lower case, no leading dot */
    char path[256];
    bool host_only, secure, http_only;
    long long expires;                  /* seconds since 1970, or 0: while the browser runs */
    unsigned long order;                /* when it was first made (older ones are sent first) */
};

static struct cookie jar[COOKIES];
static int jar_n;
static unsigned long jar_made;
static mtx_t jar_lock;                  /* (the picture loader fetches too) */
static once_flag jar_once = ONCE_FLAG_INIT;

static void jar_init(void)
{
    mtx_init(&jar_lock, mtx_plain);
}

static void jar_drop(int i)
{
    free(jar[i].name);
    free(jar[i].value);
    jar[i] = jar[--jar_n];
}

static void jar_expire(long long now)
{
    for (int i = 0; i < jar_n;)
        if (jar[i].expires && jar[i].expires <= now)
            jar_drop(i);
        else
            i++;
}

static char *trimmed(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    return s;
}

/* days from 1970-01-01 to y-m-d (the proleptic Gregorian calendar) */
static long long days_from_civil(long long y, int m, int d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* RFC 6265's date (section 5.1.1): the time, day, month and year found
 * among the tokens, whatever their order; -1 if one is missing */
static long long cookie_date(const char *text)
{
    static const char MONTHS[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    int hour = -1, minute = 0, second = 0, day = -1, month = -1, year = -1;
    const char *p = text;
    while (*p) {
        while (*p && !((*p >= '0' && *p <= '9') || (lower(*p) >= 'a' && lower(*p) <= 'z') || *p == ':'))
            p++;
        const char *t = p;
        while (*p && ((*p >= '0' && *p <= '9') || (lower(*p) >= 'a' && lower(*p) <= 'z') || *p == ':'))
            p++;
        size_t n = (size_t)(p - t);
        if (!n)
            continue;
        int h, mi, sec;
        if (hour < 0 && n <= 8 && sscanf(t, "%2d:%2d:%2d", &h, &mi, &sec) == 3) {
            hour = h, minute = mi, second = sec;
            continue;
        }
        if (day < 0 && n <= 2 && t[0] >= '0' && t[0] <= '9') {
            day = atoi(t);
            continue;
        }
        if (month < 0 && n >= 3) {
            for (int k = 0; k < 12; k++)
                if (lower(t[0]) == MONTHS[3 * k] && lower(t[1]) == MONTHS[3 * k + 1] &&
                    lower(t[2]) == MONTHS[3 * k + 2])
                    month = k + 1;
            if (month > 0)
                continue;
        }
        if (year < 0 && (n == 2 || n == 4) && t[0] >= '0' && t[0] <= '9') {
            year = atoi(t);
            if (year < 70 && n == 2)
                year += 2000;
            else if (year < 100 && n == 2)
                year += 1900;
        }
    }
    if (hour < 0 || day < 1 || day > 31 || month < 0 || year < 1601 || hour > 23 || minute > 59 || second > 59)
        return -1;
    return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

static bool is_ip(const char *host)
{
    for (const char *p = host; *p; p++)
        if (!((*p >= '0' && *p <= '9') || *p == '.'))
            return false;
    return host[0] != 0;
}

static bool domain_match(const char *host, const char *domain)
{
    size_t h = strlen(host), d = strlen(domain);
    return !strcmp(host, domain) || (h > d && !strcmp(host + h - d, domain) && host[h - d - 1] == '.' && !is_ip(host));
}

/* the path of u's address without its query (the cookie paths' "request path") */
static void request_path(const struct url *u, char *out, size_t size)
{
    snprintf(out, size, "%.*s", (int)strcspn(u->path, "?#"), u->path);
    if (out[0] != '/')
        snprintf(out, size, "/");
}

static bool path_match(const char *request, const char *path)
{
    size_t n = strlen(path);
    return !strncmp(request, path, n) && (request[n] == 0 || path[n - 1] == '/' || request[n] == '/');
}

static void lowered(char *s)
{
    for (; *s; s++)
        *s = (char)lower((unsigned char)*s);
}

/* A Set-Cookie header's value (or what a script gave document.cookie) for u. */
static void cookie_store(const struct url *u, const char *header, bool from_script)
{
    if ((strcmp(u->scheme, "http") && strcmp(u->scheme, "https")) || !u->host[0] || strlen(header) > COOKIE_SIZE)
        return;
    char *copy = strdup(header);
    if (!copy)
        return;
    char *attrs = strchr(copy, ';');
    if (attrs)
        *attrs++ = 0;
    char *eq = strchr(copy, '=');
    if (!eq) {
        free(copy);
        return;                                 /* (no name=value: ignored, as RFC 6265 says) */
    }
    *eq = 0;
    char *name = trimmed(copy), *value = trimmed(eq + 1);
    char host[256];
    snprintf(host, sizeof(host), "%s", u->host);
    lowered(host);
    struct cookie c;
    memset(&c, 0, sizeof(c));
    snprintf(c.domain, sizeof(c.domain), "%s", host);
    c.host_only = true;
    char rpath[URL_MAX];
    request_path(u, rpath, sizeof(rpath));
    char *slash = strrchr(rpath, '/');                  /* (the default path: the request's folder) */
    snprintf(c.path, sizeof(c.path), "%.*s", slash && slash != rpath ? (int)(slash - rpath) : 1, rpath);
    long long now = (long long)time(0);
    bool max_age = false;
    while (attrs && *attrs) {
        char *next = strchr(attrs, ';');
        if (next)
            *next++ = 0;
        char *v = strchr(attrs, '=');
        if (v)
            *v++ = 0;
        char *key = trimmed(attrs);
        v = v ? trimmed(v) : "";
        if (same_word(key, "expires") && !max_age) {
            long long t = cookie_date(v);
            if (t >= 0)
                c.expires = t > 0 ? t : 1;
        } else if (same_word(key, "max-age") && ((v[0] >= '0' && v[0] <= '9') || v[0] == '-')) {
            long long secs = atoll(v);
            c.expires = secs <= 0 ? 1 : now + (secs > 400LL * 86400 ? 400LL * 86400 : secs);
            max_age = true;
        } else if (same_word(key, "domain") && v[0]) {
            char d[256];
            snprintf(d, sizeof(d), "%s", v[0] == '.' ? v + 1 : v);
            lowered(d);
            if (!domain_match(host, d) || (!strchr(d, '.') && strcmp(d, host))) {
                free(copy);
                return;                         /* (another site's, or a whole top-level domain) */
            }
            snprintf(c.domain, sizeof(c.domain), "%s", d);
            c.host_only = !strcmp(d, host) && is_ip(host);
        } else if (same_word(key, "path") && v[0] == '/')
            snprintf(c.path, sizeof(c.path), "%s", v);
        else if (same_word(key, "secure"))
            c.secure = true;
        else if (same_word(key, "httponly"))
            c.http_only = true;
        attrs = next;
    }
    if ((c.secure && strcmp(u->scheme, "https")) || (from_script && c.http_only) || !name[0]) {
        free(copy);
        return;
    }
    call_once(&jar_once, jar_init);
    mtx_lock(&jar_lock);
    jar_expire(now);
    c.order = ++jar_made;
    for (int i = 0; i < jar_n; i++)
        if (!strcmp(jar[i].name, name) && !strcmp(jar[i].domain, c.domain) && !strcmp(jar[i].path, c.path) &&
            jar[i].host_only == c.host_only) {
            if (from_script && jar[i].http_only) {
                mtx_unlock(&jar_lock);
                free(copy);
                return;                         /* (a script cannot replace an HttpOnly one) */
            }
            c.order = jar[i].order;             /* (it keeps its place) */
            jar_drop(i);
            break;
        }
    if (c.expires && c.expires <= now) {
        mtx_unlock(&jar_lock);                  /* (an expiry in the past only deletes) */
        free(copy);
        return;
    }
    if (jar_n == COOKIES) {
        int oldest = 0;
        for (int i = 1; i < jar_n; i++)
            if (jar[i].order < jar[oldest].order)
                oldest = i;
        jar_drop(oldest);
    }
    c.name = strdup(name);
    c.value = strdup(value);
    if (c.name && c.value)
        jar[jar_n++] = c;
    else {
        free(c.name);
        free(c.value);
    }
    mtx_unlock(&jar_lock);
    free(copy);
}

/* The cookies for u, "a=1; b=2" into out (longer paths first, then the
 * older ones), without the HttpOnly ones for a script. */
static void cookie_list(const struct url *u, bool for_script, char *out, size_t size)
{
    out[0] = 0;
    if ((strcmp(u->scheme, "http") && strcmp(u->scheme, "https")) || !u->host[0])
        return;
    char host[256], rpath[URL_MAX];
    snprintf(host, sizeof(host), "%s", u->host);
    lowered(host);
    request_path(u, rpath, sizeof(rpath));
    call_once(&jar_once, jar_init);
    mtx_lock(&jar_lock);
    jar_expire((long long)time(0));
    int pick[COOKIES], n = 0;
    for (int i = 0; i < jar_n; i++) {
        const struct cookie *c = &jar[i];
        if ((c->host_only ? strcmp(host, c->domain) != 0 : !domain_match(host, c->domain)) ||
            !path_match(rpath, c->path) || (c->secure && strcmp(u->scheme, "https")) ||
            (for_script && c->http_only))
            continue;
        int k = n++;
        while (k > 0 && (strlen(jar[pick[k - 1]].path) < strlen(c->path) ||
                         (strlen(jar[pick[k - 1]].path) == strlen(c->path) && jar[pick[k - 1]].order > c->order))) {
            pick[k] = pick[k - 1];
            k--;
        }
        pick[k] = i;
    }
    size_t at = 0;
    for (int k = 0; k < n; k++) {
        const struct cookie *c = &jar[pick[k]];
        size_t need = strlen(c->name) + strlen(c->value) + 3;
        if (at + need >= size)
            break;                              /* (whole cookies only) */
        at += (size_t)snprintf(out + at, size - at, "%s%s=%s", at ? "; " : "", c->name, c->value);
    }
    mtx_unlock(&jar_lock);
}

void cookie_set(const char *address, const char *text, bool from_script)
{
    struct url u;
    if (url_parse(address, &u))
        cookie_store(&u, text, from_script);
}

char *cookie_get(const char *address, bool for_script)
{
    struct url u;
    char *out = malloc(COOKIE_HEADER);
    if (out && url_parse(address, &u))
        cookie_list(&u, for_script, out, COOKIE_HEADER);
    else if (out)
        out[0] = 0;
    return out;
}

/* (27r) do the header lines ("Name: value\r\n"...) have one called name? */
static bool header_named(const char *headers, const char *name)
{
    size_t n = strlen(name);
    for (const char *h = headers; h && *h;) {
        size_t k = 0;
        while (k < n && h[k] && lower((unsigned char)h[k]) == lower((unsigned char)name[k]))
            k++;
        if (k == n && h[n] == ':')
            return true;
        const char *e = strstr(h, "\r\n");
        h = e ? e + 2 : 0;
    }
    return false;
}

/* (27r) the response's header lines, as they came ("name: value\n") */
static void keep_header(struct response *r, const char *line)
{
    size_t have = r->headers ? strlen(r->headers) : 0, n = strlen(line);
    if (have + n + 2 > 65536)
        return;
    char *h = realloc(r->headers, have + n + 2);
    if (!h)
        return;
    memcpy(h + have, line, n);
    h[have + n] = '\n';
    h[have + n + 1] = 0;
    r->headers = h;
}

/* One request: 1 done, 2 redirected (r->url is the next address; *keep: a
 * 307 or 308, which keep the method and the body), 0 failed. */
static int get_once(const struct url *u, struct response *r, const char *method, const char *headers,
                    const char *body, size_t body_n, bool no_cookies, bool *keep)
{
    struct conn c = { .fd = -1 };
    char service[8];
    snprintf(service, sizeof(service), "%u", u->port);
    c.fd = connect_name(u->host, service);
    if (c.fd < 0) {
        if (errno == ETIMEDOUT)
            snprintf(r->error, sizeof(r->error), "%s: the server did not answer", u->host);
        else
            snprintf(r->error, sizeof(r->error), "%s: %s", u->host, strerror(errno));
        hang_up(&c);
        return 0;
    }
    if (!strcmp(u->scheme, "https")) {
        c.tls = tls_client();
        if (!c.tls || tls_configure(c.tls, 0) < 0 || tls_connect_socket(c.tls, c.fd, u->host) < 0 ||
            tls_handshake(c.tls) < 0) {
            snprintf(r->error, sizeof(r->error), "%s: %s", u->host,
                     c.tls ? tls_error(c.tls) : "out of memory");
            hang_up(&c);
            return 0;
        }
    }
    char host[272], cookies[COOKIE_HEADER];
    if (u->port == (c.tls ? 443u : 80u))
        snprintf(host, sizeof(host), "%s", u->host);
    else
        snprintf(host, sizeof(host), "%s:%u", u->host, u->port);
    char extra[COOKIE_HEADER + 160] = "";
    if (body)
        snprintf(extra, sizeof(extra), "Content-Length: %zu\r\n", body_n);
    cookies[0] = 0;
    if (!no_cookies)
        cookie_list(u, false, cookies, sizeof(cookies));    /* (27q) */
    if (cookies[0])
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "Cookie: %s\r\n", cookies);
    size_t room = strlen(u->path) + strlen(headers ? headers : "") + strlen(extra) + 600;
    char *request = malloc(room);
    if (!request) {
        snprintf(r->error, sizeof(r->error), "out of memory");
        hang_up(&c);
        return 0;
    }
    int n = snprintf(request, room,
                     "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: iBrowser/1 (RitterOS)\r\n%s"
                     "Accept-Encoding: gzip, deflate\r\n%s%sConnection: close\r\n\r\n",
                     method, u->path, host,
                     header_named(headers, "accept") ? "" : "Accept: text/html,image/png,image/jpeg,*/*\r\n",
                     headers ? headers : "", extra);
    bool sent = true;
    for (int part = 0; part < 2 && sent; part++) {    /* the request, then the form's data */
        const char *what = part ? body : request;
        size_t size = part ? (body ? body_n : 0) : (size_t)n;
        for (size_t at = 0; sent && at < size;) {
            ssize_t w = c.tls ? tls_write(c.tls, what + at, size - at)
                              : write(c.fd, what + at, size - at);
            sent = w > 0;
            at += sent ? (size_t)w : 0;
        }
    }
    free(request);
    if (!sent) {
        snprintf(r->error, sizeof(r->error), "%s: %s", u->host, c.tls ? tls_error(c.tls) : strerror(errno));
        hang_up(&c);
        return 0;
    }
    char line[COOKIE_SIZE + 64];
    if (!read_line(&c, line, sizeof(line)) || strncmp(line, "HTTP/1.", 7) || strlen(line) < 12) {
        snprintf(r->error, sizeof(r->error), "%s: %s", u->host,
                 c.silent ? "the server did not answer" : "not an HTTP answer");
        hang_up(&c);
        return 0;
    }
    r->status = atoi(line + 9);
    snprintf(r->status_text, sizeof(r->status_text), "%s", strlen(line) > 13 ? line + 13 : "");
    long length = -1;
    bool chunked = false;
    char location[URL_MAX] = "";
    r->type[0] = 0;
    r->encoding[0] = 0;                         /* (a redirection's own does not count) */
    free(r->headers);
    r->headers = 0;
    while (read_line(&c, line, sizeof(line)) && line[0]) {
        keep_header(r, line);
        char *value = strchr(line, ':');
        if (!value)
            continue;
        *value++ = 0;
        while (*value == ' ' || *value == '\t')
            value++;
        if (same_word(line, "content-length"))
            length = atol(value);
        else if (same_word(line, "transfer-encoding") && starts_word(value, "chunked"))
            chunked = true;
        else if (same_word(line, "content-encoding")) {
            if (starts_word(value, "gzip") || starts_word(value, "x-gzip"))
                strcpy(r->encoding, "gzip");
            else if (starts_word(value, "deflate"))
                strcpy(r->encoding, "deflate");
        } else if (same_word(line, "set-cookie")) {
            if (!no_cookies)
                cookie_store(u, value, false);  /* (27q; a redirection's too) */
        }
        else if (same_word(line, "location"))
            snprintf(location, sizeof(location), "%s", value);
        else if (same_word(line, "content-type")) {
            size_t t = strcspn(value, "; ");
            if (t >= sizeof(r->type))
                t = sizeof(r->type) - 1;
            for (size_t i = 0; i < t; i++)
                r->type[i] = (char)lower((unsigned char)value[i]);
            r->type[t] = 0;
        }
    }
    if (r->status >= 300 && r->status < 400 && r->status != 304 && location[0]) {
        *keep = r->status == 307 || r->status == 308;
        hang_up(&c);
        char here[URL_MAX];
        url_text(u, here, sizeof(here));
        if (!url_resolve(here, location, r->url, sizeof(r->url))) {
            snprintf(r->error, sizeof(r->error), "a redirection to %s", location);
            return 0;
        }
        return 2;
    }
    bool ok = true;
    if (!strcmp(method, "HEAD") || r->status == 204 || r->status == 304 || r->status < 200) {
        length = 0;                             /* (27r: answers without a body) */
        chunked = false;
    }
    if (chunked) {
        for (;;) {
            if (!read_line(&c, line, sizeof(line))) {
                ok = false;
                break;
            }
            long size = 0;
            for (const char *h = line; *h; h++) {
                int d = *h >= '0' && *h <= '9' ? *h - '0'
                      : lower(*h) >= 'a' && lower(*h) <= 'f' ? lower(*h) - 'a' + 10 : -1;
                if (d < 0)
                    break;
                size = size * 16 + d;
                if (size > BODY_MAX)
                    break;
            }
            if (size == 0)
                break;
            if (!read_body(&c, r, size) || !read_line(&c, line, sizeof(line))) {
                ok = false;
                break;
            }
        }
    } else {
        ok = read_body(&c, r, length);
    }
    hang_up(&c);
    if (!ok && !r->error[0])
        snprintf(r->error, sizeof(r->error), "%s: %s", u->host,
                 c.silent ? "the server stopped answering" : "the connection ended early");
    if (ok && r->body && !unpack_body(r))
        ok = false;
    if (!r->body)
        r->body = calloc(1, 1);
    if (!r->type[0])
        type_from_name(u->path, r->type, sizeof(r->type));
    return ok && r->body ? 1 : 0;
}

int http_request(const struct http_request *q, struct response *r)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->url, sizeof(r->url), "%s", q->url);
    const char *method = q->method ? q->method : q->body ? "POST" : "GET";
    const char *body = q->body;
    size_t body_n = q->body_n;
    for (int redirects = 0;; redirects++) {
        struct url u;
        if (!url_parse(r->url, &u)) {
            snprintf(r->error, sizeof(r->error), "%s: not an address", r->url);
            return 0;
        }
        url_text(&u, r->url, sizeof(r->url));
        if (!strcmp(u.scheme, "file") && !q->no_files)
            return get_file(u.path, r);
        if (strcmp(u.scheme, "http") && strcmp(u.scheme, "https")) {
            snprintf(r->error, sizeof(r->error), "%s: not http or https", r->url);
            return 0;
        }
        bool keep = false;
        int got = get_once(&u, r, method, q->headers, body, body_n, q->no_cookies, &keep);
        if (got == 2 && q->one_hop) {           /* (the redirection itself: r->url is where it points) */
            r->redirected = true;
            r->body = calloc(1, 1);
            return r->body ? 1 : 0;
        }
        if (got != 2)
            return got;
        r->redirected = true;
        if (!keep && strcmp(method, "HEAD")) {  /* (301-303: the next one is a GET, as browsers do) */
            method = "GET";
            body = 0, body_n = 0;
        }
        free(r->body);
        r->body = 0, r->length = 0;
        if (redirects == REDIRECTS) {
            snprintf(r->error, sizeof(r->error), "too many redirections");
            return 0;
        }
    }
}

int http_get(const char *address, struct response *r)
{
    struct http_request q = { .url = address };
    return http_request(&q, r);
}

int http_post(const char *address, const char *body, size_t n, struct response *r)
{
    struct http_request q = { .url = address, .method = "POST", .body = body, .body_n = n,
                              .headers = "Content-Type: application/x-www-form-urlencoded\r\n" };
    return http_request(&q, r);
}

void response_free(struct response *r)
{
    free(r->body);
    r->body = 0;
    free(r->headers);
    r->headers = 0;
}
