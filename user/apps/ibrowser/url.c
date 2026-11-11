/*
 * Web addresses (RFC 3986, what pages use): scheme://host:port/path?query.
 * A link's href is made absolute against the page's address: "//host/x"
 * takes the scheme, "/x" the host, "x" the folder of the page, "?q" the
 * page's path; "." and ".." segments are removed. A plain path ("/test/a.html")
 * is a local file. The fragment ("#part") is not part of a struct url (it
 * is never sent); url_resolve keeps it on the address it makes, and the
 * browser scrolls to it (27b). Also the program's small text helpers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser.h"

int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int hex_digit(int c)
{
    c = lower(c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool same_word(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
        a++, b++;
    return !*a && !*b;
}

bool starts_word(const char *s, const char *prefix)
{
    while (*prefix && lower((unsigned char)*s) == lower((unsigned char)*prefix))
        s++, prefix++;
    return !*prefix;
}

char *copy_text(const char *s, size_t n)
{
    char *t = malloc(n + 1);
    if (!t)
        return 0;
    memcpy(t, s, n);
    t[n] = 0;
    return t;
}

char *join_text(char *base, const char *more)
{
    size_t a = base ? strlen(base) : 0, b = strlen(more);
    char *t = realloc(base, a + b + 1);
    if (!t)
        return base;
    memcpy(t + a, more, b + 1);
    return t;
}

bool url_parse(const char *text, struct url *u)
{
    memset(u, 0, sizeof(*u));
    while (is_space(*text))
        text++;
    if (text[0] == '/') {                               /* a local file */
        strcpy(u->scheme, "file");
        snprintf(u->path, sizeof(u->path), "%.*s", (int)strcspn(text, "#"), text);
        return true;
    }
    if (starts_word(text, "about:")) {
        strcpy(u->scheme, "about");
        snprintf(u->path, sizeof(u->path), "%s", text + 6);
        return true;
    }
    const char *rest;
    if (starts_word(text, "http://"))
        strcpy(u->scheme, "http"), u->port = 80, rest = text + 7;
    else if (starts_word(text, "https://"))
        strcpy(u->scheme, "https"), u->port = 443, rest = text + 8;
    else if (starts_word(text, "file://"))
        return url_parse(text + 7, u);
    else if (!strchr(text, ':') || strchr(text, '/') < strchr(text, ':') ||
             (strchr(text, ':') && strchr(text, ':')[1] >= '0' && strchr(text, ':')[1] <= '9'))
        strcpy(u->scheme, "http"), u->port = 80, rest = text;  /* "host/x": http:// assumed */
    else
        return false;
    size_t h = strcspn(rest, ":/?#");
    if (rest[0] == '[') {                               /* (64d) [an IPv6 address], kept so */
        const char *end = strchr(rest, ']');
        if (!end)
            return false;
        h = (size_t)(end - rest) + 1;
    }
    if (h == 0 || h >= sizeof(u->host))
        return false;
    for (size_t i = 0; i < h; i++)
        u->host[i] = (char)lower((unsigned char)rest[i]);
    rest += h;
    if (*rest == ':') {
        long port = 0;
        rest++;
        while (*rest >= '0' && *rest <= '9')
            port = port * 10 + (*rest++ - '0');
        if (port <= 0 || port > 65535)
            return false;
        u->port = (unsigned)port;
    }
    size_t p = strcspn(rest, "#");
    if (p == 0 || rest[0] != '/') {                     /* "host" or "host?q" */
        u->path[0] = '/';
        snprintf(u->path + 1, sizeof(u->path) - 1, "%.*s", (int)p, rest);
    } else if (p >= sizeof(u->path)) {
        return false;
    } else {
        memcpy(u->path, rest, p);
        u->path[p] = 0;
    }
    for (char *c = u->path; *c; c++)                    /* (spaces typed in an address) */
        if (*c == ' ')
            *c = '+';
    return true;
}

