/*
 * Number, Boolean and Math (27o). toFixed, toExponential and toPrecision
 * round the double's exact decimal value, ties away from zero as the
 * language says (the C library's %f gives the exact digits; its own
 * rounding is to even).
 */
#include <math.h>
#include <time.h>
#include "jsint.h"

jsval this_number(struct js *J, jsval v, const char *method)
{
    if (js_is_number(v))
        return v;
    if (js_is_object(v) && obj_class(to_obj(v)) == CLASS_NUMBER)
        return to_obj(v)->u.prim;
    return throw_type(J, "Number.prototype.%s requires that 'this' be a Number", method);
}

static jsval n_ctor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val;
    double d = 0;
    if (argc) {
        bool threw = false;
        d = js_to_number(J, argv[0], &threw);
        if (threw)
            return JS_EXCEPTION;
    }
    if (nt == JS_UNDEFINED)
        return js_number(d);
    struct jsobject *proto = js_proto_from_ctor(J, nt, J->proto[P_NUMBER]);
    if (!proto)
        return JS_EXCEPTION;
    struct jsobject *o = obj_new(J, proto, CLASS_NUMBER);
    o->u.prim = js_number(d);
    return obj_val(o);
}

static jsval n_is_finite(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)J, (void)t, (void)c, (void)n;
    return js_bool(js_is_number(a[0]) && isfinite(js_number_value(a[0])));
}

static jsval n_is_nan(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)J, (void)t, (void)c, (void)n;
    return js_bool(js_is_number(a[0]) && js_number_value(a[0]) != js_number_value(a[0]));
}

static jsval n_is_integer(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)J, (void)t, (void)c, (void)n;
    if (!js_is_number(a[0]))
        return JS_FALSE;
    double d = js_number_value(a[0]);
    return js_bool(isfinite(d) && trunc(d) == d);
}

static jsval n_is_safe_integer(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)J, (void)t, (void)c, (void)n;
    if (!js_is_number(a[0]))
        return JS_FALSE;
    double d = js_number_value(a[0]);
    return js_bool(isfinite(d) && trunc(d) == d && fabs(d) <= 9007199254740991.0);
}

/* The exact decimal digits of x > 0 (no trailing zeros) and its point:
 * x = 0.DIGITS * 10^point. */
static int exact_digits(double x, char *digits, int *point)
{
    static char buf[1200];
    snprintf(buf, sizeof buf, "%.1100e", x);
    int k = 0;
    const char *s = buf;
    for (; *s && *s != 'e'; s++)
        if (*s >= '0' && *s <= '9')
            digits[k++] = *s;
    int e = atoi(s + 1);
    while (k > 1 && digits[k - 1] == '0')
        k--;
    digits[k] = 0;
    *point = e + 1;
    return k;
}

/* digits rounded half-up to `keep` significant digits (keep >= 0); may carry
 * into a new leading digit (point+1). Returns the digit count (keep). */
static int round_digits(char *digits, int n, int keep, int *point)
{
    if (keep >= n) {
        for (int i = n; i < keep; i++)
            digits[i] = '0';
        digits[keep] = 0;
        return keep;
    }
    bool up = keep < n && digits[keep] >= '5';
    digits[keep] = 0;
    if (up) {
        int i = keep - 1;
        while (i >= 0 && digits[i] == '9') {
            digits[i] = '0';
            i--;
        }
        if (i >= 0)
            digits[i]++;
        else {
            memmove(digits + 1, digits, (size_t)keep + 1);
            digits[0] = '1';
            digits[keep + 1] = 0;
            (*point)++;
            if (keep > 0)
                digits[keep] = 0;
        }
    }
    return keep;
}

