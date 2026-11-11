/*
 * ctype.h (22m): classes of characters, for ASCII (other byte values are in
 * no class; text is UTF-8, whose multi-byte characters these do not judge).
 */
#ifndef RITTEROS_CTYPE_H
#define RITTEROS_CTYPE_H

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

#endif
