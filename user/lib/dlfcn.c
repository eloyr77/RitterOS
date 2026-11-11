/* dlopen, dlsym, dlclose, dlerror (22k): ld.so does the loading, through the
 * entry points it handed over at the start (__ritteros_dl_setup); one call at
 * a time. A static program has no ld.so: dlopen fails there. */
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include "ritteros.h"

static const struct ritteros_dl_loader *loader;
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