static jsval np_to_fixed(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    jsval v = this_number(J, this_val, "toFixed");
    if (v == JS_EXCEPTION)
        return v;
    bool threw = false;
    double f = js_to_integer(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (!isfinite(f) || f < 0 || f > 100)
        return throw_range(J, "toFixed() argument must be between 0 and 100");
    double x = js_number_value(v);
    if (!isfinite(x))
        return str_val(number_to_string(J, x, 10));
    if (fabs(x) >= 1e21)
        return str_val(number_to_string(J, x, 10));
    char out[1300];
    int o = 0;
    if (x < 0) {
        out[o++] = '-';
        x = -x;
    }
    int fd = (int)f;
    if (x == 0) {
        out[o++] = '0';
        if (fd) {
            out[o++] = '.';
            for (int i = 0; i < fd; i++)
                out[o++] = '0';
        }
        out[o] = 0;
        if (out[0] == '-' && o)
            memmove(out, out + 1, (size_t)o);   /* (-0).toFixed is "0" */
        return str_val(str_from_ascii(J, out));
    }
    char digits[1200], T[1300];
    int point;
    int n = exact_digits(x, digits, &point);
    int keep = point + fd;                      /* significant digits up to the fd-th decimal */
    int total = 0;
    if (keep == 0 && digits[0] >= '5') {
        T[0] = '1';                             /* rounds up to one unit of the last place */
        total = 1;
    } else if (keep > 0) {
        n = round_digits(digits, n, keep, &point);
        total = point + fd;
        int dl = (int)strlen(digits);
        for (int i = 0; i < total; i++)
            T[i] = i < dl ? digits[i] : '0';
    }
    int ip = total - fd;                        /* digits before the point */
    if (ip <= 0) {
        out[o++] = '0';
        if (fd) {
            out[o++] = '.';
            for (int i = 0; i < fd - total; i++)
                out[o++] = '0';
            for (int i = 0; i < total; i++)
                out[o++] = T[i];
        }
    } else {
        for (int i = 0; i < ip; i++)
            out[o++] = T[i];
        if (fd) {
            out[o++] = '.';
            for (int i = ip; i < total; i++)
                out[o++] = T[i];
        }
    }
    out[o] = 0;
    return str_val(str_from_ascii(J, out));
}

static void exponential(char *out, int *o, const char *digits, int n, int point)
{
    out[(*o)++] = digits[0];
    if (n > 1) {
        out[(*o)++] = '.';
        for (int i = 1; i < n; i++)
            out[(*o)++] = digits[i];
    }
    int e = point - 1;
    *o += snprintf(out + *o, 16, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
}

static jsval np_to_exponential(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    jsval v = this_number(J, this_val, "toExponential");
    if (v == JS_EXCEPTION)
        return v;
    bool threw = false;
    double f = js_to_integer(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    double x = js_number_value(v);
    if (!isfinite(x))
        return str_val(number_to_string(J, x, 10));
    if (f < 0 || f > 100)
        return throw_range(J, "toExponential() argument must be between 0 and 100");
    char out[300], digits[1200];
    int o = 0;
    if (x < 0) {
        out[o++] = '-';
        x = -x;
    }
    int n, point;
    if (x == 0) {
        int k = argv[0] == JS_UNDEFINED ? 0 : (int)f;
        for (int i = 0; i <= k; i++)
            digits[i] = '0';
        digits[k + 1] = 0;
        n = k + 1;
        point = 1;
    } else if (argv[0] == JS_UNDEFINED)
        n = shortest_digits(x, digits, &point);
    else {
        n = exact_digits(x, digits, &point);
        n = round_digits(digits, n, (int)f + 1, &point);
    }
    exponential(out, &o, digits, n, point);
    out[o] = 0;
    return str_val(str_from_ascii(J, out));
}

static jsval np_to_precision(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    jsval v = this_number(J, this_val, "toPrecision");
    if (v == JS_EXCEPTION)
        return v;
    if (argv[0] == JS_UNDEFINED)
        return str_val(number_to_string(J, js_number_value(v), 10));
    bool threw = false;
    double p = js_to_integer(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    double x = js_number_value(v);
    if (!isfinite(x))
        return str_val(number_to_string(J, x, 10));
    if (p < 1 || p > 100)
        return throw_range(J, "toPrecision() argument must be between 1 and 100");
    int pr = (int)p;
    char out[300], digits[1200];
    int o = 0;
    if (x < 0) {
        out[o++] = '-';
        x = -x;
    }
    int n, point;
    if (x == 0) {
        for (int i = 0; i < pr; i++)
            digits[i] = '0';
        digits[pr] = 0;
        n = pr;
        point = 1;
    } else {
        n = exact_digits(x, digits, &point);
        n = round_digits(digits, n, pr, &point);
    }
    int e = point - 1;
    if (e < -6 || e >= pr) {
        exponential(out, &o, digits, n, point);
    } else if (point <= 0) {
        out[o++] = '0';
        out[o++] = '.';
        for (int i = 0; i < -point; i++)
            out[o++] = '0';
        for (int i = 0; i < n; i++)
            out[o++] = digits[i];
    } else {
        for (int i = 0; i < n; i++) {
            if (i == point)
                out[o++] = '.';
            out[o++] = digits[i];
        }
    }
    out[o] = 0;
    return str_val(str_from_ascii(J, out));
}

static jsval np_to_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    jsval v = this_number(J, this_val, "toString");
    if (v == JS_EXCEPTION)
        return v;
    double radix = 10;
    if (argv[0] != JS_UNDEFINED) {
        bool threw = false;
        radix = js_to_integer(J, argv[0], &threw);
        if (threw)
            return JS_EXCEPTION;
        if (radix < 2 || radix > 36)
            return throw_range(J, "toString() radix must be between 2 and 36");
    }
    return str_val(number_to_string(J, js_number_value(v), (int)radix));
}

static jsval np_to_locale_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    jsval v = this_number(J, this_val, "toLocaleString");
    if (v == JS_EXCEPTION)
        return v;
    return intl_number_to_locale(J, v, argv[0], argv[1]);  /* (27s) Intl.NumberFormat's */
}

static jsval np_value_of(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    return this_number(J, this_val, "valueOf");
}

/* --- Boolean ------------------------------------------------------------- */

static jsval b_ctor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc;
    jsval b = js_bool(js_to_boolean(argv[0]));
    if (nt == JS_UNDEFINED)
        return b;
    struct jsobject *proto = js_proto_from_ctor(J, nt, J->proto[P_BOOLEAN]);
    if (!proto)
        return JS_EXCEPTION;
    struct jsobject *o = obj_new(J, proto, CLASS_BOOLEAN);
    o->u.prim = b;
    return obj_val(o);
}

static jsval this_boolean(struct js *J, jsval v)
{
    if (js_is_bool(v))
        return v;
    if (js_is_object(v) && obj_class(to_obj(v)) == CLASS_BOOLEAN)
        return to_obj(v)->u.prim;
    return throw_type(J, "Boolean.prototype method requires that 'this' be a Boolean");
}

static jsval bp_to_string(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)a, (void)n;
    jsval b = this_boolean(J, t);
    if (b == JS_EXCEPTION)
        return b;
    return str_val(J->atom[b == JS_TRUE ? A_true : A_false]);
}

