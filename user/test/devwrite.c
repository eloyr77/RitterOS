/*
 * Test: raw writes to /dev (scripts/test.sh checks the disk image after).
 *   devwrite busy DISK PART       PART is mounted: opening it for writing,
 *                                 and writing its first block through DISK,
 *                                 are refused
 *   devwrite mount DISK PART      with DISK open for writing, mounting PART
 *                                 is refused
 *   devwrite text DEVICE OFFSET TEXT
 *                                 write TEXT at byte OFFSET (closing it has
 *                                 the kernel read the table again)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

static long number(const char *s)
{
    long n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

static int dev_open(const char *name, int flags)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", name);
    return open(path, flags);
}

static const char *result(long r)
{
    return r < 0 ? strerror(errno) : "done";
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "busy") == 0) {
        int fd = dev_open(argv[3], O_WRONLY);
        printf("devwrite: open %s for writing: %d, %s\n", argv[3], fd, result(fd));
        struct sys_device d;
        int i = 0;
        while (ritteros_device(i, &d) == 1 && strcmp(d.name, argv[3]) != 0)
            i++;
        fd = dev_open(argv[2], O_RDWR);
        static char block[512];
        lseek(fd, (off_t)(d.first_block * d.block_size), SEEK_SET);
        long n = write(fd, block, sizeof(block));
        printf("devwrite: write %s's first block through %s: %ld, %s\n", argv[3], argv[2], n,
               result(n));
        close(fd);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "mount") == 0) {
        int fd = dev_open(argv[2], O_WRONLY);
        char path[128];
        int r = ritteros_mount(argv[3], 0, path, sizeof(path));
        printf("devwrite: mount %s while %s is open for writing: %d, %s\n", argv[3], argv[2], r,
               result(r));
        close(fd);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "text") == 0) {
        int fd = dev_open(argv[2], O_WRONLY);
        if (fd < 0) {
            printf("devwrite: %s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        lseek(fd, number(argv[3]), SEEK_SET);
        long n = write(fd, argv[4], strlen(argv[4]));
        int c = close(fd);
        printf("devwrite: %ld bytes at %s on %s: %s, closed: %s\n", n, argv[3], argv[2],
               result(n), result(c));
        return 0;
    }
    return 2;
}
