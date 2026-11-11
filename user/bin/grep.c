/*
 * grep [-EFcilnqrsvx] [-e PATTERN]... [-f FILE]... [PATTERN] [FILE...]:
 * print the lines of the files (or of the standard input) that match
 * PATTERN, a basic regular expression (regex.h, since 58a: . [set] * ^ $
 * \( \) \{m,n\} \1 and the rest of POSIX's), or with -E an extended one
 * (| + ? ( ) {m,n}), or with -F a plain string. (68f) -e and -f give the
 * patterns instead (several: a line matching any is selected; a pattern
 * with line breaks is one per line; -f FILE one per line of it), -x
 * matches whole lines only, -q prints nothing and ends 0 at the first
 * selected line, -s says nothing of files missing or unreadable. -i: any
 * case; -v: the lines that do not match; -n: with line numbers; -c: only a
 * count per file; -l: only the names of files with a match; -r: folders
 * too, everything below them (the current folder when no file is given).
 * With several files each line starts with its file's name. A file with a
 * NUL byte is binary: "binary file matches" instead of its lines. Exit
 * status 0 if a line was selected, 1 if none, 2 on an error (as on Unix).
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

#define USAGE "grep [-EFcilnqrsvx] [-e PATTERN]... [-f FILE]... [PATTERN] [FILE...]"
PROGRAM_HELP("grep [-EFcilnqrsvx] [-e PATTERN]... [-f FILE]... [PATTERN] [FILE...]",
             "print the lines that match a pattern");     /* (its usage: USAGE too) */

#define MAX_ITEMS 256
#define PATH_LENGTH 512

/* (68f) The patterns: a regular expression each (58a: regex.h, BRE or with
 * -E ERE), or with -F (and for an empty one) its text compared as it is. */
struct pattern {
    char *text;
    size_t length;
    bool plain;
    regex_t re;
};
static struct pattern *patterns;
static int pattern_count;
static bool extended, fixed, any_case, invert, numbers, count_only, names_only, recursive,
            with_names, whole_lines, quiet, silent;
static int status = 1;                          /* 1 until a line is selected */

/* Output through a buffer of its own: a write per line would be a system
 * call per line. */
static char out[65536];
static size_t out_n;

static void flush_out(void)
{
    for (size_t at = 0; at < out_n;) {
        ssize_t w = write(STDOUT_FILENO, out + at, out_n - at);
        if (w <= 0)
            break;
        at += (size_t)w;
    }
    out_n = 0;
}

static void put(const void *data, size_t n)
{
    const char *d = data;
    while (n) {
        if (out_n == sizeof(out))
            flush_out();
        size_t k = sizeof(out) - out_n < n ? sizeof(out) - out_n : n;
        memcpy(out + out_n, d, k);
        out_n += k, d += k, n -= k;
    }
}

static void put_text(const char *t)
{
    put(t, strlen(t));
}

static void put_number(long v)
{
    char b[24];
    put(b, (size_t)snprintf(b, sizeof(b), "%ld", v));
}

/* An error: what was printed so far first, so the lines stay in order.
 * (68f) A file's (missing, unreadable) is not said with -s. */
#define complain(...) (flush_out(), dprintf(STDERR_FILENO, __VA_ARGS__))
#define complain_file(...) (silent ? 0 : complain(__VA_ARGS__))

static bool same_bytes(const unsigned char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (any_case ? tolower(a[i]) != tolower((unsigned char)b[i]) : a[i] != (unsigned char)b[i])
            return false;
    return true;
}

/* Does a plain pattern match the n bytes at s (with -x: all of them)? */
static bool plain_matches(const struct pattern *p, const unsigned char *s, size_t n)
{
    if (whole_lines)
        return n == p->length && same_bytes(s, p->text, n);
    for (size_t at = 0; at + p->length <= n; at++)
        if (same_bytes(s + at, p->text, p->length))
            return true;
    return false;
}

/* Does a regular expression match the text at s, n bytes long and ending
 * in a zero (with -x: all of it; of the matches that start first regexec
 * gives the longest, so one from 0 to n is there if any is)? */
