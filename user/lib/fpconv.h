/* Exact floating-point conversions (fpconv.c, step 22q): internal to the
 * programs' library (printf, strtod, scanf). */
#ifndef RITTEROS_FPCONV_H
#define RITTEROS_FPCONV_H

#include <stdbool.h>

/* |v| as printf's %f %e %g %a (or F E G A) with precision (-1: the
 * default) and the # flag, into out (at least 1400 bytes): the length. No
 * sign; inf and nan as "inf"/"nan". */
int fp_format(char *out, double v, char conv, int precision, bool alt);
/* C's strtod and strtof, correctly rounded (decimal and hexadecimal). */
double fp_strtod(const char *s, char **end);
float fp_strtof(const char *s, char **end);
/* A long double as the nearest double (on ARM without the compiler's helper). */
double fp_long_double(long double x);
/* A double as a long double (on ARM: binary128 built from its bits). */
long double fp_to_long_double(double d);

#endif
