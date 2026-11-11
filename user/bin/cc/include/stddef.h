/* stddef.h (C11 7.19): cc's own (60a); the types of the LP64 ABIs. */
#ifndef _STDDEF_H
#define _STDDEF_H

typedef unsigned long size_t;
typedef long ptrdiff_t;
#ifdef __x86_64__
typedef int wchar_t;
#else
typedef unsigned int wchar_t;                   /* (AAPCS64) */
#endif
typedef struct {
    long long __max_align_ll;
    long double __max_align_ld;
} max_align_t;

#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
