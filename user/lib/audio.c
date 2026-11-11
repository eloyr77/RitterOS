/*
 * libaudio (see audio.h). WAV (PCM) for now: 8- or 16-bit, mono or stereo,
 * decoded to signed 16-bit samples. Chunk walking is bounds-checked.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "audio.h"

static int fail(int e) { errno = e; return -1; }

static int read_all(const char *path, uint8_t **out, long *n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long cap = 65536, len = 0;
    uint8_t *buf = malloc((size_t)cap);
    if (!buf) { close(fd); return fail(ENOMEM); }
    for (;;) {
        if (len == cap) {
            uint8_t *nb = realloc(buf, (size_t)(cap * 2));
            if (!nb) { free(buf); close(fd); return fail(ENOMEM); }
            buf = nb, cap *= 2;
        }
        ssize_t r = read(fd, buf + len, (size_t)(cap - len));
        if (r < 0) { free(buf); close(fd); return -1; }
        if (r == 0) break;
        len += r;
    }
    close(fd);
    *out = buf, *n = len;
    return 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int decode_wav(const uint8_t *d, long n, struct audio_buffer *out, int header_only)
{
    if (n < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4))
        return fail(EINVAL);
    int channels = 0, rate = 0, bits = 0, fmt = 0;
    long at = 12, data_at = -1, data_len = 0;
    while (at + 8 <= n) {                               /* walk the chunks */
        const uint8_t *id = d + at;
        uint32_t sz = le32(d + at + 4);
        long body = at + 8;
        if (body + (long)sz > n)
            sz = (uint32_t)(n - body);
        if (!memcmp(id, "fmt ", 4) && sz >= 16) {
            fmt = le16(d + body);
            channels = le16(d + body + 2);
            rate = (int)le32(d + body + 4);
            bits = le16(d + body + 14);
        } else if (!memcmp(id, "data", 4)) {
            data_at = body, data_len = sz;
        }
        at = body + sz + (sz & 1);                      /* chunks are word-aligned */
    }
    if (fmt != 1 || (bits != 16 && bits != 8) || channels < 1 || channels > 2 || rate <= 0 ||
        data_at < 0)
        return fail(EINVAL);
    long frames = data_len / (channels * (bits / 8));
    out->rate = rate, out->channels = channels, out->format = SAMPLE_S16, out->frames = frames;
    if (header_only) { out->samples = 0; return 0; }
    int16_t *s = malloc((size_t)frames * channels * 2);
    if (!s)
        return fail(ENOMEM);
    const uint8_t *p = d + data_at;
    for (long i = 0; i < frames * channels; i++)
        s[i] = bits == 16 ? (int16_t)le16(p + i * 2) : (int16_t)((p[i] - 128) << 8);
    out->samples = s;
    return 0;
}

int audio_load_memory(const void *data, long n, struct audio_buffer *out)
{
    return decode_wav(data, n, out, 0);
}

int audio_load(const char *path, struct audio_buffer *out)
{
    uint8_t *data; long n;
    if (read_all(path, &data, &n) < 0) return -1;
    int r = decode_wav(data, n, out, 0);
    free(data);
    return r;
}

int audio_info(const char *path, struct audio_buffer *out)
{
    uint8_t *data; long n;
    if (read_all(path, &data, &n) < 0) return -1;
    int r = decode_wav(data, n, out, 1);
    free(data);
    return r;
}

void audio_free(struct audio_buffer *buf)
{
    if (buf && buf->samples) {
        free(buf->samples);
        memset(buf, 0, sizeof(*buf));
    }
}

static int write_all(int fd, const void *p, long n)
{
    const uint8_t *b = p;
    while (n > 0) {
        ssize_t w = write(fd, b, (size_t)n);
        if (w <= 0) return -1;
        b += w, n -= w;
    }
    return 0;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

int audio_save(const char *path, const struct audio_buffer *buf)
{
    if (!buf || !buf->samples || buf->format != SAMPLE_S16)
        return fail(EINVAL);
    long data_len = buf->frames * buf->channels * 2;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put32(h + 4, (uint32_t)(36 + data_len));
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);
    put16(h + 20, 1);                                  /* PCM */
    put16(h + 22, (uint16_t)buf->channels);
    put32(h + 24, (uint32_t)buf->rate);
    put32(h + 28, (uint32_t)(buf->rate * buf->channels * 2));    /* byte rate */
    put16(h + 32, (uint16_t)(buf->channels * 2));               /* block align */
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, (uint32_t)data_len);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int rc = write_all(fd, h, 44);
    if (rc == 0)
        rc = write_all(fd, buf->samples, data_len);
    if (close(fd) < 0) rc = -1;
    return rc;
}

int audio_resample(const struct audio_buffer *src, int rate, struct audio_buffer *out)
{
    if (!src || !src->samples || src->format != SAMPLE_S16 || rate <= 0)
        return fail(EINVAL);
    int ch = src->channels;
    long frames = src->frames * rate / src->rate;
    int16_t *s = malloc((size_t)frames * ch * 2);
    if (!s) return fail(ENOMEM);
    const int16_t *in = src->samples;
    for (long i = 0; i < frames; i++) {
        double pos = (double)i * src->rate / rate;      /* source index */
        long j = (long)pos;
        double f = pos - j;
        for (int c = 0; c < ch; c++) {
            long a = j * ch + c, b = (j + 1 < src->frames ? j + 1 : j) * ch + c;
            s[i * ch + c] = (int16_t)(in[a] * (1 - f) + in[b] * f);
        }
    }
    out->rate = rate, out->channels = ch, out->format = SAMPLE_S16, out->frames = frames;
    out->samples = s;
    return 0;
}

int audio_mix(struct audio_buffer *dst, const struct audio_buffer *src, float gain)
{
    if (!dst || !src || dst->format != SAMPLE_S16 || src->format != SAMPLE_S16 ||
        dst->rate != src->rate || dst->channels != src->channels)
        return fail(EINVAL);
    long n = (dst->frames < src->frames ? dst->frames : src->frames) * dst->channels;
    int16_t *d = dst->samples;
    const int16_t *s = src->samples;
    for (long i = 0; i < n; i++) {
        int v = (int)(d[i] + s[i] * gain);
        d[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    return 0;
}
