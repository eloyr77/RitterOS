/*
 * sort [-bcfnru] [-h] [-V] [-t C] [-k F[.C][,F[.C]][OPTS]]... [-o FILE]
 * [FILE]... (51e, POSIX; -h and -V are common extras): the lines of the
 * files (or the input) in order. Keys: fields (-t C between them, else
 * runs of blanks before each), -k F,G from field F to G; with none, the
 * whole line. -n numbers, -h sizes with K M G T P E, -V versions (numbers
 * inside compared as numbers), -f capitals as small letters, -b blanks
 * before a key ignored, -r reversed, -u equal ones once, -c only check
 * (exit code 1 if not sorted). Lines equal by their keys keep the order of
 * the whole line, as POSIX says.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"

PROGRAM_HELP("sort [-bcfhnruV] [-t C] [-k F[.C][,G[.C]][bfhnrV]]... [-o FILE] [FILE]...",
             "the lines in order");

struct key {
    int f1, c1, f2, c2;                 /* fields and characters (1-based; f2 0: to the end) */
    bool n, h, V, f, b, r;
};

static struct key keys[16];
static int key_count;
static struct key global;
static char sep;                        /* -t, 0: blanks */
static bool unique, check;

static bool blank(char c)
{
    return c == ' ' || c == '\t';
}

/* Where field f (1-based) of line begins; char c within it. */
static const char *field_at(const char *line, int f, int c, bool skip_blanks)
{
    const char *p = line;
    for (int k = 1; k < f && *p; k++) {
        if (sep) {
            while (*p && *p != sep)
                p++;
            if (*p)
                p++;
        } else {
            while (*p && blank(*p))
                p++;
            while (*p && !blank(*p))
                p++;
        }
    }
    if (skip_blanks)
        while (*p && blank(*p))
            p++;
    for (int k = 1; k < c && *p; k++)
        p++;
    return p;
}

static const char *field_end(const char *line, int f, int c)
{
    if (!f)
        return line + strlen(line);
    const char *p = field_at(line, f, 1, false);
    if (c)
        return field_at(line, f, c + 1, false);
    if (sep) {
        while (*p && *p != sep)
            p++;
    } else {
        while (*p && blank(*p))
            p++;
        while (*p && !blank(*p))
            p++;
    }
    return p;
}

static long double value(const char *s, const char *e, bool human)
{
    while (s < e && blank(*s))
        s++;
    char buf[64];
    size_t n = (size_t)(e - s) < sizeof(buf) - 1 ? (size_t)(e - s) : sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    char *end;
    double v = strtod(buf, &end);
    if (end == buf)
        return 0;
    if (human && *end) {
        const char *units = "KMGTPE";
        const char *u = strchr(units, *end >= 'a' ? *end - 32 : *end);
        for (int k = 0; u && k <= u - units; k++)
            v *= 1024;
    }
    return v;
}

/* Numbers in text compared as numbers, the rest as characters. */
static int version(const char *a, const char *ae, const char *b, const char *be)
{
    while (a < ae && b < be) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (a < ae && *a == '0')
                a++;
            while (b < be && *b == '0')
                b++;
            const char *as = a, *bs = b;
            while (a < ae && *a >= '0' && *a <= '9')
                a++;
            while (b < be && *b >= '0' && *b <= '9')
                b++;
            if (a - as != b - bs)
                return (a - as) < (b - bs) ? -1 : 1;
            int c = strncmp(as, bs, (size_t)(a - as));
            if (c)
                return c;
            continue;
        }
        if (*a != *b)
            return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        a++, b++;
    }
    return (a < ae) - (b < be);
}

static int compare_key(const struct key *k, const char *x, const char *y)
{
    const char *xs = field_at(x, k->f1, k->c1, k->b), *xe = field_end(x, k->f2, k->c2);
    const char *ys = field_at(y, k->f1, k->c1, k->b), *ye = field_end(y, k->f2, k->c2);
    if (xe < xs)
        xe = xs;
    if (ye < ys)
        ye = ys;
    int c = 0;
    if (k->n || k->h) {
        long double a = value(xs, xe, k->h), b = value(ys, ye, k->h);
        c = a < b ? -1 : a > b;
    } else if (k->V) {
        c = version(xs, xe, ys, ye);
    } else {
        for (; xs < xe && ys < ye; xs++, ys++) {
            unsigned char a = (unsigned char)*xs, b = (unsigned char)*ys;
            if (k->f) {
                a = a >= 'a' && a <= 'z' ? a - 32 : a;
                b = b >= 'a' && b <= 'z' ? b - 32 : b;
            }
            if (a != b) {
                c = a < b ? -1 : 1;
                break;
            }
        }
        if (!c)
            c = (xs < xe) - (ys < ye);
    }
    return k->r ? -c : c;
}

static int compare_keys(const char *x, const char *y)
{
    for (int i = 0; i < key_count; i++) {
        int c = compare_key(&keys[i], x, y);
        if (c)
            return c;
    }
    if (!key_count) {
        struct key whole = global;
        whole.f1 = 1, whole.c1 = 1, whole.f2 = 0, whole.c2 = 0;
        int c = compare_key(&whole, x, y);
        if (c)
            return c;
    }
    return 0;
}

struct line {
    char *s;
    size_t at;                                  /* its place in the input */
};

