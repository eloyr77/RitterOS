/* atomhost's stand-in (see ../README): Linux's integer types. */
#ifndef ATOMORACLE_TYPES_H
#define ATOMORACLE_TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint16_t __le16;
typedef uint32_t __le32;
struct mutex { int unused; };
#endif
