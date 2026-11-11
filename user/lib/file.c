/*
 * C's streams (22m): FILE over a descriptor with a buffer of BUFSIZ bytes,
 * either for reading (filled by read, handed out) or for writing (filled,
 * written when full, at fflush, fseek and fclose). One direction at a time:
 * switching flushes or drops what the buffer held, so a stream opened with
 * "+" reads and writes as C allows (with a seek or flush between). stdin is
 * buffered for reading; stdout and stderr write at once, keeping their order
 * with printf, which writes at once too. Streams are on a list for
 * fflush(0) and for exit. Also sscanf, and temporary files (22n: in
 * ~/.tmp, the user's own; a tmpfile is removed at fclose and at exit).
 * Wide characters on streams (22v): one byte each (the "C" locale), and
 * wprintf/wscanf through the same formatter and scanner as the bytes'.
 * (65e-53) Streams are safe between threads, as POSIX asks: see "Locks".
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include "ritteros.h"
#include "fpconv.h"
#include "wideformat.h"
#include <ctype.h>

#define READING 1
#define WRITING 2

struct ritteros_file {
    int fd;
    bool can_read, can_write, append, unbuffered, eof, error;
    int mode;                           /* what the buffer holds: 0, READING or WRITING */
    unsigned char buf[BUFSIZ];
    size_t at, len;                     /* reading: buf[at..len); writing: buf[0..len) */
    int pushed;                         /* a byte given back by ungetc, or -1 */
    bool line_buffered;                 /* _IOLBF: written at every line break (22q) */
    char *remove_at_close;              /* a tmpfile's name */
    int orientation;                    /* fwide's: 1 wide, -1 bytes, 0 neither yet (22v) */
    int lock_state;                     /* (65e-53) 0 free, 1 held, 2 held and waited for */
    unsigned lock_count;                /* the holder's holds (flockfile nests) */
    void *lock_owner;                   /* the holder's this_thread */
    int pins;                           /* fflush(0)s waiting for it: not freed (list's lock) */
    bool unlinked, left;                /* fclose took it off the list; ended, freed by the last pin */
    struct ritteros_file *next;
};

static struct ritteros_file std_in = { .fd = 0, .can_read = true, .pushed = -1 };
static struct ritteros_file std_out = { .fd = 1, .can_write = true, .unbuffered = true, .pushed = -1 };
static struct ritteros_file std_err = { .fd = 2, .can_write = true, .unbuffered = true, .pushed = -1 };
static struct ritteros_file *open_files;

FILE *ritteros_stdin(void) { return &std_in; }
FILE *ritteros_stdout(void) { return &std_out; }
FILE *ritteros_stderr(void) { return &std_err; }

/*
 * Locks (65e-53). Each stream has one: every function on a stream holds
 * it for the whole call (a line fputs or fprintf writes stays whole, two
 * threads never change one buffer at once), and flockfile holds it across
 * several (it nests: the functions inside take it again; the _unlocked
 * ones take none). It is threads.c's futex mutex, its owner named by the
 * address of the thread's this_thread. Until the program makes its first
 * thread the functions take no lock (nobody else could hold one);
 * flockfile always does. The list of streams has a lock of its own, held
 * briefly: fdopen and fclose put a stream on and off it (fclose before it
 * holds the stream). Nothing waits for a stream holding the list's lock:
 * fflush(0) flushes the free ones under it and, for one another thread
 * holds, pins it (fclose leaves its freeing to the last pin), gives the
 * list back and waits (glibc and musl wait under the list's lock: a
 * thread holding that stream and calling fopen waited for them, for
 * good). A thread that holds a stream itself waits for no other in
 * fflush(0), it passes the busy ones (two holding one each and flushing
 * all would wait for each other); `streams_held` counts its own. exit
 * flushes holding only the list (a thread waiting in fgets holds its
 * stream to the end), and fork holds the list across the call; the child
 * frees every lock but its own thread's.
 */
extern int __ritteros_threads_made;             /* (syscalls.c: at the first thread) */
static _Thread_local char this_thread;
static _Thread_local unsigned streams_held;
static int list_lock;

