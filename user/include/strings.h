/*
 * strings.h (22m): comparing text without regard to case (ASCII letters),
 * as BSD and POSIX have it.
 */
#ifndef RITTEROS_STRINGS_H
#define RITTEROS_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#endif
