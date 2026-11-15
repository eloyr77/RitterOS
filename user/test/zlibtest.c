/*
 * Test: (46a) zlib, the system library libz.so.1 built from upstream's
 * sources. Each line says ok or what failed.
 *   zlibtest                the library's own checks: its version, CRC-32 and
 *                           Adler-32 of the standard check string and their
 *                           combine functions, compress/uncompress at every
 *                           level, deflate and inflate as zlib, gzip and raw
 *                           streams in small pieces with every strategy, a
 *                           preset dictionary, a corrupted stream refused,
 *                           and a gzip file written with gzwrite/gzprintf and
 *                           read back with gzread, gzgets and gzseek
 *   zlibtest gunzip FILE    FILE (made elsewhere, by the Mac's gzip) decoded
 *                           with gzread: its length and CRC-32, for the test
 *                           to compare with the Mac's
 *   zlibtest inflate FILE   FILE a zlib stream (the Mac's Python): the same
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define T "zlibtest: "

static int failures;

static void check(const char *what, int ok)
{
    printf(T "%s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

/* Data that compresses somewhat: text with repeats and some noise. */
static unsigned char *sample(size_t n)
{
    unsigned char *p = malloc(n);
    unsigned x = 12345;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245 + 12345;
        p[i] = (i % 97 < 60) ? "the quick brown fox jumps over the lazy dog "[i % 44]
                             : (unsigned char)(x >> 16);
    }
    return p;
}

/* Deflate src with the given window bits and strategy, feeding the input
 * and taking the output a few bytes at a time, then inflate it the same
 * way; true when the bytes come back. */
static int round_trip(const unsigned char *src, size_t n, int level, int bits, int strategy)
{
    size_t cap = deflateBound(0, n) + 64;
    unsigned char *mid = malloc(cap), *back = malloc(n + 1);
    z_stream d = { 0 }, f = { 0 };
    int ok = deflateInit2(&d, level, Z_DEFLATED, bits, 8, strategy) == Z_OK;
    size_t in = 0, out = 0;
    int r = Z_OK;
    while (ok && r != Z_STREAM_END) {
        size_t give = n - in < 777 ? n - in : 777;
        d.next_in = (unsigned char *)src + in;
        d.avail_in = (uInt)give;
        d.next_out = mid + out;
        d.avail_out = (uInt)(cap - out < 100 ? cap - out : 100);
        r = deflate(&d, in + give == n ? Z_FINISH : Z_NO_FLUSH);
        in += give - d.avail_in;
        out = (size_t)(d.next_out - mid);
        if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR)
            ok = 0;
    }
    deflateEnd(&d);
    size_t made = out;
    ok = ok && inflateInit2(&f, bits) == Z_OK;
    in = out = 0;
    r = Z_OK;
    while (ok && r != Z_STREAM_END) {
        f.next_in = mid + in;
        f.avail_in = (uInt)(made - in < 50 ? made - in : 50);
        f.next_out = back + out;
        f.avail_out = (uInt)(n + 1 - out < 300 ? n + 1 - out : 300);
        size_t had = f.avail_in;
        r = inflate(&f, Z_NO_FLUSH);
        in += had - f.avail_in;
        out = (size_t)(f.next_out - back);
        if (r != Z_OK && r != Z_STREAM_END)
            ok = 0;
    }
    inflateEnd(&f);
    ok = ok && out == n && memcmp(src, back, n) == 0;
    free(mid);
    free(back);
    return ok;
}

