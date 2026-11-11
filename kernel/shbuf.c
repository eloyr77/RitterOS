/*
 * Shared buffers (see shbuf.h): an array of physical pages, reference
 * counted. The mapping into a program's address space and the fd that holds
 * a reference live in process.c; this file only owns the pages.
 */
#include "shbuf.h"
#include "heap.h"
#include "memory.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "syscalls.h"

struct shbuf {
    int refs;
    uint64_t size;
    unsigned pages;
    uint64_t *page;                 /* physical address of each page */
};

struct shbuf *shbuf_create(uint64_t size)
{
    if (size == 0 || size > SHARE_MAX)
        return 0;
    unsigned pages = (unsigned)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    struct shbuf *b = kzalloc(sizeof(*b));
    if (!b)
        return 0;
    b->page = kzalloc(pages * sizeof(uint64_t));
    if (!b->page) {
        kfree(b);
        return 0;
    }
    for (unsigned i = 0; i < pages; i++) {
        uint64_t p = pmm_alloc_page();
        if (!p) {
            for (unsigned j = 0; j < i; j++)
                pmm_free_page(b->page[j]);
            kfree(b->page);
            kfree(b);
            return 0;
        }
        memset(phys_to_virt(p), 0, PAGE_SIZE);
        b->page[i] = p;
    }
    b->refs = 1;
    b->size = size;
    b->pages = pages;
    return b;
}

void shbuf_hold(struct shbuf *b)
{
    __atomic_add_fetch(&b->refs, 1, __ATOMIC_SEQ_CST);
}

void shbuf_unref(struct shbuf *b)
{
    if (__atomic_sub_fetch(&b->refs, 1, __ATOMIC_SEQ_CST) > 0)
        return;
    for (unsigned i = 0; i < b->pages; i++)
        pmm_free_page(b->page[i]);
    kfree(b->page);
    kfree(b);
}

uint64_t shbuf_size(struct shbuf *b)
{
    return b->size;
}

unsigned shbuf_pages(struct shbuf *b)
{
    return b->pages;
}

uint64_t shbuf_page(struct shbuf *b, unsigned i)
{
    return i < b->pages ? b->page[i] : 0;
}
