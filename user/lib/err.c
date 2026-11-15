/*
 * (43a) err.h: messages with the program's name. Each is formatted whole
 * and written once, so lines from several threads stay whole.
 */
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void message(const char *format, va_list ap, int error)
{
    char line[1024];
    int n = snprintf(line, sizeof(line), "%s: ", getprogname());
    if (format && n < (int)sizeof(line))
        n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
    if (error >= 0 && n < (int)sizeof(line))
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%s", format ? ": " : "", strerror(error));
    if (n >= (int)sizeof(line))
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    write(STDERR_FILENO, line, (size_t)n);
}

void vwarn(const char *format, va_list ap) { message(format, ap, errno); }
void vwarnx(const char *format, va_list ap) { message(format, ap, -1); }

void warn(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vwarn(format, ap);
    va_end(ap);
}

void warnx(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vwarnx(format, ap);
    va_end(ap);
}

void verr(int status, const char *format, va_list ap)
{
    vwarn(format, ap);
    exit(status);
}

void verrx(int status, const char *format, va_list ap)
{
    vwarnx(format, ap);
    exit(status);
}

void err(int status, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    verr(status, format, ap);
}

void errx(int status, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    verrx(status, format, ap);
}