static jsval bp_value_of(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)a, (void)n;
    return this_boolean(J, t);
}

void init_number(struct js *J)
{
    struct jsobject *p = obj_new(J, J->proto[P_OBJECT], CLASS_NUMBER);
    p->u.prim = js_number(0);
    J->proto[P_NUMBER] = p;
    struct jsobject *c = make_ctor(J, "Number", n_ctor, 1, p);
    J->ctor[C_NUMBER] = c;
    obj_define_value(J, c, key_cstr(J, "EPSILON"), js_number(2.220446049250313e-16), 0);
    obj_define_value(J, c, key_cstr(J, "MAX_SAFE_INTEGER"), js_number(9007199254740991.0), 0);
    obj_define_value(J, c, key_cstr(J, "MIN_SAFE_INTEGER"), js_number(-9007199254740991.0), 0);
    obj_define_value(J, c, key_cstr(J, "MAX_VALUE"), js_number(1.7976931348623157e308), 0);
    obj_define_value(J, c, key_cstr(J, "MIN_VALUE"), js_number(4.9406564584124654e-324), 0);
    obj_define_value(J, c, key_cstr(J, "NaN"), js_number(NAN), 0);
    obj_define_value(J, c, key_cstr(J, "NEGATIVE_INFINITY"), js_number(-INFINITY), 0);
    obj_define_value(J, c, key_cstr(J, "POSITIVE_INFINITY"), js_number(INFINITY), 0);
    obj_define_native(J, c, "isFinite", n_is_finite, 1, 0);
    obj_define_native(J, c, "isInteger", n_is_integer, 1, 0);
    obj_define_native(J, c, "isNaN", n_is_nan, 1, 0);
    obj_define_native(J, c, "isSafeInteger", n_is_safe_integer, 1, 0);
    obj_define_native(J, p, "toExponential", np_to_exponential, 1, 0);
    obj_define_native(J, p, "toFixed", np_to_fixed, 1, 0);
    obj_define_native(J, p, "toLocaleString", np_to_locale_string, 0, 0);
    obj_define_native(J, p, "toPrecision", np_to_precision, 1, 0);
    obj_define_native(J, p, "toString", np_to_string, 1, 0);
    obj_define_native(J, p, "valueOf", np_value_of, 0, 0);

    struct jsobject *bp = obj_new(J, J->proto[P_OBJECT], CLASS_BOOLEAN);
    bp->u.prim = JS_FALSE;
    J->proto[P_BOOLEAN] = bp;
    struct jsobject *bc = make_ctor(J, "Boolean", b_ctor, 1, bp);
    J->ctor[C_BOOLEAN] = bc;
    obj_define_native(J, bp, "toString", bp_to_string, 0, 0);
    obj_define_native(J, bp, "valueOf", bp_value_of, 0, 0);
}

