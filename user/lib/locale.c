/*
 * (43a-2) Locales. RitterOS has two: "C" (also "POSIX": a byte is a
 * character, ASCII's classes) and "C.UTF-8" (characters are UTF-8, the
 * wide classes and cases Unicode's: ctypetab.h). Any name ending in
 * ".UTF-8" or ".utf8" (en_US.UTF-8, ...) is taken as C.UTF-8 and keeps its
 * name; numbers, dates, money and messages are the C locale's in both.
 *
 * A locale object (locale_t) holds the name of each category; setlocale
 * changes the program's (the global one), uselocale a thread's own (a
 * thread without one uses the global). Everything that turns bytes into
 * wide characters and back (mbrtowc, wcrtomb, printf's %ls, the wide
 * streams...) asks __ritteros_utf8() which way: the calling thread's
 * locale's LC_CTYPE.
 */
#include <ctype.h>
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#define NAME_MAX_LOCALE 32
#define CATEGORIES 6                    /* LC_CTYPE ... LC_MESSAGES */

struct __locale_struct {
    char names[CATEGORIES][NAME_MAX_LOCALE];
    bool utf8;                          /* LC_CTYPE is UTF-8 */
};

static struct __locale_struct global = {
    { "C", "C", "C", "C", "C", "C" }, false,
};
static const struct __locale_struct c_locale = {
    { "C", "C", "C", "C", "C", "C" }, false,
};
static const struct __locale_struct utf8_locale = {
    { "C.UTF-8", "C.UTF-8", "C.UTF-8", "C.UTF-8", "C.UTF-8", "C.UTF-8" }, true,
};
static _Thread_local locale_t own;      /* uselocale's; 0: the global one */

static const struct __locale_struct *current(void)
{
    return own ? own : &global;
}

int __ritteros_utf8(void)
{
    return current()->utf8;
}

int __ritteros_mb_cur_max(void)
{
    return current()->utf8 ? 4 : 1;
}

/* A name RitterOS has: 1 the C locale, 2 UTF-8, 0 none. */
static int known(const char *name)
{
    if (!strcmp(name, "C") || !strcmp(name, "POSIX"))
        return 1;
    size_t n = strlen(name);
    if (n >= NAME_MAX_LOCALE)
        return 0;
    const char *dot = strchr(name, '.');
    if (!dot)
        return 0;
    const char *code = dot + 1;
    size_t k = strcspn(code, "@");
    if ((k == 5 && !strncasecmp(code, "UTF-8", 5)) || (k == 4 && !strncasecmp(code, "utf8", 4)))
        return 2;
    return 0;
}

static void set_category(struct __locale_struct *l, int category, const char *name)
{
    strcpy(l->names[category], name);
    if (category == LC_CTYPE)
        l->utf8 = known(name) == 2;
}

/* "" asks the environment: LC_ALL, then LC_<category>, then LANG, else C. */
static const char *from_environment(int category)
{
    static const char *const vars[CATEGORIES] = {
        "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE", "LC_MONETARY", "LC_MESSAGES",
    };
    const char *v = getenv("LC_ALL");
    if (!v || !*v)
        v = getenv(vars[category]);
    if (!v || !*v)
        v = getenv("LANG");
    return v && *v ? v : "C";
}

char *setlocale(int category, const char *name)
{
    static char composite[CATEGORIES * (NAME_MAX_LOCALE + 16)];
    if (category < LC_CTYPE || category > LC_ALL)
        return 0;
    if (name) {
        int first = category == LC_ALL ? 0 : category, last = category == LC_ALL ? CATEGORIES - 1 : category;
        for (int c = first; c <= last; c++)
            if (!known(*name ? name : from_environment(c)))
                return 0;
        for (int c = first; c <= last; c++)
            set_category(&global, c, *name ? name : from_environment(c));
    }
    if (category != LC_ALL)
        return global.names[category];
    bool same = true;
    for (int c = 1; c < CATEGORIES; c++)
        same = same && !strcmp(global.names[c], global.names[0]);
    if (same)
        return global.names[0];
    static const char *const labels[CATEGORIES] = {
        "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE", "LC_MONETARY", "LC_MESSAGES",
    };
    composite[0] = 0;
    for (int c = 0; c < CATEGORIES; c++) {
        if (c)
            strcat(composite, ";");
        strcat(composite, labels[c]);
        strcat(composite, "=");
        strcat(composite, global.names[c]);
    }
    return composite;
}

