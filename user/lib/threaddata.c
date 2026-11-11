/*
 * Per-thread data (step 22i): `__thread` variables, a copy for each thread.
 * Every module -- the program and each library -- with per-thread data has a
 * block in every thread's area, at the same offset from the thread pointer
 * in all threads (the CPU register the kernel keeps per thread: x86 the FS
 * base, ARM TPIDR_EL0; SYS_THREAD_POINTER sets it). The layouts are each
 * architecture's standard ones, since the compiler and linker build offsets
 * into code: x86-64 puts the blocks below the thread pointer, which points
 * at a header whose first word points to itself; AArch64 puts them above it,
 * after a 16-byte header. The program's block is the first (its own
 * variables' offsets are fixed when it is linked).
 *
 * A dynamic program's modules come from ld.so, which assigns the offsets and
 * applies the relocations, then calls __ritteros_tls_setup with the list. A
 * static program's start code calls __ritteros_start, which finds its one
 * block in the program headers (the auxiliary vector). Each new thread gets
 * an area of its own (thread_create, freed by thread_join); a reserve after
 * the blocks is kept for libraries loaded later (dlopen, 22k).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../common/elf.h"
#include "ritteros.h"

#define MODULES     32
#define RESERVE     4096                /* for modules loaded later */
#define HEADER      64                  /* x86: the header the thread pointer points at */

static struct ritteros_tls_module modules[MODULES];
static int nmodules;

/* The live threads' pointers (22k: a module loaded later gets its initial
 * data into every one), under a small lock. */
static uintptr_t live[PROCESS_THREADS + 1];
static int live_lock;

static void lock(void)
{
    while (__atomic_exchange_n(&live_lock, 1, __ATOMIC_ACQUIRE))
        usleep(1000);
}

static void unlock(void)
{
    __atomic_store_n(&live_lock, 0, __ATOMIC_RELEASE);
}

static void live_add(uintptr_t tp)
{
    lock();
    for (int i = 0; i <= PROCESS_THREADS; i++)
        if (!live[i]) {
            live[i] = tp;
            break;
        }
    unlock();
}
static size_t extent;                   /* x86: bytes below the pointer; ARM: above, header included */
static size_t max_align = 16;
static int ready;

static uintptr_t thread_pointer(void)
{
    uintptr_t tp;
#if defined(__x86_64__)
    __asm__ volatile ("movq %%fs:0, %0" : "=r"(tp));
#else
    __asm__ volatile ("mrs %0, tpidr_el0" : "=r"(tp));
#endif
    return tp;
}

