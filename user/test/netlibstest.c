/*
 * Test: (46c) nghttp2, brotli and libpsl, system libraries built from
 * upstream's sources. Each line says ok or what failed; the last is
 * "all ok" or "FAILED".
 *   netlibstest DIR   nghttp2: its version; HPACK, the three requests of
 *                     RFC 7541's C.4 (Huffman-coded, one dynamic table)
 *                     decoded; a client's and a server's session talking
 *                     HTTP/2 to each other in memory (the settings, a
 *                     request, a 100,000-byte answer in DATA frames, the
 *                     stream closed without an error). brotli: its version,
 *                     the reference streams in DIR (brotli's tests/testdata:
 *                     ukkonooa, empty, zerosukkanooa) decoded, 300,000 bytes
 *                     compressed at every quality and back, a stream fed a
 *                     byte at a time, a damaged stream refused. libpsl: the
 *                     list built in, public suffixes (ICANN's and the
 *                     private ones), registrable domains, cookie domains.
 */
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <libpsl.h>
#include <nghttp2/nghttp2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T "netlibstest: "

static int failures;

static void check(const char *what, int ok)
{
    printf(T "%s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

static unsigned char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t cap = 1 << 16, len = 0, got;
    unsigned char *p = malloc(cap);
    while ((got = fread(p + len, 1, cap - len, f)) > 0) {
        len += got;
        if (len == cap)
            p = realloc(p, cap *= 2);
    }
    fclose(f);
    *n = len;
    return p;
}

static size_t unhex(const char *h, unsigned char *out)
{
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (unsigned char)v;
    }
    return n;
}

/* ---- nghttp2 ---- */

/* One header block through the inflater: "name: value|" for each field. */
static int inflate_block(nghttp2_hd_inflater *in, const char *hex, char *out, size_t size)
{
    unsigned char block[64];
    size_t n = unhex(hex, block), used = 0, at = 0;
    out[0] = 0;
    for (;;) {
        nghttp2_nv nv;
        int flags = 0;
        nghttp2_ssize r = nghttp2_hd_inflate_hd3(in, &nv, &flags, block + used, n - used, 1);
        if (r < 0)
            return 0;
        used += (size_t)r;
        if (flags & NGHTTP2_HD_INFLATE_EMIT)
            at += (size_t)snprintf(out + at, size - at, "%.*s: %.*s|", (int)nv.namelen, nv.name,
                                   (int)nv.valuelen, nv.value);
        if (flags & NGHTTP2_HD_INFLATE_FINAL) {
            nghttp2_hd_inflate_end_headers(in);
            return 1;
        }
        if (r == 0 && used == n)
            return 0;
    }
}

static void hpack(void)
{
    static const struct { const char *hex, *want; } c4[] = {
        { "828684418cf1e3c2e5f23a6ba0ab90f4ff",
          ":method: GET|:scheme: http|:path: /|:authority: www.example.com|" },
        { "828684be5886a8eb10649cbf",
          ":method: GET|:scheme: http|:path: /|:authority: www.example.com|cache-control: no-cache|" },
        { "828785bf408825a849e95ba97d7f8925a849e95bb8e8b4bf",
          ":method: GET|:scheme: https|:path: /index.html|:authority: www.example.com|custom-key: custom-value|" },
    };
    nghttp2_hd_inflater *in = 0;
    int ok = nghttp2_hd_inflate_new(&in) == 0;
    for (int i = 0; ok && i < 3; i++) {
        char got[256];
        ok = inflate_block(in, c4[i].hex, got, sizeof(got)) && strcmp(got, c4[i].want) == 0;
        if (!ok)
            printf(T "  request %d: %s\n", i + 1, got);
    }
    nghttp2_hd_inflate_del(in);
    check("HPACK: RFC 7541's C.4 requests decoded", ok);
}

#define BODY 100000

struct peer {
    int status, closed, close_error, requests;
    size_t got;
    unsigned long sum;
};

static nghttp2_ssize body_read(nghttp2_session *s, int32_t id, uint8_t *buf, size_t length, uint32_t *flags,
                               nghttp2_data_source *source, void *user)
{
    (void)s, (void)id, (void)user;
    size_t *sent = source->ptr, n = BODY - *sent < length ? BODY - *sent : length;
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((*sent + i) * 7 + 3);
    *sent += n;
    if (*sent == BODY)
        *flags |= NGHTTP2_DATA_FLAG_EOF;
    return (nghttp2_ssize)n;
}

static size_t body_sent;