static int compare(const void *a, const void *b)
{
    const struct line *x = a, *y = b;
    int c = compare_keys(x->s, y->s);
    if (c)
        return c;
    if (!unique) {
        c = strcmp(x->s, y->s);                 /* (equal keys: the whole line) */
        if (c)
            return global.r ? -c : c;
    }
    return x->at < y->at ? -1 : x->at > y->at;  /* (then the input's order: -u keeps the first) */
}

static void parse_key(const char *s)
{
    struct key *k = &keys[key_count++];
    *k = global;
    k->f1 = (int)strtol(s, (char **)&s, 10);
    k->c1 = *s == '.' ? (int)strtol(s + 1, (char **)&s, 10) : 1;
    for (; *s && *s != ','; s++)
        switch (*s) {
        case 'n': k->n = true; break;
        case 'r': k->r = true; break;
        case 'f': k->f = true; break;
        case 'b': k->b = true; break;
        case 'h': k->h = true; break;
        case 'V': k->V = true; break;
        }
    k->f2 = 0, k->c2 = 0;
    if (*s == ',') {
        s++;
        k->f2 = (int)strtol(s, (char **)&s, 10);
        if (*s == '.')
            k->c2 = (int)strtol(s + 1, (char **)&s, 10);
        for (; *s; s++)
            switch (*s) {
            case 'n': k->n = true; break;
            case 'r': k->r = true; break;
            case 'f': k->f = true; break;
            case 'b': k->b = true; break;
            case 'h': k->h = true; break;
            case 'V': k->V = true; break;
            }
    }
    if (k->f1 < 1)
        k->f1 = 1;
}

/* A whole line (without its break) from f, malloc'd, or 0 at the end. */
static char *line_of(FILE *f)
{
    size_t size = 128, n = 0;
    char *s = malloc(size);
    int c;
    while (s && (c = fgetc(f)) != EOF && c != '\n') {
        if (n + 1 == size) {
            char *bigger = realloc(s, size *= 2);
            if (!bigger) {
                free(s);
                return 0;
            }
            s = bigger;
        }
        s[n++] = (char)c;
    }
    if (!s || (c == EOF && n == 0)) {
        free(s);
        return 0;
    }
    s[n] = 0;
    return s;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IOFBF, BUFSIZ);         /* (a write per buffer, not per byte) */
    const char *output = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'n': global.n = true; break;
            case 'r': global.r = true; break;
            case 'f': global.f = true; break;
            case 'b': global.b = true; break;
            case 'h': global.h = true; break;
            case 'V': global.V = true; break;
            case 'u': unique = true; break;
            case 'c': check = true; break;
            case 't': case 'k': case 'o': {
                const char *v = o[1] ? o + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!v || key_count == 16)
                    goto usage;
                if (*o == 't')
                    sep = v[0];
                else if (*o == 'k')
                    parse_key(v);
                else
                    output = v;
                o = v + strlen(v) - 1;
                break;
            }
            default:
                goto usage;
            }
        }
    }
    for (int k = 0; k < key_count; k++) {       /* (global options for keys without their own) */
        struct key *K = &keys[k];
        if (!K->n && !K->h && !K->V && !K->f && !K->b && !K->r) {
            int f1 = K->f1, c1 = K->c1, f2 = K->f2, c2 = K->c2;
            *K = global;
            K->f1 = f1, K->c1 = c1, K->f2 = f2, K->c2 = c2;
        }
    }
    size_t max = 1024, count = 0;
    struct line *lines = malloc(max * sizeof(struct line));
    int status = 0;
    for (int k = i; lines && (k < argc || (k == i && i == argc)); k++) {
        FILE *f = k < argc && strcmp(argv[k], "-") ? fopen(argv[k], "r") : stdin;
        if (!f) {
            perror(argv[k]);
            status = 2;
            continue;
        }
        char *s;
        while ((s = line_of(f))) {
            if (count == max) {
                struct line *bigger = realloc(lines, (max *= 2) * sizeof(struct line));
                if (!bigger) {
                    fprintf(stderr, "sort: out of memory\n");
                    return 2;
                }
                lines = bigger;
            }
            lines[count].s = s;
            lines[count].at = count;
            count++;
        }
        if (f != stdin)
            fclose(f);
    }
    if (!lines)
        return 2;
    if (check) {
        for (size_t k = 1; k < count; k++)
            if (compare_keys(lines[k - 1].s, lines[k].s) > 0 ||
                (!unique && !compare_keys(lines[k - 1].s, lines[k].s) &&
                 (global.r ? -1 : 1) * strcmp(lines[k - 1].s, lines[k].s) > 0) ||
                (unique && !compare_keys(lines[k - 1].s, lines[k].s))) {
                fprintf(stderr, "sort: disorder: %s\n", lines[k].s);
                return 1;
            }
        return 0;
    }
    qsort(lines, count, sizeof(struct line), compare);
    FILE *out = output ? fopen(output, "w") : stdout;
    if (!out) {
        perror(output);
        return 2;
    }
    for (size_t k = 0; k < count; k++) {
        if (unique && k > 0 && compare_keys(lines[k - 1].s, lines[k].s) == 0)
            continue;
        fprintf(out, "%s\n", lines[k].s);
    }
    if (out != stdout)
        fclose(out);
    return status;
usage:
    fprintf(stderr, "usage: sort [-nruhVfbc] [-t C] [-k F,G] [-o FILE] [FILE]...\n");
    return 2;
}
