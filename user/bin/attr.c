/*
 * attr PATH [KEY | KEY=VALUE | -KEY]: a file's or directory's attributes.
 * Without KEY it lists them (key=value, one per line); KEY prints one
 * value; KEY=VALUE sets it; -KEY removes it. Values that are not plain
 * text are shown as their size.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("attr PATH [KEY | KEY=VALUE | -KEY]",
             "list, show, set or remove a file's attributes");

static char keys[4096];
static char value[ATTR_VALUE_MAX + 1];

static bool printable(const char *v, long n)
{
    for (long i = 0; i < n; i++)
        if ((unsigned char)v[i] < 0x20 || v[i] == 0x7F)
            return false;
    return true;
}

static void show_value(const char *key, long n)
{
    if (printable(value, n))
        printf("%s=%.*s\n", key, (int)n, value);
    else
        printf("%s: %ld bytes\n", key, n);
}

static int fail(const char *path, const char *key)
{
    if (key)
        dprintf(STDERR_FILENO, "attr: %s: %s: %s\n", path, key, strerror(errno));
    else
        dprintf(STDERR_FILENO, "attr: %s: %s\n", path, strerror(errno));
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        dprintf(STDERR_FILENO, "usage: attr PATH [KEY | KEY=VALUE | -KEY]\n");
        return 2;
    }
    const char *path = argv[1];
    if (argc == 2) {
        int64_t n = ritteros_attr(ATTR_LIST, path, 0, keys, sizeof(keys));
        if (n < 0)
            return fail(path, 0);
        if (n > (int64_t)sizeof(keys))
            n = sizeof(keys);                   /* only the keys that fit */
        for (int64_t at = 0; at < n; at += (int64_t)strlen(keys + at) + 1) {
            int64_t v = ritteros_attr(ATTR_GET, path, keys + at, value, ATTR_VALUE_MAX);
            if (v < 0)
                return fail(path, keys + at);
            show_value(keys + at, v);
        }
        return 0;
    }
    char *arg = argv[2], *equals = strchr(arg, '=');
    if (arg[0] == '-') {
        if (ritteros_attr(ATTR_REMOVE, path, arg + 1, 0, 0) < 0)
            return fail(path, arg + 1);
        return 0;
    }
    if (equals) {
        *equals = 0;
        if (ritteros_attr(ATTR_SET, path, arg, equals + 1, strlen(equals + 1)) < 0)
            return fail(path, arg);
        return 0;
    }
    int64_t n = ritteros_attr(ATTR_GET, path, arg, value, ATTR_VALUE_MAX);
    if (n < 0)
        return fail(path, arg);
    show_value(arg, n);
    return 0;
}