static int server_frame(nghttp2_session *s, const nghttp2_frame *f, void *user)
{
    struct peer *p = user;
    if (f->hd.type == NGHTTP2_HEADERS && (f->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        p->requests++;
        nghttp2_nv nv[] = { { (uint8_t *)":status", (uint8_t *)"200", 7, 3, NGHTTP2_NV_FLAG_NONE },
                            { (uint8_t *)"content-type", (uint8_t *)"application/octet-stream", 12, 24,
                              NGHTTP2_NV_FLAG_NONE } };
        nghttp2_data_provider2 body = { { .ptr = &body_sent }, body_read };
        nghttp2_submit_response2(s, f->hd.stream_id, nv, 2, &body);
    }
    return 0;
}

static int client_header(nghttp2_session *s, const nghttp2_frame *f, const uint8_t *name, size_t nl,
                         const uint8_t *value, size_t vl, uint8_t flags, void *user)
{
    (void)s, (void)f, (void)flags;
    struct peer *p = user;
    if (nl == 7 && memcmp(name, ":status", 7) == 0)
        p->status = atoi((const char *)value) * (vl == 3);
    return 0;
}

static int client_data(nghttp2_session *s, uint8_t flags, int32_t id, const uint8_t *data, size_t len, void *user)
{
    (void)s, (void)flags, (void)id;
    struct peer *p = user;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != (uint8_t)((p->got + i) * 7 + 3))
            p->sum++;                   /* (a wrong byte) */
    }
    p->got += len;
    return 0;
}

static int client_close(nghttp2_session *s, int32_t id, uint32_t error, void *user)
{
    (void)s, (void)id;
    struct peer *p = user;
    p->closed++;
    p->close_error = (int)error;
    return 0;
}

/* Everything one session has to send, received by the other; how many bytes. */
static long pump(nghttp2_session *from, nghttp2_session *to)
{
    long total = 0;
    for (;;) {
        const uint8_t *data;
        nghttp2_ssize n = nghttp2_session_mem_send2(from, &data);
        if (n < 0)
            return -1;
        if (n == 0)
            return total;
        if (nghttp2_session_mem_recv2(to, data, (size_t)n) != n)
            return -1;
        total += n;
    }
}

static void session(void)
{
    struct peer client = { 0 }, server = { 0 };
    nghttp2_session_callbacks *cc, *sc;
    nghttp2_session_callbacks_new(&cc);
    nghttp2_session_callbacks_new(&sc);
    nghttp2_session_callbacks_set_on_header_callback(cc, client_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cc, client_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(cc, client_close);
    nghttp2_session_callbacks_set_on_frame_recv_callback(sc, server_frame);
    nghttp2_session *c = 0, *s = 0;
    int ok = nghttp2_session_client_new(&c, cc, &client) == 0 && nghttp2_session_server_new(&s, sc, &server) == 0 &&
             nghttp2_submit_settings(c, NGHTTP2_FLAG_NONE, 0, 0) == 0 &&
             nghttp2_submit_settings(s, NGHTTP2_FLAG_NONE, 0, 0) == 0;
    nghttp2_nv req[] = { { (uint8_t *)":method", (uint8_t *)"GET", 7, 3, NGHTTP2_NV_FLAG_NONE },
                         { (uint8_t *)":scheme", (uint8_t *)"https", 7, 5, NGHTTP2_NV_FLAG_NONE },
                         { (uint8_t *)":authority", (uint8_t *)"test.local", 10, 10, NGHTTP2_NV_FLAG_NONE },
                         { (uint8_t *)":path", (uint8_t *)"/big", 5, 4, NGHTTP2_NV_FLAG_NONE } };
    int32_t id = ok ? nghttp2_submit_request2(c, 0, req, 4, 0, 0) : -1;
    long moved = 0;
    for (int round = 0; ok && round < 200; round++) {
        long a = pump(c, s), b = pump(s, c);
        if (a < 0 || b < 0)
            ok = 0;
        moved += a + b;
        if (a == 0 && b == 0 && round > 2)
            break;
    }
    check("HTTP/2 in memory: a request, a 100,000-byte answer, the stream closed",
          ok && id == 1 && server.requests == 1 && client.status == 200 && client.got == BODY && client.sum == 0 &&
          client.closed == 1 && client.close_error == NGHTTP2_NO_ERROR);
    printf(T "  (%ld bytes between the sessions)\n", moved);
    nghttp2_session_del(c);
    nghttp2_session_del(s);
    nghttp2_session_callbacks_del(cc);
    nghttp2_session_callbacks_del(sc);
}

/* ---- brotli ---- */

static void brotli_reference(const char *dir)
{
    static const char *const names[] = { "ukkonooa", "empty" };
    char path[512];
    for (int i = 0; i < 2; i++) {
        size_t zn = 0, wn = 0;
        snprintf(path, sizeof(path), "%s/%s.compressed", dir, names[i]);
        unsigned char *z = read_file(path, &zn);
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        unsigned char *want = read_file(path, &wn);
        size_t outn = wn + 64;
        unsigned char *out = malloc(outn);
        int ok = z && want && BrotliDecoderDecompress(zn, z, &outn, out) == BROTLI_DECODER_RESULT_SUCCESS &&
                 outn == wn && memcmp(out, want, wn) == 0;
        char what[96];
        snprintf(what, sizeof(what), "brotli: the reference stream %s decoded", names[i]);
        check(what, ok);
        free(z);
        free(want);
        free(out);
    }
    size_t zn = 0;
    snprintf(path, sizeof(path), "%s/zerosukkanooa.compressed", dir);
    unsigned char *z = read_file(path, &zn);
    size_t outn = 1 << 20;
    unsigned char *out = malloc(outn);
    int ok = z && BrotliDecoderDecompress(zn, z, &outn, out) == BROTLI_DECODER_RESULT_SUCCESS && outn > 0;
    check("brotli: the reference stream zerosukkanooa decoded", ok);
    free(z);
    free(out);
}

static void brotli_round_trips(void)
{
    size_t n = 300000;
    unsigned char *src = malloc(n), *z = malloc(BrotliEncoderMaxCompressedSize(n)), *back = malloc(n);
    unsigned x = 99;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245 + 12345;
        src[i] = (i % 71 < 50) ? "brotli compresses words, words and more words "[i % 47] : (unsigned char)(x >> 16);
    }
    int ok = 1;
    size_t size11 = 0, size1 = 0;
    for (int q = 0; q <= 11; q++) {
        size_t zn = BrotliEncoderMaxCompressedSize(n), bn = n;
        ok = ok && BrotliEncoderCompress(q, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, n, src, &zn, z) &&
             BrotliDecoderDecompress(zn, z, &bn, back) == BROTLI_DECODER_RESULT_SUCCESS && bn == n &&
             memcmp(src, back, n) == 0;
        if (q == 1)
            size1 = zn;
        if (q == 11)
            size11 = zn;
    }
    check("brotli: 300,000 bytes at qualities 0-11 and back", ok);
    check("brotli: quality 11 smaller than quality 1", size11 > 0 && size11 < size1);

    /* The stream interface, a byte of input at a time. */
    size_t zn = BrotliEncoderMaxCompressedSize(n);
    BrotliEncoderCompress(9, 22, BROTLI_MODE_TEXT, n, src, &zn, z);
    BrotliDecoderState *d = BrotliDecoderCreateInstance(0, 0, 0);
    size_t fed = 0, made = 0;
    BrotliDecoderResult r = BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT;
    while (r != BROTLI_DECODER_RESULT_SUCCESS && r != BROTLI_DECODER_RESULT_ERROR) {
        size_t in_left = fed < zn ? 1 : 0, out_left = n - made;
        const uint8_t *in = z + fed;
        uint8_t *out = back + made;
        r = BrotliDecoderDecompressStream(d, &in_left, &in, &out_left, &out, 0);
        fed = (size_t)(in - z);
        made = (size_t)(out - back);
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && fed == zn)
            break;
    }
    BrotliDecoderDestroyInstance(d);
    check("brotli: decoded a byte of input at a time", r == BROTLI_DECODER_RESULT_SUCCESS && made == n &&
                                                       memcmp(src, back, n) == 0);
    z[zn / 2] ^= 0x5A;
    size_t bn = n;
    check("brotli: a damaged stream refused", BrotliDecoderDecompress(zn, z, &bn, back) != BROTLI_DECODER_RESULT_SUCCESS ||
                                              bn != n || memcmp(src, back, n) != 0);
    free(src);
    free(z);
    free(back);
}

