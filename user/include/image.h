/*
 * libimage (user/lib/image.c): decode and encode still images, to and from a
 * `struct image` of RGBA8888 pixels in memory. Formats are written from their
 * specifications (PPM, BMP, PNG and JPEG so far). All colour conversion,
 * scaling and blitting is here too, so libvideo and viewers share it; those
 * are where SIMD or a GPU would plug in later, behind these same signatures.
 */
#ifndef RITTEROS_IMAGE_H
#define RITTEROS_IMAGE_H

#include "media.h"

/* Decode a file (or a buffer in memory) into *out (RGBA8888; pixels malloc'd).
 * 0, or -1 with errno set (ENOMEM, or EINVAL for a format we do not read). */
int image_load(const char *path, struct image *out);
int image_load_memory(const void *data, long n, struct image *out);

/* Fill *out's w/h/format/stride without decoding the pixels (pixels = 0). */
int image_info(const char *path, struct image *out);

/* Encode *img to a file, by the path's extension: .png (41h-2; RGBA, or RGB
 * when every pixel is opaque), .bmp, else PPM. */
int image_save(const char *path, const struct image *img);

/* Release img->pixels (and zero the struct). Safe on a zero struct. */
void image_free(struct image *img);

/* A scaled copy (nearest-neighbour) of w x h into *out. 0 or -1/errno. */
int image_scale(const struct image *src, int w, int h, struct image *out);

#endif
