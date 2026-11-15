/*
 * libintl.h (43a-2): GNU gettext's calls. There are no translations
 * (English only): gettext and dgettext give their text back, ngettext the
 * singular for n == 1 and else the plural; textdomain, bindtextdomain and
 * bind_textdomain_codeset are taken and change nothing.
 */
#ifndef RITTEROS_LIBINTL_H
#define RITTEROS_LIBINTL_H

#ifdef __cplusplus
extern "C" {
#endif

char *gettext(const char *text);
char *dgettext(const char *domain, const char *text);
char *dcgettext(const char *domain, const char *text, int category);
char *ngettext(const char *singular, const char *plural, unsigned long n);
char *dngettext(const char *domain, const char *singular, const char *plural, unsigned long n);
char *dcngettext(const char *domain, const char *singular, const char *plural, unsigned long n, int category);
char *textdomain(const char *domain);
char *bindtextdomain(const char *domain, const char *folder);
char *bind_textdomain_codeset(const char *domain, const char *codeset);

#ifdef __cplusplus
}
#endif

#endif
