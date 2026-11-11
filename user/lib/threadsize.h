/* (54d) Threads with a stack of a given size (0: USER_STACK_SIZE, 8 MiB),
 * for pthread_attr_setstacksize; internal to the library. */
#ifndef RITTEROS_THREADSIZE_H
#define RITTEROS_THREADSIZE_H

#include <stddef.h>
#include <threads.h>

int __ritteros_thread_create_sized(int (*fn)(void *arg), void *arg, size_t stack_size);
int __ritteros_thrd_create_sized(thrd_t *thr, thrd_start_t func, void *arg, size_t stack_size);

#endif
