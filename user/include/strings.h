/*
 * strings.h (22m): comparing text without regard to case (ASCII letters),
 * as BSD and POSIX have it.
 */
#ifndef RITTEROS_STRINGS_H
#define RITTEROS_STRINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

/* (43a) BSD's older names: bcmp (memcmp's zero or not), bcopy (memmove
 * with the arguments the other way), bzero (memset to 0), index and rindex
 * (strchr, strrchr); ffs, ffsl, ffsll: the lowest bit set, counted from 1
 * (0 for none). */
int bcmp(const void *a, const void *b, size_t n);
void bcopy(const void *src, void *dst, size_t n);
void bzero(void *s, size_t n);
char *index(const char *s, int c);
char *rindex(const char *s, int c);
int ffs(int v);
int ffsl(long v);
int ffsll(long long v);

#ifndef RITTEROS_LOCALE_T
#define RITTEROS_LOCALE_T
typedef struct __locale_struct *locale_t;
#endif
int strcasecmp_l(const char *a, const char *b, locale_t l);
int strncasecmp_l(const char *a, const char *b, size_t n, locale_t l);

#ifdef __cplusplus
}
#endif

#endif
