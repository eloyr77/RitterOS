/*
 * byteswap.h (43a): GNU's bswap_16, bswap_32 and bswap_64: a number with
 * its bytes in the other order (the compiler's builtins).
 */
#ifndef RITTEROS_BYTESWAP_H
#define RITTEROS_BYTESWAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define bswap_16(x) __builtin_bswap16(x)
#define bswap_32(x) __builtin_bswap32(x)
#define bswap_64(x) __builtin_bswap64(x)

#ifdef __cplusplus
}
#endif

#endif
