#ifndef RITTEROS_STRING_H
#define RITTEROS_STRING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strcspn(const char *s, const char *reject);    /* bytes before one of reject */
char *strcpy(char *dst, const char *src);
/* (32e) stpcpy: strcpy, giving the end of dst (its NUL); memrchr: memchr
 * from the end (the last c in n bytes, or 0; GNU's). */
char *stpcpy(char *dst, const char *src);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
void *memrchr(const void *s, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);

/* (22m) A copy on the heap (strndup: of at most n bytes); 0 if out of memory. */
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
/* As C's: n bytes always written (padded with NULs; no NUL if src is longer). */
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);

/* (22p) strspn: bytes at the start that are all in accept; strpbrk: the
 * first of breaks (or 0); strtok: the next word between delimiters (s for
 * the first call, 0 for the rest; it writes NULs into s); strnlen: strlen
 * up to n; memccpy: copies until c (the place after it in dst, or 0);
 * strcoll and strxfrm as strcmp and strncpy (the "C" locale's order). */
size_t strspn(const char *s, const char *accept);
char *strpbrk(const char *s, const char *breaks);
char *strtok(char *s, const char *delimiters);
size_t strnlen(const char *s, size_t n);
void *memccpy(void *dst, const void *src, int c, size_t n);
int strcoll(const char *a, const char *b);
size_t strxfrm(char *dst, const char *src, size_t n);

/* The text for an error number, e.g. "No such file or directory". */
char *strerror(int error);

/* (43a) What ported code expects beside C's: strtok_r (strtok with the
 * place kept in *save: threads may each use one), strerror_r (POSIX's: the
 * text into buf, 0 or ERANGE when it does not fit), strsignal (a signal's
 * name in words), strcasestr (strstr without case), strchrnul (strchr, or
 * the string's end), stpncpy, mempcpy (the place after the copy), memmem
 * (memory in memory), strsep (the next field of *s, ended by one of
 * delimiters), strlcpy and strlcat (BSD's: always a NUL within size; the
 * length they tried to make), explicit_bzero (zeroes that the compiler
 * cannot leave out), strverscmp (GNU's: digits as numbers, "a9" < "a10"). */
char *strtok_r(char *s, const char *delimiters, char **save);
int strerror_r(int error, char *buf, size_t size);
char *strsignal(int signal);
char *strcasestr(const char *haystack, const char *needle);
char *strchrnul(const char *s, int c);
char *stpncpy(char *dst, const char *src, size_t n);
void *mempcpy(void *dst, const void *src, size_t n);
void *memmem(const void *haystack, size_t n, const void *needle, size_t m);
char *strsep(char **s, const char *delimiters);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
void explicit_bzero(void *s, size_t n);
int strverscmp(const char *a, const char *b);

/* (43a-2) The same in a given locale (every locale collates as C). */
#ifndef RITTEROS_LOCALE_T
#define RITTEROS_LOCALE_T
typedef struct __locale_struct *locale_t;
#endif
int strcoll_l(const char *a, const char *b, locale_t l);
size_t strxfrm_l(char *dst, const char *src, size_t n, locale_t l);
char *strerror_l(int error, locale_t l);

#ifdef __cplusplus
}
#endif

#endif