int ritteros_set_thread_pointer(void *tp)
{
    int64_t r = syscall6(SYS_THREAD_POINTER, (int64_t)tp, 0, 0, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

/* The size of an area and where its thread pointer goes in one at `area`. */
static size_t area_size(void)
{
#if defined(__x86_64__)
    return extent + RESERVE + HEADER + max_align;
#else
    return extent + RESERVE + max_align;
#endif
}

static uintptr_t area_pointer(uintptr_t area)
{
    uintptr_t a = max_align;
#if defined(__x86_64__)
    return (area + extent + RESERVE + a - 1) & ~(a - 1);
#else
    return (area + a - 1) & ~(a - 1);
#endif
}

/* Fill a new area (zeroed) with every module's initial data; its pointer. */
static uintptr_t area_fill(uintptr_t area)
{
    uintptr_t tp = area_pointer(area);
    for (int i = 0; i < nmodules; i++)
        if (modules[i].filesz)
            memcpy((void *)(tp + modules[i].tpoff), modules[i].image, modules[i].filesz);
#if defined(__x86_64__)
    *(uintptr_t *)tp = tp;              /* %fs:0: the header points to itself */
#endif
    return tp;
}

/* The main thread's area, from the system (not malloc: errno is not there
 * yet). 0 or -1. */
static int setup_main(void)
{
    size_t size = (area_size() + 4095) & ~(size_t)4095;
    int64_t area = syscall6(SYS_MMAP, 0, (int64_t)size, MMAP_READ | MMAP_WRITE,
                            MMAP_PRIVATE | MMAP_ANONYMOUS, -1, 0);
    if (area < 0)
        return -1;
    uintptr_t tp = area_fill((uintptr_t)area);
    if (syscall6(SYS_THREAD_POINTER, (int64_t)tp, 0, 0, 0, 0, 0) < 0)
        return -1;
    live[0] = tp;
    ready = 1;
    return 0;
}

int __ritteros_tls_add(const struct ritteros_tls_module *m)
{
#if defined(__x86_64__)
    int fits = (size_t)-m->tpoff <= extent + RESERVE;
#else
    int fits = (size_t)m->tpoff + m->memsz <= extent + RESERVE;
#endif
    if (!ready || nmodules == MODULES || !fits || m->align > max_align)
        return -1;
    lock();
    modules[nmodules++] = *m;           /* (new threads copy it from here) */
    for (int i = 0; i <= PROCESS_THREADS; i++)
        if (live[i]) {
            unsigned char *block = (unsigned char *)(live[i] + m->tpoff);
            memset(block, 0, m->memsz);
            memcpy(block, m->image, m->filesz);
        }
    unlock();
    return 0;
}

int __ritteros_tls_setup(const struct ritteros_tls_module *list, int count)
{
    if (count < 0 || count > MODULES)
        return -1;
    nmodules = count;
    extent = 0;
    for (int i = 0; i < count; i++) {
        modules[i] = list[i];
        if (modules[i].align > max_align)
            max_align = modules[i].align;
#if defined(__x86_64__)
        size_t below = (size_t)-modules[i].tpoff;
        if (below > extent)
            extent = below;
#else
        size_t above = (size_t)modules[i].tpoff + modules[i].memsz;
        if (above > extent)
            extent = above;
#endif
    }
#if !defined(__x86_64__)
    if (extent < 16)
        extent = 16;                    /* the header */
#endif
    return setup_main();
}

/* A static program's start: its per-thread data from its program headers
 * (a dynamic program's was set up by ld.so already). */
void __ritteros_environ_set(char **envp);           /* env.c (22t) */
void __ritteros_progname_set(const char *argv0);    /* env.c (41g-3) */

void __ritteros_start(long *sp)
{
    /* The environment, after the arguments (also for a program ld.so
     * started, which has its per-thread data already). */
    __ritteros_environ_set((char **)(sp + 1 + sp[0] + 1));
    __ritteros_progname_set(sp[0] > 0 ? (const char *)sp[1] : 0);
    if (ready)
        return;
    long argc = sp[0];
    long *p = sp + 1 + argc + 1;        /* past argv and its null */
    while (*p)
        p++;                            /* the environment */
    const Elf64_Phdr *ph = 0;
    long phnum = 0;
    for (p++; p[0] != AT_NULL; p += 2) {
        if (p[0] == AT_PHDR)
            ph = (const Elf64_Phdr *)p[1];
        else if (p[0] == AT_PHNUM)
            phnum = p[1];
    }
    struct ritteros_tls_module m = { 0, 0, 0, 1, 0 };
    for (long i = 0; ph && i < phnum; i++)
        if (ph[i].p_type == PT_TLS) {
            m.image = (const void *)ph[i].p_vaddr;
            m.filesz = ph[i].p_filesz;
            m.memsz = ph[i].p_memsz;
            m.align = ph[i].p_align ? ph[i].p_align : 1;
        }
    size_t a = m.align;
#if defined(__x86_64__)
    m.tpoff = -(long)((m.memsz + a - 1) & ~(a - 1));
#else
    m.tpoff = (long)((16 + a - 1) & ~(a - 1));
#endif
    if (__ritteros_tls_setup(&m, m.memsz ? 1 : 0) < 0)
        exit(127);                      /* (no memory for the main thread's data) */
}

/* x86's general-dynamic access (a library's variable): the module's block
 * in the calling thread's area, plus the offset. AArch64 uses descriptors
 * (ld.so's) instead, but has this too. */
struct tls_index {
    unsigned long module, offset;
};

void *__tls_get_addr(void *index)
{
    const struct tls_index *ti = index;
    if (ti->module == 0 || ti->module > (unsigned long)nmodules)
        return 0;
    return (void *)(thread_pointer() + modules[ti->module - 1].tpoff + ti->offset);
}

/* A new thread's area (thread_create): malloc'd, filled; its pointer in *tp. */
void *__ritteros_tls_area(uintptr_t *tp)
{
    if (!ready)
        return 0;                       /* (no per-thread data set up) */
    void *area = calloc(1, area_size());
    if (area) {
        lock();                         /* (a dlopen meanwhile fills it as well) */
        *tp = area_fill((uintptr_t)area);
        unlock();
        live_add(*tp);
    }
    return area;
}

/* A joined thread's area goes. */
void __ritteros_tls_release(void *area)
{
    if (!area)
        return;
    uintptr_t tp = area_pointer((uintptr_t)area);
    lock();
    for (int i = 1; i <= PROCESS_THREADS; i++)
        if (live[i] == tp)
            live[i] = 0;
    unlock();
    free(area);
}
