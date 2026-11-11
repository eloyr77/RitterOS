/* libswap.so.1: a library of the tests only (step 22d), in two compatible
 * versions: the same function, a different answer. */
#ifndef RITTEROS_SWAP_H
#define RITTEROS_SWAP_H

/* Which version of the library this is: "one" or "two". */
const char *swap_version(void);

/* Where its relocated data is: a table of pointers (22f: read-only once
 * ld.so has relocated it). */
const void *swap_relro(void);

#endif
