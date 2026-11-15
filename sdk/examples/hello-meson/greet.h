/* hello-meson's own shared library (libgreet.so.1). */
#ifndef GREET_H
#define GREET_H

#ifdef __cplusplus
extern "C" {
#endif

int greet(char *out, int size, const char *name);

#ifdef __cplusplus
}
#endif

#endif
