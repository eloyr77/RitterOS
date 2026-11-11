/*
 * Regular expressions (58a), POSIX's: basic (BRE: grep, sed) and, with
 * REG_EXTENDED, extended syntax (ERE: grep -E, awk). regcomp compiles a
 * pattern, regexec finds its leftmost-longest match in a string, with
 * the groups' places in pmatch (-1 for a group that took no part); regfree
 * gives the memory back, regerror describes an error code.
 *
 * Both syntaxes: . [set] [^set] (ranges, [:alpha:] and the other classes,
 * [=c=], [.c.]) * ^ $ \c; BRE \( \) \{m,n\} \1-\9; ERE ( ) | + ? {m,n}
 * (and \1-\9, an extension as on BSD). In the "C" locale. Groups take,
 * inside the longest match, the greedy leftmost-first choice: POSIX's rule
 * for ambiguous groups ((a|ab)(c|bcd) on "abcd") may choose otherwise.
 */
#ifndef RITTEROS_REGEX_H
#define RITTEROS_REGEX_H

#include <stddef.h>
#include <sys/types.h>

typedef ssize_t regoff_t;

typedef struct {
    size_t re_nsub;                 /* the number of groups */
    void *re_program;               /* (the library's) */
} regex_t;

typedef struct {
    regoff_t rm_so, rm_eo;          /* the match's start and end, or -1 */
} regmatch_t;

/* regcomp's flags */
#define REG_EXTENDED    1           /* extended syntax (ERE) */
#define REG_ICASE       2           /* letters match either case */
#define REG_NOSUB       4           /* only whether it matches (pmatch is not filled) */
#define REG_NEWLINE     8           /* . and [^...] never match a line break; ^ and $ also
                                     * match after and before one */
/* regexec's flags */
#define REG_NOTBOL      1           /* the string's start is no line's start (^ fails) */
#define REG_NOTEOL      2           /* nor is its end a line's end ($ fails) */

/* Errors (regcomp, regexec) */
#define REG_NOMATCH     1           /* regexec: no match */
#define REG_BADPAT      2           /* an invalid pattern */
#define REG_ECOLLATE    3           /* an unknown collating element */
#define REG_ECTYPE      4           /* an unknown character class */
#define REG_EESCAPE     5           /* a \ at the end */
#define REG_ESUBREG     6           /* \N for a group that is not there */
#define REG_EBRACK      7           /* [ without ] */
#define REG_EPAREN      8           /* ( or \( without its end, or the other way */
#define REG_EBRACE      9           /* { or \{ without its end */
#define REG_BADBR       10          /* an invalid {m,n} */
#define REG_ERANGE      11          /* an invalid range (z-a) */
#define REG_ESPACE      12          /* out of memory, or too much work */
#define REG_BADRPT      13          /* * + ? or { with nothing before it */

#define RE_DUP_MAX      255         /* the largest m and n of {m,n} */

int regcomp(regex_t *re, const char *pattern, int flags);
int regexec(const regex_t *re, const char *string, size_t nmatch, regmatch_t pmatch[],
            int flags);
size_t regerror(int error, const regex_t *re, char *buffer, size_t size);
void regfree(regex_t *re);

#endif
