/*
 * sys/random.h (43a-3): getrandom: size random bytes from the kernel's
 * generator (it never waits: the flags change nothing); the bytes given,
 * or -1.
 */
#ifndef RITTEROS_SYS_RANDOM_H
#define RITTEROS_SYS_RANDOM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#define GRND_NONBLOCK   1
#define GRND_RANDOM     2
#define GRND_INSECURE   4
ssize_t getrandom(void *buf, size_t size, unsigned flags);
/* (43c) Also here, as glibc has it (unistd.h's: up to 256 random bytes). */
int getentropy(void *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
