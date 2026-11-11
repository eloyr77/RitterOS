/*
 * printf and friends. One formatter writes into a "sink": a buffer
 * (snprintf), a buffer of wide characters (swprintf, 22v) or a file
 * descriptor, through a small buffer so that a printf costs one system
 * call in most cases. The format is bytes or wide characters (wprintf).
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include "fpconv.h"
#include "wideformat.h"

struct sink {
    int fd;                         /* -1: into buffer[0..size) (or wide[0..size)) */
    char *buffer;
    wchar_t *wide;
    size_t size;
    size_t total;                   /* characters produced (also those cut off) */
    bool bad;                       /* a wide character without a byte (EILSEQ) */
    char out[256];                  /* fd: not yet written */
    size_t pending;
};

static void flush(struct sink *s)
{
    if (s->pending)
        write(s->fd, s->out, s->pending);
    s->pending = 0;
}

static void put(struct sink *s, char c)
{
    if (s->fd >= 0) {
        s->out[s->pending++] = c;
        if (s->pending == sizeof(s->out))
            flush(s);
    } else if (s->total + 1 < s->size) {
        if (s->wide)
            s->wide[s->total] = (wchar_t)(unsigned char)c;     /* (a byte's own value) */
        else
            s->buffer[s->total] = c;
    }
    s->total++;
}

/* A wide character: itself into wide[], else the byte of its value. */
static void put_wide(struct sink *s, unsigned wc)
{
    if (s->wide) {
        if (s->total + 1 < s->size)
            s->wide[s->total] = (wchar_t)wc;
        s->total++;
    } else if (wc > 255) {
        s->bad = true;
    } else {
        put(s, (char)wc);
    }
}

static void put_repeated(struct sink *s, char c, int n)
{
    for (; n > 0; n--)
        put(s, c);
}

/* How a number is written: the flags and sizes of its conversion. */
struct style {
    int width, precision;               /* precision -1: none */
    bool left, zero, plus, space, alt, upper;
};

/* A number with sign, padding and alignment; a precision is the least
 * number of digits (22m: "%.3d" of 33 is 033; of 0 with precision 0, none). */
static void put_number(struct sink *s, uint64_t value, bool negative, unsigned base,
                       const struct style *st)
{
    char digits[24];
    int n = 0;
    const char *set = st->upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (value || st->precision != 0)
        do {
            digits[n++] = set[value % base];
            value /= base;
        } while (value);
    int zeros = st->precision > n ? st->precision - n : 0;
    if (st->alt && base == 8 && !zeros && (n == 0 || digits[n - 1] != '0'))
        zeros = 1;                          /* "%#o": a leading 0 */
    const char *prefix = negative ? "-" : st->plus ? "+" : st->space ? " " : "";
    const char *hex = st->alt && base == 16 && n && (n > 1 || digits[0] != '0')
                    ? (st->upper ? "0X" : "0x") : "";
    int length = (int)strlen(prefix) + (int)strlen(hex) + zeros + n;
    bool zero = st->zero && st->precision < 0;  /* (a precision cancels the 0 flag) */
    if (!st->left && !zero)
        put_repeated(s, ' ', st->width - length);
    for (const char *c = prefix; *c; c++)
        put(s, *c);
    for (const char *c = hex; *c; c++)
        put(s, *c);
    if (!st->left && zero)
        put_repeated(s, '0', st->width - length);
    put_repeated(s, '0', zeros);
    while (n > 0)
        put(s, digits[--n]);
    if (st->left)
        put_repeated(s, ' ', st->width - length);
}

#define C format_at(f, 0)
#define NEXT format_next(f)