/* --- Math ----------------------------------------------------------------- */

static int math_args(struct js *J, int argc, jsval *argv, double *d, int want)
{
    for (int i = 0; i < want; i++) {
        bool threw = false;
        d[i] = i < argc ? js_to_number(J, argv[i], &threw) : NAN;
        if (threw)
            return -1;
    }
    return 0;
}

static double js_round(double x)
{
    if (!isfinite(x) || x == trunc(x))
        return x;
    if (x > 0 && x < 0.5)
        return 0;
    if (x < 0 && x >= -0.5)
        return -0.0;
    double r = floor(x);
    return x - r >= 0.5 ? r + 1 : r;
}

static double js_sign(double x)
{
    if (x != x || x == 0)
        return x;
    return x > 0 ? 1 : -1;
}

static uint64_t rng[2];

static double js_random(void)
{
    if (!rng[0] && !rng[1]) {
        rng[0] = (uint64_t)time(0) * 6364136223846793005ull + 1442695040888963407ull;
        rng[1] = (uint64_t)(uintptr_t)&rng ^ 0x9E3779B97F4A7C15ull;
    }
    uint64_t s1 = rng[0];
    const uint64_t s0 = rng[1];
    rng[0] = s0;
    s1 ^= s1 << 23;
    rng[1] = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);
    return (double)((rng[1] + s0) >> 11) / 9007199254740992.0;
}

static float to_float(double d)
{
    return (float)d;
}

enum {
    M_ABS, M_ACOS, M_ACOSH, M_ASIN, M_ASINH, M_ATAN, M_ATANH, M_CBRT, M_CEIL, M_CLZ32, M_COS, M_COSH,
    M_EXP, M_EXPM1, M_FLOOR, M_FROUND, M_LOG, M_LOG1P, M_LOG10, M_LOG2, M_ROUND, M_SIGN, M_SIN, M_SINH,
    M_SQRT, M_TAN, M_TANH, M_TRUNC,
};

static jsval math1(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt);

static const struct { const char *name; int op; } math1_list[] = {
    { "abs", M_ABS }, { "acos", M_ACOS }, { "acosh", M_ACOSH }, { "asin", M_ASIN }, { "asinh", M_ASINH },
    { "atan", M_ATAN }, { "atanh", M_ATANH }, { "cbrt", M_CBRT }, { "ceil", M_CEIL }, { "clz32", M_CLZ32 },
    { "cos", M_COS }, { "cosh", M_COSH }, { "exp", M_EXP }, { "expm1", M_EXPM1 }, { "floor", M_FLOOR },
    { "fround", M_FROUND }, { "log", M_LOG }, { "log1p", M_LOG1P }, { "log10", M_LOG10 }, { "log2", M_LOG2 },
    { "round", M_ROUND }, { "sign", M_SIGN }, { "sin", M_SIN }, { "sinh", M_SINH }, { "sqrt", M_SQRT },
    { "tan", M_TAN }, { "tanh", M_TANH }, { "trunc", M_TRUNC },
};

