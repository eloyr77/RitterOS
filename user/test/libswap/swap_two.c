/* libswap.so.1, the second version: compatible with the first (the same
 * function, the same name), so programs built with the first use it as is. */
#include "swap.h"

/* Pointers: data the loader relocates, so read-only after (RELRO, 22f). */
static const char *const words[] = { "one", "two" };

const void *swap_relro(void)
{
    return words;
}

const char *swap_version(void)
{
    return "two";
}
