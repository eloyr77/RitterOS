/*
 * libimage's own decoders (image.c dispatches by the file's first bytes):
 * not part of the library's interface (image.h), so nothing here is
 * exported. inflate is the DEFLATE/zlib decompressor (inflate.c), the rest
 * decode one format each into a `struct image` (RGBA8888).
 */
#ifndef RITTEROS_IMAGEDEC_H
#define RITTEROS_IMAGEDEC_H

#include <stddef.h>
#include <stdint.h>
#include "media.h"

/* DEFLATE (RFC 1951): decompress in into out (cap bytes at most); the bytes
 * written, or -1 if the data is damaged or would not fit. used (if not 0):
 * the input bytes the stream took. */
long inflate_raw(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *used);

/* zlib (RFC 1950): its header, DEFLATE, the Adler-32 check; as inflate_raw. */
long inflate_zlib(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

/* The most pixels an image may have (bounds the memory a file can ask for):
 * 64 million, e.g. 8192 x 8192. */
#define IMAGE_MAX_PIXELS (64L * 1024 * 1024)

/* Allocate out's pixels for w x h RGBA8888 (image.c); 0 or -1/errno. */
int image_alloc(struct image *out, int w, int h);

/* PNG (png.c): 0, or -1 with errno EINVAL (damaged or unsupported). With
 * header_only, only the size is filled in. */
int decode_png(const uint8_t *d, long n, struct image *out, int header_only);

/* JPEG (jpeg.c): the same. */
int decode_jpeg(const uint8_t *d, long n, struct image *out, int header_only);

/* (41h-2) zlib-wrapped DEFLATE of in's n bytes (deflate.c) into *out
 * (malloc'd, the caller frees): its length, or -1 when out of memory. */
long deflate_zlib(const uint8_t *in, size_t n, uint8_t **out);

/* (41h-2) img as a PNG file in memory (png.c): RGBA, or RGB when every
 * pixel is opaque; *out malloc'd. Its length, or -1 (errno ENOMEM). */
long encode_png(const struct image *img, uint8_t **out);

#endif
