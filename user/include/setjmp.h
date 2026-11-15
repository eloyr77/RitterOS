/* Non-local jumps (step 22n): setjmp keeps the place of a call, longjmp goes
 * back to it from deeper calls (setjmp then returns longjmp's value, 1 for
 * 0). The function that called setjmp must not have returned; local
 * variables it changed after setjmp should be volatile. setjmp keeps no
 * signal mask, and there is no sigsetjmp. */
#ifndef RITTEROS_SETJMP_H
#define RITTEROS_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

/* The registers a call keeps (x86-64: 8 words; AArch64: 21), with room. */
typedef long jmp_buf[22];

__attribute__((returns_twice)) int setjmp(jmp_buf env);
__attribute__((noreturn)) void longjmp(jmp_buf env, int value);

/* (43a-4) POSIX's sigsetjmp/siglongjmp: setjmp/longjmp that also keep
 * (save nonzero) and put back the blocked signals; BSD's _setjmp and
 * _longjmp, which never touch them. */
typedef long sigjmp_buf[24];
__attribute__((returns_twice)) int sigsetjmp(sigjmp_buf env, int save);
__attribute__((noreturn)) void siglongjmp(sigjmp_buf env, int value);
__attribute__((returns_twice)) int _setjmp(jmp_buf env);
__attribute__((noreturn)) void _longjmp(jmp_buf env, int value);

#ifdef __cplusplus
}
#endif

#endif