static int selftest(void)
{
    check("version 1.4", strcmp(zlibVersion(), "1.4") == 0 && strcmp(ZLIB_VERSION, zlibVersion()) == 0);

    const unsigned char *nine = (const unsigned char *)"123456789";
    uLong c = crc32(0, nine, 9), a = adler32(1, nine, 9);
    check("crc32 cbf43926", c == 0xcbf43926);
    check("adler32 091e01de", a == 0x091e01de);
    uLong c1 = crc32(0, nine, 4), c2 = crc32(0, nine + 4, 5);
    uLong a1 = adler32(1, nine, 4), a2 = adler32(1, nine + 4, 5);
    check("crc32_combine", crc32_combine(c1, c2, 5) == c);
    check("adler32_combine", adler32_combine(a1, a2, 5) == a);

    size_t n = 200000;
    unsigned char *src = sample(n), *back = malloc(n);
    int levels = 1;
    uLong sizes[10];
    for (int level = 0; level <= 9; level++) {
        uLongf clen = compressBound(n);
        unsigned char *z = malloc(clen);
        uLongf blen = n;
        if (compress2(z, &clen, src, n, level) != Z_OK || uncompress(back, &blen, z, clen) != Z_OK ||
            blen != n || memcmp(src, back, n) != 0)
            levels = 0;
        sizes[level] = clen;
        free(z);
    }
    check("compress2/uncompress at levels 0-9", levels);
    check("level 9 smaller than level 1 smaller than level 0", sizes[9] < sizes[1] && sizes[1] < sizes[0]);

    static const struct { const char *name; int bits; } kinds[] = {
        { "zlib", 15 }, { "gzip", 31 }, { "raw", -15 }, { "zlib, a 512-byte window", 9 },
    };
    static const struct { const char *name; int strategy; } strategies[] = {
        { "default", Z_DEFAULT_STRATEGY }, { "filtered", Z_FILTERED }, { "huffman only", Z_HUFFMAN_ONLY },
        { "rle", Z_RLE }, { "fixed", Z_FIXED },
    };
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++)
        for (size_t s = 0; s < sizeof(strategies) / sizeof(strategies[0]); s++) {
            char what[96];
            snprintf(what, sizeof(what), "streams in pieces: %s, %s", kinds[k].name, strategies[s].name);
            check(what, round_trip(src, n, 6, kinds[k].bits, strategies[s].strategy));
        }

    /* A preset dictionary: inflate asks for it (Z_NEED_DICT, its Adler-32). */
    const char *dict = "the quick brown fox jumps over the lazy dog ";
    const char *text = "the lazy dog and the quick brown fox";
    unsigned char z[256], out[256];
    z_stream d = { 0 }, f = { 0 };
    int ok = deflateInit(&d, 9) == Z_OK &&
             deflateSetDictionary(&d, (const Bytef *)dict, (uInt)strlen(dict)) == Z_OK;
    uLong dict_id = d.adler;            /* the dictionary's Adler-32 (deflate moves it on) */
    d.next_in = (Bytef *)text;
    d.avail_in = (uInt)strlen(text);
    d.next_out = z;
    d.avail_out = sizeof(z);
    ok = ok && deflate(&d, Z_FINISH) == Z_STREAM_END;
    size_t zlen = sizeof(z) - d.avail_out;
    deflateEnd(&d);
    ok = ok && inflateInit(&f) == Z_OK;
    f.next_in = z;
    f.avail_in = (uInt)zlen;
    f.next_out = out;
    f.avail_out = sizeof(out);
    ok = ok && inflate(&f, Z_FINISH) == Z_NEED_DICT && f.adler == dict_id &&
         inflateSetDictionary(&f, (const Bytef *)dict, (uInt)strlen(dict)) == Z_OK &&
         dict_id == adler32(1, (const Bytef *)dict, (uInt)strlen(dict)) &&
         inflate(&f, Z_FINISH) == Z_STREAM_END && sizeof(out) - f.avail_out == strlen(text) &&
         memcmp(out, text, strlen(text)) == 0;
    inflateEnd(&f);
    check("a preset dictionary", ok);

    /* A corrupted stream is refused (a flipped byte: the check or the data). */
    uLongf clen = compressBound(n);
    unsigned char *cz = malloc(clen);
    compress2(cz, &clen, src, n, 6);
    cz[clen / 2] ^= 0x55;
    uLongf blen = n;
    int r = uncompress(back, &blen, cz, clen);
    check("a corrupted stream refused", r == Z_DATA_ERROR || r == Z_BUF_ERROR);
    free(cz);

    /* A gzip file. */
    const char *path = "/tmp/zlibtest.gz";
    gzFile g = gzopen(path, "wb9");
    ok = g != 0 && gzwrite(g, src, (unsigned)n) == (int)n && gzprintf(g, "line %d of %s\n", 2, "two") > 0 &&
         gzputs(g, "last line\n") > 0 && gzclose(g) == Z_OK;
    check("gzwrite, gzprintf, gzputs", ok);
    g = gzopen(path, "rb");
    int got = g ? gzread(g, back, (unsigned)n) : -1;
    char line[64] = "", last[64] = "";
    ok = got == (int)n && memcmp(back, src, n) == 0 && gzgets(g, line, sizeof(line)) &&
         strcmp(line, "line 2 of two\n") == 0 && gzgets(g, last, sizeof(last)) &&
         strcmp(last, "last line\n") == 0 && gzeof(g) == 0 && gzgetc(g) == -1 && gzeof(g) == 1;
    check("gzread, gzgets, gzeof", ok);
    ok = gzseek(g, 1000, SEEK_SET) == 1000 && gzgetc(g) == src[1000] && gztell(g) == 1001;
    check("gzseek, gztell", ok);
    gzclose(g);
    remove(path);

    free(src);
    free(back);
    printf(T "%s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}

static unsigned char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t cap = 1 << 16, len = 0;
    unsigned char *p = malloc(cap);
    size_t got;
    while ((got = fread(p + len, 1, cap - len, f)) > 0) {
        len += got;
        if (len == cap)
            p = realloc(p, cap *= 2);
    }
    fclose(f);
    *n = len;
    return p;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return selftest();
    if (argc == 3 && strcmp(argv[1], "gunzip") == 0) {
        gzFile g = gzopen(argv[2], "rb");
        if (!g) {
            printf(T "%s: cannot open\n", argv[2]);
            return 1;
        }
        unsigned char buf[8192];
        uLong crc = crc32(0, 0, 0);
        unsigned long total = 0;
        int got;
        while ((got = gzread(g, buf, sizeof(buf))) > 0) {
            crc = crc32(crc, buf, (uInt)got);
            total += (unsigned long)got;
        }
        int err;
        gzerror(g, &err);
        gzclose(g);
        printf(T "gunzip %s: %lu bytes, crc32 %08lx%s\n", argv[2], total, crc,
               got < 0 || err != Z_OK ? ", an error" : "");
        return got < 0;
    }
    if (argc == 3 && strcmp(argv[1], "inflate") == 0) {
        size_t n;
        unsigned char *z = read_file(argv[2], &n);
        if (!z) {
            printf(T "%s: cannot open\n", argv[2]);
            return 1;
        }
        z_stream f = { 0 };
        unsigned char buf[8192];
        uLong crc = crc32(0, 0, 0);
        unsigned long total = 0;
        int r = inflateInit(&f);
        f.next_in = z;
        f.avail_in = (uInt)n;
        while (r == Z_OK) {
            f.next_out = buf;
            f.avail_out = sizeof(buf);
            r = inflate(&f, Z_NO_FLUSH);
            crc = crc32(crc, buf, (uInt)(sizeof(buf) - f.avail_out));
            total += sizeof(buf) - f.avail_out;
        }
        inflateEnd(&f);
        printf(T "inflate %s: %lu bytes, crc32 %08lx%s\n", argv[2], total, crc,
               r == Z_STREAM_END ? "" : ", an error");
        return r != Z_STREAM_END;
    }
    fprintf(stderr, "usage: zlibtest [gunzip FILE | inflate FILE]\n");
    return 2;
}
