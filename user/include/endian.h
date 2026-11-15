/*
 * endian.h (43a): the byte order (both RitterOS architectures are little
 * endian) and the conversions to and from big and little endian of 16,
 * 32 and 64 bits (htobe32: the host's to big endian, be32toh back, ...).
 */
#ifndef RITTEROS_ENDIAN_H
#define RITTEROS_ENDIAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define LITTLE_ENDIAN   1234
#define BIG_ENDIAN      4321
#define PDP_ENDIAN      3412
#define BYTE_ORDER      LITTLE_ENDIAN
#define __LITTLE_ENDIAN LITTLE_ENDIAN
#define __BIG_ENDIAN    BIG_ENDIAN
#define __PDP_ENDIAN    PDP_ENDIAN
#define __BYTE_ORDER    BYTE_ORDER

#define htobe16(x) __builtin_bswap16(x)
#define htole16(x) ((uint16_t)(x))
#define be16toh(x) __builtin_bswap16(x)
#define le16toh(x) ((uint16_t)(x))
#define htobe32(x) __builtin_bswap32(x)
#define htole32(x) ((uint32_t)(x))
#define be32toh(x) __builtin_bswap32(x)
#define le32toh(x) ((uint32_t)(x))
#define htobe64(x) __builtin_bswap64(x)
#define htole64(x) ((uint64_t)(x))
#define be64toh(x) __builtin_bswap64(x)
#define le64toh(x) ((uint64_t)(x))

#ifdef __cplusplus
}
#endif

#endif
