/*
 * glob.h (43a): the paths that match a shell pattern (fnmatch's, part by
 * part between the '/'s; a leading '.' only matched by a '.'), sorted,
 * into pglob's gl_pathv (gl_pathc of them, then a null pointer; freed by
 * globfree). Flags: GLOB_ERR (stop at a folder that cannot be read),
 * GLOB_MARK (a '/' after folders), GLOB_NOSORT, GLOB_DOOFFS (gl_offs null
 * pointers first), GLOB_NOCHECK (no match: the pattern itself),
 * GLOB_APPEND (add to an earlier glob's result), GLOB_NOESCAPE, and GNU's
 * GLOB_PERIOD (a leading '.' matched by wildcards) and GLOB_TILDE (~ and
 * ~user are home folders). errfunc (or 0) is told of each folder that
 * cannot be read; nonzero stops. 0, GLOB_NOMATCH, GLOB_NOSPACE or
 * GLOB_ABORTED.
 */
#ifndef RITTEROS_GLOB_H
#define RITTEROS_GLOB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef struct {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
} glob_t;

#define GLOB_ERR        0x0001
#define GLOB_MARK       0x0002
#define GLOB_NOSORT     0x0004
#define GLOB_DOOFFS     0x0008
#define GLOB_NOCHECK    0x0010
#define GLOB_APPEND     0x0020
#define GLOB_NOESCAPE   0x0040
#define GLOB_PERIOD     0x0080
#define GLOB_TILDE      0x1000
#define GLOB_NOSPACE    1
#define GLOB_ABORTED    2
#define GLOB_NOMATCH    3
#define GLOB_NOSYS      4

int glob(const char *pattern, int flags, int (*errfunc)(const char *path, int error), glob_t *pglob);
void globfree(glob_t *pglob);

#ifdef __cplusplus
}
#endif

#endif
