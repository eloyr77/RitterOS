/*
 * (43a) fnmatch: shell patterns, matched byte by byte (the C locale).
 * A '*' tries every length from the shortest, which is quick enough for
 * names (a pattern with many stars and a long name could take longer).
 */
#include <ctype.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <string.h>

static int fold(int c, int flags)
{
    return (flags & FNM_CASEFOLD) ? tolower(c) : c;
}

static bool in_class(const char *name, size_t n, int c)
{
#define CLASS(word, test) if (n == sizeof(word) - 1 && !strncmp(name, word, n)) return test(c) != 0;
    CLASS("alnum", isalnum) CLASS("alpha", isalpha) CLASS("blank", isblank) CLASS("cntrl", iscntrl)
    CLASS("digit", isdigit) CLASS("graph", isgraph) CLASS("lower", islower) CLASS("print", isprint)
    CLASS("punct", ispunct) CLASS("space", isspace) CLASS("upper", isupper) CLASS("xdigit", isxdigit)
#undef CLASS
    return false;
}

/* A bracket expression at p (after '['): *matched whether c is in it;
 * the place after its ']', or 0 when there is no ']' (then '[' is an
 * ordinary character). */
static const char *bracket(const char *p, int c, int flags, bool *matched)
{
    bool negate = *p == '!' || *p == '^';
    if (negate)
        p++;
    bool found = false, first = true;
    while (*p && (first || *p != ']')) {
        first = false;
        if (p[0] == '[' && p[1] == ':') {
            const char *end = strstr(p + 2, ":]");
            if (end) {
                if (in_class(p + 2, (size_t)(end - p - 2), c))
                    found = true;
                p = end + 2;
                continue;
            }
        }
        int lo = (unsigned char)*p;
        if (lo == '\\' && !(flags & FNM_NOESCAPE) && p[1])
            lo = (unsigned char)*++p;
        p++;
        int hi = lo;
        if (p[0] == '-' && p[1] && p[1] != ']') {
            hi = (unsigned char)p[1];
            p += 2;
            if (hi == '\\' && !(flags & FNM_NOESCAPE) && *p)
                hi = (unsigned char)*p++;
        }
        if ((lo <= c && c <= hi) ||
            ((flags & FNM_CASEFOLD) && ((lo <= tolower(c) && tolower(c) <= hi) ||
                                        (lo <= toupper(c) && toupper(c) <= hi))))
            found = true;
    }
    if (*p != ']')
        return 0;
    *matched = found != negate;
    return p + 1;
}

static bool leading_period(const char *name, const char *start, int flags)
{
    if (!(flags & FNM_PERIOD) || *name != '.')
        return false;
    return name == start || ((flags & FNM_PATHNAME) && name[-1] == '/');
}

static int match(const char *p, const char *n, const char *start, int flags)
{
    for (;;) {
        int c = (unsigned char)*p;
        if (c == 0) {
            if (*n == 0 || ((flags & FNM_LEADING_DIR) && *n == '/'))
                return 0;
            return FNM_NOMATCH;
        }
        if (c == '*') {
            while (*p == '*')
                p++;
            if (leading_period(n, start, flags))
                return FNM_NOMATCH;
            for (const char *t = n;; t++) {
                if (match(p, t, start, flags) == 0)
                    return 0;
                if (!*t || ((flags & FNM_PATHNAME) && *t == '/'))
                    return FNM_NOMATCH;
            }
        }
        if (*n == 0)
            return FNM_NOMATCH;
        if (c == '?') {
            if (((flags & FNM_PATHNAME) && *n == '/') || leading_period(n, start, flags))
                return FNM_NOMATCH;
            p++, n++;
            continue;
        }
        if (c == '[') {
            bool matched = false;
            const char *after = bracket(p + 1, (unsigned char)*n, flags, &matched);
            if (after) {
                if (((flags & FNM_PATHNAME) && *n == '/') || leading_period(n, start, flags) || !matched)
                    return FNM_NOMATCH;
                p = after, n++;
                continue;
            }
        }
        if (c == '\\' && !(flags & FNM_NOESCAPE) && p[1])
            c = (unsigned char)*++p;
        if (fold(c, flags) != fold((unsigned char)*n, flags))
            return FNM_NOMATCH;
        p++, n++;
    }
}

int fnmatch(const char *pattern, const char *name, int flags)
{
    return match(pattern, name, name, flags);
}
