/*
 * ctype.h (22m): classes of characters, for ASCII (other byte values are in
 * no class; text is UTF-8, whose multi-byte characters these do not judge).
 */
#ifndef RITTEROS_CTYPE_H
#define RITTEROS_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);

/* (43a) isascii: c is 0-127; toascii: its low 7 bits. */
int isascii(int c);
int toascii(int c);

/* (43a-2) The same in a given locale (POSIX's *_l): the bytes' classes
 * are ASCII's in every locale. */
#ifndef RITTEROS_LOCALE_T
#define RITTEROS_LOCALE_T
typedef struct __locale_struct *locale_t;
#endif
int isalnum_l(int c, locale_t l);
int isalpha_l(int c, locale_t l);
int isblank_l(int c, locale_t l);
int iscntrl_l(int c, locale_t l);
int isdigit_l(int c, locale_t l);
int isgraph_l(int c, locale_t l);
int islower_l(int c, locale_t l);
int isprint_l(int c, locale_t l);
int ispunct_l(int c, locale_t l);
int isspace_l(int c, locale_t l);
int isupper_l(int c, locale_t l);
int isxdigit_l(int c, locale_t l);
int tolower_l(int c, locale_t l);
int toupper_l(int c, locale_t l);

#ifdef __cplusplus
}
#endif

#endif