static jsval math1_op(struct js *J, int argc, jsval *argv, int op)
{
    double x;
    if (math_args(J, argc, argv, &x, 1) < 0)
        return JS_EXCEPTION;
    double r;
    switch (op) {
    case M_ABS: r = fabs(x); break;
    case M_ACOS: r = acos(x); break;
    case M_ACOSH: r = acosh(x); break;
    case M_ASIN: r = asin(x); break;
    case M_ASINH: r = asinh(x); break;
    case M_ATAN: r = atan(x); break;
    case M_ATANH: r = atanh(x); break;
    case M_CBRT: r = cbrt(x); break;
    case M_CEIL: r = ceil(x); break;
    case M_CLZ32: {
        uint32_t u = js_to_uint32(x);
        r = u ? __builtin_clz(u) : 32;
        break;
    }
    case M_COS: r = cos(x); break;
    case M_COSH: r = cosh(x); break;
    case M_EXP: r = exp(x); break;
    case M_EXPM1: r = expm1(x); break;
    case M_FLOOR: r = floor(x); break;
    case M_FROUND: r = to_float(x); break;
    case M_LOG: r = log(x); break;
    case M_LOG1P: r = log1p(x); break;
    case M_LOG10: r = log10(x); break;
    case M_LOG2: r = log2(x); break;
    case M_ROUND: r = js_round(x); break;
    case M_SIGN: r = js_sign(x); break;
    case M_SIN: r = sin(x); break;
    case M_SINH: r = sinh(x); break;
    case M_SQRT: r = sqrt(x); break;
    case M_TAN: r = tan(x); break;
    case M_TANH: r = tanh(x); break;
    default: r = trunc(x); break;
    }
    return js_number(r);
}

/* One native per function, each knowing its operation by its magic number. */
#define MATH1(fname, op) \
    static jsval fname(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t, (void)n; return math1_op(J, c, a, op); }
MATH1(m_abs, M_ABS) MATH1(m_acos, M_ACOS) MATH1(m_acosh, M_ACOSH) MATH1(m_asin, M_ASIN) MATH1(m_asinh, M_ASINH)
MATH1(m_atan, M_ATAN) MATH1(m_atanh, M_ATANH) MATH1(m_cbrt, M_CBRT) MATH1(m_ceil, M_CEIL) MATH1(m_clz32, M_CLZ32)
MATH1(m_cos, M_COS) MATH1(m_cosh, M_COSH) MATH1(m_exp, M_EXP) MATH1(m_expm1, M_EXPM1) MATH1(m_floor, M_FLOOR)
MATH1(m_fround, M_FROUND) MATH1(m_log, M_LOG) MATH1(m_log1p, M_LOG1P) MATH1(m_log10, M_LOG10) MATH1(m_log2, M_LOG2)
MATH1(m_round, M_ROUND) MATH1(m_sign, M_SIGN) MATH1(m_sin, M_SIN) MATH1(m_sinh, M_SINH) MATH1(m_sqrt, M_SQRT)
MATH1(m_tan, M_TAN) MATH1(m_tanh, M_TANH) MATH1(m_trunc, M_TRUNC)

static const js_native math1_fns[] = {
    m_abs, m_acos, m_acosh, m_asin, m_asinh, m_atan, m_atanh, m_cbrt, m_ceil, m_clz32, m_cos, m_cosh, m_exp,
    m_expm1, m_floor, m_fround, m_log, m_log1p, m_log10, m_log2, m_round, m_sign, m_sin, m_sinh, m_sqrt,
    m_tan, m_tanh, m_trunc,
};

static jsval math1(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)nt;
    return math1_op(J, argc, argv, M_ABS);
}

static jsval m_atan2(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)t, (void)n;
    double d[2];
    if (math_args(J, c, a, d, 2) < 0)
        return JS_EXCEPTION;
    return js_number(atan2(d[0], d[1]));
}