static bool re_matches(const struct pattern *p, const char *s, size_t n, int flags)
{
    regmatch_t m;                               /* (only with -x: else REG_NOSUB) */
    if (regexec(&p->re, s, whole_lines ? 1 : 0, &m, flags) != 0)
        return false;
    return !whole_lines || (m.rm_so == 0 && (size_t)m.rm_eo == n);
}

/* Does the line (n bytes at s; s[n] may be written) match any pattern? A
 * line with zero bytes (a binary file) is searched by the regular
 * expressions piece by piece between them. */
static bool line_matches(unsigned char *s, size_t n)
{
    unsigned char saved = s[n];
    s[n] = 0;
    bool found = false;
    for (int k = 0; k < pattern_count && !found; k++) {
        const struct pattern *p = &patterns[k];
        if (p->plain) {
            found = plain_matches(p, s, n);
            continue;
        }
        for (size_t at = 0; at <= n && !found;) {
            size_t piece = strlen((const char *)s + at);
            found = re_matches(p, (const char *)s + at, piece, at ? REG_NOTBOL : 0);
            at += piece + 1;
        }
    }
    s[n] = saved;
    return found;
}

/* One file (fd), named name: its selected lines, count or name. */
static void search(int fd, const char *name)
{
    size_t cap = 4096, have = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) {
        complain("grep: %s: out of memory\n", name);
        status = 2;
        return;
    }
    long line = 0, selected = 0;
    bool binary = false, ended = false;
    while (!ended || have) {
        unsigned char *nl = have ? memchr(buf, '\n', have) : 0;
        if (!nl && !ended) {                    /* more of the line first */
            if (have + 1 >= cap) {              /* (and room for a zero after it) */
                unsigned char *bigger = realloc(buf, cap * 2);
                if (!bigger) {
                    complain("grep: %s: a line too long\n", name);
                    status = 2;
                    break;
                }
                buf = bigger, cap *= 2;
            }
            ssize_t r = read(fd, buf + have, cap - have);
            if (r < 0) {
                complain_file("grep: %s: %s\n", name, strerror(errno));
                status = 2;
                break;
            }
            if (r == 0)
                ended = true;
            have += (size_t)r;
            continue;
        }
        size_t n = nl ? (size_t)(nl - buf) : have;
        line++;
        if (!binary && memchr(buf, 0, n))
            binary = true;
        if (line_matches(buf, n) != invert) {
            selected++;
            status = status == 2 ? 2 : 0;
            if (quiet) {                        /* (68f) the answer is known: 0 */
                flush_out();
                exit(0);
            }
            if (names_only)
                break;
            if (!count_only && !binary) {
                if (with_names) {
                    put_text(name);
                    put(":", 1);
                }
                if (numbers) {
                    put_number(line);
                    put(":", 1);
                }
                put(buf, n);
                put("\n", 1);
            }
        }
        size_t used = nl ? n + 1 : n;
        memmove(buf, buf + used, have - used);
        have -= used;
    }
    free(buf);
    if (names_only && selected) {
        put_text(name);
        put("\n", 1);
    } else if (count_only) {
        if (with_names) {
            put_text(name);
            put(":", 1);
        }
        put_number(selected);
        put("\n", 1);
    } else if (binary && selected) {
        put_text("grep: ");
        put_text(name);
        put_text(": binary file matches\n");
    }
}

static void search_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        complain_file("grep: %s: %s\n", path, strerror(errno));
        status = 2;
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            complain_file("grep: %s: is a folder (-r searches it)\n", path);
            status = 2;
            return;
        }
        DIR *dir = opendir(path);
        if (!dir) {
            complain_file("grep: %s: %s\n", path, strerror(errno));
            status = 2;
            return;
        }
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char full[PATH_LENGTH];
            snprintf(full, sizeof(full), "%s%s%s", path,
                     path[strlen(path) - 1] == '/' ? "" : "/", e->d_name);
            search_path(full);
        }
        closedir(dir);
        return;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        complain_file("grep: %s: %s\n", path, strerror(errno));
        status = 2;
        return;
    }
    search(fd, path);
    close(fd);
}

