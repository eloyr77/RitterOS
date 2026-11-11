/*
 * Environment variables (step 22t): "NAME=value" strings a program gets
 * from the one that started it (the kernel puts them on the new program's
 * stack after its arguments; __ritteros_start finds them there) and passes
 * on to the programs it starts (ritteros_spawn, system). getenv reads them;
 * setenv, unsetenv and putenv change the program's own copy: the array
 * moves to the heap at the first change (the stack's strings stay where
 * they are). Not for threads changing them at once, as on Unix.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char **env;                      /* the current array, ended by a null pointer */
static bool on_heap;                    /* env is ours (malloc'd), not the stack's */
static char *no_variables[1];

char ***__ritteros_environ(void)
{
    if (!env)
        env = no_variables;
    return &env;
}

void __ritteros_environ_set(char **envp)
{
    env = envp ? envp : no_variables;
    on_heap = false;
}

/* (41g-3) The program's name, set at its start with the environment (the
 * arguments stay on the stack for good). */
static const char *progname = "";

void __ritteros_progname_set(const char *argv0)
{
    if (!argv0)
        return;
    const char *slash = strrchr(argv0, '/');
    progname = slash ? slash + 1 : argv0;
}

const char *getprogname(void)
{
    return progname;
}

static size_t count(void)
{
    size_t n = 0;
    while (env && env[n])
        n++;
    return n;
}

/* The entry for name (NAME= at its start), or -1. */
static long find(const char *name, size_t len)
{
    for (size_t i = 0; env && env[i]; i++)
        if (!strncmp(env[i], name, len) && env[i][len] == '=')
            return (long)i;
    return -1;
}

static bool valid(const char *name)
{
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return false;
    }
    return true;
}

/* Room for one more entry: the array on the heap, with space. */
static bool grow(void)
{
    size_t n = count();
    char **bigger = malloc((n + 2) * sizeof(char *));
    if (!bigger) {
        errno = ENOMEM;
        return false;
    }
    if (env)
        memcpy(bigger, env, n * sizeof(char *));
    bigger[n] = 0;
    if (on_heap)
        free(env);
    env = bigger;
    on_heap = true;
    return true;
}

char *getenv(const char *name)
{
    if (!name || !*name)
        return 0;
    long i = find(name, strlen(name));
    return i < 0 ? 0 : env[i] + strlen(name) + 1;
}

/* Put entry (a NAME=value string) in, in place of an old one of the name. */
static int put(char *entry, size_t name_len)
{
    long i = find(entry, name_len);
    if (i >= 0) {
        env[i] = entry;                 /* (the old string: possibly the stack's, kept) */
        return 0;
    }
    if (!grow())
        return -1;
    size_t n = count();
    env[n] = entry;
    env[n + 1] = 0;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (!valid(name))
        return -1;
    size_t len = strlen(name);
    if (!overwrite && find(name, len) >= 0)
        return 0;
    size_t vlen = strlen(value ? value : "");
    char *entry = malloc(len + vlen + 2);
    if (!entry) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(entry, name, len);
    entry[len] = '=';
    memcpy(entry + len + 1, value ? value : "", vlen + 1);
    return put(entry, len);
}

int putenv(char *string)
{
    char *eq = string ? strchr(string, '=') : 0;
    if (!eq || eq == string) {
        errno = EINVAL;
        return -1;
    }
    return put(string, (size_t)(eq - string));      /* (the string itself, as POSIX says) */
}

int unsetenv(const char *name)
{
    if (!valid(name))
        return -1;
    size_t len = strlen(name);
    long i;
    while ((i = find(name, len)) >= 0) {
        if (!on_heap && !grow())        /* (the stack's array is not ours to change) */
            return -1;
        for (size_t k = (size_t)i; env[k]; k++)
            env[k] = env[k + 1];
    }
    return 0;
}
