#ifndef RITTEROS_STDLIB_H
#define RITTEROS_STDLIB_H

#ifdef __cplusplus
extern "C" {
#endif

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

/* (22p) Multibyte characters: in the "C" locale every byte is a character
 * of its own (MB_CUR_MAX 1), (43a-2) in C.UTF-8 a character is UTF-8's 1
 * to 4 bytes (MB_CUR_MAX 4; the calling thread's locale, see locale.h).
 * mblen/mbtowc: the bytes of the character at s (0 for the NUL, -1 for
 * none); wctomb: the bytes written (-1, EILSEQ, for a character the
 * locale has no bytes for). */
int __ritteros_mb_cur_max(void);
#define MB_CUR_MAX ((size_t)__ritteros_mb_cur_max())
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

/* (43a) What ported code expects beside C's:
 * - memory: posix_memalign (aligned_alloc's, into *out: 0 or an error
 *   number), reallocarray (realloc of count * size, ENOMEM if that
 *   overflows);
 * - temporary files: mkostemp (mkstemp with open's O_CLOEXEC/O_APPEND
 *   flags), mkstemps/mkostemps (XXXXXX followed by suffix_length more
 *   bytes), mkdtemp (a new folder, 0700: the name, or 0), mktemp (only
 *   the name: another program may take it first; prefer mkstemp);
 * - realpath: the path without ., .. or symbolic links (into resolved,
 *   PATH_MAX bytes, or a new malloc'd string when resolved is 0);
 * - the environment: secure_getenv (getenv, but 0 in a setuid program),
 *   clearenv (empties it);
 * - random numbers: random/srandom/initstate/setstate (the BSD generator
 *   glibc and macOS share: the same numbers for a seed), the 48-bit
 *   generators (drand48 ... lcong48, as POSIX defines them), rand_r (a
 *   generator in *seed: threads may each have one);
 * - qsort_r: qsort with arg passed to every compare (POSIX 2024's and
 *   GNU's order);
 * - getloadavg: -1 (RitterOS keeps no load averages). */
int posix_memalign(void **out, size_t alignment, size_t size);
void *reallocarray(void *ptr, size_t count, size_t size);
int mkostemp(char *name, int flags);
int mkstemps(char *name, int suffix_length);
int mkostemps(char *name, int suffix_length, int flags);
char *mkdtemp(char *name);
char *mktemp(char *name);
char *realpath(const char *path, char *resolved);
char *secure_getenv(const char *name);
int clearenv(void);
long random(void);
void srandom(unsigned int seed);
char *initstate(unsigned int seed, char *state, size_t size);
char *setstate(char *state);
double drand48(void);
double erand48(unsigned short x[3]);
long lrand48(void);
long nrand48(unsigned short x[3]);
long mrand48(void);
long jrand48(unsigned short x[3]);
void srand48(long seed);
unsigned short *seed48(unsigned short seed[3]);
void lcong48(unsigned short parameters[7]);
int rand_r(unsigned int *seed);
void qsort_r(void *base, size_t count, size_t size,
             int (*compare)(const void *a, const void *b, void *arg), void *arg);
int getloadavg(double loads[], int count);

/* (43a-2) glibc's strto*_l (every locale's decimal point is '.'). */
#ifndef RITTEROS_LOCALE_T
#define RITTEROS_LOCALE_T
typedef struct __locale_struct *locale_t;
#endif
double strtod_l(const char *s, char **end, locale_t l);
float strtof_l(const char *s, char **end, locale_t l);
long double strtold_l(const char *s, char **end, locale_t l);
long strtol_l(const char *s, char **end, int base, locale_t l);
unsigned long strtoul_l(const char *s, char **end, int base, locale_t l);
long long strtoll_l(const char *s, char **end, int base, locale_t l);
unsigned long long strtoull_l(const char *s, char **end, int base, locale_t l);

/* (43a-4) The C++ ABI's calls (the compiler's and the C++ library's):
 * __cxa_atexit registers a static object's destructor (fn(arg)) with the
 * module (dso) that made it, run by exit with atexit's functions, last
 * first; __cxa_finalize(dso) runs dso's now (0: all);
 * __cxa_thread_atexit_impl a thread_local object's, run when its thread
 * ends (or, for the thread calling exit, before the others). */
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
void __cxa_finalize(void *dso);
int __cxa_thread_atexit_impl(void (*fn)(void *), void *obj, void *dso);
int __cxa_thread_atexit(void (*fn)(void *), void *obj, void *dso);

/* (45d) The compiler's __builtin___clear_cache (GCC's and compiler-rt's
 * name): code just written into [start, end), as a JIT writes it through
 * a writable mapping of the memory another maps executable, made
 * runnable. On AArch64 the data cache cleaned and the instruction cache
 * invalidated over it, line by line; on x86-64 nothing (coherent). */
void __clear_cache(void *start, void *end);

#ifdef __cplusplus
}
#endif

#endif
