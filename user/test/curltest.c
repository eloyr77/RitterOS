/*
 * Test: (46d-2) libcurl, the system library built from upstream's sources,
 * as a browser uses it (scripts/testcurl.py serves the other end).
 *   curltest features          libcurl's version and its libraries' and
 *                              the features WebKit needs
 *   curltest multi URL N [CA] N transfers of URL (testcurl's /big: 200,000
 *                              bytes, byte i = i * 7 + 3) at once on one
 *                              multi handle, waited for with
 *                              curl_multi_poll; first a curl_multi_wakeup
 *                              from another thread ends a long poll early;
 *                              CA: the roots to trust (else the system's)
 *   curltest threads URL N     N threads, each its own handle, three
 *                              transfers of URL each, at the same time
 *   curltest cookies BASE      the cookie engine: BASE/setcookie sets one
 *                              for a public suffix and one for its domain,
 *                              BASE/cookies shows what was sent back
 *   curltest ws URL            a WebSocket: a text and a 3,000-byte binary
 *                              message sent, their echoes received
 *   curltest connect URL PATH  CURLOPT_CONNECT_ONLY: the connection only,
 *                              then an HTTP/1.0 request sent and the answer
 *                              read with curl_easy_send/curl_easy_recv
 */
#include <curl/curl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define T "curltest: "
#define BIG 200000

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* A body as it arrives: its length and how many bytes differ from /big's. */
struct body {
    size_t got, wrong;
    char text[256];
};

static size_t take(char *data, size_t size, size_t n, void *arg)
{
    struct body *b = arg;
    for (size_t i = 0; i < size * n; i++) {
        if ((unsigned char)data[i] != (unsigned char)((b->got + i) * 7 + 3))
            b->wrong++;
        if (b->got + i < sizeof(b->text) - 1)
            b->text[b->got + i] = data[i];
    }
    b->got += size * n;
    return size * n;
}

static CURL *handle(const char *url, struct body *b)
{
    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, take);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, b);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    return e;
}

static int features(void)
{
    curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    printf(T "%s\n", curl_version());     /* libcurl and each library it uses, with versions */
    static const struct { long bit; const char *name; } f[] = {
        { CURL_VERSION_SSL, "SSL" }, { CURL_VERSION_HTTP2, "HTTP2" }, { CURL_VERSION_IPV6, "IPv6" },
        { CURL_VERSION_ASYNCHDNS, "AsynchDNS" }, { CURL_VERSION_THREADSAFE, "threadsafe" },
        { CURL_VERSION_LIBZ, "libz" }, { CURL_VERSION_BROTLI, "brotli" }, { CURL_VERSION_PSL, "PSL" },
        { CURL_VERSION_HSTS, "HSTS" }, { CURL_VERSION_ALTSVC, "alt-svc" },
    };
    printf(T "features:");
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++)
        printf(" %s %s", f[i].name, v->features & f[i].bit ? "yes" : "no");
    printf("\n" T "protocols:");
    for (const char *const *p = v->protocols; *p; p++)
        if (!strcmp(*p, "http") || !strcmp(*p, "https") || !strcmp(*p, "ws") || !strcmp(*p, "wss"))
            printf(" %s", *p);
    printf("\n");
    return 0;
}

static void *wake_later(void *multi)
{
    usleep(200000);
    curl_multi_wakeup(multi);
    return 0;
}

