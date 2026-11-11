/*
 * assert.h (22m): assert(expression) ends the program when the expression
 * is false, saying which and where ("assertion failed: x > 0 (game.c:42,
 * main)"), with exit code 134; NDEBUG defined before this header leaves
 * every assert out. Not guarded: the header may come again with NDEBUG
 * changed, as C allows.
 */
#undef assert
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
__attribute__((noreturn)) void ritteros_assert_failed(const char *expression, const char *file,
                                                      int line, const char *function);
#define assert(expression) \
    ((expression) ? (void)0 : ritteros_assert_failed(#expression, __FILE__, __LINE__, __func__))
#endif
