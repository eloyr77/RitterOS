/*
 * Test: devread NAME OFFSET LENGTH reads LENGTH raw bytes of /dev/NAME
 * from byte OFFSET and prints their SHA-256 (scripts/test.sh recomputes it
 * from the disk's image file); then what stat says, whether it may be
 * opened for writing (not where a file system is mounted), and that a user
 * who is not root may not open it.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

static unsigned char data[64 * 1024];

static long number(const char *s)
{
    long n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", argv[1]);
    long offset = number(argv[2]), length = number(argv[3]);
    if (length > (long)sizeof(data))
        return 2;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("devread: %s: %s\n", path, strerror(errno));
        return 1;
    }
    lseek(fd, offset, SEEK_SET);
    long got = 0;
    ssize_t n;
    while (got < length && (n = read(fd, data + got, (size_t)(length - got))) > 0)
        got += n;
    close(fd);
    uint8_t hash[SHA256_SIZE];
    sha256(data, (size_t)got, hash);
    char hex[2 * SHA256_SIZE + 1];
    for (int i = 0; i < SHA256_SIZE; i++)
        snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    printf("devread: %s at %ld, %ld bytes: sha256 %s\n", path, offset, got, hex);

    struct stat st;
    if (stat(path, &st) == 0)
        printf("devread: stat: %s, %ld bytes, mode %o, owner %u\n",
               S_ISBLK(st.st_mode) ? "block device" : "not a block device", (long)st.st_size,
               st.st_mode & 0777, st.st_uid);
    int w = open(path, O_WRONLY);
    printf("devread: open for writing: %d, %s\n", w, w < 0 ? strerror(errno) : "opened");
    if (setgid(1000) == 0 && setuid(1000) == 0) {
        int u = open(path, O_RDONLY);
        printf("devread: as user 1000: %d, %s\n", u, u < 0 ? strerror(errno) : "opened");
    }
    return 0;
}
