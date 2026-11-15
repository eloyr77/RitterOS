/*
 * iconv.h (43a-2): converting text between encodings: UTF-8, UTF-16,
 * UTF-32 (UCS-2, UCS-4; LE, BE, or with a byte order mark), WCHAR_T,
 * ASCII, ISO-8859-1 and Windows-1252 (names without regard to case, '-'
 * or '_'). iconv_open(to, from): a converter, or (iconv_t)-1 with errno
 * EINVAL for an encoding it does not know ("//TRANSLIT" after to: '?' for
 * a character the target has not; "//IGNORE": such characters left out).
 * iconv converts *in (*in_left bytes) into *out (*out_left bytes room),
 * moving both: the characters converted irreversibly (TRANSLIT's), or -1
 * with errno E2BIG (out is full), EILSEQ (a character it cannot convert:
 * *in stays at it) or EINVAL (*in ends inside a character). in 0: back to
 * the initial state.
 */
#ifndef RITTEROS_ICONV_H
#define RITTEROS_ICONV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef struct ritteros_iconv *iconv_t;

iconv_t iconv_open(const char *to, const char *from);
size_t iconv(iconv_t cd, char **in, size_t *in_left, char **out, size_t *out_left);
int iconv_close(iconv_t cd);

#ifdef __cplusplus
}
#endif

#endif
