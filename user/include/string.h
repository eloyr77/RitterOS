#ifndef RITTEROS_STRING_H
#define RITTEROS_STRING_H

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
const char *strerror(int error);

#endif
