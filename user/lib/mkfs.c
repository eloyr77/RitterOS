/*
 * What the file system writers share (mkfs.h): writing at a place on the
 * device, zeros, little-endian numbers.
 */
#include <string.h>
#include <unistd.h>
#include "mkfs.h"

static const uint8_t zeros[16 * 4096];

bool mkfs_write(int fd, uint64_t offset, const void *data, size_t n)
{
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0)
        return false;
    for (size_t done = 0; done < n;) {
        ssize_t r = write(fd, (const uint8_t *)data + done, n - done);
        if (r <= 0)
            return false;
        done += (size_t)r;
    }
    return true;
}

bool mkfs_zero(int fd, uint64_t offset, uint64_t n)
{
    while (n) {
        uint64_t take = n < sizeof(zeros) ? n : sizeof(zeros);
        if (!mkfs_write(fd, offset, zeros, take))
            return false;
        offset += take, n -= take;
    }
    return true;
}

void mkfs_put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
}

void mkfs_put32(uint8_t *p, uint32_t v)
{
    mkfs_put16(p, v), mkfs_put16(p + 2, v >> 16);
}

void mkfs_put64(uint8_t *p, uint64_t v)
{
    mkfs_put32(p, (uint32_t)v), mkfs_put32(p + 4, (uint32_t)(v >> 32));
}
