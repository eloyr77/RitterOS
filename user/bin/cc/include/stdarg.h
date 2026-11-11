/* stdarg.h (C11 7.16): cc's own (60a). The compiler knows the ABIs' lists
 * of variable arguments (System V's register save area, AAPCS64's
 * va_list); these are its names for them, the same as GCC's and clang's. */
#ifndef _STDARG_H
#define _STDARG_H

typedef __builtin_va_list va_list;
typedef __builtin_va_list __gnuc_va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(dest, src) __builtin_va_copy(dest, src)
#define __va_copy(dest, src) __builtin_va_copy(dest, src)

#endif