static int multi(const char *url, int n, const char *ca)
{
    CURLM *m = curl_multi_init();
    pthread_t waker;
    pthread_create(&waker, 0, wake_later, m);
    double t0 = now_ms();
    int fds = 0;
    curl_multi_poll(m, 0, 0, 10000, &fds);
    double woke = now_ms() - t0;
    pthread_join(waker, 0);
    printf(T "multi: curl_multi_wakeup ended a 10 s poll after %s\n", woke < 2000 ? "under 2 s" : "too long");

    struct body *b = calloc((size_t)n, sizeof(*b));
    CURL **e = calloc((size_t)n, sizeof(*e));
    for (int i = 0; i < n; i++) {
        e[i] = handle(url, &b[i]);
        if (ca)
            curl_easy_setopt(e[i], CURLOPT_CAINFO, ca);
        curl_multi_add_handle(m, e[i]);
    }
    int running = 1, rounds = 0;
    while (running && rounds++ < 100000) {
        curl_multi_perform(m, &running);
        if (running)
            curl_multi_poll(m, 0, 0, 1000, 0);
    }
    int ok = 0, done = 0;
    CURLMsg *msg;
    int left;
    while ((msg = curl_multi_info_read(m, &left)))
        if (msg->msg == CURLMSG_DONE) {
            done++;
            ok += msg->data.result == CURLE_OK;
        }
    int whole = 0;
    for (int i = 0; i < n; i++) {
        long code = 0;
        curl_easy_getinfo(e[i], CURLINFO_RESPONSE_CODE, &code);
        whole += code == 200 && b[i].got == BIG && b[i].wrong == 0;
        curl_multi_remove_handle(m, e[i]);
        curl_easy_cleanup(e[i]);
    }
    printf(T "multi: %d transfers at once, %d done, %d without an error, %d of %d bytes each and right\n", n, done,
           ok, whole, BIG);
    curl_multi_cleanup(m);
    return !(woke < 2000 && done == n && ok == n && whole == n);
}

static const char *thread_url;
static int thread_good;
static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;

static void *transfers(void *arg)
{
    (void)arg;
    for (int k = 0; k < 3; k++) {
        struct body b = { 0 };
        CURL *e = handle(thread_url, &b);
        long code = 0;
        int r = curl_easy_perform(e);
        curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(e);
        pthread_mutex_lock(&thread_lock);
        thread_good += r == CURLE_OK && code == 200 && b.got == BIG && b.wrong == 0;
        pthread_mutex_unlock(&thread_lock);
    }
    return 0;
}

static int threads(const char *url, int n)
{
    thread_url = url;
    pthread_t *t = calloc((size_t)n, sizeof(*t));
    for (int i = 0; i < n; i++)
        pthread_create(&t[i], 0, transfers, 0);
    for (int i = 0; i < n; i++)
        pthread_join(t[i], 0);
    printf(T "threads: %d threads, %d of %d transfers whole\n", n, thread_good, 3 * n);
    return thread_good != 3 * n;
}

static int cookies(const char *base)
{
    char url[512];
    struct body b = { 0 };
    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_COOKIEFILE, "");            /* the engine on, no file */
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, take);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &b);
    snprintf(url, sizeof(url), "%s/setcookie", base);
    curl_easy_setopt(e, CURLOPT_URL, url);
    int r1 = curl_easy_perform(e);
    struct curl_slist *list = 0;
    curl_easy_getinfo(e, CURLINFO_COOKIELIST, &list);
    int kept = 0;
    for (struct curl_slist *l = list; l; l = l->next)
        kept++;
    curl_slist_free_all(list);
    memset(&b, 0, sizeof(b));
    snprintf(url, sizeof(url), "%s/cookies", base);
    curl_easy_setopt(e, CURLOPT_URL, url);
    int r2 = curl_easy_perform(e);
    curl_easy_cleanup(e);
    b.text[b.got < sizeof(b.text) ? b.got : sizeof(b.text) - 1] = 0;
    char *nl = strchr(b.text, '\n');
    if (nl)
        *nl = 0;
    printf(T "cookies: %d kept, sent back \"%s\"%s\n", kept, b.text, r1 || r2 ? ", an error" : "");
    return r1 || r2;
}

/* Wait until the connection's socket is readable (or a second passed). */
static void wait_readable(CURL *e)
{
    curl_socket_t s;
    if (curl_easy_getinfo(e, CURLINFO_ACTIVESOCKET, &s) == CURLE_OK) {
        struct pollfd p = { (int)s, POLLIN, 0 };
        poll(&p, 1, 1000);
    }
}

