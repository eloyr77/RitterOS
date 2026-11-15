/*
 * malloc.h (43a): the heap's calls (stdlib.h's) and glibc's extra ones
 * ported code includes this header for: memalign (aligned_alloc), valloc
 * and pvalloc (page-aligned; pvalloc's size rounded up to pages), and
 * malloc_usable_size (the bytes a block may hold: at least what was asked).
 */
#ifndef RITTEROS_MALLOC_H
#define RITTEROS_MALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>

void *memalign(size_t alignment, size_t size);
void *valloc(size_t size);
void *pvalloc(size_t size);
size_t malloc_usable_size(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
