#ifndef RITTEROS_STDLIB_H
#define RITTEROS_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS    0
#define EXIT_FAILURE    1

/* End the program with an exit code (0-255; 0: success). */
__attribute__((noreturn)) void exit(int code);

/* The heap (user/lib/malloc.c): memory from the kernel in large arenas, a
 * free list inside them. Returns 0 (and errno ENOMEM) if none is left. free
 * of 0 does nothing; realloc(0, n) is malloc, realloc(p, 0) frees. */
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
/* (22p) Memory at an address that is a multiple of alignment (a power of
 * two); freed with free. 0 and EINVAL for another alignment. */
void *aligned_alloc(size_t alignment, size_t size);

/* The decimal number at the start of s (spaces and a sign before it
 * allowed; 0 if there is none). */
long atol(const char *s);
int atoi(const char *s);
long long atoll(const char *s);                     /* (22p) */

/* Numbers from text (22m): as C's -- spaces, a sign, then digits in base
 * (0: from the prefix, 0x hexadecimal, 0 octal); *end (if not 0) where
 * reading stopped. strtod/atof read decimal numbers with a fraction and an
 * exponent (and inf, nan). */
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
double atof(const char *s);
/* (22p) strtof: strtod's value rounded to float; strtold: strtod's value
 * (long double has double's precision here: exact for every double, but
 * not the extra digits of an 80-bit or 128-bit long double). */
float strtof(const char *s, char **end);
long double strtold(const char *s, char **end);

int abs(int v);
long labs(long v);
long long llabs(long long v);

/* (22p) A quotient and its remainder at once (rounded toward zero). */
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);

/* (22p) Sorting and searching an array of count elements of size bytes, by
 * compare (negative, 0, positive: before, the same, after). qsort is not
 * stable (equal elements may change places); bsearch needs it sorted. */
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *a, const void *b));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *key, const void *element));

/* (22p) Pseudo-random numbers, 0 to RAND_MAX: the same series for the same
 * seed (srand; 1 at the start). Not for secrets: ritteros_random is. */
#define RAND_MAX 2147483647
int rand(void);
void srand(unsigned int seed);

/* (22p) Functions exit calls first, the last given first (up to 32 each);
 * quick_exit calls at_quick_exit's and ends without flushing streams;
 * _Exit ends at once (no functions, no flushing). 0, or nonzero when full. */
int atexit(void (*fn)(void));
int at_quick_exit(void (*fn)(void));
__attribute__((noreturn)) void quick_exit(int code);
__attribute__((noreturn)) void _Exit(int code);

/* (22p) Multibyte characters in the "C" locale: every byte is a character
 * of its own (MB_CUR_MAX 1; UTF-8 is not decoded here). mblen/mbtowc: the
 * bytes of the character at s (0 for the NUL, -1 for none); wctomb: the
 * bytes written (-1, EILSEQ, for a character above 255). */
#define MB_CUR_MAX 1
int mblen(const char *s, size_t n);
int mbtowc(wchar_t *wc, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);

/* (22t) Environment variables: getenv a variable's value (0 if not set);
 * setenv sets one (overwrite 0: only if not set yet), unsetenv removes one,
 * putenv puts the string "NAME=value" itself in; 0, or -1 and errno
 * (EINVAL: a name with '=' or empty). Programs started later get them. */
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);

/* (41g-3) The program's name: the last part of its argv[0] (BSD's and
 * macOS's getprogname), "" when it had none. */
const char *getprogname(void);

/* Run a command with the shell (sh -c COMMAND): its status as waitpid gives
 * it (WEXITSTATUS), or -1 if it could not start; system(0) is 1 (there is a
 * shell). */
int system(const char *command);

/* A new file from a name ending in "XXXXXX" (22n): the X's become random
 * letters and digits no file has; the file is made (mode 0600) and its
 * descriptor, open for reading and writing, returned; -1 and errno
 * (EINVAL: no XXXXXX at the end). */
int mkstemp(char *name);

/* End the program at once, as a failed assert does: with SIGABRT (22r; a
 * handler may run first, but the program ends even if it returns). */
__attribute__((noreturn)) void abort(void);

#endif