static char empty[] = "", dot[] = ".";
static struct lconv c_lconv = {
    dot, empty, empty, empty, empty, empty, empty, empty, empty, empty,
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
};

struct lconv *localeconv(void)
{
    return &c_lconv;
}

locale_t newlocale(int mask, const char *name, locale_t base)
{
    if (!name || (mask & ~LC_ALL_MASK)) {
        errno = EINVAL;
        return 0;
    }
    for (int c = 0; c < CATEGORIES; c++)
        if ((mask & (1 << c)) && !known(*name ? name : from_environment(c))) {
            errno = ENOENT;
            return 0;
        }
    locale_t l = base;
    if (!l || l == LC_GLOBAL_LOCALE) {
        l = malloc(sizeof(*l));
        if (!l) {
            errno = ENOMEM;
            return 0;
        }
        *l = base == LC_GLOBAL_LOCALE ? global : c_locale;
    }
    for (int c = 0; c < CATEGORIES; c++)
        if (mask & (1 << c))
            set_category(l, c, *name ? name : from_environment(c));
    return l;
}

locale_t duplocale(locale_t l)
{
    locale_t copy = malloc(sizeof(*copy));
    if (!copy) {
        errno = ENOMEM;
        return 0;
    }
    *copy = l == LC_GLOBAL_LOCALE ? global : *l;
    return copy;
}

void freelocale(locale_t l)
{
    if (l && l != LC_GLOBAL_LOCALE && l != &c_locale && l != &utf8_locale)
        free(l);
}

locale_t uselocale(locale_t l)
{
    locale_t before = own ? own : LC_GLOBAL_LOCALE;
    if (l)
        own = l == LC_GLOBAL_LOCALE ? 0 : l;
    return before;
}

/* The locale a *_l call works in, in place of the thread's. */
#define WITH(l, call)                                                       \
    do {                                                                    \
        locale_t saved_ = own;                                              \
        own = (l) == LC_GLOBAL_LOCALE ? 0 : (l);                            \
        __typeof__(call) r_ = (call);                                       \
        own = saved_;                                                       \
        return r_;                                                          \
    } while (0)

/* --- UTF-8 and bytes --- */

/* mbrtowc's work, either way: the next character of s (at most n bytes)
 * into *wc; its bytes, 0 for the NUL, (size_t)-2 when n bytes end inside
 * one (kept in *state), (size_t)-1 and EILSEQ for bytes no character has. */
size_t __ritteros_mbr(wchar_t *wc, const char *s, size_t n, mbstate_t *state)
{
    static mbstate_t internal;
    if (!state)
        state = &internal;
    if (!s) {
        state->need = state->bits = 0;
        return 0;
    }
    if (n == 0)
        return (size_t)-2;
    if (!current()->utf8) {
        if (wc)
            *wc = (wchar_t)(unsigned char)*s;
        return *s ? 1 : 0;
    }
    /* state->need: the bytes still to come, and (<< 4) the character's
     * length; state->bits: its bits so far */
    const unsigned char *p = (const unsigned char *)s;
    unsigned need = state->need & 15, length = state->need >> 4, bits = state->bits;
    size_t used = 0;
    if (!need) {
        unsigned c = p[used++];
        if (c < 0x80) {
            if (wc)
                *wc = (wchar_t)c;
            return c ? 1 : 0;
        }
        if (c >= 0xC2 && c <= 0xDF)
            need = 1, bits = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF)
            need = 2, bits = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4)
            need = 3, bits = c & 0x07;
        else {
            errno = EILSEQ;
            return (size_t)-1;
        }
        length = need + 1;
    }
    while (need && used < n) {
        unsigned c = p[used++];
        if ((c & 0xC0) != 0x80) {
            state->need = state->bits = 0;
            errno = EILSEQ;
            return (size_t)-1;
        }
        bits = bits << 6 | (c & 0x3F);
        need--;
    }
    if (need) {
        state->need = need | length << 4, state->bits = bits;
        return (size_t)-2;
    }
    state->need = state->bits = 0;
    static const unsigned least[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (bits < least[length] || (bits >= 0xD800 && bits <= 0xDFFF) || bits > 0x10FFFF) {
        errno = EILSEQ;                         /* (too long a form, a surrogate, too large) */
        return (size_t)-1;
    }
    if (wc)
        *wc = (wchar_t)bits;
    return bits ? used : 0;
}