static int ws_echo(CURL *e, const char *data, size_t n, unsigned flags, char *back, size_t *backn)
{
    size_t sent;
    if (curl_ws_send(e, data, n, &sent, 0, flags) != CURLE_OK || sent != n)
        return 0;
    size_t got = 0;
    for (int tries = 0; tries < 50 && got < n;) {
        size_t r = 0;
        const struct curl_ws_frame *meta;
        CURLcode c = curl_ws_recv(e, back + got, *backn - got, &r, &meta);
        if (c == CURLE_AGAIN) {
            wait_readable(e);
            tries++;
            continue;
        }
        if (c != CURLE_OK)
            return 0;
        got += r;
        if (meta->bytesleft == 0 && got >= n)
            break;
    }
    *backn = got;
    return 1;
}

static int ws(const char *url)
{
    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_CONNECT_ONLY, 2L);          /* the WebSocket's upgrade, then ours */
    int r = curl_easy_perform(e);
    char back[4096], bin[3000];
    size_t n = sizeof(back);
    const char *text = "hello over a WebSocket";
    int t = r == CURLE_OK && ws_echo(e, text, strlen(text), CURLWS_TEXT, back, &n) && n == strlen(text) &&
            memcmp(back, text, n) == 0;
    for (size_t i = 0; i < sizeof(bin); i++)
        bin[i] = (char)(i * 13);
    n = sizeof(back);
    int b = t && ws_echo(e, bin, sizeof(bin), CURLWS_BINARY, back, &n) && n == sizeof(bin) &&
            memcmp(back, bin, n) == 0;
    size_t sent;
    curl_ws_send(e, "", 0, &sent, 0, CURLWS_CLOSE);
    curl_easy_cleanup(e);
    printf(T "ws: upgrade %s, text echo %s, binary echo %s\n", r == CURLE_OK ? "ok" : curl_easy_strerror(r),
           t ? "ok" : "FAILED", b ? "ok" : "FAILED");
    return !(t && b);
}

static int connect_only(const char *url, const char *path)
{
    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_CONNECT_ONLY, 1L);
    int r = curl_easy_perform(e);
    char req[512], answer[8192];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: test\r\n\r\n", path);
    size_t sent = 0, got = 0;
    while (r == CURLE_OK && sent < (size_t)n) {
        size_t s = 0;
        CURLcode c = curl_easy_send(e, req + sent, (size_t)n - sent, &s);
        if (c != CURLE_OK && c != CURLE_AGAIN)
            r = c;
        sent += s;
    }
    for (int tries = 0; r == CURLE_OK && tries < 50 && got < sizeof(answer) - 1;) {
        size_t g = 0;
        CURLcode c = curl_easy_recv(e, answer + got, sizeof(answer) - 1 - got, &g);
        if (c == CURLE_AGAIN) {
            wait_readable(e);
            tries++;
            continue;
        }
        if (c != CURLE_OK || g == 0)
            break;                                  /* the end (HTTP/1.0: the server closes) */
        got += g;
    }
    answer[got] = 0;
    curl_easy_cleanup(e);
    char *end = strchr(answer, '\r'), *body = strstr(answer, "\r\n\r\n");
    printf(T "connect: %.*s; body %s", end ? (int)(end - answer) : 0, answer, body ? body + 4 : "(none)\n");
    return r != CURLE_OK || !body;
}

int main(int argc, char **argv)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int rc = 2;
    if (argc == 2 && !strcmp(argv[1], "features"))
        rc = features();
    else if ((argc == 4 || argc == 5) && !strcmp(argv[1], "multi"))
        rc = multi(argv[2], atoi(argv[3]), argc == 5 ? argv[4] : 0);
    else if (argc == 4 && !strcmp(argv[1], "threads"))
        rc = threads(argv[2], atoi(argv[3]));
    else if (argc == 3 && !strcmp(argv[1], "cookies"))
        rc = cookies(argv[2]);
    else if (argc == 3 && !strcmp(argv[1], "ws"))
        rc = ws(argv[2]);
    else if (argc == 4 && !strcmp(argv[1], "connect"))
        rc = connect_only(argv[2], argv[3]);
    else
        fprintf(stderr, "usage: curltest features | multi URL N [CA] | threads URL N | cookies BASE | ws URL | "
                        "connect URL PATH\n");
    curl_global_cleanup();
    return rc;
}
