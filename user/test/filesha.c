/*
 * Test: filesha PATH prints the file's size and SHA-256 (scripts/test.sh
 * computes the same on the host): big files are compared without printing
 * them.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

static unsigned char chunk[16 * 1024];

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("filesha: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    struct sha256 s;
    sha256_init(&s);
    long total = 0;
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        sha256_update(&s, chunk, (size_t)n);
        total += n;
    }
    close(fd);
    if (n < 0) {
        printf("filesha: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    uint8_t hash[SHA256_SIZE];
    sha256_final(&s, hash);
    char hex[2 * SHA256_SIZE + 1];
    for (int i = 0; i < SHA256_SIZE; i++)
        snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    printf("filesha: %s: %ld bytes, sha256 %s\n", argv[1], total, hex);
    return 0;
}
