/*
 * wchar.h (step 22v): wide characters and strings, C11's set. RitterOS has
 * the "C" locale only, one byte per character (MB_CUR_MAX 1, as stdlib.h's
 * mbtowc): a byte's wide character is its value (0-255), and a wide
 * character above 255 has no byte (EILSEQ where one is needed: wcrtomb,
 * fputwc, %ls in printf). For UTF-8 there is uchar.h (always UTF-8).
 * Streams: the wide calls read and write bytes; fwide records a stream's
 * orientation (the byte calls do not set it). Formatting: wprintf's and
 * wscanf's conversions are printf's and scanf's.
 */
#ifndef RITTEROS_WCHAR_H
#define RITTEROS_WCHAR_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>                     /* WCHAR_MIN, WCHAR_MAX */
#include <stdio.h>

#ifndef RITTEROS_WINT_T
#define RITTEROS_WINT_T
typedef __WINT_TYPE__ wint_t;
#endif
#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

/* A conversion's state between calls (uchar.h's UTF-8: the bytes still
 * needed and the bits so far). All zero: the initial state. */
#ifndef RITTEROS_MBSTATE_T
#define RITTEROS_MBSTATE_T
typedef struct {
    unsigned int need, bits;
} mbstate_t;
#endif

struct tm;

/* Strings, as string.h's (counts in wide characters). wcstok's third
 * argument keeps its place between calls. */
size_t wcslen(const wchar_t *s);
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int wcscoll(const wchar_t *a, const wchar_t *b);
size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
size_t wcsspn(const wchar_t *s, const wchar_t *accept);
size_t wcscspn(const wchar_t *s, const wchar_t *reject);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);
wchar_t *wcstok(wchar_t *s, const wchar_t *delimiters, wchar_t **place);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);

/* Numbers from wide text, as strtol and strtod. */
long wcstol(const wchar_t *s, wchar_t **end, int base);
long long wcstoll(const wchar_t *s, wchar_t **end, int base);
unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base);
unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base);
double wcstod(const wchar_t *s, wchar_t **end);
float wcstof(const wchar_t *s, wchar_t **end);
long double wcstold(const wchar_t *s, wchar_t **end);

/* A date as wide text, as strftime. */
size_t wcsftime(wchar_t *s, size_t size, const wchar_t *format, const struct tm *tm);

/* Bytes and wide characters (one byte each, see above). mbrtowc: the
 * character at s into *wc; 1, 0 for the NUL, (size_t)-2 for n = 0.
 * wcrtomb: wc's byte into s; 1, or (size_t)-1 (EILSEQ). The s...rtowcs
 * forms convert strings, with *src moved past what was converted (0 at
 * the end). */
wint_t btowc(int c);
int wctob(wint_t wc);
int mbsinit(const mbstate_t *state);
size_t mbrlen(const char *s, size_t n, mbstate_t *state);
size_t mbrtowc(wchar_t *wc, const char *s, size_t n, mbstate_t *state);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *state);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *state);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *state);

/* Wide characters on streams. fwide: mode > 0 makes a stream wide, < 0
 * byte-oriented (once); the result tells which it is (0: neither yet). */
int fwide(FILE *f, int mode);
wint_t fgetwc(FILE *f);
wint_t getwc(FILE *f);
wint_t getwchar(void);
wchar_t *fgetws(wchar_t *s, int size, FILE *f);
wint_t ungetwc(wint_t wc, FILE *f);
wint_t fputwc(wchar_t wc, FILE *f);
wint_t putwc(wchar_t wc, FILE *f);
wint_t putwchar(wchar_t wc);
int fputws(const wchar_t *s, FILE *f);

/* Formatted: printf's conversions with a wide format (%s takes bytes, %ls
 * wide characters, in every form). swprintf writes at most size wide
 * characters with the NUL, and is negative when more were needed. */
int wprintf(const wchar_t *format, ...);
int fwprintf(FILE *f, const wchar_t *format, ...);
int swprintf(wchar_t *s, size_t size, const wchar_t *format, ...);
int vwprintf(const wchar_t *format, va_list ap);
int vfwprintf(FILE *f, const wchar_t *format, va_list ap);
int vswprintf(wchar_t *s, size_t size, const wchar_t *format, va_list ap);
int wscanf(const wchar_t *format, ...);
int fwscanf(FILE *f, const wchar_t *format, ...);
int swscanf(const wchar_t *s, const wchar_t *format, ...);
int vwscanf(const wchar_t *format, va_list ap);
int vfwscanf(FILE *f, const wchar_t *format, va_list ap);
int vswscanf(const wchar_t *s, const wchar_t *format, va_list ap);

#endif
