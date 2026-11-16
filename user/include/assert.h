/*
 * assert.h (22m): assert(expression) ends the program when the expression
 * is false, saying which and where ("assertion failed: x > 0 (game.c:42,
 * main)"), with exit code 134; NDEBUG defined before this header leaves
 * every assert out. Not guarded: the header may come again with NDEBUG
 * changed, as C allows.
 */
/* (71a) C11's static_assert (C23 and C++ have it as a keyword). */
#if !defined(__cplusplus) && !defined(static_assert) && \
    (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
#define static_assert _Static_assert
#endif

#undef assert
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#ifdef __cplusplus
extern "C" {
#endif
__attribute__((noreturn)) void ritteros_assert_failed(const char *expression, const char *file,
                                                      int line, const char *function);
#ifdef __cplusplus
}
#endif
#define assert(expression) \
    ((expression) ? (void)0 : ritteros_assert_failed(#expression, __FILE__, __LINE__, __func__))
#endif