void url_text(const struct url *u, char *out, size_t size)
{
    if (!strcmp(u->scheme, "file")) {
        snprintf(out, size, "%s", u->path);
    } else if (!strcmp(u->scheme, "about")) {
        snprintf(out, size, "about:%s", u->path);
    } else if (u->port == (strcmp(u->scheme, "https") ? 80u : 443u)) {
        snprintf(out, size, "%s://%s%s", u->scheme, u->host, u->path);
    } else {
        snprintf(out, size, "%s://%s:%u%s", u->scheme, u->host, u->port, u->path);
    }
}

/* Remove "." and ".." segments of path (in place; the query is left alone). */
static void remove_dots(char *path)
{
    char tail[URL_MAX] = "";
    char *query = strchr(path, '?');
    if (query) {
        snprintf(tail, sizeof(tail), "%s", query);
        *query = 0;
    }
    const char *start[128];                     /* the kept segments */
    size_t length[128];
    int kept = 0;
    bool slash = false;                         /* a '/' after the last one */
    for (const char *s = path; *s;) {
        if (*s == '/') {
            s++;
            slash = true;
            continue;
        }
        size_t n = strcspn(s, "/");
        bool last = !s[n];
        if (n == 1 && s[0] == '.') {
            slash = last;
        } else if (n == 2 && s[0] == '.' && s[1] == '.') {
            if (kept > 0)
                kept--;
            slash = last;
        } else if (kept < 128) {
            start[kept] = s, length[kept] = n, kept++;
            slash = false;
        }
        s += n;
        if (*s == '/' && !s[1])
            slash = true;
    }
    char out[URL_MAX];
    size_t o = 0;
    for (int i = 0; i < kept && o + length[i] + 2 < sizeof(out); i++) {
        out[o++] = '/';
        memcpy(out + o, start[i], length[i]);
        o += length[i];
    }
    if (slash || kept == 0)
        out[o++] = '/';
    out[o] = 0;
    snprintf(path, URL_MAX, "%s%s", out, tail);
}

static bool resolve(const char *base, const char *ref, char *out, size_t size)
{
    while (is_space(*ref))
        ref++;
    struct url b, u;
    if (starts_word(ref, "http://") || starts_word(ref, "https://") || starts_word(ref, "about:") ||
        starts_word(ref, "file://")) {
        if (!url_parse(ref, &u))
            return false;
        remove_dots(u.path);
        url_text(&u, out, size);
        return true;
    }
    if (strchr(ref, ':') && strcspn(ref, ":") < strcspn(ref, "/?#"))
        return false;                                   /* mailto:, javascript:, ... */
    if (!url_parse(base, &b))
        return false;
    u = b;
    char path[URL_MAX];
    size_t n = strcspn(ref, "#");
    if (ref[0] == '/' && ref[1] == '/') {               /* the same scheme, another host */
        char full[URL_MAX + 16];
        snprintf(full, sizeof(full), "%s:%.*s", b.scheme, (int)n, ref);
        return resolve(base, full, out, size);
    }
    if (n == 0) {
        url_text(&b, out, size);                        /* "#part": the page itself */
        return true;
    }
    if (ref[0] == '/') {
        snprintf(path, sizeof(path), "%.*s", (int)n, ref);
    } else if (ref[0] == '?') {
        size_t keep = strcspn(b.path, "?");
        snprintf(path, sizeof(path), "%.*s%.*s", (int)keep, b.path, (int)n, ref);
    } else {
        size_t keep = strcspn(b.path, "?");
        while (keep > 0 && b.path[keep - 1] != '/')
            keep--;                                     /* the page's folder */
        snprintf(path, sizeof(path), "%.*s%.*s", (int)keep, b.path, (int)n, ref);
    }
    for (char *c = path; *c; c++)
        if (*c == ' ')
            *c = '+';
    snprintf(u.path, sizeof(u.path), "%s", path);
    remove_dots(u.path);
    url_text(&u, out, size);
    return true;
}

bool url_resolve(const char *base, const char *ref, char *out, size_t size)
{
    if (!resolve(base, ref, out, size))
        return false;
    const char *fragment = strchr(ref, '#');            /* (27b) kept: the page scrolls there */
    size_t n = strlen(out);
    if (fragment && fragment[1] && n + strlen(fragment) < size)
        snprintf(out + n, size - n, "%s", fragment);
    return true;
}
