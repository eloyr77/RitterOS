/* Test: writing files: a large one (past the 8 MiB intermediate commit),
 * O_RDWR with a seek, a hole, O_APPEND, O_TRUNC, the refusals; the host
 * checks the result (scripts/test.sh). */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BIG (10 * 1024 * 1024)
#define CHUNK (64 * 1024)

static uint64_t state;
static char chunk[CHUNK], back[CHUNK];

/* The next bytes of the pattern (xorshift64 from "RITTERFS", like the
 * kernel's write test and the host's check). */
static void pattern(char *buf, size_t n)
{
    for (size_t i = 0; i < n; i += 8) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        memcpy(buf + i, &state, n - i < 8 ? n - i : 8);
    }
}

static void report(const char *what, long result)
{
    if (result < 0)
        printf("fwrite: %s: -1, %s\n", what, strerror(errno));
    else
        printf("fwrite: %s: %ld\n", what, result);
}

int main(void)
{
    mkdir("/fw", 0755);
    /* 10 MiB in 64 KiB writes, then read back. */
    int fd = open("/fw/big.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    long total = 0;
    state = 0x5249545445524653;
    for (int i = 0; fd >= 0 && i < BIG / CHUNK; i++) {
        pattern(chunk, CHUNK);
        if (write(fd, chunk, CHUNK) != CHUNK)
            break;
        total += CHUNK;
    }
    report("close the big file", close(fd));
    fd = open("/fw/big.bin", O_RDONLY);
    state = 0x5249545445524653;
    long same = 0;
    for (int i = 0; fd >= 0 && i < BIG / CHUNK; i++) {
        pattern(chunk, CHUNK);
        if (read(fd, back, CHUNK) != CHUNK || memcmp(chunk, back, CHUNK) != 0)
            break;
        same += CHUNK;
    }
    close(fd);
    printf("fwrite: %ld bytes written, %ld read back the same\n", total, same);

    /* O_RDWR: overwrite in the middle, read around it. */
    fd = open("/fw/small.txt", O_RDWR | O_CREAT, 0644);
    write(fd, "0123456789", 10);
    lseek(fd, 3, SEEK_SET);
    write(fd, "abc", 3);
    lseek(fd, 0, SEEK_SET);
    char text[32] = {0};
    long n = read(fd, text, sizeof(text) - 1);
    printf("fwrite: read-write: '%.*s'\n", (int)n, text);
    close(fd);

    /* Past the end: a hole of zeros (not stored). */
    fd = open("/fw/hole.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    lseek(fd, 1024 * 1024, SEEK_SET);
    write(fd, "end", 3);
    close(fd);
    struct stat st;
    stat("/fw/hole.bin", &st);
    fd = open("/fw/hole.bin", O_RDONLY);
    n = read(fd, text, 8);
    close(fd);
    printf("fwrite: hole: %ld bytes, the first %ld %s\n", (long)st.st_size, n,
           n == 8 && !memcmp(text, "\0\0\0\0\0\0\0\0", 8) ? "zero" : "WRONG");

    /* O_APPEND and O_TRUNC. */
    fd = open("/fw/log.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    write(fd, "one\n", 4);
    close(fd);
    fd = open("/fw/log.txt", O_WRONLY | O_APPEND);
    write(fd, "two\n", 4);
    close(fd);
    stat("/fw/log.txt", &st);
    report("appended twice, size", (long)st.st_size);
    fd = open("/fw/log.txt", O_WRONLY | O_TRUNC);
    close(fd);
    stat("/fw/log.txt", &st);
    report("truncated, size", (long)st.st_size);

    /* What is refused. */
    fd = open("/fw/small.txt", O_RDONLY);
    report("write to a read-only descriptor", write(fd, "x", 1));
    close(fd);
    fd = open("/fw/small.txt", O_WRONLY);
    report("read from a write-only descriptor", read(fd, text, 1));
    close(fd);
    report("open a missing file without O_CREAT", open("/fw/none", O_WRONLY));
    report("open a directory for writing", open("/fw", O_WRONLY));
    report("create in a missing directory", open("/nodir/x", O_WRONLY | O_CREAT, 0644));
    report("unknown flags", open("/fw/small.txt", O_WRONLY | 0x100));
    fd = open("/fw/gone.txt", O_WRONLY | O_CREAT, 0644);
    unlink("/fw/gone.txt");
    report("write after another removed it", write(fd, "x", 1));
    close(fd);
    return 0;
}
