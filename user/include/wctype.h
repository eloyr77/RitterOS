/*
 * wctype.h (step 22v): classes of wide characters, as ctype.h's: the "C"
 * locale's, ASCII (characters above 127 are in no class and have no other
 * case). wctype and wctrans name a class or a mapping ("alpha",
 * "toupper", ...; 0 for a name not known).
 */
#ifndef RITTEROS_WCTYPE_H
#define RITTEROS_WCTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

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

/* (43a-2) The same in a given locale (POSIX's *_l). */
#ifndef RITTEROS_LOCALE_T
#define RITTEROS_LOCALE_T
typedef struct __locale_struct *locale_t;
#endif
int iswalnum_l(wint_t c, locale_t l);
int iswalpha_l(wint_t c, locale_t l);
int iswblank_l(wint_t c, locale_t l);
int iswcntrl_l(wint_t c, locale_t l);
int iswdigit_l(wint_t c, locale_t l);
int iswgraph_l(wint_t c, locale_t l);
int iswlower_l(wint_t c, locale_t l);
int iswprint_l(wint_t c, locale_t l);
int iswpunct_l(wint_t c, locale_t l);
int iswspace_l(wint_t c, locale_t l);
int iswupper_l(wint_t c, locale_t l);
int iswxdigit_l(wint_t c, locale_t l);
wint_t towlower_l(wint_t c, locale_t l);
wint_t towupper_l(wint_t c, locale_t l);
int iswctype_l(wint_t c, wctype_t type, locale_t l);
wctype_t wctype_l(const char *name, locale_t l);
wint_t towctrans_l(wint_t c, wctrans_t t, locale_t l);
wctrans_t wctrans_l(const char *name, locale_t l);

#ifdef __cplusplus
}
#endif

#endif
