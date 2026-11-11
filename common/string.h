/*
 * Memory functions. Even freestanding, the C compiler may emit calls to
 * these (e.g. for struct copies), so every RitterOS binary must provide them.
 */
#ifndef RITTEROS_STRING_H
#define RITTEROS_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);   /* at most n characters */

#endif
