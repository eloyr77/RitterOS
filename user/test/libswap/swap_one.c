/* libswap.so.1, the first version (see swap.h). */
#include "swap.h"

/* Pointers: data the loader relocates, so read-only after (RELRO, 22f). */
static const char *const words[] = { "one", "two" };

const void *swap_relro(void)
{
    return words;
}

const char *swap_version(void)
{
    return "one";
}