static void format(struct sink *s, struct format_text *f, va_list ap)
{
    for (; C && !s->bad; NEXT) {
        if (C != '%') {
            put_wide(s, C);
            continue;
        }
        NEXT;
        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; NEXT) {
            if (C == '-')
                left = true;
            else if (C == '0')
                zero = true;
            else if (C == '+')
                plus = true;
            else if (C == ' ')
                space = true;
            else if (C == '#')
                alt = true;
            else
                break;
        }
        int width = 0, precision = -1;
        if (C == '*') {
            width = va_arg(ap, int);
            if (width < 0)
                left = true, width = -width;
            NEXT;
        }
        for (; C >= '0' && C <= '9'; NEXT)
            width = width * 10 + (int)(C - '0');
        if (C == '.') {
            precision = 0;
            NEXT;
            if (C == '*') {
                precision = va_arg(ap, int);
                NEXT;
            }
            for (; C >= '0' && C <= '9'; NEXT)
                precision = precision * 10 + (int)(C - '0');
        }
        bool wide = false;                  /* 64-bit argument */
        int halves = 0;                     /* h: short, hh: char */
        bool long_double = false;           /* L: %Lf and the like */
        bool l = false;                     /* l: %lc and %ls take wide characters (22v) */
        for (; C == 'l' || C == 'z' || C == 'h' || C == 'j' || C == 't' || C == 'L'; NEXT) {
            long_double |= C == 'L';
            wide |= C != 'h' && C != 'L';
            halves += C == 'h';
            l |= C == 'l';
        }
        unsigned conv = C;
        struct style st = { width, precision, left, zero, plus, space, alt, conv == 'X' };
        switch (conv) {
        case 'd':
        case 'i': {
            int64_t v = wide ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (!wide && halves == 1)
                v = (short)v;
            else if (!wide && halves >= 2)
                v = (signed char)v;
            put_number(s, v < 0 ? -(uint64_t)v : (uint64_t)v, v < 0, 10, &st);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v = wide ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            if (!wide && halves == 1)
                v = (unsigned short)v;
            else if (!wide && halves >= 2)
                v = (unsigned char)v;
            unsigned base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
            st.plus = st.space = false;
            put_number(s, v, false, base, &st);
            break;
        }
        case 'p': {                         /* as the kernel prints them: 0x + 16 digits */
            put(s, '0');
            put(s, 'x');
            struct style p16 = { 16, -1, false, true, false, false, false, true };
            put_number(s, (uint64_t)va_arg(ap, void *), false, 16, &p16);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {                         /* (22q: exact, fpconv.c) */
            double v = long_double ? fp_long_double(va_arg(ap, long double)) : va_arg(ap, double);
            char text[1500];                /* (a double's digits: up to some 1100) */
            uint64_t bits;
            memcpy(&bits, &v, 8);
            bool special = (bits >> 52 & 0x7FF) == 0x7FF, nan = special && (bits << 12);
            char sign = !nan && bits >> 63 ? '-' : plus ? '+' : space ? ' ' : 0;
            int n = fp_format(text, v, (char)conv, precision, alt);
            int prefix = (conv | 0x20) == 'a' && !special ? 2 : 0;   /* 0x: zeros go after it */
            int length = n + (sign != 0);
            bool zeros = zero && !left && !special;
            if (!left && !zeros)
                put_repeated(s, ' ', width - length);
            if (sign)
                put(s, sign);
            for (int i = 0; i < prefix; i++)
                put(s, text[i]);
            if (zeros)
                put_repeated(s, '0', width - length);
            for (int i = prefix; i < n; i++)
                put(s, text[i]);
            if (left)
                put_repeated(s, ' ', width - length);
            break;
        }
        case 'c':
            if (!left)
                put_repeated(s, ' ', width - 1);
            if (l)
                put_wide(s, (unsigned)va_arg(ap, wint_t));
            else
                put(s, (char)va_arg(ap, int));
            if (left)
                put_repeated(s, ' ', width - 1);
            break;
        case 's': {
            const char *str = 0;
            const wchar_t *wstr = 0;
            if (l) {
                wstr = va_arg(ap, const wchar_t *);
                if (!wstr)
                    wstr = L"(null)";
            } else {
                str = va_arg(ap, const char *);
                if (!str)
                    str = "(null)";
            }
            int n = 0;
            while ((l ? wstr[n] != 0 : str[n] != 0) && (precision < 0 || n < precision))
                n++;
            if (!left)
                put_repeated(s, ' ', width - n);
            for (int i = 0; i < n; i++)
                if (l)
                    put_wide(s, (unsigned)wstr[i]);
                else
                    put(s, str[i]);
            if (left)
                put_repeated(s, ' ', width - n);
            break;
        }
        case 'n': {                         /* (22v) the characters written so far */
            void *at = va_arg(ap, void *);
            if (wide)
                *(int64_t *)at = (int64_t)s->total;
            else if (halves == 1)
                *(short *)at = (short)s->total;
            else if (halves >= 2)
                *(signed char *)at = (signed char)s->total;
            else
                *(int *)at = (int)s->total;
            break;
        }
        case '%':
            put(s, '%');
            break;
        case 0:
            return;
        default:                            /* unknown: show it as written */
            put(s, '%');
            put_wide(s, conv);
            break;
        }
    }
}