/* ---- libpsl ---- */

static void psl(void)
{
    const psl_ctx_t *p = psl_builtin();
    check("libpsl: the list built in", p != 0 && psl_suffix_count(p) > 9000);
    if (!p)
        return;
    printf(T "  (%d suffixes, %d exceptions, %d wildcards)\n", psl_suffix_count(p), psl_suffix_exception_count(p),
           psl_suffix_wildcard_count(p));
    check("libpsl: com, co.uk, github.io are public suffixes; example.com, example.co.uk are not",
          psl_is_public_suffix(p, "com") && psl_is_public_suffix(p, "co.uk") && psl_is_public_suffix(p, "github.io") &&
          !psl_is_public_suffix(p, "example.com") && !psl_is_public_suffix(p, "example.co.uk"));
    const char *r1 = psl_registrable_domain(p, "www.example.co.uk"), *r2 = psl_registrable_domain(p, "a.b.github.io");
    const char *r3 = psl_registrable_domain(p, "co.uk");
    check("libpsl: registrable domains (example.co.uk, b.github.io, none for co.uk)",
          r1 && strcmp(r1, "example.co.uk") == 0 && r2 && strcmp(r2, "b.github.io") == 0 && r3 == 0);
    check("libpsl: a cookie for example.co.uk from www.example.co.uk, none for co.uk",
          psl_is_cookie_domain_acceptable(p, "www.example.co.uk", "example.co.uk") &&
          !psl_is_cookie_domain_acceptable(p, "www.example.co.uk", "co.uk"));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: netlibstest DIR\n");
        return 2;
    }
    nghttp2_info *info = nghttp2_version(0);
    check("nghttp2 1.71.0", info && strcmp(info->version_str, "1.71.0") == 0);
    hpack();
    session();
    uint32_t v = BrotliDecoderVersion();
    check("brotli 1.2.1", v == (1u << 24 | 2u << 12 | 1u) && BrotliEncoderVersion() == v);
    brotli_reference(argv[1]);
    brotli_round_trips();
    psl();
    printf(T "%s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}
