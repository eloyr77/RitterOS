/*
 * Test: libimage decode. Decodes each image file named and prints its size
 * and the SHA-256 of its RGBA8888 pixels, which the host computes
 * independently and compares (and the host checks a PPM and a BMP of the same
 * picture decode to the same pixels, and every PNG variant to its pattern).
 * With -c, files come in pairs, FILE REFERENCE: FILE is decoded and compared
 * with the reference picture (lossy formats: JPEG against the exact
 * pixels of its coefficients), printing the largest difference of any
 * channel and the mean in thousandths. With -s, pairs FILE COPY: FILE is
 * decoded, saved as COPY (image_save: .bmp or .ppm) and COPY shown (63e).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "image.h"
#include "ritteros.h"

static int show(const char *path)
{
    struct image img;
    if (image_load(path, &img) < 0) {
        printf("imagetest: %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, img.pixels, (size_t)img.w * img.h * 4);
    uint8_t hash[SHA256_SIZE];
    sha256_final(&s, hash);
    char hex[2 * SHA256_SIZE + 1];
    for (int i = 0; i < SHA256_SIZE; i++)
        snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    printf("imagetest: %s %dx%d sha256 %s\n", path, img.w, img.h, hex);
    image_free(&img);
    return 0;
}

static int compare(const char *path, const char *reference)
{
    struct image a, b;
    if (image_load(path, &a) < 0) {
        printf("imagetest: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (image_load(reference, &b) < 0) {
        printf("imagetest: %s: %s\n", reference, strerror(errno));
        image_free(&a);
        return 1;
    }
    int failed = 0;
    if (a.w != b.w || a.h != b.h) {
        printf("imagetest: %s %dx%d, but %s is %dx%d\n", path, a.w, a.h, reference, b.w, b.h);
        failed = 1;
    } else {
        long n = (long)a.w * a.h * 4, sum = 0;
        int most = 0;
        for (long i = 0; i < n; i++) {
            int d = a.pixels[i] - b.pixels[i];
            d = d < 0 ? -d : d;
            sum += d;
            if (d > most)
                most = d;
        }
        printf("imagetest: %s %dx%d, from %s at most %d apart, mean %ld/1000\n", path, a.w, a.h,
               reference, most, sum * 1000 / n);
    }
    image_free(&a);
    image_free(&b);
    return failed;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    int failed = 0;
    if (!strcmp(argv[1], "-s")) {
        for (int i = 2; i + 1 < argc; i += 2) {
            struct image img;
            if (image_load(argv[i], &img) < 0 || image_save(argv[i + 1], &img) < 0) {
                printf("imagetest: %s -> %s: %s\n", argv[i], argv[i + 1], strerror(errno));
                failed = 1;
                continue;
            }
            image_free(&img);
            failed |= show(argv[i + 1]);
        }
        return failed;
    }
    if (!strcmp(argv[1], "-c")) {
        for (int i = 2; i + 1 < argc; i += 2)
            failed |= compare(argv[i], argv[i + 1]);
        return failed;
    }
    for (int i = 1; i < argc; i++)
        failed |= show(argv[i]);
    return failed;
}
