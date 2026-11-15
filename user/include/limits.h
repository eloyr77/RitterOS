/*
 * limits.h (43a): the compiler's limits of C's types (CHAR_BIT, INT_MAX,
 * ...: its own limits.h, found after this one), and POSIX's of the system
 * (RitterOS's own numbers):
 * - PATH_MAX: a path's bytes with the NUL (the kernel takes 4096);
 *   NAME_MAX: a name in a folder (RitterFS: 255);
 * - OPEN_MAX: descriptors a process may have (256); IOV_MAX: parts of a
 *   readv/writev; ARG_MAX: bytes of a new program's arguments and
 *   environment (128 KiB); HOST_NAME_MAX, LOGIN_NAME_MAX, TTY_NAME_MAX;
 * - SYMLOOP_MAX: links in one path (40); PTHREAD_STACK_MIN (16 KiB),
 *   PTHREAD_KEYS_MAX, PTHREAD_THREADS_MAX (256 a program); PIPE_BUF: a
 *   pipe's writes up to this size are never mixed with others' (4096);
 * - NGROUPS_MAX, LINE_MAX, NL_ARGMAX, SSIZE_MAX, and the _POSIX_ minimums.
 */
#ifndef RITTEROS_LIMITS_H
#define RITTEROS_LIMITS_H

#ifdef __cplusplus
extern "C" {
#endif

#include_next <limits.h>

#undef MB_LEN_MAX
#define MB_LEN_MAX              4       /* (43a-2) a UTF-8 character's bytes */
#define PATH_MAX                4096
#define NAME_MAX                255
#define OPEN_MAX                256
#define IOV_MAX                 1024
#define ARG_MAX                 131072
#define HOST_NAME_MAX           64
#define LOGIN_NAME_MAX          256
#define TTY_NAME_MAX            32
#define SYMLOOP_MAX             40
#define PTHREAD_STACK_MIN       65536
#define PTHREAD_KEYS_MAX        64
#define PTHREAD_THREADS_MAX     256
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PIPE_BUF                4096
#define NGROUPS_MAX             32
#define LINE_MAX                4096
#define NL_ARGMAX               9
#define NL_TEXTMAX              2048
#define NZERO                   20
#define SSIZE_MAX               LONG_MAX
#define CHARCLASS_NAME_MAX      14
#define COLL_WEIGHTS_MAX        2
#define EXPR_NEST_MAX           32
#define RE_DUP_MAX              255
#define BC_BASE_MAX             99
#define BC_DIM_MAX              2048
#define BC_SCALE_MAX            99
#define BC_STRING_MAX           1000
#define DELAYTIMER_MAX          0x7fffffff
#define SEM_VALUE_MAX           0x7fffffff
#define SEM_NSEMS_MAX           256
#define WORD_BIT                32
#define LONG_BIT                64

#define _POSIX_ARG_MAX          4096
#define _POSIX_CHILD_MAX        25
#define _POSIX_HOST_NAME_MAX    255
#define _POSIX_LINK_MAX         8
#define _POSIX_LOGIN_NAME_MAX   9
#define _POSIX_MAX_CANON        255
#define _POSIX_MAX_INPUT        255
#define _POSIX_NAME_MAX         14
#define _POSIX_NGROUPS_MAX      8
#define _POSIX_OPEN_MAX         20
#define _POSIX_PATH_MAX         256
#define _POSIX_PIPE_BUF         512
#define _POSIX_SSIZE_MAX        32767
#define _POSIX_SYMLOOP_MAX      8
#define _POSIX_TTY_NAME_MAX     9
#define _POSIX_TZNAME_MAX       6
#define _POSIX2_LINE_MAX        2048
#define _POSIX2_RE_DUP_MAX      255
#define _XOPEN_IOV_MAX          16
#define _XOPEN_NAME_MAX         255
#define _XOPEN_PATH_MAX         1024

#ifdef __cplusplus
}
#endif

#endif