/* wcrtomb's work: wc's bytes into s (MB_LEN_MAX room); how many, or
 * (size_t)-1 and EILSEQ. */
size_t __ritteros_wcr(char *s, wchar_t wc, mbstate_t *state)
{
    (void)state;
    char room[4];
    if (!s)
        s = room, wc = 0;
    unsigned c = (unsigned)wc;
    if (!current()->utf8) {
        if (c > 255) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        *s = (char)c;
        return 1;
    }
    if (c < 0x80) {
        *s = (char)c;
        return 1;
    }
    if (c < 0x800) {
        s[0] = (char)(0xC0 | c >> 6);
        s[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (c < 0x10000) {
        s[0] = (char)(0xE0 | c >> 12);
        s[1] = (char)(0x80 | (c >> 6 & 0x3F));
        s[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    s[0] = (char)(0xF0 | c >> 18);
    s[1] = (char)(0x80 | (c >> 12 & 0x3F));
    s[2] = (char)(0x80 | (c >> 6 & 0x3F));
    s[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* --- nl_langinfo: the C locale's words --- */

char *nl_langinfo(nl_item item)
{
    static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                        "Friday", "Saturday" };
    static const char *const months[] = { "January", "February", "March", "April", "May", "June",
                                          "July", "August", "September", "October", "November",
                                          "December" };
    static char short_name[4];
    if (item == CODESET)
        return (char *)(current()->utf8 ? "UTF-8" : "ANSI_X3.4-1968");
    if (item >= DAY_1 && item <= DAY_7)
        return (char *)days[item - DAY_1];
    if (item >= MON_1 && item <= MON_12)
        return (char *)months[item - MON_1];
    if (item >= ABDAY_1 && item <= ABDAY_7) {
        memcpy(short_name, days[item - ABDAY_1], 3);
        return short_name;
    }
    if (item >= ABMON_1 && item <= ABMON_12) {
        memcpy(short_name, months[item - ABMON_1], 3);
        return short_name;
    }
    switch (item) {
    case D_T_FMT: return (char *)"%a %b %e %H:%M:%S %Y";
    case D_FMT: return (char *)"%m/%d/%y";
    case T_FMT: return (char *)"%H:%M:%S";
    case T_FMT_AMPM: return (char *)"%I:%M:%S %p";
    case AM_STR: return (char *)"AM";
    case PM_STR: return (char *)"PM";
    case RADIXCHAR: return (char *)".";
    case YESEXPR: return (char *)"^[yY]";
    case NOEXPR: return (char *)"^[nN]";
    case CRNCYSTR: return (char *)"-";
    default: return (char *)"";
    }
}

char *nl_langinfo_l(nl_item item, locale_t l) { WITH(l, nl_langinfo(item)); }

/* --- the *_l functions: the call in that locale --- */

#define BYTE_CLASS(name) int name##_l(int c, locale_t l) { (void)l; return name(c); }
BYTE_CLASS(isalnum) BYTE_CLASS(isalpha) BYTE_CLASS(isblank) BYTE_CLASS(iscntrl)
BYTE_CLASS(isdigit) BYTE_CLASS(isgraph) BYTE_CLASS(islower) BYTE_CLASS(isprint)
BYTE_CLASS(ispunct) BYTE_CLASS(isspace) BYTE_CLASS(isupper) BYTE_CLASS(isxdigit)
BYTE_CLASS(tolower) BYTE_CLASS(toupper)

#define WIDE_CLASS(name) int name##_l(wint_t c, locale_t l) { WITH(l, name(c)); }
WIDE_CLASS(iswalnum) WIDE_CLASS(iswalpha) WIDE_CLASS(iswblank) WIDE_CLASS(iswcntrl)
WIDE_CLASS(iswdigit) WIDE_CLASS(iswgraph) WIDE_CLASS(iswlower) WIDE_CLASS(iswprint)
WIDE_CLASS(iswpunct) WIDE_CLASS(iswspace) WIDE_CLASS(iswupper) WIDE_CLASS(iswxdigit)

wint_t towlower_l(wint_t c, locale_t l) { WITH(l, towlower(c)); }
wint_t towupper_l(wint_t c, locale_t l) { WITH(l, towupper(c)); }
int iswctype_l(wint_t c, wctype_t type, locale_t l) { WITH(l, iswctype(c, type)); }
wctype_t wctype_l(const char *name, locale_t l) { (void)l; return wctype(name); }
wint_t towctrans_l(wint_t c, wctrans_t t, locale_t l) { WITH(l, towctrans(c, t)); }
wctrans_t wctrans_l(const char *name, locale_t l) { (void)l; return wctrans(name); }

int strcoll_l(const char *a, const char *b, locale_t l) { (void)l; return strcoll(a, b); }
size_t strxfrm_l(char *dst, const char *src, size_t n, locale_t l) { (void)l; return strxfrm(dst, src, n); }
int wcscoll_l(const wchar_t *a, const wchar_t *b, locale_t l) { (void)l; return wcscoll(a, b); }
size_t wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t n, locale_t l) { (void)l; return wcsxfrm(dst, src, n); }
int strcasecmp_l(const char *a, const char *b, locale_t l) { (void)l; return strcasecmp(a, b); }
int strncasecmp_l(const char *a, const char *b, size_t n, locale_t l) { (void)l; return strncasecmp(a, b, n); }
int wcscasecmp_l(const wchar_t *a, const wchar_t *b, locale_t l) { WITH(l, wcscasecmp(a, b)); }
int wcsncasecmp_l(const wchar_t *a, const wchar_t *b, size_t n, locale_t l) { WITH(l, wcsncasecmp(a, b, n)); }
char *strerror_l(int error, locale_t l) { (void)l; return strerror(error); }

size_t strftime_l(char *s, size_t max, const char *format, const struct tm *tm, locale_t l)
{
    (void)l;
    return strftime(s, max, format, tm);
}

size_t wcsftime_l(wchar_t *s, size_t max, const wchar_t *format, const struct tm *tm, locale_t l)
{
    (void)l;
    return wcsftime(s, max, format, tm);
}

double strtod_l(const char *s, char **end, locale_t l) { (void)l; return strtod(s, end); }
float strtof_l(const char *s, char **end, locale_t l) { (void)l; return strtof(s, end); }
long double strtold_l(const char *s, char **end, locale_t l) { (void)l; return strtold(s, end); }
long strtol_l(const char *s, char **end, int base, locale_t l) { (void)l; return strtol(s, end, base); }
unsigned long strtoul_l(const char *s, char **end, int base, locale_t l) { (void)l; return strtoul(s, end, base); }
long long strtoll_l(const char *s, char **end, int base, locale_t l) { (void)l; return strtoll(s, end, base); }
unsigned long long strtoull_l(const char *s, char **end, int base, locale_t l)
{
    (void)l;
    return strtoull(s, end, base);
}
