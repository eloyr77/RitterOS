/*
 * sys/param.h (43a): BSD's helpers: MIN and MAX, howmany (how many y's
 * hold x), roundup, powerof2, NBBY (bits a byte), MAXPATHLEN and
 * MAXHOSTNAMELEN (limits.h's PATH_MAX, HOST_NAME_MAX), the byte order.
 */
#ifndef RITTEROS_SYS_PARAM_H
#define RITTEROS_SYS_PARAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <endian.h>
#include <limits.h>

#ifndef MIN
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#endif
#define howmany(x, y)   (((x) + ((y) - 1)) / (y))
#define roundup(x, y)   ((((x) + ((y) - 1)) / (y)) * (y))
#define powerof2(x)     ((((x) - 1) & (x)) == 0)
#define NBBY            8
#define MAXPATHLEN      PATH_MAX
#define MAXHOSTNAMELEN  HOST_NAME_MAX
#define MAXSYMLINKS     SYMLOOP_MAX
#define NOFILE          OPEN_MAX

#ifdef __cplusplus
}
#endif

#endif
