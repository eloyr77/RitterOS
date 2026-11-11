/*
 * printf FORMAT [ARGUMENT]... (51d, POSIX): FORMAT with its escapes (\n
 * \t \\ \a \b \f \r \v \0NNN) and conversions (%d %i %o %u %x %X %c %s %b
 * %e %E %f %F %g %G %a %A %%, with flags, width and precision, * from an
 * argument), used again while arguments are left. A number's argument may
 * be 'C (the character's code); a missing one is 0 or empty. %b prints its
 * argument with escapes, \c ending all output. Exit code 1 if an argument
 * was not a number (it is used as 0).
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"

PROGRAM_HELP("printf FORMAT [ARGUMENT]...", "print the arguments in a format");

static char **args;
static int arg_count, next;
static int status;
static bool stop;                               /* \c in %b: no more output */

static const char *take(void)
{
    return next < arg_count ? args[next++] : 0;
}

/* An escape at s (just after the backslash); its byte into *c. Returns the
 * characters it took. octal_zero: %b's \0NNN (else \NNN). */
static int escape(const char *s, char *c, bool octal_zero)
{
    static const char from[] = "abfnrtv\\\"'", to[] = "\a\b\f\n\r\t\v\\\"'";
    const char *f = strchr(from, *s);
    if (*s && f) {
        *c = to[f - from];
        return 1;
    }
    if (*s >= '0' && *s <= '7') {
        int n = 0, v = 0;
        const char *p = s;
        if (octal_zero && *p == '0')
            p++;                                /* (\0NNN: up to three digits after the 0) */
        while (n < 3 && *p >= '0' && *p <= '7') {
            v = v * 8 + (*p++ - '0');
            n++;
        }
        *c = (char)v;
        return (int)(p - s);
    }
    *c = '\\';
    return 0;                                   /* (an unknown one: the backslash itself) */
}

static void put(char c)
{
    if (!stop)
        putchar(c);
}

static long long integer(const char *s)
{
    if (!s || !*s)
        return 0;
    if (*s == '\'' || *s == '"')
        return (unsigned char)s[1];
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 0);
    if (*end || errno) {
        if (end == s || errno)
            v = 0;
        fprintf(stderr, "printf: %s: %s\n", s, errno ? "out of range" : "not a number");
        status = 1;
    }
    return v;
}

static double real(const char *s)
{
    if (!s || !*s)
        return 0;
    if (*s == '\'' || *s == '"')
        return (unsigned char)s[1];
    char *end;
    double v = strtod(s, &end);
    if (*end) {
        fprintf(stderr, "printf: %s: not a number\n", s);
        status = 1;
    }
    return v;
}

/* One pass over the format. */
static void format(const char *f)
{
    while (*f && !stop) {
        if (*f == '\\') {
            char c;
            int n = escape(f + 1, &c, false);
            if (n == 0 && f[1] == 'c')
                n = 1, stop = true;             /* (\c in the format: stop, too) */
            else
                put(c);
            f += 1 + n;
            continue;
        }
        if (*f != '%') {
            put(*f++);
            continue;
        }
        if (f[1] == '%') {
            put('%');
            f += 2;
            continue;
        }
        char spec[64];
        size_t n = 0;
        spec[n++] = *f++;
        while (*f && strchr("-+ #0", *f) && n < 40)
            spec[n++] = *f++;
        for (int part = 0; part < 2; part++) {  /* width, then . precision */
            if (part == 1) {
                if (*f != '.')
                    break;
                spec[n++] = *f++;
            }
            if (*f == '*') {
                n += (size_t)snprintf(spec + n, sizeof(spec) - n, "%d", (int)integer(take()));
                f++;
            } else {
                while (*f >= '0' && *f <= '9' && n < 50)
                    spec[n++] = *f++;
            }
        }
        while (*f && strchr("hlLqjzt", *f))
            f++;                                /* (sizes: the argument is taken whole) */
        char conv = *f ? *f++ : 0;
        const char *a;
        char text[4096];
        switch (conv) {
        case 'd': case 'i':
            spec[n++] = 'l', spec[n++] = 'l', spec[n++] = conv, spec[n] = 0;
            snprintf(text, sizeof(text), spec, integer(take()));
            break;
        case 'o': case 'u': case 'x': case 'X':
            spec[n++] = 'l', spec[n++] = 'l', spec[n++] = conv, spec[n] = 0;
            snprintf(text, sizeof(text), spec, (unsigned long long)integer(take()));
            break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
            spec[n++] = conv, spec[n] = 0;
            snprintf(text, sizeof(text), spec, real(take()));
            break;
        case 'c':
            a = take();
            spec[n++] = 'c', spec[n] = 0;
            snprintf(text, sizeof(text), spec, a && *a ? a[0] : 0);
            if (!(a && *a)) {                   /* (no argument: nothing, not a NUL) */
                text[0] = 0;
            }
            break;
        case 's':
            a = take();
            spec[n++] = 's', spec[n] = 0;
            snprintf(text, sizeof(text), spec, a ? a : "");
            break;
        case 'b': {
            a = take();
            char expanded[4096];
            size_t k = 0;
            for (const char *p = a ? a : ""; *p && k + 1 < sizeof(expanded); p++) {
                if (*p != '\\') {
                    expanded[k++] = *p;
                    continue;
                }
                if (p[1] == 'c') {
                    stop = true;                /* (\c: no more output after this) */
                    break;
                }
                char c;
                int used = escape(p + 1, &c, true);
                expanded[k++] = c;
                p += used;
            }
            expanded[k] = 0;
            spec[n++] = 's', spec[n] = 0;
            snprintf(text, sizeof(text), spec, expanded);
            for (const char *t = text; *t; t++)
                putchar(*t);                    /* (what came before \c still shows) */
            continue;
        }
        default:
            fprintf(stderr, "printf: %%%c: not a conversion\n", conv ? conv : ' ');
            status = 1;
            return;
        }
        for (const char *t = text; *t; t++)
            put(*t);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: printf FORMAT [ARGUMENT]...\n");
        return 2;
    }
    args = argv + 2;
    arg_count = argc - 2;
    do {
        int before = next;
        format(argv[1]);
        if (next == before)
            break;                              /* (a format that takes nothing: once) */
    } while (next < arg_count && !stop);
    fflush(stdout);
    return status;
}
