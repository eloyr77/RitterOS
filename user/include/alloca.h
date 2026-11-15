/*
 * alloca.h (43a): alloca(size): size bytes on the caller's stack, freed
 * when the caller returns (the compiler's builtin; nothing checks that
 * they fit: the stack is 8 MiB).
 */
#ifndef RITTEROS_ALLOCA_H
#define RITTEROS_ALLOCA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define alloca(size) __builtin_alloca(size)

#ifdef __cplusplus
}
#endif

#endif
