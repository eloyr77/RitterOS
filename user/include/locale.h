/* Locales (step 22n): RitterOS has one, the "C" locale (also called
 * "POSIX"): ASCII, a '.' before decimals, no grouping. setlocale takes it
 * by those names or "" (the system's: the same) and refuses others (0);
 * setlocale(category, 0) says which is in use. */
#ifndef RITTEROS_LOCALE_H
#define RITTEROS_LOCALE_H

#ifdef __cplusplus
extern "C" {
#endif

#define LC_CTYPE    0
#define LC_NUMERIC  1
#define LC_TIME     2
#define LC_COLLATE  3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL      6

/* How numbers and money are written (C's struct; the "C" locale's values:
 * "." and empty texts, CHAR_MAX (127) for "not given"). */
struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

/* (43a-2) Locales: "C" (or "POSIX") and "C.UTF-8" (any name ending in
 * ".UTF-8" or ".utf8", such as "en_US.UTF-8", is C.UTF-8 under its own
 * name): in C.UTF-8 characters are UTF-8 and the wide classes Unicode's;
 * numbers, dates and messages are the C locale's in both. setlocale(c,
 * "") takes LC_ALL, LC_<category> or LANG from the environment (else C).
 * newlocale makes (or changes base into) a locale with the categories of
 * mask from name; uselocale makes it the calling thread's own
 * (LC_GLOBAL_LOCALE: back to the program's; 0: only ask), returning the
 * one before; duplocale copies one; freelocale frees one. */
#ifndef RITTEROS_LOCALE_T
#define RITTEROS_LOCALE_T
typedef struct __locale_struct *locale_t;
#endif
#define LC_CTYPE_MASK       (1 << LC_CTYPE)
#define LC_NUMERIC_MASK     (1 << LC_NUMERIC)
#define LC_TIME_MASK        (1 << LC_TIME)
#define LC_COLLATE_MASK     (1 << LC_COLLATE)
#define LC_MONETARY_MASK    (1 << LC_MONETARY)
#define LC_MESSAGES_MASK    (1 << LC_MESSAGES)
#define LC_ALL_MASK         0x3f
#define LC_GLOBAL_LOCALE    ((locale_t)-1)
locale_t newlocale(int mask, const char *name, locale_t base);
locale_t duplocale(locale_t locale);
void freelocale(locale_t locale);
locale_t uselocale(locale_t locale);

#ifdef __cplusplus
}
#endif

#endif