static void word_lock(int *w)
{
    int c = 0;
    if (__atomic_compare_exchange_n(w, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    if (c != 2)
        c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        syscall6(SYS_FUTEX, (int64_t)w, FUTEX_WAIT, 2, 0, 0, 0);
        c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    }
}

static void word_unlock(int *w)
{
    if (__atomic_exchange_n(w, 0, __ATOMIC_RELEASE) == 2)
        syscall6(SYS_FUTEX, (int64_t)w, FUTEX_WAKE, 1, 0, 0, 0);
}

void flockfile(FILE *f)
{
    if (__atomic_load_n(&f->lock_owner, __ATOMIC_RELAXED) == &this_thread) {
        f->lock_count++;
        return;
    }
    word_lock(&f->lock_state);
    __atomic_store_n(&f->lock_owner, &this_thread, __ATOMIC_RELAXED);
    f->lock_count = 1;
    streams_held++;
}

int ftrylockfile(FILE *f)
{
    if (__atomic_load_n(&f->lock_owner, __ATOMIC_RELAXED) == &this_thread) {
        f->lock_count++;
        return 0;
    }
    int c = 0;
    if (!__atomic_compare_exchange_n(&f->lock_state, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return -1;
    __atomic_store_n(&f->lock_owner, &this_thread, __ATOMIC_RELAXED);
    f->lock_count = 1;
    streams_held++;
    return 0;
}

void funlockfile(FILE *f)
{
    if (--f->lock_count > 0)
        return;
    streams_held--;
    __atomic_store_n(&f->lock_owner, 0, __ATOMIC_RELAXED);
    word_unlock(&f->lock_state);
}

/* A function's own hold: none while the program has one thread. */
static bool hold(FILE *f)
{
    if (!__atomic_load_n(&__ritteros_threads_made, __ATOMIC_ACQUIRE))
        return false;
    flockfile(f);
    return true;
}

static void release(FILE *f, bool held)
{
    if (held)
        funlockfile(f);
}

/* fork's (fork.c): the list held across the call; in the child every
 * stream's lock but this thread's starts free (the others are gone). */
void __ritteros_stdio_lock(void)
{
    word_lock(&list_lock);
}

void __ritteros_stdio_unlock(void)
{
    word_unlock(&list_lock);
}

static void child_frees(FILE *f)
{
    if (f->lock_owner == &this_thread) {
        f->lock_state = 1;                      /* (its waiters are gone) */
    } else {
        f->lock_state = 0;
        f->lock_owner = 0;
        f->lock_count = 0;
    }
}

void __ritteros_stdio_child(void)
{
    for (FILE *p = open_files; p; p = p->next)
        child_frees(p);
    child_frees(&std_in);
    child_frees(&std_out);
    child_frees(&std_err);
    list_lock = 0;
}

static bool write_all(int fd, const unsigned char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return false;
        p += w, n -= (size_t)w;
    }
    return true;
}

/* Empty the buffer: written out, or (reading) the unread part given back by
 * moving the descriptor's position back to where the program is. */
static int drain(FILE *f)
{
    int r = 0;
    if (f->mode == WRITING && f->len) {
        if (!write_all(f->fd, f->buf, f->len))
            f->error = true, r = EOF;
    } else if (f->mode == READING) {
        off_t back = (off_t)(f->len - f->at) + (f->pushed >= 0);
        if (back)
            lseek(f->fd, -back, SEEK_CUR);
    }
    f->at = f->len = 0;
    f->pushed = -1;
    f->mode = 0;
    return r;
}

FILE *fdopen(int fd, const char *mode)
{
    FILE *f = calloc(1, sizeof(*f));
    if (!f) {
        errno = ENOMEM;
        return 0;
    }
    f->fd = fd;
    f->pushed = -1;
    bool plus = strchr(mode, '+') != 0;
    f->can_read = mode[0] == 'r' || plus;
    f->can_write = mode[0] != 'r' || plus;
    f->append = mode[0] == 'a';
    word_lock(&list_lock);
    f->next = open_files;
    open_files = f;
    word_unlock(&list_lock);
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    int flags;
    bool plus = strchr(mode, '+') != 0;
    switch (mode[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default: errno = EINVAL; return 0;
    }
    if (mode[0] == 'w' && strchr(mode, 'x'))
        flags |= O_EXCL;                        /* (52j) C11's "wx": only a new file */
    int fd = open(path, flags, 0666);
    if (fd < 0)
        return 0;
    FILE *f = fdopen(fd, mode);
    if (!f)
        close(fd);
    return f;
}

int fclose(FILE *f)
{
    bool std = f == &std_in || f == &std_out || f == &std_err;
    if (!std) {
        word_lock(&list_lock);                  /* (off the list first: see "Locks") */
        for (FILE **p = &open_files; *p; p = &(*p)->next)
            if (*p == f) {
                *p = f->next;
                break;
            }
        f->unlinked = true;
        word_unlock(&list_lock);
    }
    bool held = hold(f);
    int r = drain(f);
    release(f, held);
    if (std)
        return r;
    if (f->lock_owner == &this_thread)
        streams_held--;                         /* (closed while its flockfile held it) */
    if (close(f->fd) < 0)
        r = EOF;
    if (f->remove_at_close) {
        unlink(f->remove_at_close);
        free(f->remove_at_close);
    }
    word_lock(&list_lock);
    bool pinned = f->pins > 0;
    f->left = pinned;
    word_unlock(&list_lock);
    if (!pinned)
        free(f);
    return r;
}

static int flush_held(FILE *f)
{
    bool held = hold(f);
    int r = f->mode == WRITING ? drain(f) : 0;
    release(f, held);
    return r;
}

/* fflush(0)'s look at one stream: flushed if free (or this thread's);
 * false when another thread holds it */
static bool flush_if_free(FILE *f, int *r)
{
    bool threads = __atomic_load_n(&__ritteros_threads_made, __ATOMIC_ACQUIRE);
    if (threads && ftrylockfile(f) != 0)
        return false;
    if (f->mode == WRITING && drain(f))
        *r = EOF;
    if (threads)
        funlockfile(f);
    return true;
}

int fflush(FILE *f)
{
    if (f)
        return flush_held(f);
    int r = 0;
    bool may_wait = streams_held == 0;          /* (see "Locks") */
    word_lock(&list_lock);
    for (FILE *p = open_files; p;) {
        if (flush_if_free(p, &r) || !may_wait) {
            p = p->next;
            continue;
        }
        p->pins++;                              /* (busy: waited for without the list) */
        word_unlock(&list_lock);
        if (flush_held(p))
            r = EOF;
        word_lock(&list_lock);
        p->pins--;
        if (!p->unlinked) {
            p = p->next;
            continue;
        }
        if (p->left && p->pins == 0)
            free(p);                            /* (fclose ended meanwhile) */
        p = open_files;                         /* (its place is gone: from the start) */
    }
    word_unlock(&list_lock);
    /* (51e) stdout and stderr are not on the list: buffered by setvbuf,
     * exit's flush missed them and the output was lost. */
    if (!flush_if_free(&std_out, &r) && may_wait && flush_held(&std_out))
        r = EOF;
    if (!flush_if_free(&std_err, &r) && may_wait && flush_held(&std_err))
        r = EOF;
    return r;
}

/* Exit flushes every stream (called by exit), holding none of them: see
 * "Locks". A tmpfile goes with the program. */
void ritteros_flush_streams(void)
{
    word_lock(&list_lock);
    for (FILE *p = open_files; p; p = p->next) {
        if (p->mode == WRITING)
            drain(p);
        if (p->remove_at_close)
            unlink(p->remove_at_close);
    }
    word_unlock(&list_lock);
    if (std_out.mode == WRITING)
        drain(&std_out);
    if (std_err.mode == WRITING)
        drain(&std_err);
}

/* --- Temporary files --- */

/* The user's ~/.tmp into dir (made if missing), or false. */
static bool tmp_folder(char *dir, size_t size)
{
    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_dir || (size_t)snprintf(dir, size, "%s/.tmp", pw->pw_dir) >= size) {
        errno = ENOENT;
        return false;
    }
    struct stat st;
    if (stat(dir, &st) < 0 && mkdir(dir, 0700) < 0)
        return false;
    return true;
}

/* Replace the six characters at x by random letters and digits. */
static void randomize(char *x)
{
    static const char set[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned char r[6];
    ritteros_random(r, sizeof(r));
    for (int i = 0; i < 6; i++)
        x[i] = set[r[i] % (sizeof(set) - 1)];
}

/* A name in the folder no file has: tries a few random ones. */
static bool fresh_name(char *name, size_t len)
{
    struct stat st;
    for (int tries = 0; tries < 100; tries++) {
        randomize(name + len - 6);
        if (stat(name, &st) < 0)
            return true;
    }
    errno = EEXIST;
    return false;
}

char *tmpnam(char *s)
{
    static char own[L_tmpnam];
    char *out = s ? s : own;
    char dir[L_tmpnam - 16];
    if (!tmp_folder(dir, sizeof(dir)))
        return 0;
    int n = snprintf(out, L_tmpnam, "%s/tmp-XXXXXX", dir);
    return fresh_name(out, (size_t)n) ? out : 0;
}

int mkstemp(char *name)
{
    size_t len = strlen(name);
    if (len < 6 || strcmp(name + len - 6, "XXXXXX")) {
        errno = EINVAL;
        return -1;
    }
    for (int tries = 0; tries < 100; tries++) {     /* (52j: made exclusively) */
        randomize(name + len - 6);
        int fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    return -1;
}

FILE *tmpfile(void)
{
    char name[L_tmpnam];
    if (!tmpnam(name))
        return 0;
    FILE *f = fopen(name, "w+b");
    if (!f)
        return 0;
    f->remove_at_close = strdup(name);
    if (!f->remove_at_close) {
        fclose(f);
        unlink(name);
        errno = ENOMEM;
        return 0;
    }
    return f;
}

static bool start_reading(FILE *f)
{
    if (!f->can_read) {
        f->error = true;
        errno = EBADF;
        return false;
    }
    if (f->mode == WRITING)
        drain(f);
    f->mode = READING;
    return true;
}

static bool start_writing(FILE *f)
{
    if (!f->can_write) {
        f->error = true;
        errno = EBADF;
        return false;
    }
    if (f->mode == READING)
        drain(f);
    if (f->append && f->mode != WRITING)
        lseek(f->fd, 0, SEEK_END);
    f->mode = WRITING;
    return true;
}

int getc_unlocked(FILE *f)
{
    if (!start_reading(f))
        return EOF;
    if (f->pushed >= 0) {
        int c = f->pushed;
        f->pushed = -1;
        return c;
    }
    if (f->at == f->len) {
        if (f->eof)
            return EOF;
        ssize_t r = read(f->fd, f->buf, sizeof(f->buf));
        if (r <= 0) {
            r < 0 ? (void)(f->error = true) : (void)(f->eof = true);
            return EOF;
        }
        f->at = 0, f->len = (size_t)r;
    }
    return f->buf[f->at++];
}

int fgetc(FILE *f)
{
    bool held = hold(f);
    int c = getc_unlocked(f);
    release(f, held);
    return c;
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(&std_in); }
int getchar_unlocked(void) { return getc_unlocked(&std_in); }

static int unget(int c, FILE *f)
{
    if (c == EOF || !start_reading(f) || f->pushed >= 0)
        return EOF;
    f->pushed = (unsigned char)c;
    f->eof = false;
    return (unsigned char)c;
}

int ungetc(int c, FILE *f)
{
    bool held = hold(f);
    int r = unget(c, f);
    release(f, held);
    return r;
}

static size_t read_items(void *buf, size_t size, size_t count, FILE *f)
{
    size_t want = size * count, got = 0;
    if (!want || !start_reading(f))
        return 0;
    unsigned char *out = buf;
    if (f->pushed >= 0)
        out[got++] = (unsigned char)f->pushed, f->pushed = -1;
    while (got < want) {
        if (f->at < f->len) {                   /* what the buffer holds */
            size_t n = f->len - f->at < want - got ? f->len - f->at : want - got;
            memcpy(out + got, f->buf + f->at, n);
            f->at += n, got += n;
            continue;
        }
        if (f->eof)
            break;
        if (want - got >= sizeof(f->buf)) {     /* large reads straight into the caller's */
            ssize_t r = read(f->fd, out + got, want - got);
            if (r <= 0) {
                r < 0 ? (void)(f->error = true) : (void)(f->eof = true);
                break;
            }
            got += (size_t)r;
            continue;
        }
        ssize_t r = read(f->fd, f->buf, sizeof(f->buf));
        if (r <= 0) {
            r < 0 ? (void)(f->error = true) : (void)(f->eof = true);
            break;
        }
        f->at = 0, f->len = (size_t)r;
    }
    return got / size;
}

size_t fread(void *buf, size_t size, size_t count, FILE *f)
{
    bool held = hold(f);
    size_t n = read_items(buf, size, count, f);
    release(f, held);
    return n;
}

char *fgets(char *s, int size, FILE *f)
{
    int n = 0;
    bool held = hold(f);
    while (n < size - 1) {
        int c = getc_unlocked(f);
        if (c == EOF)
            break;
        s[n++] = (char)c;
        if (c == '\n')
            break;
    }
    release(f, held);
    if (n == 0 || size <= 0)
        return 0;
    s[n] = 0;
    return s;
}

static size_t write_items(const void *buf, size_t size, size_t count, FILE *f)
{
    size_t total = size * count;
    if (!total || !start_writing(f))
        return 0;
    const unsigned char *p = buf;
    if (f->unbuffered || total >= sizeof(f->buf)) {
        if (drain(f) || !write_all(f->fd, p, total)) {
            f->error = true;
            return 0;
        }
        f->mode = WRITING;
        return count;
    }
    if (f->len + total > sizeof(f->buf) && drain(f))
        return 0;
    f->mode = WRITING;
    memcpy(f->buf + f->len, p, total);
    f->len += total;
    if (f->line_buffered && memchr(p, '\n', total) && drain(f))
        return 0;
    return count;
}

size_t fwrite(const void *buf, size_t size, size_t count, FILE *f)
{
    bool held = hold(f);
    size_t n = write_items(buf, size, count, f);
    release(f, held);
    return n;
}

int putc_unlocked(int c, FILE *f)
{
    unsigned char b = (unsigned char)c;
    return write_items(&b, 1, 1, f) == 1 ? b : EOF;
}

int fputc(int c, FILE *f)
{
    bool held = hold(f);
    int r = putc_unlocked(c, f);
    release(f, held);
    return r;
}

int putc(int c, FILE *f) { return fputc(c, f); }

int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);
    return n == 0 || fwrite(s, 1, n, f) == n ? 0 : EOF;    /* (until 63e: == 1, EOF for most) */
}

static int seek(FILE *f, long offset, int whence)
{
    if (drain(f) < 0)
        return -1;
    if (lseek(f->fd, offset, whence) < 0)
        return -1;
    f->eof = false;
    return 0;
}

int fseek(FILE *f, long offset, int whence)
{
    bool held = hold(f);
    int r = seek(f, offset, whence);
    release(f, held);
    return r;
}

long ftell(FILE *f)
{
    bool held = hold(f);
    off_t at = lseek(f->fd, 0, SEEK_CUR);
    if (at >= 0 && f->mode == READING)
        at -= (off_t)(f->len - f->at) + (f->pushed >= 0);
    else if (at >= 0 && f->mode == WRITING)
        at += (off_t)f->len;
    release(f, held);
    return at < 0 ? -1 : (long)at;
}

void rewind(FILE *f)
{
    bool held = hold(f);
    seek(f, 0, SEEK_SET);
    f->error = false;
    release(f, held);
}

int feof(FILE *f)
{
    bool held = hold(f);
    int r = f->eof && f->pushed < 0;
    release(f, held);
    return r;
}

int ferror(FILE *f)
{
    bool held = hold(f);
    int r = f->error;
    release(f, held);
    return r;
}

void clearerr(FILE *f)
{
    bool held = hold(f);
    f->eof = f->error = false;
    release(f, held);
}

int fileno(FILE *f) { return f->fd; }

static int print_stream(FILE *f, const struct format_text *format, va_list ap)
{
    char small[512];
    va_list again;
    va_copy(again, ap);
    int n = ritteros_format_bytes(small, sizeof(small), format, ap);
    if (n < 0) {
        va_end(again);
        return n;
    }
    char *text = small;
    if ((size_t)n >= sizeof(small)) {           /* a long one: on the heap */
        text = malloc((size_t)n + 1);
        if (!text) {
            va_end(again);
            errno = ENOMEM;
            return -1;
        }
        ritteros_format_bytes(text, (size_t)n + 1, format, again);
    }
    va_end(again);
    size_t done = n ? fwrite(text, (size_t)n, 1, f) : 1;
    if (text != small)
        free(text);
    return done == 1 ? n : -1;
}

int vfprintf(FILE *f, const char *format, va_list ap)
{
    struct format_text t = { .n = format };
    return print_stream(f, &t, ap);
}

int vfwprintf(FILE *f, const wchar_t *format, va_list ap)
{
    struct format_text t = { .w = format };
    bool held = hold(f);
    if (!f->orientation)
        f->orientation = 1;
    int n = print_stream(f, &t, ap);
    release(f, held);
    return n;
}

int fwprintf(FILE *f, const wchar_t *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vfwprintf(f, format, ap);
    va_end(ap);
    return n;
}

int vwprintf(const wchar_t *format, va_list ap)
{
    return vfwprintf(&std_out, format, ap);
}

int wprintf(const wchar_t *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vfwprintf(&std_out, format, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vfprintf(f, format, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *format, va_list ap)
{
    return vfprintf(&std_out, format, ap);
}

int remove(const char *path)
{
    if (unlink(path) == 0)
        return 0;
    int first = errno;
    if (rmdir(path) == 0)
        return 0;
    errno = first;
    return -1;
}

/* --- scanf (22m; streams and floating point since 22q) --- */

/* Where scanf reads: a string, or a stream (one byte of look-ahead, given
 * back with ungetc at the end, as C allows). */
struct reader {
    const char *s;
    const wchar_t *w;                   /* (22v) wide text, for swscanf */
    FILE *f;
    int ahead;                          /* f: a byte read, not yet taken (-2: none) */
    long taken;                         /* bytes taken (%n) */
};

static int peek(struct reader *r)
{
    if (r->w)
        return *r->w ? (int)(*r->w & 0x7FFFFFFF) : EOF;
    if (!r->f)
        return *r->s ? (unsigned char)*r->s : EOF;
    if (r->ahead == -2)
        r->ahead = getc_unlocked(r->f);       /* (scan_stream holds the stream) */
    return r->ahead;
}

/* The byte after the next one, for a string (a stream cannot tell: -2). */
static int peek2(struct reader *r)
{
    if (r->f)
        return -2;
    if (r->w)
        return *r->w && r->w[1] ? (int)(r->w[1] & 0x7FFFFFFF) : EOF;
    return *r->s && r->s[1] ? (unsigned char)r->s[1] : EOF;
}

static void take(struct reader *r)
{
    if (r->w) {
        if (*r->w)
            r->w++;
    } else if (!r->f) {
        if (*r->s)
            r->s++;
    } else {
        r->ahead = -2;
    }
    r->taken++;
}

static bool space(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static int digit_in(int c, int base)
{
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10
          : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
    return d < base ? d : -1;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* A floating-point number's text into buf (at most width bytes, 0: any):
 * the longest start of one, as strtod reads them (decimal, hexadecimal,
 * inf, infinity, nan). Its length. */
static int float_text(struct reader *r, int width, char *buf, int size)
{
    int n = 0, limit = width ? width : size - 1;
    if (limit > size - 1)
        limit = size - 1;
#define ROOM (n < limit)
#define TAKE (buf[n++] = (char)peek(r), take(r))
    if (ROOM && (peek(r) == '+' || peek(r) == '-'))
        TAKE;
    int c = lower(peek(r));
    if (c == 'i' || c == 'n') {
        const char *word = c == 'i' ? "infinity" : "nan";
        for (int k = 0; word[k] && ROOM && lower(peek(r)) == word[k]; k++)
            TAKE;
        if (c == 'n' && ROOM && peek(r) == '(') {
            TAKE;
            while (ROOM && (isalnum(peek(r)) || peek(r) == '_'))
                TAKE;
            if (ROOM && peek(r) == ')')
                TAKE;
        }
        buf[n] = 0;
        return n;
    }
    int base = 10;
    if (ROOM && peek(r) == '0') {
        TAKE;
        int x = lower(peek(r)), after = peek2(r);
        if (ROOM && x == 'x' && (after == -2 || digit_in(after, 16) >= 0 || after == '.')) {
            TAKE;
            base = 16;
        }
    }
    while (ROOM && digit_in(peek(r), base) >= 0)
        TAKE;
    if (ROOM && peek(r) == '.') {
        TAKE;
        while (ROOM && digit_in(peek(r), base) >= 0)
            TAKE;
    }
    int e = lower(peek(r)), next = peek2(r);
    if (ROOM && e == (base == 16 ? 'p' : 'e') &&
        (next == -2 || (next >= '0' && next <= '9') || next == '+' || next == '-')) {
        TAKE;
        if (ROOM && (peek(r) == '+' || peek(r) == '-'))
            TAKE;
        while (ROOM && peek(r) >= '0' && peek(r) <= '9')
            TAKE;
    }
#undef ROOM
#undef TAKE
    buf[n] = 0;
    return n;
}

/* Is c in the scan set at f (just after "[" and a "^")? */
static bool in_set(struct format_text f, int c)
{
    for (size_t i = 0; format_at(&f, i) && (i == 0 || format_at(&f, i) != ']'); i++) {
        unsigned a = format_at(&f, i);
        if (format_at(&f, i + 1) == '-' && format_at(&f, i + 2) && format_at(&f, i + 2) != ']') {
            if ((unsigned)c >= a && (unsigned)c <= format_at(&f, i + 2))
                return true;
            i += 2;
        } else if ((unsigned)c == a) {
            return true;
        }
    }
    return false;
}

#define C format_at(f, 0)
#define NEXT format_next(f)

static int scan(struct reader *r, struct format_text *f, va_list ap)
{
    int assigned = 0;
    bool converted = false;                     /* a conversion was done (EOF otherwise) */
    for (; C; NEXT) {
        if (C < 256 && space((int)C)) {
            while (space(peek(r)))
                take(r);
            continue;
        }
        if (C != '%' || format_at(f, 1) == '%') {
            if (C == '%') {
                NEXT;
                while (space(peek(r)))
                    take(r);
            }
            if (peek(r) != (int)C) {
                if (peek(r) == EOF && !converted)
                    return EOF;
                break;
            }
            take(r);
            continue;
        }
        NEXT;
        bool skip = C == '*';
        if (skip)
            NEXT;
        int width = 0;
        for (; C >= '0' && C <= '9'; NEXT)
            width = width * 10 + (int)(C - '0');
        int size = 0;                           /* -2 hh, -1 h, 0 int, 1 l, 2 ll; 3 L */
        for (; C == 'h' || C == 'l' || C == 'z' || C == 'j' || C == 't' || C == 'L'; NEXT)
            size += C == 'h' ? -1 : C == 'L' ? 3 : 1;
        char conv = C < 128 ? (char)C : '?';
        bool wide_out = size == 1;              /* (22v) %lc, %ls, %l[: wide characters */
        if (conv == 'n') {
            if (!skip)
                *va_arg(ap, int *) = (int)r->taken;
            continue;
        }
        if (conv != 'c' && conv != '[')
            while (space(peek(r)))
                take(r);
        if (peek(r) == EOF)
            return converted || assigned ? assigned : EOF;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' ||
            conv == 'o' || conv == 'p') {
            int base = conv == 'x' || conv == 'X' || conv == 'p' ? 16 : conv == 'o' ? 8 : 10;
            int left = width ? width : -1;
            bool negative = false, digits = false;
            if (left && (peek(r) == '-' || peek(r) == '+')) {
                negative = peek(r) == '-';
                take(r), left--;
            }
            if ((conv == 'i' || base == 16) && left && peek(r) == '0') {
                take(r), left--;
                digits = true;                  /* (a 0 already) */
                int after = peek2(r);
                if (left && lower(peek(r)) == 'x' && (after == -2 || digit_in(after, 16) >= 0)) {
                    take(r), left--;
                    base = 16;
                    digits = false;
                } else if (conv == 'i') {
                    base = 8;
                }
            }
            uint64_t v = 0;
            for (; left && digit_in(peek(r), base) >= 0; left--) {
                v = v * (unsigned)base + (unsigned)digit_in(peek(r), base);
                take(r);
                digits = true;
            }
            if (!digits)
                break;                          /* no number: a matching failure */
            converted = true;
            if (skip)
                continue;
            int64_t sv = negative ? -(int64_t)v : (int64_t)v;
            bool is_signed = conv == 'd' || conv == 'i';
            if (conv == 'p')
                *va_arg(ap, void **) = (void *)(uintptr_t)sv;
            else if (size <= -2)
                *va_arg(ap, char *) = (char)sv;
            else if (size == -1)
                *va_arg(ap, short *) = (short)sv;
            else if (size == 0)
                is_signed ? (void)(*va_arg(ap, int *) = (int)sv)
                          : (void)(*va_arg(ap, unsigned *) = (unsigned)sv);
            else
                *va_arg(ap, int64_t *) = sv;
            assigned++;
        } else if (conv && strchr("eEfFgGaA", conv)) {
            char text[1100], *end;
            int n = float_text(r, width, text, sizeof(text));
            if (n == 0)
                break;
            int saved = errno;
            double d = size == 0 ? 0 : strtod(text, &end);
            float fv = size == 0 ? strtof(text, &end) : 0;
            errno = saved;
            if (end != text + n)
                break;                          /* "1e+" and the like: not a number */
            converted = true;
            if (skip)
                continue;
            if (size == 0)
                *va_arg(ap, float *) = fv;
            else if (size >= 3)
                *va_arg(ap, long double *) = fp_to_long_double(d);
            else
                *va_arg(ap, double *) = d;
            assigned++;
        } else if (conv == 'c') {
            int n = width ? width : 1;
            void *out = skip ? 0 : va_arg(ap, void *);
            for (int i = 0; i < n; i++) {
                if (peek(r) == EOF)
                    return assigned || converted ? assigned : EOF;
                if (!wide_out && peek(r) > 255)
                    return assigned;            /* (no byte for it: EILSEQ) */
                if (out && wide_out)
                    ((wchar_t *)out)[i] = (wchar_t)peek(r);
                else if (out)
                    ((char *)out)[i] = (char)peek(r);
                take(r);
            }
            converted = true;
            assigned += !skip;
        } else if (conv == 's' || conv == '[') {
            bool invert = false;
            struct format_text set = *f;
            if (conv == '[') {
                NEXT;
                if (C == '^')
                    invert = true, NEXT;
                set = *f;                       /* (a ']' first is one of the set) */
                if (C == ']')
                    NEXT;
                while (C && C != ']')
                    NEXT;
                if (!C)
                    break;
            }
            void *out = skip ? 0 : va_arg(ap, void *);
            int n = 0;
            for (int c = peek(r); c != EOF && (!width || n < width) &&
                                  (wide_out || c <= 255) &&
                                  (conv == 's' ? !space(c) : in_set(set, c) != invert);
                 c = peek(r)) {
                if (out && wide_out)
                    ((wchar_t *)out)[n] = (wchar_t)c;
                else if (out)
                    ((char *)out)[n] = (char)c;
                n++;
                take(r);
            }
            if (n == 0)
                break;
            if (out && wide_out)
                ((wchar_t *)out)[n] = 0;
            else if (out)
                ((char *)out)[n] = 0;
            converted = true;
            assigned += !skip;
        } else {
            break;                              /* a conversion not known here */
        }
    }
    return assigned;
}

#undef C
#undef NEXT

static int scan_stream(FILE *f, struct format_text *fmt, va_list ap)
{
    struct reader r = { .f = f, .ahead = -2 };
    bool held = hold(f);
    int n = EOF;
    if (start_reading(f)) {
        n = scan(&r, fmt, ap);
        if (r.ahead >= 0)
            unget(r.ahead, f);                  /* (the one byte looked at) */
    }
    release(f, held);
    return n;
}

int vsscanf(const char *text, const char *fmt, va_list ap)
{
    struct reader r = { .s = text, .ahead = -2 };
    struct format_text t = { .n = fmt };
    return scan(&r, &t, ap);
}

int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    struct format_text t = { .n = fmt };
    return scan_stream(f, &t, ap);
}

int vscanf(const char *fmt, va_list ap)
{
    return vfscanf(&std_in, fmt, ap);
}

int vswscanf(const wchar_t *text, const wchar_t *fmt, va_list ap)
{
    struct reader r = { .w = text, .ahead = -2 };
    struct format_text t = { .w = fmt };
    return scan(&r, &t, ap);
}

int vfwscanf(FILE *f, const wchar_t *fmt, va_list ap)
{
    struct format_text t = { .w = fmt };
    bool held = hold(f);
    if (!f->orientation)
        f->orientation = 1;
    int n = scan_stream(f, &t, ap);
    release(f, held);
    return n;
}

int vwscanf(const wchar_t *fmt, va_list ap)
{
    return vfwscanf(&std_in, fmt, ap);
}

int swscanf(const wchar_t *text, const wchar_t *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vswscanf(text, format, ap);
    va_end(ap);
    return n;
}

int fwscanf(FILE *f, const wchar_t *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vfwscanf(f, format, ap);
    va_end(ap);
    return n;
}

int wscanf(const wchar_t *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vfwscanf(&std_in, format, ap);
    va_end(ap);
    return n;
}

/* --- wide characters on streams (22v): a byte each --- */

int fwide(FILE *f, int mode)
{
    bool held = hold(f);
    if (!f->orientation && mode)
        f->orientation = mode > 0 ? 1 : -1;
    int r = f->orientation;
    release(f, held);
    return r;
}

wint_t fgetwc(FILE *f)
{
    bool held = hold(f);
    if (!f->orientation)
        f->orientation = 1;
    int c = getc_unlocked(f);
    release(f, held);
    return c == EOF ? WEOF : (wint_t)c;
}

wint_t getwc(FILE *f) { return fgetwc(f); }
wint_t getwchar(void) { return fgetwc(&std_in); }

wchar_t *fgetws(wchar_t *s, int size, FILE *f)
{
    int n = 0;
    bool held = hold(f);
    while (n < size - 1) {
        wint_t c = fgetwc(f);
        if (c == WEOF)
            break;
        s[n++] = (wchar_t)c;
        if (c == L'\n')
            break;
    }
    release(f, held);
    if (n == 0 || size <= 0)
        return 0;
    s[n] = 0;
    return s;
}

wint_t ungetwc(wint_t wc, FILE *f)
{
    if (wc == WEOF || (unsigned)wc > 255)
        return WEOF;
    return ungetc((int)wc, f) == EOF ? WEOF : wc;
}

wint_t fputwc(wchar_t wc, FILE *f)
{
    bool held = hold(f);
    wint_t r = (wint_t)wc;
    if (!f->orientation)
        f->orientation = 1;
    if ((unsigned)wc > 255) {
        errno = EILSEQ;
        f->error = true;
        r = WEOF;
    } else if (putc_unlocked((int)wc, f) == EOF) {
        r = WEOF;
    }
    release(f, held);
    return r;
}

wint_t putwc(wchar_t wc, FILE *f) { return fputwc(wc, f); }
wint_t putwchar(wchar_t wc) { return fputwc(wc, &std_out); }

int fputws(const wchar_t *s, FILE *f)
{
    bool held = hold(f);
    int r = 0;
    for (; *s && !r; s++)
        if (fputwc(*s, f) == WEOF)
            r = -1;
    release(f, held);
    return r;
}

int sscanf(const char *text, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsscanf(text, format, ap);
    va_end(ap);
    return n;
}

int fscanf(FILE *f, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vfscanf(f, format, ap);
    va_end(ap);
    return n;
}

int scanf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vfscanf(&std_in, format, ap);
    va_end(ap);
    return n;
}

/* --- The rest of stdio.h (22q) --- */

void perror(const char *s)
{
    const char *text = strerror(errno);
    if (s && *s)
        fprintf(&std_err, "%s: %s\n", s, text);
    else
        fprintf(&std_err, "%s\n", text);
}

/* The mode only: the stream keeps its own buffer of BUFSIZ bytes (C lets
 * an implementation not use the caller's). */
int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    (void)buf, (void)size;
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)
        return -1;
    bool held = hold(f);
    int r = drain(f) ? -1 : 0;
    if (!r) {
        f->unbuffered = mode == _IONBF;
        f->line_buffered = mode == _IOLBF;
    }
    release(f, held);
    return r;
}

void setbuf(FILE *f, char *buf)
{
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    int flags;
    bool plus = strchr(mode, '+') != 0;
    switch (mode[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default: errno = EINVAL; return 0;
    }
    bool held = hold(f);
    drain(f);
    /* The old file goes first, so the new one takes its descriptor (the
     * lowest free one): stdout reopened is descriptor 1 again. */
    close(f->fd);
    int fd = path ? open(path, flags, 0666) : -1;
    if (fd < 0) {
        if (!path)
            errno = EINVAL;                     /* (changing only the mode: not here) */
        release(f, held);
        return 0;
    }
    f->fd = fd;
    f->can_read = mode[0] == 'r' || plus;
    f->can_write = mode[0] != 'r' || plus;
    f->append = mode[0] == 'a';
    f->eof = f->error = false;
    f->mode = 0;
    f->at = f->len = 0;
    f->pushed = -1;
    release(f, held);
    return f;
}

int fgetpos(FILE *f, fpos_t *pos)
{
    long at = ftell(f);
    if (at < 0)
        return -1;
    *pos = at;
    return 0;
}

int fsetpos(FILE *f, const fpos_t *pos)
{
    return fseek(f, *pos, SEEK_SET);
}
