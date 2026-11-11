/*
 * Test: regex.h (58a). Lines "regextest: ...": for each case the compile
 * result, then whether it matched and where (the whole match and up to
 * four groups). Plain POSIX: the test builds it on the Mac too and the
 * lines must be the same as with the Mac's own regcomp/regexec. The cases
 * leave out what POSIX leaves open (errors whose code differs between
 * libraries, ambiguous groups).
 */
#include <regex.h>
#include <stdio.h>
#include <string.h>

#define E REG_EXTENDED
#define I REG_ICASE
#define N REG_NEWLINE

static const struct {
    int flags;
    const char *pattern, *string;
} cases[] = {
    /* BRE */
    { 0, "abc", "xxabcxx" }, { 0, "a.c", "abc a-c" }, { 0, "a*", "baaa" }, { 0, "ba*", "baaa" },
    { 0, "^ab", "abab" }, { 0, "ab$", "abab" }, { 0, "^$", "" }, { 0, "x^y", "x^y" },
    { 0, "a$b", "a$b" }, { 0, "*a", "x*a" }, { 0, "\\(ab\\)*c", "ababc" },
    { 0, "\\(a\\)\\(b\\)", "xab" }, { 0, "a\\{2\\}", "aaaa" }, { 0, "a\\{2,\\}", "aaaa" },
    { 0, "a\\{1,3\\}b", "aaaab" }, { 0, "\\(.\\)\\1", "abccd" }, { 0, "\\(a*\\)b\\1", "aabaa" },
    { 0, "[abc]*", "cabd" }, { 0, "[^a-c]", "abcd" }, { 0, "[]a]", "]" }, { 0, "[a-]", "-" },
    { 0, "[[:digit:]]\\{3\\}", "ab1234" }, { 0, "[[:alpha:][:space:]]*", "ab c1" },
    { 0, "[[:upper:]]", "abC" }, { 0, "a\\.b", "a.b axb" }, { 0, "a+", "a+" }, { 0, "a?", "a?" },
    { 0, "a|b", "a|b" }, { 0, "\\(a\\|b\\)", "x" }, { 0, "x*", "" }, { 0, "[.]", "a.b" },
    /* ERE */
    { E, "a|ab", "abc" }, { E, "(a|ab)(c|bcd)", "abcd" }, { E, "a+", "baaa" }, { E, "a?b", "ab b" },
    { E, "(ab)+", "ababab" }, { E, "(a)(b)(c)(d)", "abcd" }, { E, "a{2,3}", "aaaa" },
    { E, "^(x|y)*$", "xyxy" }, { E, "(foo|foobar)baz", "foobarbaz" }, { E, "[0-9]+\\.[0-9]*", "v1.25" },
    { E, "(a*)*", "b" }, { E, "(a*)+", "b" }, { E, "(a|b)*c", "abac" }, { E, ".*x", "axbxc" },
    { E, "x(y?)z", "xz" }, { E, "(^|,)b", "a,b" }, { E, "b($|,)", "ab" }, { E, "\\(a\\)", "(a)" },
    { E, "a{0}b", "ab" }, { E, "(abc|abcd)(d|e)", "abcde" }, { E, "[[:xdigit:]]+", "zz0aF9g" },
    { E, "colou?r", "my color" },
    /* flags */
    { E | I, "ABC", "xabcx" }, { I, "[a-c]*", "AbCd" },
    { E | N, "^b", "a\nb" }, { E | N, "a$", "a\nb" }, { E | N, "a.b", "a\nb" }, { E, "a.b", "a\nb" },
    { E | N, "[^x]*", "ab\ncd" }, { E, "^b", "a\nb" },
    /* errors */
    { 0, "a\\", "" }, { 0, "[abc", "" }, { E, "(ab", "" }, { 0, "\\(ab", "" },
    { E, "a{2,1}", "" }, { 0, "[z-a]", "" }, { 0, "[[:nope:]]", "" }, { 0, "\\(a\\)\\2", "" },
};

static const char *code_name(int e)
{
    static const char *const names[] = { "0", "REG_NOMATCH", "REG_BADPAT", "REG_ECOLLATE",
        "REG_ECTYPE", "REG_EESCAPE", "REG_ESUBREG", "REG_EBRACK", "REG_EPAREN", "REG_EBRACE",
        "REG_BADBR", "REG_ERANGE", "REG_ESPACE", "REG_BADRPT" };
    int codes[] = { 0, REG_NOMATCH, REG_BADPAT, REG_ECOLLATE, REG_ECTYPE, REG_EESCAPE, REG_ESUBREG,
                    REG_EBRACK, REG_EPAREN, REG_EBRACE, REG_BADBR, REG_ERANGE, REG_ESPACE,
                    REG_BADRPT };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
        if (codes[i] == e)
            return names[i];
    return "another";
}

static void shown(const char *s, char *out)
{
    for (; *s; s++)
        out += *s == '\n' ? sprintf(out, "\\n") : sprintf(out, "%c", *s);
    *out = 0;
}

int main(void)
{
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        regex_t re;
        char p[200], s[200];
        shown(cases[k].pattern, p);
        shown(cases[k].string, s);
        int c = regcomp(&re, cases[k].pattern, cases[k].flags);
        if (c) {
            printf("regextest: %d /%s/ compile %s\n", cases[k].flags, p, code_name(c));
            continue;
        }
        regmatch_t m[5];
        int e = regexec(&re, cases[k].string, 5, m, 0);
        printf("regextest: %d /%s/ \"%s\" %zu group(s): ", cases[k].flags, p, s, re.re_nsub);
        if (e)
            printf("%s\n", code_name(e));
        else {
            for (size_t g = 0; g <= re.re_nsub && g < 5; g++)
                printf("%s%lld-%lld", g ? " " : "", (long long)m[g].rm_so, (long long)m[g].rm_eo);
            printf("\n");
        }
        regfree(&re);
    }
    regex_t re;
    regcomp(&re, "b", 0);
    regmatch_t m[1];
    printf("regextest: REG_NOTBOL on ^a: %s\n", regcomp(&re, "^a", 0) == 0 &&
           regexec(&re, "a", 1, m, REG_NOTBOL) == REG_NOMATCH ? "no match" : "?");
    printf("regextest: REG_NOSUB: %d\n", regcomp(&re, "(a)", E | REG_NOSUB) == 0 &&
           regexec(&re, "xa", 0, 0, 0) == 0);
    char text[100];
    regerror(REG_NOMATCH, 0, text, sizeof(text));
    printf("regextest: regerror gives a text: %s\n", text[0] ? "yes" : "no");
#ifndef __APPLE__
    /* RitterOS's own: back references in extended syntax too (as GNU's
     * grep -E has them; the Mac's ERE takes \1 for a 1). */
    if (regcomp(&re, "(.)\\1", E) == 0 && regexec(&re, "abbc", 1, m, 0) == 0)
        printf("regextest-own: (.)\\1 in ERE on abbc: %lld-%lld\n", (long long)m[0].rm_so,
               (long long)m[0].rm_eo);
#endif
    printf("regextest: done\n");
    return 0;
}