static jsval m_pow(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)t, (void)n;
    double d[2];
    if (math_args(J, c, a, d, 2) < 0)
        return JS_EXCEPTION;
    return js_number(js_pow(d[0], d[1]));
}

static jsval m_imul(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)t, (void)n;
    double d[2];
    if (math_args(J, c, a, d, 2) < 0)
        return JS_EXCEPTION;
    uint32_t x = js_to_uint32(d[0]), y = js_to_uint32(d[1]);
    return js_number((int32_t)(x * y));
}

static jsval m_hypot(struct js *J, jsval t, int argc, jsval *argv, jsval n)
{
    (void)t, (void)n;
    double sum = 0, big = 0;
    bool inf = false, nan = false;
    double vals[64];
    int k = 0;
    for (int i = 0; i < argc; i++) {
        bool threw = false;
        double x = js_to_number(J, argv[i], &threw);
        if (threw)
            return JS_EXCEPTION;
        if (isinf(x))
            inf = true;
        if (x != x)
            nan = true;
        x = fabs(x);
        if (x > big)
            big = x;
        if (k < 64)
            vals[k++] = x;
    }
    if (inf)
        return js_number(INFINITY);
    if (nan)
        return js_number(NAN);
    if (big == 0)
        return js_number(0);
    double comp = 0;
    for (int i = 0; i < k; i++) {
        double r = vals[i] / big;
        double y = r * r - comp;
        double s = sum + y;
        comp = (s - sum) - y;
        sum = s;
    }
    return js_number(sqrt(sum) * big);
}

static jsval minmax(struct js *J, int argc, jsval *argv, bool max)
{
    double r = max ? -INFINITY : INFINITY;
    bool nan = false;
    for (int i = 0; i < argc; i++) {
        bool threw = false;
        double x = js_to_number(J, argv[i], &threw);
        if (threw)
            return JS_EXCEPTION;
        if (x != x)
            nan = true;
        else if (max ? (x > r || (x == 0 && r == 0 && !signbit(x))) : (x < r || (x == 0 && r == 0 && signbit(x))))
            r = x;
    }
    return js_number(nan ? NAN : r);
}

static jsval m_max(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t, (void)n; return minmax(J, c, a, true); }
static jsval m_min(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t, (void)n; return minmax(J, c, a, false); }

static jsval m_random(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)J, (void)t, (void)c, (void)a, (void)n;
    return js_number(js_random());
}

void init_math(struct js *J)
{
    struct jsobject *m = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    static const struct { const char *name; double v; } consts[] = {
        { "E", 2.718281828459045 }, { "LN10", 2.302585092994046 }, { "LN2", 0.6931471805599453 },
        { "LOG10E", 0.4342944819032518 }, { "LOG2E", 1.4426950408889634 }, { "PI", 3.141592653589793 },
        { "SQRT1_2", 0.7071067811865476 }, { "SQRT2", 1.4142135623730951 },
    };
    for (size_t i = 0; i < sizeof consts / sizeof consts[0]; i++)
        obj_define_value(J, m, key_cstr(J, consts[i].name), js_number(consts[i].v), 0);
    for (size_t i = 0; i < sizeof math1_list / sizeof math1_list[0]; i++)
        obj_define_native(J, m, math1_list[i].name, math1_fns[math1_list[i].op], 1, 0);
    obj_define_native(J, m, "atan2", m_atan2, 2, 0);
    obj_define_native(J, m, "hypot", m_hypot, 2, 0);
    obj_define_native(J, m, "imul", m_imul, 2, 0);
    obj_define_native(J, m, "max", m_max, 2, 0);
    obj_define_native(J, m, "min", m_min, 2, 0);
    obj_define_native(J, m, "pow", m_pow, 2, 0);
    obj_define_native(J, m, "random", m_random, 0, 0);
    obj_define_value(J, m, J->symbol[SYM_TO_STRING_TAG], str_val(atom_cstr(J, "Math")), PROP_CONFIGURABLE);
    obj_define_value(J, J->global, key_cstr(J, "Math"), obj_val(m), PROP_WRITABLE | PROP_CONFIGURABLE);
    (void)math1;
    /* Number.parseFloat and parseInt are the global functions (defined later by init_global) */
}
