#ifndef RITTEROS_SYS_TYPES_H
#define RITTEROS_SYS_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int mode_t;
typedef int64_t ssize_t;
typedef unsigned int useconds_t;
typedef int64_t off_t;
/* (43a) The rest of POSIX's: a device and an object's numbers, links,
 * blocks, ids, and BSD's short names. */
typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef uint64_t nlink_t;
typedef int64_t blksize_t;
typedef int64_t blkcnt_t;
typedef uint64_t fsblkcnt_t;
typedef uint64_t fsfilcnt_t;
typedef unsigned int id_t;
typedef int key_t;
typedef long suseconds_t;
typedef int64_t loff_t;
typedef char *caddr_t;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned char u_int8_t;
typedef unsigned short u_int16_t;
typedef unsigned int u_int32_t;
typedef unsigned long u_int64_t;
#ifndef RITTEROS_TIME_TYPES
#define RITTEROS_TIME_TYPES
typedef int64_t time_t;
typedef long clock_t;
#endif
#include <stddef.h>

#ifdef __cplusplus
}
#endif

#endif
