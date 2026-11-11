/*
 * wctype.h (step 22v): the "C" locale's classes of wide characters, which
 * are ctype.h's for ASCII; nothing above 127 is in a class.
 */
#include <ctype.h>
#include <string.h>
#include <wctype.h>

static const char *const class_names[] = {
    0, "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct",
    "space", "upper", "xdigit",
};
static int (*const classes[])(int) = {
    0, isalnum, isalpha, isblank, iscntrl, isdigit, isgraph, islower, isprint, ispunct,
    isspace, isupper, isxdigit,
};
#define CLASSES (sizeof(classes) / sizeof(classes[0]))

static int ascii(wint_t wc)
{
    return wc >= 0 && wc < 128;
}

int iswctype(wint_t wc, wctype_t type)
{
    return type && type < CLASSES && ascii(wc) && classes[type]((int)wc);
}

wctype_t wctype(const char *name)
{
    for (wctype_t i = 1; i < CLASSES; i++)
        if (!strcmp(name, class_names[i]))
            return i;
    return 0;
}

int iswalnum(wint_t wc) { return ascii(wc) && isalnum((int)wc); }
int iswalpha(wint_t wc) { return ascii(wc) && isalpha((int)wc); }
int iswblank(wint_t wc) { return ascii(wc) && isblank((int)wc); }
int iswcntrl(wint_t wc) { return ascii(wc) && iscntrl((int)wc); }
int iswdigit(wint_t wc) { return ascii(wc) && isdigit((int)wc); }
int iswgraph(wint_t wc) { return ascii(wc) && isgraph((int)wc); }
int iswlower(wint_t wc) { return ascii(wc) && islower((int)wc); }
int iswprint(wint_t wc) { return ascii(wc) && isprint((int)wc); }
int iswpunct(wint_t wc) { return ascii(wc) && ispunct((int)wc); }
int iswspace(wint_t wc) { return ascii(wc) && isspace((int)wc); }
int iswupper(wint_t wc) { return ascii(wc) && isupper((int)wc); }
int iswxdigit(wint_t wc) { return ascii(wc) && isxdigit((int)wc); }

wint_t towlower(wint_t wc)
{
    return wc >= 'A' && wc <= 'Z' ? wc + 32 : wc;
}

wint_t towupper(wint_t wc)
{
    return wc >= 'a' && wc <= 'z' ? wc - 32 : wc;
}

wctrans_t wctrans(const char *name)
{
    return !strcmp(name, "tolower") ? 1 : !strcmp(name, "toupper") ? 2 : 0;
}

wint_t towctrans(wint_t wc, wctrans_t mapping)
{
    return mapping == 1 ? towlower(wc) : mapping == 2 ? towupper(wc) : wc;
}