#undef C
#undef NEXT

static int ended(const struct sink *s)
{
    if (s->bad) {
        errno = EILSEQ;
        return -1;
    }
    return (int)s->total;
}

int ritteros_format_bytes(char *buffer, size_t size, const struct format_text *fmt, va_list ap)
{
    struct sink s = { .fd = -1, .buffer = buffer, .size = size };
    struct format_text f = *fmt;
    format(&s, &f, ap);
    if (size)
        buffer[s.total < size ? s.total : size - 1] = 0;
    return ended(&s);
}

int vsnprintf(char *buffer, size_t size, const char *fmt, va_list ap)
{
    struct format_text f = { .n = fmt };
    return ritteros_format_bytes(buffer, size, &f, ap);
}

int vsprintf(char *buffer, const char *fmt, va_list ap)
{
    return vsnprintf(buffer, (size_t)-1 >> 1, fmt, ap);
}

int sprintf(char *buffer, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buffer, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buffer, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buffer, size, fmt, ap);
    va_end(ap);
    return n;
}

int vswprintf(wchar_t *buffer, size_t size, const wchar_t *fmt, va_list ap)
{
    struct sink s = { .fd = -1, .wide = buffer, .size = size };
    struct format_text f = { .w = fmt };
    format(&s, &f, ap);
    if (size)
        buffer[s.total < size ? s.total : size - 1] = 0;
    if (s.total >= size)
        return -1;                          /* (C: negative when it did not fit) */
    return ended(&s);
}

int swprintf(wchar_t *buffer, size_t size, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vswprintf(buffer, size, fmt, ap);
    va_end(ap);
    return n;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    struct sink s = { .fd = fd };
    struct format_text f = { .n = fmt };
    format(&s, &f, ap);
    if (s.bad)
        s.pending = 0;                      /* (a failed printf writes nothing, if it can) */
    flush(&s);
    return ended(&s);
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(STDOUT_FILENO, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s)
{
    size_t n = strlen(s);
    char line[256];
    if (n < sizeof(line)) {                     /* (65e-53) one write: whole beside other threads' */
        memcpy(line, s, n);
        line[n] = '\n';
        return write(STDOUT_FILENO, line, n + 1) == (ssize_t)(n + 1) ? (int)n + 1 : EOF;
    }
    if (write(STDOUT_FILENO, s, n) < 0 || write(STDOUT_FILENO, "\n", 1) < 0)
        return EOF;
    return (int)n + 1;
}

int putchar(int c)
{
    char ch = (char)c;
    return write(STDOUT_FILENO, &ch, 1) == 1 ? (unsigned char)ch : EOF;
}

int putchar_unlocked(int c)
{
    return putchar(c);
}
