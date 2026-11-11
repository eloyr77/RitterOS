/* errno, one per thread (22i; a function since 22c: see errno.h). A file of
 * its own, so that ld.so -- which links this library's archive but runs on
 * the program's threads for dlopen (22k) -- can have a plain one instead and
 * never write into a thread's per-thread data. */
#include <errno.h>

static __thread int last_error;

int *__errno(void)
{
    return &last_error;
}
