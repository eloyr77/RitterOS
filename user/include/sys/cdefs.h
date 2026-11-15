/*
 * sys/cdefs.h (43a): the BSDs' and glibc's helper macros ported headers
 * use: __BEGIN_DECLS/__END_DECLS (extern "C" in C++), __P (a prototype's
 * parameters), __THROW and __wur (nothing here), __restrict, __inline,
 * __unused, __dead2.
 */
#ifndef RITTEROS_SYS_CDEFS_H
#define RITTEROS_SYS_CDEFS_H

#include <features.h>

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#define __P(params)     params
#define __THROW
#define __wur
#ifndef __cplusplus
#define __restrict      restrict        /* (C++ compilers know __restrict themselves) */
#endif
#define __inline        inline
#define __unused        __attribute__((unused))
#define __dead2         __attribute__((noreturn))
#define __pure2         __attribute__((const))
#define __printflike(f, a) __attribute__((format(printf, f, a)))

#endif
