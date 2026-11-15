/* dlopen, dlsym, dlclose, dlerror (22k): ld.so does the loading, through the
 * entry points it handed over at the start (__ritteros_dl_setup); one call at
 * a time. A static program has no ld.so: dlopen fails there. */
#include <dlfcn.h>
#include <link.h>
#include <sys/auxv.h>
#include <stdio.h>
#include <unistd.h>
#include "ritteros.h"

static const struct ritteros_dl_loader *loader;
static unsigned long long loads;        /* (43a-4) dlopen's: dl_iterate_phdr's dlpi_adds */
static int busy;
static __thread char message[256];      /* dlerror's, per thread */
static __thread int has_message;

int __ritteros_dl_setup(const struct ritteros_dl_loader *l)
{
    loader = l;
    return 0;
}

static void lock(void)
{
    while (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
        usleep(1000);
}

static void unlock(void)
{
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

static int no_loader(void)
{
    if (loader)
        return 0;
    snprintf(message, sizeof(message), "a static program has no dynamic loader");
    has_message = 1;
    return 1;
}

void *dlopen(const char *file, int mode)
{
    if (no_loader())
        return 0;
    char error[256];
    lock();
    void *handle = loader->open(file, mode, error, sizeof(error));
    if (handle)
        __atomic_fetch_add(&loads, 1, __ATOMIC_RELAXED);
    unlock();
    if (!handle) {
        snprintf(message, sizeof(message), "%s", error);
        has_message = 1;
    }
    return handle;
}

void *dlsym(void *handle, const char *name)
{
    if (no_loader())
        return 0;
    char error[256];
    lock();
    void *address = loader->symbol(handle, name, error, sizeof(error));
    unlock();
    if (!address) {
        snprintf(message, sizeof(message), "%s", error);
        has_message = 1;
    }
    return address;
}

int dlclose(void *handle)
{
    if (no_loader())
        return -1;
    lock();
    int r = loader->close(handle);
    unlock();
    if (r < 0) {
        snprintf(message, sizeof(message), "%p is not a library dlopen gave", handle);
        has_message = 1;
    }
    return r;
}

char *dlerror(void)
{
    if (!has_message)
        return 0;
    has_message = 0;
    return message;
}

/* --- (43a-4) dl_iterate_phdr and dladdr: ld.so's answers (a static
 * program: itself alone, from the kernel's vector) --- */

static const struct ritteros_dl_query *query;

int __ritteros_dl_setup2(const struct ritteros_dl_query *q)
{
    query = q;
    return 0;
}

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size, void *data), void *data)
{
    struct dl_phdr_info info;
    if (!query) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)getauxval(AT_PHDR);
        unsigned long n = getauxval(AT_PHNUM);
        Elf64_Addr base = 0;
        for (unsigned long i = 0; ph && i < n; i++)
            if (ph[i].p_type == PT_PHDR)
                base = (Elf64_Addr)ph - ph[i].p_vaddr;
        info = (struct dl_phdr_info){ base, "", ph, (Elf64_Half)n, 1, 0, 0, 0 };
        return callback(&info, sizeof(info), data);
    }
    /* (no lock held across a callback: it may itself dlopen or unwind) */
    for (int i = 0;; i++) {
        struct ritteros_dl_object o;
        lock();
        int have = query->object(i, &o);
        unsigned long long now = __atomic_load_n(&loads, __ATOMIC_RELAXED);
        unlock();
        if (!have)
            return 0;
        info = (struct dl_phdr_info){ o.base, o.name, o.phdr, o.phnum, now + 1, 0,
                                      o.tls_module, o.tls_data };
        int r = callback(&info, sizeof(info), data);
        if (r)
            return r;
    }
}

int dladdr(const void *address, Dl_info *info)
{
    if (!query) {
        has_message = 0;
        return 0;
    }
    const char *file = 0, *symbol = 0;
    void *file_base = 0, *symbol_address = 0;
    lock();
    int found = query->address(address, &file, &file_base, &symbol, &symbol_address);
    unlock();
    if (!found)
        return 0;
    info->dli_fname = file;
    info->dli_fbase = file_base;
    info->dli_sname = symbol;
    info->dli_saddr = symbol_address;
    return 1;
}
