/*
 * Shared buffers: a region of physical pages that can be mapped into more
 * than one program's address space, so they share memory without copying
 * (the window system's pixels: a client draws, windowd composites). A
 * program makes one, maps it, and passes it to another over a local socket
 * (process.c and localsock.c); the pages are freed once no fd holds the
 * buffer any more.
 */
#ifndef RITTEROS_SHBUF_H
#define RITTEROS_SHBUF_H

#include <stdint.h>

struct shbuf;

/* A new buffer of `size` bytes (rounded up to whole pages, zeroed), with one
 * reference. 0 if the size is 0, too large, or out of memory. */
struct shbuf *shbuf_create(uint64_t size);

/* One more / one fewer holder; the pages are freed after the last. */
void shbuf_hold(struct shbuf *b);
void shbuf_unref(struct shbuf *b);

uint64_t shbuf_size(struct shbuf *b);       /* the size asked for */
unsigned shbuf_pages(struct shbuf *b);      /* whole pages it occupies */
uint64_t shbuf_page(struct shbuf *b, unsigned i);   /* physical address of page i */

#endif
