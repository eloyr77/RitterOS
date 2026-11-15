/*
 * err.h (43a): BSD's messages: "PROGRAM: TEXT: ERROR'S WORDS" on the
 * standard error (warn, vwarn: errno's words; warnx, vwarnx: none); err,
 * errx and their v forms then end the program with status.
 */
#ifndef RITTEROS_ERR_H
#define RITTEROS_ERR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

void warn(const char *format, ...) __attribute__((format(printf, 1, 2)));
void vwarn(const char *format, va_list ap);
void warnx(const char *format, ...) __attribute__((format(printf, 1, 2)));
void vwarnx(const char *format, va_list ap);
__attribute__((noreturn)) void err(int status, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
__attribute__((noreturn)) void verr(int status, const char *format, va_list ap);
__attribute__((noreturn)) void errx(int status, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
__attribute__((noreturn)) void verrx(int status, const char *format, va_list ap);

#ifdef __cplusplus
}
#endif

#endif
