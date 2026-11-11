/* Formats that are narrow (printf, scanf) or wide (wprintf, wscanf), step
 * 22v: internal to the programs' library (stdio.c, file.c). */
#ifndef RITTEROS_WIDEFORMAT_H
#define RITTEROS_WIDEFORMAT_H

#include <stdarg.h>
#include <stddef.h>

/* A format being read: n (bytes) or w (wide characters). */
struct format_text {
    const char *n;
    const wchar_t *w;
};

static inline unsigned format_at(const struct format_text *f, size_t i)
{
    return f->w ? (unsigned)f->w[i] : (unsigned char)f->n[i];
}

static inline void format_next(struct format_text *f)
{
    if (f->w)
        f->w++;
    else
        f->n++;
}

/* printf's formatter with either format, into buffer as vsnprintf (bytes:
 * a wide character above 255 is EILSEQ, -1). */
int ritteros_format_bytes(char *buffer, size_t size, const struct format_text *f, va_list ap);

#endif
