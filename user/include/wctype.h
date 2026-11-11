/*
 * wctype.h (step 22v): classes of wide characters, as ctype.h's: the "C"
 * locale's, ASCII (characters above 127 are in no class and have no other
 * case). wctype and wctrans name a class or a mapping ("alpha",
 * "toupper", ...; 0 for a name not known).
 */
#ifndef RITTEROS_WCTYPE_H
#define RITTEROS_WCTYPE_H

#ifndef RITTEROS_WINT_T
#define RITTEROS_WINT_T
typedef __WINT_TYPE__ wint_t;
#endif
#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;

int iswalnum(wint_t wc);
int iswalpha(wint_t wc);
int iswblank(wint_t wc);
int iswcntrl(wint_t wc);
int iswdigit(wint_t wc);
int iswgraph(wint_t wc);
int iswlower(wint_t wc);
int iswprint(wint_t wc);
int iswpunct(wint_t wc);
int iswspace(wint_t wc);
int iswupper(wint_t wc);
int iswxdigit(wint_t wc);
int iswctype(wint_t wc, wctype_t type);
wctype_t wctype(const char *name);
wint_t towlower(wint_t wc);
wint_t towupper(wint_t wc);
wint_t towctrans(wint_t wc, wctrans_t mapping);
wctrans_t wctrans(const char *name);

#endif
