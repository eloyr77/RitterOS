/*
 * (43b-3) Memory files and madvise: memfd_create (SYS_MEMFD), shm_open and
 * shm_unlink (files in the kernel's /dev/shm), madvise (SYS_MADVISE); and
 * named semaphores (sem_open: a memory file in /dev/shm named "sem.NAME",
 * as glibc's, holding a sem_t that every program mapping it shares: the
 * kernel keys a futex there by the memory, not the address).
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

static int64_t check(int64_t r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

int memfd_create(const char *name, unsigned int flags)
{
    if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING)) {
        errno = EINVAL;
        return -1;
    }
    int fd = (int)check(syscall6(SYS_MEMFD, (int64_t)name, flags & MFD_CLOEXEC ? OPEN_CLOEXEC : 0,
                                 0, 0, 0, 0));
    if (fd >= 0 && !(flags & MFD_ALLOW_SEALING))
        fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL);        /* (Linux's: no seals without asking) */
    return fd;
}

/* /dev/shm/NAME for "/NAME" or "NAME" (into out, of size room): 0, or -1
 * with errno (EINVAL: empty or another slash; ENAMETOOLONG). */
static int shm_path(const char *prefix, const char *name, char *out, size_t room)
{
    if (!name) {
        errno = EINVAL;
        return -1;
    }
    if (*name == '/')
        name++;
    if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        errno = EINVAL;
        return -1;
    }
    size_t a = strlen(prefix), b = strlen(name);
    /* (the kernel's name is what follows /dev/shm/: at most 255 bytes) */
    if (a + b + 1 > room || a - (sizeof("/dev/shm/") - 1) + b > 255) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, prefix, a);
    memcpy(out + a, name, b + 1);
    return 0;
}

int shm_open(const char *name, int flags, mode_t mode)
{
    char path[300];
    if (shm_path("/dev/shm/", name, path, sizeof(path)) < 0)
        return -1;
    return open(path, flags | O_CLOEXEC, mode);     /* (POSIX: FD_CLOEXEC set) */
}

int shm_unlink(const char *name)
{
    char path[300];
    if (shm_path("/dev/shm/", name, path, sizeof(path)) < 0)
        return -1;
    return unlink(path);
}

int madvise(void *addr, size_t length, int advice)
{
    if (advice == MADV_NORMAL || advice == MADV_RANDOM || advice == MADV_SEQUENTIAL)
        return (uintptr_t)addr % 4096 ? (errno = EINVAL, -1) : 0;
    return (int)check(syscall6(SYS_MADVISE, (int64_t)addr, (int64_t)length, advice, 0, 0, 0));
}

int posix_madvise(void *addr, size_t length, int advice)
{
    if (advice < POSIX_MADV_NORMAL || advice > POSIX_MADV_DONTNEED || (uintptr_t)addr % 4096)
        return EINVAL;
    (void)length;
    return 0;                                       /* (advice only: nothing is thrown away) */
}

/* --- named semaphores --- */

sem_t *sem_open(const char *name, int flags, ...)
{
    char path[300];
    if (shm_path("/dev/shm/sem.", name, path, sizeof(path)) < 0)
        return SEM_FAILED;
    mode_t mode = 0;
    unsigned value = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned);
        value = va_arg(ap, unsigned);
        va_end(ap);
        if (value > SEM_VALUE_MAX) {
            errno = EINVAL;
            return SEM_FAILED;
        }
    }
    /* Made by O_EXCL, then filled: one that opens with size 0 is still
     * being made, and is looked at again until its maker wrote it. */
    int fd = -1;
    bool made = false;
    for (int tries = 0; fd < 0 && tries < 100; tries++) {
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0 || errno != ENOENT || !(flags & O_CREAT))
            break;
        fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, mode);
        made = fd >= 0;
        if (fd < 0 && errno != EEXIST)
            break;
    }
    if (fd < 0)
        return SEM_FAILED;
    if (!made && (flags & O_CREAT) && (flags & O_EXCL)) {
        close(fd);
        errno = EEXIST;
        return SEM_FAILED;
    }
    sem_t *s = MAP_FAILED;
    if (made) {
        sem_t init;
        sem_init(&init, 1, value);
        if (write(fd, &init, sizeof(init)) == (ssize_t)sizeof(init))
            s = mmap(0, sizeof(sem_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    } else {
        struct stat st;
        for (int tries = 0; tries < 1000 && fstat(fd, &st) == 0 && st.st_size < (off_t)sizeof(sem_t);
             tries++)
            usleep(1000);                           /* (its maker is still writing it) */
        if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(sem_t))
            s = mmap(0, sizeof(sem_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        else
            errno = EINVAL;
    }
    int saved = errno;
    close(fd);
    errno = saved;
    return s == MAP_FAILED ? SEM_FAILED : s;
}

int sem_close(sem_t *s)
{
    return munmap(s, sizeof(sem_t));
}

int sem_unlink(const char *name)
{
    char path[300];
    if (shm_path("/dev/shm/sem.", name, path, sizeof(path)) < 0)
        return -1;
    return unlink(path);
}
