/*
 * tr [-c] [-d] [-s] SET1 [SET2] (51e, POSIX): the input with characters
 * changed: those of SET1 into the ones at the same place in SET2 (the last
 * one of SET2 repeated if it is shorter, as BSD), -d deletes SET1's, -s
 * squeezes runs of one character of the last set given into one, -c takes
 * every byte not in SET1 instead (in byte order). Sets: characters, a-z
 * ranges, \n \t \\ \NNN (octal) and the like, [:alpha:] and the other
 * classes, [c*N] (c N times; [c*] in SET2: as often as SET1 needs).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("tr [-c] [-d] [-s] SET1 [SET2]", "change, delete or squeeze characters");

#define SET_MAX 8192

struct set {
    unsigned char c[SET_MAX];
    int n;
    int fill;                           /* place of a [c*] (-1: none) */
};

static bool in_class(const char *name, int c)
{
    bool upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z', digit = c >= '0' && c <= '9';
    if (!strcmp(name, "alpha")) return upper || lower;
    if (!strcmp(name, "upper")) return upper;
    if (!strcmp(name, "lower")) return lower;
    if (!strcmp(name, "digit")) return digit;
    if (!strcmp(name, "alnum")) return upper || lower || digit;
    if (!strcmp(name, "xdigit")) return digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!strcmp(name, "space")) return c == ' ' || (c >= '\t' && c <= '\r');
    if (!strcmp(name, "blank")) return c == ' ' || c == '\t';
    if (!strcmp(name, "punct")) return c > ' ' && c < 127 && !upper && !lower && !digit;
    if (!strcmp(name, "print")) return c >= ' ' && c < 127;
    if (!strcmp(name, "graph")) return c > ' ' && c < 127;
    if (!strcmp(name, "cntrl")) return c < ' ' || c == 127;
    return false;
}

/* One character of a set's text at *s (escapes taken), moving *s on. */
static int one(const char **s)
{
    const char *p = *s;
    int c = (unsigned char)*p++;
    if (c == '\\' && *p) {
        c = (unsigned char)*p++;
        if (c >= '0' && c <= '7') {
            c -= '0';
            for (int k = 0; k < 2 && *p >= '0' && *p <= '7'; k++)
                c = c * 8 + (*p++ - '0');
        } else {
            const char *from = "abfnrtv", *to = "\a\b\f\n\r\t\v";
            const char *e = strchr(from, c);
            if (e && c)
                c = (unsigned char)to[e - from];
        }
    }
    *s = p;
    return c & 0xFF;
}

static bool add(struct set *set, int c)
{
    if (set->n == SET_MAX)
        return false;
    set->c[set->n++] = (unsigned char)c;
    return true;
}

static bool parse(const char *s, struct set *set)
{
    set->n = 0, set->fill = -1;
    while (*s) {
        if (s[0] == '[' && s[1] == ':') {
            const char *end = strstr(s + 2, ":]");
            char name[16];
            if (end && (size_t)(end - s - 2) < sizeof(name)) {
                memcpy(name, s + 2, (size_t)(end - s - 2));
                name[end - s - 2] = 0;
                for (int c = 0; c < 256; c++)
                    if (in_class(name, c) && !add(set, c))
                        return false;
                s = end + 2;
                continue;
            }
        }
        if (s[0] == '[' && s[1]) {              /* [c*N] */
            const char *p = s + 1;
            int c = one(&p);
            if (*p == '*') {
                char *end;
                long n = strtol(p + 1, &end, *(p + 1) == '0' ? 8 : 10);
                if (*end == ']') {
                    if (end == p + 1 || n == 0)
                        set->fill = set->n, n = 0;
                    for (long k = 0; k < n; k++)
                        if (!add(set, c))
                            return false;
                    if (set->fill == set->n)
                        add(set, c);        /* (one now; more at the fill) */
                    s = end + 1;
                    continue;
                }
            }
        }
        int c = one(&s);
        if (s[0] == '-' && s[1]) {
            s++;
            int to = one(&s);
            if (to < c) {
                fprintf(stderr, "tr: range %c-%c in reverse order\n", c, to);
                exit(1);
            }
            for (int k = c; k <= to; k++)
                if (!add(set, k))
                    return false;
            continue;
        }
        if (!add(set, c))
            return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    bool complement = false, del = false, squeeze = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++)
            if (*o == 'c' || *o == 'C')
                complement = true;
            else if (*o == 'd')
                del = true;
            else if (*o == 's')
                squeeze = true;
            else
                goto usage;
    }
    int sets = argc - i;
    if (sets < 1 || sets > 2 || (del && !squeeze && sets != 1) || (!del && !squeeze && sets != 2))
        goto usage;
    static struct set s1, s2;
    if (!parse(argv[i], &s1) || (sets == 2 && !parse(argv[i + 1], &s2))) {
        fprintf(stderr, "tr: set too long\n");
        return 1;
    }
    bool member[256] = { false };
    for (int k = 0; k < s1.n; k++)
        member[s1.c[k]] = true;
    unsigned char from[256];
    int nfrom = 0;
    if (complement) {
        for (int c = 0; c < 256; c++)
            if (!member[c])
                from[nfrom++] = (unsigned char)c;
        for (int c = 0; c < 256; c++)
            member[c] = !member[c];
    } else {
        for (int k = 0; k < s1.n && nfrom < 256; k++)
            from[nfrom++] = s1.c[k];
    }
    int map[256];
    for (int c = 0; c < 256; c++)
        map[c] = c;
    if (sets == 2 && !del) {
        if (s2.fill >= 0 && s2.n < nfrom) {     /* ([c*]: as many as SET1 needs) */
            int extra = nfrom - s2.n;
            memmove(s2.c + s2.fill + extra, s2.c + s2.fill, (size_t)(s2.n - s2.fill));
            memset(s2.c + s2.fill, s2.c[s2.fill + extra], (size_t)extra);
            s2.n += extra;
        }
        if (s2.n == 0) {
            fprintf(stderr, "tr: empty SET2\n");
            return 1;
        }
        for (int k = 0; k < nfrom; k++)         /* (a later place wins, as BSD's) */
            map[from[k]] = s2.c[k < s2.n ? k : s2.n - 1];
    }
    bool squeezed[256] = { false };
    if (squeeze) {
        const struct set *last = sets == 2 ? &s2 : &s1;
        if (sets == 1 && complement)
            for (int c = 0; c < 256; c++)
                squeezed[c] = member[c];
        else
            for (int k = 0; k < last->n; k++)
                squeezed[last->c[k]] = true;
    }
    static unsigned char in[4096], out[4096];
    int previous = -1;
    ssize_t got;
    while ((got = read(STDIN_FILENO, in, sizeof(in))) > 0) {
        size_t n = 0;
        for (ssize_t k = 0; k < got; k++) {
            int c = in[k];
            if (del && member[c])
                continue;
            c = map[c];
            if (squeeze && squeezed[c] && c == previous)
                continue;
            out[n++] = (unsigned char)c;
            previous = c;
        }
        if (n && write(STDOUT_FILENO, out, n) < 0)
            return 1;
    }
    return 0;
usage:
    fprintf(stderr, "usage: tr [-c] [-d] [-s] SET1 [SET2]\n");
    return 1;
}
