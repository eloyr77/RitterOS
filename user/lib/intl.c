/*
 * (43a-2) libintl.h: gettext's calls for ported programs. RitterOS speaks
 * English only, so there are no translations: each call gives the text it
 * was given (ngettext the singular for n == 1, else the plural), and the
 * domain calls remember nothing and answer as glibc's do without a
 * catalogue.
 */
#include <libintl.h>
#include <stddef.h>

char *gettext(const char *text) { return (char *)text; }
char *dgettext(const char *domain, const char *text) { (void)domain; return (char *)text; }

char *dcgettext(const char *domain, const char *text, int category)
{
    (void)domain, (void)category;
    return (char *)text;
}

char *ngettext(const char *singular, const char *plural, unsigned long n)
{
    return (char *)(n == 1 ? singular : plural);
}

char *dngettext(const char *domain, const char *singular, const char *plural, unsigned long n)
{
    (void)domain;
    return ngettext(singular, plural, n);
}

char *dcngettext(const char *domain, const char *singular, const char *plural, unsigned long n, int category)
{
    (void)domain, (void)category;
    return ngettext(singular, plural, n);
}

char *textdomain(const char *domain)
{
    (void)domain;
    return (char *)"messages";
}

char *bindtextdomain(const char *domain, const char *folder)
{
    (void)domain;
    return (char *)(folder ? folder : "/system/locale");
}

char *bind_textdomain_codeset(const char *domain, const char *codeset)
{
    (void)domain;
    return (char *)codeset;
}
