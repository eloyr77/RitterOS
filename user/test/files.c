/* Test: opening, reading and closing files, and the errors on the way. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void report(const char *what, long result)
{
    if (result < 0)
        printf("files: %s: -1, %s\n", what, strerror(errno));
    else
        printf("files: %s: %ld\n", what, result);
}

int main(void)
{
    char buf[64];
    /* Reading continues where the last read stopped. */
    int fd = open("/readme.txt", O_RDONLY);
    report("open /readme.txt", fd);
    ssize_t n = read(fd, buf, 10);
    printf("files: first 10 bytes '%.*s'\n", (int)n, buf);
    n = read(fd, buf, 9);
    printf("files: next 9 bytes '%.*s'\n", (int)n, buf);
    report("close", close(fd));
    report("read after close", read(fd, buf, 1));
    /* A whole file, to its end. */
    fd = open("/test/hello.txt", O_RDONLY);
    n = read(fd, buf, sizeof(buf));
    printf("files: /test/hello.txt: %ld bytes: %.*s", (long)n, (int)n, buf);   /* ends with \n */
    report("read at the end", read(fd, buf, sizeof(buf)));
    close(fd);
    /* Errors. */
    report("open /nope", open("/nope", O_RDONLY));
    fd = open("readme.txt", O_RDONLY);         /* relative to the current directory: / */
    report("open readme.txt (relative)", fd);
    close(fd);
    report("open in /.snapshots for writing", open("/.snapshots/x", O_WRONLY | O_CREAT, 0644));
    fd = open("/test", O_RDONLY);
    report("read a directory", read(fd, buf, 1));
    close(fd);
    report("open with a kernel address", open((const char *)0xFFFF800000000000ULL, O_RDONLY));
    int count = 0;
    while (open("/readme.txt", O_RDONLY) >= 0)
        count++;
    printf("files: opened /readme.txt %d more times, then: %s\n", count, strerror(errno));
    return 0;
}
