/*
 * fnmatch.h (43a): a name against a shell pattern (*, ?, [...] with
 * ranges, classes such as [:alpha:] and ! or ^ for "not"; \ quotes the
 * next character): 0 when it matches, FNM_NOMATCH when not. Flags:
 * FNM_PATHNAME ('/' only matched by a '/'), FNM_PERIOD (a leading '.',
 * after a '/' too with FNM_PATHNAME, only matched by a '.'), FNM_NOESCAPE
 * (\ is an ordinary character), and GNU's FNM_CASEFOLD (no case) and
 * FNM_LEADING_DIR (the pattern may match a leading folder of the name).
 */
#ifndef RITTEROS_FNMATCH_H
#define RITTEROS_FNMATCH_H

#ifdef __cplusplus
extern "C" {
#endif

#define FNM_NOMATCH     1
#define FNM_NOESCAPE    0x01
#define FNM_PATHNAME    0x02
#define FNM_PERIOD      0x04
#define FNM_LEADING_DIR 0x08
#define FNM_CASEFOLD    0x10
#define FNM_FILE_NAME   FNM_PATHNAME
#define FNM_IGNORECASE  FNM_CASEFOLD

int fnmatch(const char *pattern, const char *name, int flags);

#ifdef __cplusplus
}
#endif

#endif
