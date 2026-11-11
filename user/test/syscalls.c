/* Test: the kernel checks what programs ask for (raw results: -ERR_*). */
#include <stdio.h>
#include <unistd.h>
#include "ritteros.h"

static int64_t raw_write(int fd, const void *buffer, size_t size)
{
    return syscall6(SYS_WRITE, fd, (int64_t)buffer, (int64_t)size, 0, 0, 0);
}

int main(void)
{
    static const char text[] = "syscalls: a message to standard error\n";
    printf("syscalls: write to stdout returned %ld\n", (long)raw_write(STDOUT_FILENO, "ok\n", 3));
    raw_write(STDERR_FILENO, text, sizeof(text) - 1);
    printf("syscalls: write to fd 7 returned %ld\n", (long)raw_write(7, "x", 1));
    printf("syscalls: write from a kernel address returned %ld\n",
           (long)raw_write(STDOUT_FILENO, (const void *)0xFFFF800000000000ULL, 8));
    printf("syscalls: write from an unmapped address returned %ld\n",
           (long)raw_write(STDOUT_FILENO, (const void *)0x10000000ULL, 8));
    printf("syscalls: system call 1000 returned %ld\n", (long)syscall6(1000, 0, 0, 0, 0, 0, 0));
    return 3;
}
