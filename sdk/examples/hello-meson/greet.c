/* hello-meson's own shared library (libgreet.so.1), in C. */
#include <stdio.h>
#include "greet.h"

int greet(char *out, int size, const char *name)
{
    return snprintf(out, (size_t)size, "hello, %s, from libgreet", name);
}
