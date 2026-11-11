/*
 * Loading shared libraries while a program runs (step 22k), as on Unix:
 * dlopen maps a library (a name in /system/<arch>/lib, or a path) and the
 * libraries it needs, links them and runs their initializers; dlsym finds a
 * function or variable in it (per-thread variables: the calling thread's);
 * dlclose gives it back -- it stays loaded until the program ends, since
 * other code may already use it; dlerror says what the last failure in this
 * thread was. The dynamic loader ld.so does the work, so a static program has
 * none of it: dlopen fails there.
 */
#ifndef RITTEROS_DLFCN_H
#define RITTEROS_DLFCN_H

#define RTLD_LAZY       1               /* (all are resolved at once, either way) */
#define RTLD_NOW        2
#define RTLD_LOCAL      0               /* (every library's symbols are visible to all) */
#define RTLD_GLOBAL     0x100
#define RTLD_DEFAULT    ((void *)0)     /* dlsym: everything loaded, the program first */

/* The library's handle, or 0 (dlerror says why). dlopen(0, ...) is the
 * program itself. A library already loaded gives the same handle. */
void *dlopen(const char *file, int mode);

/* The address of `name` in the library (then in everything loaded), or 0. */
void *dlsym(void *handle, const char *name);

/* 0, or -1 for something dlopen did not give. */
int dlclose(void *handle);

/* The last failure's message in this thread, once; 0 if none since. */
char *dlerror(void);

#endif
