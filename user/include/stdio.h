/*
 * Formatted output and streams. printf's conversions: %d %i %u %x %X %o %c
 * %s %p %%, and since 22q %f %F %e %E %g %G %a %A exactly (every digit of a
 * double's value, rounded to nearest with ties to even; L for long double,
 * printed at double's precision); the length modifiers hh h l ll z j t L,
 * the flags - 0 + # and space, a width and a precision, both also as '*'.
 */
#ifndef RITTEROS_STDIO_H
#define RITTEROS_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

int printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char *format, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buffer, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buffer, size_t size, const char *format, va_list ap);
int vdprintf(int fd, const char *format, va_list ap);
/* (22q) Into buffer without a size: snprintf is the safe one. */
int sprintf(char *buffer, const char *format, ...) __attribute__((format(printf, 2, 3)));
int vsprintf(char *buffer, const char *format, va_list ap);
int puts(const char *s);                /* s and a newline */

/* Move or rename a file or directory (to must not exist); 0, or -1 with
 * errno set. */
int rename(const char *from, const char *to);
int putchar(int c);

/* Streams (22m): files through a buffer, as C's FILE. stdin, stdout and
 * stderr are the descriptors 0, 1 and 2 (stdout and stderr not buffered, so
 * they keep their order with printf). Modes "r", "w", "a", with "+" for
 * both ways; "b" is accepted and changes nothing (files are bytes). The
 * stream functions return EOF or 0 on failure as C's do, errno set. */
typedef struct ritteros_file FILE;
FILE *ritteros_stdin(void);
FILE *ritteros_stdout(void);
FILE *ritteros_stderr(void);
#define stdin  (ritteros_stdin())
#define stdout (ritteros_stdout())
#define stderr (ritteros_stderr())

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ   4096

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
/* (58c) A command run by sh -c, as a stream: "r" reads its output, "w"
 * writes its input; pclose waits for it and gives its status (as wait's:
 * WEXITSTATUS). At most 32 open at once. */
FILE *popen(const char *command, const char *mode);
int pclose(FILE *stream);
int fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int fflush(FILE *f);                    /* 0: every stream */
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f);
char *fgets(char *s, int size, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int fileno(FILE *f);
int fprintf(FILE *f, const char *format, ...) __attribute__((format(printf, 2, 3)));
int vfprintf(FILE *f, const char *format, va_list ap);
int vprintf(const char *format, va_list ap);
/* Remove a file or an empty folder. */
int remove(const char *path);

/* Temporary files (22n), in the user's own ~/.tmp (made when first needed,
 * 0700). tmpnam: a name no file has (into s, L_tmpnam bytes, or a buffer of
 * its own for s = 0; 0 when no home folder); tmpfile: such a file, open for
 * reading and writing, removed again at fclose or exit. */
#define L_tmpnam 256
#define TMP_MAX  1000000
char *tmpnam(char *s);
FILE *tmpfile(void);

/* Reading numbers and words (22m; from streams and with floating point
 * since 22q): %d %i %u %x %X %o %p (with hh, h, l, ll), %e %f %g %a and
 * their capitals (float; l: double, L: long double), %c, %s, %[set], %n,
 * %%, widths and %*... (assigned to nothing); the number of fields
 * assigned, or EOF if the input ended before the first. A stream gives
 * back one byte at the end (what scanf looked at and did not take). */
int sscanf(const char *text, const char *format, ...) __attribute__((format(scanf, 2, 3)));
int vsscanf(const char *text, const char *format, va_list ap);
int scanf(const char *format, ...) __attribute__((format(scanf, 1, 2)));
int fscanf(FILE *f, const char *format, ...) __attribute__((format(scanf, 2, 3)));
int vscanf(const char *format, va_list ap);
int vfscanf(FILE *f, const char *format, va_list ap);

/* (22q) "s: the error's text" on stderr (errno's; without s, only the text). */
void perror(const char *s);
/* (22q) How a stream writes: _IOFBF when its buffer is full, _IOLBF at every
 * line break too, _IONBF at once (the buffer stays the stream's own). */
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setbuf(FILE *f, char *buf);
/* (22q) The stream on another file (stdout on a file: descriptor 1 again). */
FILE *freopen(const char *path, const char *mode, FILE *f);
/* (22q) A place in a stream, to come back to. */
typedef long fpos_t;
int fgetpos(FILE *f, fpos_t *pos);
int fsetpos(FILE *f, const fpos_t *pos);

/* (65e-53) Every stream function holds the stream's lock for its call;
 * flockfile holds it across several (nesting), ftrylockfile only if free
 * (0, else nonzero). The _unlocked functions take no lock: between
 * flockfile and funlockfile. */
void flockfile(FILE *f);
int ftrylockfile(FILE *f);
void funlockfile(FILE *f);
int getc_unlocked(FILE *f);
int getchar_unlocked(void);
int putc_unlocked(int c, FILE *f);
int putchar_unlocked(int c);

#endif