/* (68f) Patterns from text: one per line of it (a last line break ends
 * the last one, it starts no empty one). */
static bool add_patterns(const char *text, size_t n)
{
    for (size_t at = 0; at < n || (n == 0 && at == 0);) {
        const char *nl = memchr(text + at, '\n', n - at);
        size_t len = nl ? (size_t)(nl - (text + at)) : n - at;
        struct pattern *more = realloc(patterns, (size_t)(pattern_count + 1) * sizeof(*more));
        char *copy = malloc(len + 1);
        if (!more || !copy) {
            complain("grep: out of memory\n");
            return false;
        }
        patterns = more;
        memcpy(copy, text + at, len);
        copy[len] = 0;
        patterns[pattern_count++] = (struct pattern){ .text = copy, .length = len };
        at += len + 1;
        if (n == 0)
            break;
    }
    return true;
}

/* -f FILE: its lines; an empty file gives none (so nothing matches). */
static bool add_pattern_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        complain("grep: %s: %s\n", path, strerror(errno));
        return false;
    }
    size_t cap = 4096, n = 0;
    char *text = malloc(cap);
    for (;;) {
        if (!text) {
            complain("grep: %s: out of memory\n", path);
            close(fd);
            return false;
        }
        ssize_t r = read(fd, text + n, cap - n);
        if (r < 0) {
            complain("grep: %s: %s\n", path, strerror(errno));
            free(text);
            close(fd);
            return false;
        }
        if (r == 0)
            break;
        n += (size_t)r;
        if (n == cap) {
            char *bigger = realloc(text, cap * 2);
            if (!bigger)
                free(text);
            text = bigger, cap *= 2;
        }
    }
    close(fd);
    bool ok = n == 0 || add_patterns(text, n);
    free(text);
    return ok;
}

static int usage(void)
{
    complain("usage: " USAGE "\n");
    return 2;
}

int main(int argc, char **argv)
{
    int i = 1;
    bool given = false;                         /* (68f) patterns from -e or -f */
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'e' || *o == 'f') {       /* their argument: the rest, else the next */
                const char *arg = o[1] ? o + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!arg) {
                    complain("grep: -%c needs an argument\n", *o);
                    return usage();
                }
                if (!(*o == 'e' ? add_patterns(arg, strlen(arg)) : add_pattern_file(arg)))
                    return 2;
                given = true;
                break;
            }
            switch (*o) {
            case 'i': any_case = true; break;
            case 'E': extended = true; break;
            case 'F': fixed = true; break;
            case 'v': invert = true; break;
            case 'n': numbers = true; break;
            case 'c': count_only = true; break;
            case 'l': names_only = true; break;
            case 'r': recursive = true; break;
            case 'x': whole_lines = true; break;
            case 'q': quiet = true; break;
            case 's': silent = true; break;
            default:
                complain("grep: unknown option -%c\n", *o);
                return usage();
            }
        }
    }
    if (!given) {
        if (i >= argc)
            return usage();
        if (!add_patterns(argv[i], strlen(argv[i])))
            return 2;
        i++;
    }
    for (int k = 0; k < pattern_count; k++) {
        struct pattern *p = &patterns[k];
        p->plain = fixed || p->length == 0;     /* (an empty one matches every line) */
        if (p->plain)
            continue;
        int error = regcomp(&p->re, p->text, (extended ? REG_EXTENDED : 0) |
                                             (any_case ? REG_ICASE : 0) |
                                             (whole_lines ? 0 : REG_NOSUB));
        if (error) {
            char why[100];
            regerror(error, &p->re, why, sizeof(why));
            complain("grep: %s: %s\n", p->text, why);
            return 2;
        }
    }
    if (i == argc && !recursive) {
        search(STDIN_FILENO, "(standard input)");
    } else {
        with_names = recursive || argc - i > 1;
        if (i == argc)
            search_path(".");
        for (; i < argc; i++)
            search_path(argv[i]);
    }
    flush_out();
    return status;
}
