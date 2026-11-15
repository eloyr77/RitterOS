/*
 * libgen.h (43a): POSIX's basename (the last part of a path: "usr" of
 * "/usr/", "/" of "/") and dirname (the part before it: "/" of "/usr", "."
 * of "usr"). Both may write into path, and may return a constant (".").
 */
#ifndef RITTEROS_LIBGEN_H
#define RITTEROS_LIBGEN_H

#ifdef __cplusplus
extern "C" {
#endif

char *basename(char *path);
char *dirname(char *path);

#ifdef __cplusplus
}
#endif

#endif
