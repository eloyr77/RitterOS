/*
 * The dlopen test's library (22k): libplug.so.1, in /system/<arch>/test (not
 * among the system's libraries), loaded by dltest while it runs. A function,
 * a constructor (ready is set when it is loaded), per-thread data with an
 * initial value (each thread its own count), and a call into libritteros (a
 * library it needs, already loaded).
 */
#include <stdio.h>
#include <stdlib.h>

static int ready, env_ok;
static __thread int count = 5;

/* (71d) argc, argv, envp as glibc gives them; getenv works already. */
__attribute__((constructor)) static void loaded(int argc, char **argv, char **envp)
{
    ready = 1;
    env_ok = getenv("HOME") != 0 && argc >= 1 && argv && argv[0] && envp && envp[0];
}

int plug_env(void)
{
    return env_ok;
}

int plug_ready(void)
{
    return ready;
}

int plug_add(int a, int b)
{
    return a + b;
}

int plug_count(void)                    /* 6, 7, ... in each thread */
{
    return ++count;
}

const char *plug_name(char *buf, int size)
{
    snprintf(buf, (size_t)size, "plug %d", 22);
    return buf;
}
