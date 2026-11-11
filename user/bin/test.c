/*
 * test EXPRESSION, [ EXPRESSION ] (51d, POSIX's, with XSI's -a -o and
 * parentheses): exit code 0 if the expression is true, 1 if false, 2 on
 * an error. Strings: STRING (not empty), -n, -z, = and !=, < and > (byte
 * order; a common extra). Numbers: -eq -ne -lt -le -gt -ge. Files: -e -f
 * -d -r -w -x -s -b (a disk), -h and -L (a symbolic link, 57a), -c (a
 * character device: /dev/null, a terminal), -p (a pipe) and -S (a
 * socket), -k (sticky, 57b), and as extras -nt and -ot (newer, older); -g
 * is false.
 * -t FD: a terminal. ! inverts. Run as [, the last word must be ].
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("test EXPRESSION", "check files, strings and numbers (exit code 0: true); also [ ... ]");

static char **words;
static int count, at;
static bool failed;

static void error(const char *what, const char *word)
{
    if (!failed)
        fprintf(stderr, "test: %s%s%s\n", word ? word : "", word ? ": " : "", what);
    failed = true;
}

static long long number(const char *s)
{
    char *end;
    errno = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    long long v = strtoll(s, &end, 10);
    while (*end == ' ' || *end == '\t')
        end++;
    if (end == s || *end || errno)
        error("not an integer", s);
    return v;
}

/* May the caller read (4), write (2) or run (1) the file? */
static bool allowed(const struct stat *st, unsigned want)
{
    uid_t uid = geteuid();
    if (uid == 0)
        return want != 1 || (st->st_mode & 0111) || S_ISDIR(st->st_mode);
    unsigned bits = st->st_uid == uid ? st->st_mode >> 6 : st->st_gid == getgid()
                    ? st->st_mode >> 3 : st->st_mode;
    return (bits & want) != 0;
}

static bool unary(const char *op, const char *arg)
{
    struct stat st;
    char c = op[1];
    if (c == 'n')
        return arg[0] != 0;
    if (c == 'z')
        return arg[0] == 0;
    if (c == 't')
        return isatty((int)number(arg));
    if (c == 'h' || c == 'L')
        return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);
    if (c == 'p' || c == 'S' || c == 'c' || c == 'g' || c == 'u' || c == 'k') {
        if (stat(arg, &st) != 0)
            return false;
        return c == 'u' ? (st.st_mode & S_ISUID) != 0 : c == 'p' ? S_ISFIFO(st.st_mode)
             : c == 'S' ? S_ISSOCK(st.st_mode) : c == 'c' ? S_ISCHR(st.st_mode)
             : c == 'k' ? (st.st_mode & S_ISVTX) != 0 : false;
    }
    if (stat(arg, &st) != 0)
        return false;
    switch (c) {
    case 'e': return true;
    case 'f': return S_ISREG(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 'b': return S_ISBLK(st.st_mode);
    case 's': return st.st_size > 0;
    case 'r': return allowed(&st, 4);
    case 'w': return allowed(&st, 2);
    case 'x': return allowed(&st, 1);
    }
    error("unknown operator", op);
    return false;
}

static bool is_unary(const char *w)
{
    return w[0] == '-' && w[1] && !w[2] && strchr("bcdefghknprstuwxzLS", w[1]);
}

static bool is_binary(const char *w)
{
    static const char *ops[] = { "=", "!=", "==", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt",
                                 "-ge", "-nt", "-ot" };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (!strcmp(w, ops[i]))
            return true;
    return false;
}

static bool binary(const char *a, const char *op, const char *b)
{
    if (!strcmp(op, "=") || !strcmp(op, "=="))
        return strcmp(a, b) == 0;
    if (!strcmp(op, "!="))
        return strcmp(a, b) != 0;
    if (!strcmp(op, "<"))
        return strcmp(a, b) < 0;
    if (!strcmp(op, ">"))
        return strcmp(a, b) > 0;
    if (!strcmp(op, "-nt") || !strcmp(op, "-ot")) {
        struct stat sa, sb;
        bool ha = stat(a, &sa) == 0, hb = stat(b, &sb) == 0;
        if (op[1] == 'n')
            return ha && (!hb || sa.st_mtime > sb.st_mtime);
        return hb && (!ha || sa.st_mtime < sb.st_mtime);
    }
    long long x = number(a), y = number(b);
    switch (op[1] * 256 + op[2]) {
    case 'e' * 256 + 'q': return x == y;
    case 'n' * 256 + 'e': return x != y;
    case 'l' * 256 + 't': return x < y;
    case 'l' * 256 + 'e': return x <= y;
    case 'g' * 256 + 't': return x > y;
    default:              return x >= y;
    }
}

static bool expression(void);

/* primary: ( expression ) | unary-op word | word binary-op word | word */
static bool primary(void)
{
    if (at >= count) {
        error("an argument is missing", 0);
        return false;
    }
    const char *w = words[at];
    if (at + 2 < count && is_binary(words[at + 1])) {
        at += 3;
        return binary(w, words[at - 2], words[at - 1]);
    }
    if (!strcmp(w, "(") ) {
        at++;
        bool v = expression();
        if (at >= count || strcmp(words[at], ")"))
            error("a ')' is missing", 0);
        at++;
        return v;
    }
    if (is_unary(w) && at + 1 < count) {
        at += 2;
        return unary(w, words[at - 1]);
    }
    at++;
    return w[0] != 0;                               /* a string alone: not empty */
}

static bool negation(void)
{
    if (at < count && !strcmp(words[at], "!") && at + 1 < count) {
        at++;
        return !negation();
    }
    return primary();
}

static bool conjunction(void)
{
    bool v = negation();
    while (at < count && !strcmp(words[at], "-a")) {
        at++;
        bool w = negation();
        v = v && w;
    }
    return v;
}

static bool expression(void)
{
    bool v = conjunction();
    while (at < count && !strcmp(words[at], "-o")) {
        at++;
        bool w = conjunction();
        v = v || w;
    }
    return v;
}

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    words = argv + 1;
    count = argc - 1;
    if (!strcmp(name, "[")) {
        if (count == 0 || strcmp(words[count - 1], "]")) {
            fprintf(stderr, "[: a ']' is missing\n");
            return 2;
        }
        count--;
    }
    if (count == 0)
        return 1;                                   /* (nothing: false) */
    /* POSIX's rules by the number of words, before the general grammar. */
    bool v;
    if (count == 1) {
        v = words[0][0] != 0;
    } else if (count == 2 && !strcmp(words[0], "!")) {
        v = words[1][0] == 0;
    } else if (count == 2 && is_unary(words[0])) {
        v = unary(words[0], words[1]);
    } else if (count == 3 && is_binary(words[1])) {
        v = binary(words[0], words[1], words[2]);
    } else if (count == 3 && !strcmp(words[0], "!")) {
        at = 1;
        v = !expression();
    } else {
        v = expression();
        if (at < count)
            error("too many arguments", words[at]);
    }
    return failed ? 2 : v ? 0 : 1;
}
