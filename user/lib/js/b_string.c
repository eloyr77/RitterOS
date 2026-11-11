/*
 * String and String.prototype (27o). match, matchAll, replace, search and
 * split hand a regular expression's work to its Symbol methods (b_regexp.c).
 * Case mapping is string.c's (Unicode's tables, casetab.h); normalize
 * returns the text as it is.
 */
#include <math.h>
#include "jsint.h"

struct jsstring *this_string(struct js *J, jsval v, const char *method)
{
    if (js_is_nullish(v)) {
        throw_type(J, "String.prototype.%s called on null or undefined", method);
        return 0;
    }
    return js_to_str(J, v);
}

static jsval s_ctor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val;
    struct jsstring *s;
    if (argc == 0)
        s = J->atom[A_empty];
    else if (nt == JS_UNDEFINED && js_is_symbol(argv[0]))
        s = js_to_str_noexcept(J, argv[0]);
    else if (!(s = js_to_str(J, argv[0])))
        return JS_EXCEPTION;
    if (nt == JS_UNDEFINED)
        return str_val(s);
    struct jsobject *proto = js_proto_from_ctor(J, nt, J->proto[P_STRING]);
    if (!proto)
        return JS_EXCEPTION;
    struct jsobject *o = obj_new(J, proto, CLASS_STRING);
    o->u.prim = str_val(s);
    return obj_val(o);
}

static jsval s_from_char_code(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)nt;
    struct sbuf b = { 0 };
    for (int i = 0; i < argc; i++) {
        bool threw = false;
        double d = js_to_number(J, argv[i], &threw);
        if (threw) {
            sb_free(&b);
            return JS_EXCEPTION;
        }
        sb_unit(&b, js_to_uint32(d) & 0xFFFF);
    }
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

static jsval s_from_code_point(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)nt;
    struct sbuf b = { 0 };
    for (int i = 0; i < argc; i++) {
        bool threw = false;
        double d = js_to_number(J, argv[i], &threw);
        if (threw) {
            sb_free(&b);
            return JS_EXCEPTION;
        }
        if (d != trunc(d) || d < 0 || d > 0x10FFFF) {
            sb_free(&b);
            char buf[40];
            number_to_ascii(d, buf);
            return throw_range(J, "Invalid code point %s", buf);
        }
        sb_code_point(&b, (uint32_t)d);
    }
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

static jsval s_raw(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)nt;
    struct jsobject *cooked = js_to_object(J, argv[0]);
    if (!cooked)
        return JS_EXCEPTION;
    jsval rawv = obj_get(J, cooked, str_val(J->atom[A_raw]), argv[0]);
    if (rawv == JS_EXCEPTION)
        return rawv;
    struct jsobject *raw = js_to_object(J, rawv);
    if (!raw)
        return JS_EXCEPTION;
    double len;
    jsval l = obj_get(J, raw, str_val(J->atom[A_length]), rawv);
    if (l == JS_EXCEPTION || js_to_length(J, l, &len) < 0)
        return JS_EXCEPTION;
    struct sbuf b = { 0 };
    for (double i = 0; i < len; i++) {
        jsval seg = obj_get(J, raw, str_val(atom_index(J, (uint32_t)i)), rawv);
        struct jsstring *s = seg == JS_EXCEPTION ? 0 : js_to_str(J, seg);
        if (!s) {
            sb_free(&b);
            return JS_EXCEPTION;
        }
        sb_str(J, &b, s);
        if (i + 1 < len && i + 1 < argc) {
            struct jsstring *sub = js_to_str(J, argv[(int)i + 1]);
            if (!sub) {
                sb_free(&b);
                return JS_EXCEPTION;
            }
            sb_str(J, &b, sub);
        }
    }
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

/* --- String.prototype ------------------------------------------------------ */

#define THIS_STR(name)                                       \
    struct jsstring *S = this_string(J, this_val, name);     \
    if (!S)                                                  \
        return JS_EXCEPTION;                                 \
    struct jsstring *F = str_flat(J, S);                     \
    uint32_t N = str_len(F);                                 \
    (void)N

static jsval sp_to_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    if (js_is_string(this_val))
        return this_val;
    if (js_is_object(this_val) && obj_class(to_obj(this_val)) == CLASS_STRING)
        return to_obj(this_val)->u.prim;
    return throw_type(J, "String.prototype.toString requires that 'this' be a String");
}

static jsval sp_at(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    THIS_STR("at");
    bool threw = false;
    double k = js_to_integer(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (k < 0)
        k += N;
    if (k < 0 || k >= N)
        return JS_UNDEFINED;
    return str_val(str_from_char(J, F->chars[(uint32_t)k]));
}

static jsval sp_char_at(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    THIS_STR("charAt");
    bool threw = false;
    double k = js_to_integer(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (k < 0 || k >= N)
        return str_val(J->atom[A_empty]);
    return str_val(str_from_char(J, F->chars[(uint32_t)k]));
}

static jsval sp_char_code_at(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    THIS_STR("charCodeAt");
    bool threw = false;
    double k = js_to_integer(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (k < 0 || k >= N)
        return js_number(NAN);
    return js_number(F->chars[(uint32_t)k]);
}

static jsval sp_code_point_at(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    THIS_STR("codePointAt");
    bool threw = false;
    double k = js_to_integer(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (k < 0 || k >= N)
        return JS_UNDEFINED;
    uint32_t i = (uint32_t)k, c = F->chars[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < N && F->chars[i + 1] >= 0xDC00 && F->chars[i + 1] <= 0xDFFF)
        c = 0x10000 + ((c - 0xD800) << 10) + (F->chars[i + 1] - 0xDC00);
    return js_number(c);
}

static jsval sp_concat(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    THIS_STR("concat");
    struct jsstring *r = S;
    for (int i = 0; i < argc; i++) {
        struct jsstring *x = js_to_str(J, argv[i]);
        if (!x || !(r = str_concat(J, r, x)))
            return JS_EXCEPTION;
    }
    return str_val(r);
}

static int is_regexp(struct js *J, jsval v)
{
    if (!js_is_object(v))
        return 0;
    jsval m = obj_get(J, to_obj(v), J->symbol[SYM_MATCH], v);
    if (m == JS_EXCEPTION)
        return -1;
    if (m != JS_UNDEFINED)
        return js_to_boolean(m);
    return obj_class(to_obj(v)) == CLASS_REGEXP;
}

/* startsWith (0), endsWith (1), includes (2) */
static jsval contains(struct js *J, jsval this_val, int argc, jsval *argv, int kind)
{
    static const char *const names[] = { "startsWith", "endsWith", "includes" };
    THIS_STR(names[kind]);
    int re = is_regexp(J, argv[0]);
    if (re < 0)
        return JS_EXCEPTION;
    if (re)
        return throw_type(J, "Argument to String.prototype.%s cannot be a RegExp", names[kind]);
    struct jsstring *what = js_to_str(J, argv[0]);
    if (!what)
        return JS_EXCEPTION;
    struct jsstring *W = str_flat(J, what);
    uint32_t m = str_len(W);
    bool threw = false;
    double pos;
    if (kind == 1)
        pos = argc > 1 && argv[1] != JS_UNDEFINED ? js_to_integer(J, argv[1], &threw) : N;
    else
        pos = argc > 1 ? js_to_integer(J, argv[1], &threw) : 0;
    if (threw)
        return JS_EXCEPTION;
    pos = fmin(fmax(pos, 0), N);
    uint32_t p = (uint32_t)pos;
    if (kind == 0)
        return js_bool(p + m <= N && memcmp(F->chars + p, W->chars, (size_t)m * 2) == 0);
    if (kind == 1)
        return js_bool(m <= p && memcmp(F->chars + p - m, W->chars, (size_t)m * 2) == 0);
    return js_bool(str_index_of(J, F, W, p) >= 0);
}

static jsval sp_starts_with(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)n; return contains(J, t, c, a, 0); }
static jsval sp_ends_with(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)n; return contains(J, t, c, a, 1); }
static jsval sp_includes(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)n; return contains(J, t, c, a, 2); }

static jsval sp_index_of(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    THIS_STR("indexOf");
    struct jsstring *what = js_to_str(J, argv[0]);
    if (!what)
        return JS_EXCEPTION;
    bool threw = false;
    double pos = js_to_integer(J, argv[1], &threw);
    if (threw)
        return JS_EXCEPTION;
    pos = fmin(fmax(pos, 0), N);
    return js_number(str_index_of(J, F, what, (uint32_t)pos));
}

static jsval sp_last_index_of(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    THIS_STR("lastIndexOf");
    struct jsstring *what = js_to_str(J, argv[0]);
    if (!what)
        return JS_EXCEPTION;
    struct jsstring *W = str_flat(J, what);
    bool threw = false;
    double pos = js_to_number(J, argv[1], &threw);
    if (threw)
        return JS_EXCEPTION;
    pos = pos != pos ? INFINITY : trunc(pos);
    uint32_t m = str_len(W);
    double start = fmin(fmax(pos, 0), N);
    if (m > N)
        return js_number(-1);
    for (int64_t i = (int64_t)fmin(start, N - m); i >= 0; i--)
        if (memcmp(F->chars + i, W->chars, (size_t)m * 2) == 0)
            return js_number((double)i);
    return js_number(-1);
}

/* (27s) Intl.Collator's compare */
static jsval sp_locale_compare(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    THIS_STR("localeCompare");
    struct jsstring *that = js_to_str(J, argv[0]);
    if (!that)
        return JS_EXCEPTION;
    return intl_locale_compare(J, str_val(S), str_val(that), argv[1], argv[2]);
}

/* match (0), matchAll (1), search (2) */
static jsval regexp_call(struct js *J, jsval this_val, jsval *argv, int kind)
{
    static const char *const names[] = { "match", "matchAll", "search" };
    static const int syms[] = { SYM_MATCH, SYM_MATCH_ALL, SYM_SEARCH };
    if (js_is_nullish(this_val))
        return throw_type(J, "String.prototype.%s called on null or undefined", names[kind]);
    jsval re = argv[0];
    if (!js_is_nullish(re)) {
        if (kind == 1) {
            int isr = is_regexp(J, re);
            if (isr < 0)
                return JS_EXCEPTION;
            if (isr) {
                jsval flags = js_get_value(J, re, str_val(J->atom[A_flags]));
                if (flags == JS_EXCEPTION)
                    return flags;
                if (js_is_nullish(flags))
                    return throw_type(J, "matchAll: flags are null or undefined");
                struct jsstring *fs = js_to_str(J, flags);
                if (!fs)
                    return JS_EXCEPTION;
                if (str_index_of(J, fs, str_from_char(J, 'g'), 0) < 0)
                    return throw_type(J, "String.prototype.matchAll called with a non-global RegExp");
            }
        }
        jsval m = js_get_method(J, re, J->symbol[syms[kind]]);
        if (m == JS_EXCEPTION)
            return m;
        if (m != JS_UNDEFINED)
            return vm_call(J, m, re, 1, &this_val, JS_UNDEFINED);
    }
    struct jsstring *s = js_to_str(J, this_val);
    if (!s)
        return JS_EXCEPTION;
    jsval rx = regexp_create(J, re, kind == 1 ? str_val(str_from_char(J, 'g')) : JS_UNDEFINED, JS_UNDEFINED);
    if (rx == JS_EXCEPTION)
        return rx;
    jsval sv = str_val(s);
    jsval m = js_get_value(J, rx, J->symbol[syms[kind]]);
    if (m == JS_EXCEPTION)
        return m;
    return vm_call(J, m, rx, 1, &sv, JS_UNDEFINED);
}

static jsval sp_match(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return regexp_call(J, t, a, 0); }
static jsval sp_match_all(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return regexp_call(J, t, a, 1); }
static jsval sp_search(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return regexp_call(J, t, a, 2); }

static jsval sp_normalize(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    THIS_STR("normalize");
    if (argv[0] != JS_UNDEFINED) {
        struct jsstring *f = js_to_str(J, argv[0]);
        if (!f)
            return JS_EXCEPTION;
        if (!str_is_ascii_cstr(J, f, "NFC") && !str_is_ascii_cstr(J, f, "NFD") && !str_is_ascii_cstr(J, f, "NFKC") &&
            !str_is_ascii_cstr(J, f, "NFKD"))
            return throw_range(J, "The normalization form should be one of NFC, NFD, NFKC, NFKD.");
    }
    return str_val(S);
}

static jsval pad(struct js *J, jsval this_val, int argc, jsval *argv, bool at_start)
{
    THIS_STR(at_start ? "padStart" : "padEnd");
    double max;
    if (js_to_length(J, argv[0], &max) < 0)
        return JS_EXCEPTION;
    if (max <= N)
        return str_val(S);
    struct jsstring *fill = str_from_char(J, ' ');
    if (argc > 1 && argv[1] != JS_UNDEFINED) {
        fill = js_to_str(J, argv[1]);
        if (!fill)
            return JS_EXCEPTION;
        if (!str_len(fill))
            return str_val(S);
    }
    if (max > STR_MAX_LEN)
        return throw_range(J, "Out of memory");
    struct jsstring *FF = str_flat(J, fill);
    uint32_t need = (uint32_t)max - N, fl = str_len(FF);
    struct sbuf b = { 0 };
    if (!at_start)
        sb_units(&b, F->chars, N);
    for (uint32_t i = 0; i < need; i++)
        sb_unit(&b, FF->chars[i % fl]);
    if (at_start)
        sb_units(&b, F->chars, N);
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

static jsval sp_pad_start(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)n; return pad(J, t, c, a, true); }
static jsval sp_pad_end(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)n; return pad(J, t, c, a, false); }

static jsval sp_repeat(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    THIS_STR("repeat");
    bool threw = false;
    double n = js_to_integer(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (n < 0 || n == INFINITY)
        return throw_range(J, "String.prototype.repeat argument must be greater than or equal to 0 and not be Infinity");
    if (N == 0 || n == 0)
        return str_val(J->atom[A_empty]);
    if (n * N > STR_MAX_LEN)
        return throw_range(J, "Out of memory");
    struct sbuf b = { 0 };
    for (uint32_t i = 0; i < (uint32_t)n; i++)
        sb_units(&b, F->chars, N);
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

/* GetSubstitution: the replacement text with $$, $&, $`, $', $n, $<name> */
struct jsstring *get_substitution(struct js *J, struct jsstring *matched, struct jsstring *str, uint32_t pos,
                                  jsval *captures, uint32_t ncap, jsval groups, struct jsstring *repl)
{
    struct jsstring *R = str_flat(J, repl), *Sf = str_flat(J, str), *M = str_flat(J, matched);
    uint32_t rn = str_len(R), sn = str_len(Sf), mn = str_len(M);
    struct sbuf b = { 0 };
    for (uint32_t i = 0; i < rn; i++) {
        uint32_t c = R->chars[i];
        if (c != '$' || i + 1 >= rn) {
            sb_unit(&b, c);
            continue;
        }
        uint32_t d = R->chars[i + 1];
        if (d == '$') {
            sb_unit(&b, '$');
            i++;
        } else if (d == '&') {
            sb_units(&b, M->chars, mn);
            i++;
        } else if (d == '`') {
            sb_units(&b, Sf->chars, pos);
            i++;
        } else if (d == '\'') {
            uint32_t tail = pos + mn;
            if (tail < sn)
                sb_units(&b, Sf->chars + tail, sn - tail);
            i++;
        } else if (d >= '0' && d <= '9') {
            uint32_t idx = d - '0', used = 2;
            if (i + 2 < rn && R->chars[i + 2] >= '0' && R->chars[i + 2] <= '9') {
                uint32_t two = idx * 10 + (R->chars[i + 2] - '0');
                if (two >= 1 && two <= ncap) {
                    idx = two;
                    used = 3;
                }
            }
            if (idx >= 1 && idx <= ncap) {
                jsval cap = captures[idx - 1];
                if (cap != JS_UNDEFINED)
                    sb_str(J, &b, to_str(cap));
                i += used - 1;
            } else
                sb_unit(&b, '$');
        } else if (d == '<' && groups != JS_UNDEFINED) {
            uint32_t close = i + 2;
            while (close < rn && R->chars[close] != '>')
                close++;
            if (close >= rn) {
                sb_unit(&b, '$');
                continue;
            }
            struct jsstring *name = str_from_u16(J, R->chars + i + 2, close - i - 2);
            jsval v = js_get_value(J, groups, str_val(atom(J, name)));
            if (v == JS_EXCEPTION) {
                sb_free(&b);
                return 0;
            }
            if (v != JS_UNDEFINED) {
                struct jsstring *vs = js_to_str(J, v);
                if (!vs) {
                    sb_free(&b);
                    return 0;
                }
                sb_str(J, &b, vs);
            }
            i = close;
        } else
            sb_unit(&b, '$');
    }
    return sb_finish(J, &b);
}

static jsval replace(struct js *J, jsval this_val, jsval *argv, bool all)
{
    if (js_is_nullish(this_val))
        return throw_type(J, "String.prototype.%s called on null or undefined", all ? "replaceAll" : "replace");
    jsval pattern = argv[0], replacement = argv[1];
    if (!js_is_nullish(pattern)) {
        if (all) {
            int isr = is_regexp(J, pattern);
            if (isr < 0)
                return JS_EXCEPTION;
            if (isr) {
                jsval flags = js_get_value(J, pattern, str_val(J->atom[A_flags]));
                if (flags == JS_EXCEPTION)
                    return flags;
                struct jsstring *fs = js_is_nullish(flags) ? 0 : js_to_str(J, flags);
                if (!fs || str_index_of(J, fs, str_from_char(J, 'g'), 0) < 0)
                    return throw_type(J, "replaceAll must be called with a global RegExp");
            }
        }
        jsval m = js_get_method(J, pattern, J->symbol[SYM_REPLACE]);
        if (m == JS_EXCEPTION)
            return m;
        if (m != JS_UNDEFINED) {
            jsval args[2] = { this_val, replacement };
            return vm_call(J, m, pattern, 2, args, JS_UNDEFINED);
        }
    }
    struct jsstring *S = js_to_str(J, this_val);
    if (!S)
        return JS_EXCEPTION;
    struct jsstring *search = js_to_str(J, pattern);
    if (!search)
        return JS_EXCEPTION;
    bool functional = js_is_callable(replacement);
    struct jsstring *repl = 0;
    if (!functional && !(repl = js_to_str(J, replacement)))
        return JS_EXCEPTION;
    uint32_t sl = str_len(search), n = str_len(S);
    uint32_t adv = sl ? sl : 1;
    struct sbuf b = { 0 };
    uint32_t last = 0;
    int32_t pos = str_index_of(J, S, search, 0);
    struct jsstring *F = str_flat(J, S);
    while (pos >= 0) {
        sb_units(&b, F->chars + last, (uint32_t)pos - last);
        struct jsstring *r;
        if (functional) {
            jsval args[3] = { str_val(search), js_number(pos), str_val(S) };
            jsval v = vm_call(J, replacement, JS_UNDEFINED, 3, args, JS_UNDEFINED);
            if (v == JS_EXCEPTION || !(r = js_to_str(J, v))) {
                sb_free(&b);
                return JS_EXCEPTION;
            }
        } else if (!(r = get_substitution(J, search, S, (uint32_t)pos, 0, 0, JS_UNDEFINED, repl))) {
            sb_free(&b);
            return JS_EXCEPTION;
        }
        sb_str(J, &b, r);
        last = (uint32_t)pos + sl;
        if (!all)
            break;
        if ((uint32_t)pos + adv > n)
            break;
        pos = str_index_of(J, S, search, (uint32_t)pos + adv);
    }
    if (last == 0 && pos < 0 && b.n == 0) {
        sb_free(&b);
        return str_val(S);
    }
    if (last < n)
        sb_units(&b, F->chars + last, n - last);
    struct jsstring *r = sb_finish(J, &b);
    return r ? str_val(r) : JS_EXCEPTION;
}

static jsval sp_replace(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return replace(J, t, a, false); }
static jsval sp_replace_all(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return replace(J, t, a, true); }

static jsval sp_slice(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    THIS_STR("slice");
    bool threw = false;
    double a = js_to_integer(J, argv[0], &threw);
    double e = argc > 1 && argv[1] != JS_UNDEFINED ? js_to_integer(J, argv[1], &threw) : N;
    if (threw)
        return JS_EXCEPTION;
    a = a < 0 ? fmax(N + a, 0) : fmin(a, N);
    e = e < 0 ? fmax(N + e, 0) : fmin(e, N);
    return str_val(str_sub(J, F, (uint32_t)a, e > a ? (uint32_t)e : (uint32_t)a));
}

static jsval sp_substring(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    THIS_STR("substring");
    bool threw = false;
    double a = js_to_integer(J, argv[0], &threw);
    double e = argc > 1 && argv[1] != JS_UNDEFINED ? js_to_integer(J, argv[1], &threw) : N;
    if (threw)
        return JS_EXCEPTION;
    a = fmin(fmax(a, 0), N);
    e = fmin(fmax(e, 0), N);
    double from = fmin(a, e), to = fmax(a, e);
    return str_val(str_sub(J, F, (uint32_t)from, (uint32_t)to));
}

static jsval sp_substr(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    THIS_STR("substr");
    bool threw = false;
    double a = js_to_integer(J, argv[0], &threw);
    double len = argc > 1 && argv[1] != JS_UNDEFINED ? js_to_integer(J, argv[1], &threw) : INFINITY;
    if (threw)
        return JS_EXCEPTION;
    if (a < 0)
        a = fmax(N + a, 0);
    a = fmin(a, N);
    double e = fmin(a + fmax(len, 0), N);
    return str_val(str_sub(J, F, (uint32_t)a, e > a ? (uint32_t)e : (uint32_t)a));
}

static jsval sp_split(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (js_is_nullish(this_val))
        return throw_type(J, "String.prototype.split called on null or undefined");
    if (!js_is_nullish(argv[0])) {
        jsval m = js_get_method(J, argv[0], J->symbol[SYM_SPLIT]);
        if (m == JS_EXCEPTION)
            return m;
        if (m != JS_UNDEFINED) {
            jsval args[2] = { this_val, argv[1] };
            return vm_call(J, m, argv[0], 2, args, JS_UNDEFINED);
        }
    }
    struct jsstring *S = js_to_str(J, this_val);
    if (!S)
        return JS_EXCEPTION;
    uint32_t lim = 0xFFFFFFFF;
    if (argv[1] != JS_UNDEFINED) {
        bool threw = false;
        lim = js_to_uint32(js_to_number(J, argv[1], &threw));
        if (threw)
            return JS_EXCEPTION;
    }
    struct jsstring *sep = js_to_str(J, argv[0]);
    if (!sep)
        return JS_EXCEPTION;
    struct jsobject *a = array_new(J, 0);
    if (lim == 0)
        return obj_val(a);
    if (argv[0] == JS_UNDEFINED) {
        array_grow(J, a, 1);
        a->elems[a->length++] = str_val(S);
        return obj_val(a);
    }
    struct jsstring *F = str_flat(J, S);
    uint32_t n = str_len(F), sl = str_len(sep);
    if (sl == 0) {
        for (uint32_t i = 0; i < n && a->length < lim; i++) {
            array_grow(J, a, a->length + 1);
            a->elems[a->length++] = str_val(str_from_char(J, F->chars[i]));
        }
        return obj_val(a);
    }
    if (n == 0) {
        array_grow(J, a, 1);
        a->elems[a->length++] = str_val(S);
        return obj_val(a);
    }
    uint32_t p = 0;
    for (;;) {
        int32_t q = str_index_of(J, F, sep, p);
        if (q < 0)
            break;
        array_grow(J, a, a->length + 1);
        a->elems[a->length++] = str_val(str_sub(J, F, p, (uint32_t)q));
        if (a->length >= lim)
            return obj_val(a);
        p = (uint32_t)q + sl;
    }
    array_grow(J, a, a->length + 1);
    a->elems[a->length++] = str_val(str_sub(J, F, p, n));
    return obj_val(a);
}

/* the code point at i (a pair counts as one), its units in *len */
static uint32_t point_at(const uint16_t *s, uint32_t n, uint32_t i, uint32_t *len)
{
    uint32_t c = s[i];
    *len = 1;
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
        *len = 2;
        return 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
    }
    return c;
}

/* the code point ending before i, its units in *len */
static uint32_t point_before(const uint16_t *s, uint32_t i, uint32_t *len)
{
    uint32_t c = s[i - 1];
    *len = 1;
    if (c >= 0xDC00 && c <= 0xDFFF && i >= 2 && s[i - 2] >= 0xD800 && s[i - 2] <= 0xDBFF) {
        *len = 2;
        return 0x10000 + ((s[i - 2] - 0xD800) << 10) + (c - 0xDC00);
    }
    return c;
}

/* Unicode's Final_Sigma: a cased letter before the sigma and none after,
 * case-ignorable characters between them skipped (and an ignorable one
 * never counted as cased, as ICU) */
static bool final_sigma(const uint16_t *s, uint32_t n, uint32_t i)
{
    bool before = false;
    for (uint32_t j = i, len; j > 0; j -= len) {
        uint32_t c = point_before(s, j, &len);
        if (js_case_ignorable(c))
            continue;
        before = js_case_cased(c);
        break;
    }
    if (!before)
        return false;
    for (uint32_t j = i + 1, len; j < n; j += len) {
        uint32_t c = point_at(s, n, j, &len);
        if (js_case_ignorable(c))
            continue;
        return !js_case_cased(c);
    }
    return true;
}

/* the full, language-independent mappings (ß -> SS, İ -> i̇) by code point */
static jsval change_case(struct js *J, jsval this_val, bool upper, const char *name)
{
    THIS_STR(name);
    struct sbuf b = { 0 };
    bool changed = false;
    for (uint32_t i = 0, len; i < N; i += len) {
        uint32_t c = point_at(F->chars, N, i, &len), m[3];
        int k;
        if (!upper && c == 0x3A3) {
            m[0] = final_sigma(F->chars, N, i) ? 0x3C2 : 0x3C3;
            k = 1;
        } else
            k = js_case_full(c, upper, m);
        changed |= k != 1 || m[0] != c;
        for (int j = 0; j < k; j++) {
            if (m[j] >= 0x10000) {
                sb_unit(&b, 0xD800 + ((m[j] - 0x10000) >> 10));
                sb_unit(&b, 0xDC00 + ((m[j] - 0x10000) & 0x3FF));
            } else
                sb_unit(&b, m[j]);
        }
    }
    if (!changed) {
        sb_free(&b);
        return str_val(S);
    }
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

static jsval sp_to_lower(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return change_case(J, t, false, "toLowerCase"); }
static jsval sp_to_upper(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return change_case(J, t, true, "toUpperCase"); }

static jsval trim(struct js *J, jsval this_val, int which)   /* 1 start, 2 end, 3 both */
{
    THIS_STR("trim");
    uint32_t a = 0, e = N;
    if (which & 1)
        while (a < e && js_is_space(F->chars[a]))
            a++;
    if (which & 2)
        while (e > a && js_is_space(F->chars[e - 1]))
            e--;
    return str_val(str_sub(J, F, a, e));
}

static jsval sp_trim(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return trim(J, t, 3); }
static jsval sp_trim_start(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return trim(J, t, 1); }
static jsval sp_trim_end(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return trim(J, t, 2); }

static jsval sp_iterator(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    THIS_STR("[Symbol.iterator]");
    return make_iterator(J, J->proto[P_STRING_ITERATOR], str_val(S), ITER_STRING);
}

static jsval well_formed(struct js *J, jsval this_val, bool fix)
{
    THIS_STR(fix ? "toWellFormed" : "isWellFormed");
    struct sbuf b = { 0 };
    bool ok = true;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t c = F->chars[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < N && F->chars[i + 1] >= 0xDC00 && F->chars[i + 1] <= 0xDFFF) {
            if (fix) {
                sb_unit(&b, c);
                sb_unit(&b, F->chars[i + 1]);
            }
            i++;
            continue;
        }
        if (c >= 0xD800 && c <= 0xDFFF) {
            ok = false;
            c = 0xFFFD;
        }
        if (fix)
            sb_unit(&b, c);
    }
    if (!fix)
        return js_bool(ok);
    if (ok) {
        sb_free(&b);
        return str_val(S);
    }
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

static jsval sp_is_well_formed(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return well_formed(J, t, false); }
static jsval sp_to_well_formed(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return well_formed(J, t, true); }

/* the HTML methods (Annex B): anchor, bold, link, ... */
static jsval html(struct js *J, jsval this_val, const char *tag, const char *attr, jsval value)
{
    THIS_STR(tag);
    struct sbuf b = { 0 };
    sb_unit(&b, '<');
    sb_ascii(&b, tag);
    if (attr) {
        struct jsstring *v = js_to_str(J, value);
        if (!v) {
            sb_free(&b);
            return JS_EXCEPTION;
        }
        sb_unit(&b, ' ');
        sb_ascii(&b, attr);
        sb_ascii(&b, "=\"");
        struct jsstring *vf = str_flat(J, v);
        for (uint32_t i = 0; i < str_len(vf); i++) {
            if (vf->chars[i] == '"')
                sb_ascii(&b, "&quot;");
            else
                sb_unit(&b, vf->chars[i]);
        }
        sb_unit(&b, '"');
    }
    sb_unit(&b, '>');
    sb_units(&b, F->chars, N);
    sb_ascii(&b, "</");
    sb_ascii(&b, tag);
    sb_unit(&b, '>');
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

static jsval sp_anchor(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return html(J, t, "a", "name", a[0]); }
static jsval sp_link(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return html(J, t, "a", "href", a[0]); }
static jsval sp_bold(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return html(J, t, "b", 0, JS_UNDEFINED); }
static jsval sp_italics(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return html(J, t, "i", 0, JS_UNDEFINED); }

void init_string(struct js *J)
{
    struct jsobject *p = obj_new(J, J->proto[P_OBJECT], CLASS_STRING);
    p->u.prim = str_val(J->atom[A_empty]);
    J->proto[P_STRING] = p;
    struct jsobject *c = make_ctor(J, "String", s_ctor, 1, p);
    J->ctor[C_STRING] = c;
    obj_define_native(J, c, "fromCharCode", s_from_char_code, 1, 0);
    obj_define_native(J, c, "fromCodePoint", s_from_code_point, 1, 0);
    obj_define_native(J, c, "raw", s_raw, 1, 0);
    static const struct { const char *name; js_native fn; int len; } methods[] = {
        { "at", sp_at, 1 }, { "charAt", sp_char_at, 1 }, { "charCodeAt", sp_char_code_at, 1 },
        { "codePointAt", sp_code_point_at, 1 }, { "concat", sp_concat, 1 }, { "endsWith", sp_ends_with, 1 },
        { "includes", sp_includes, 1 }, { "indexOf", sp_index_of, 1 }, { "isWellFormed", sp_is_well_formed, 0 },
        { "lastIndexOf", sp_last_index_of, 1 }, { "localeCompare", sp_locale_compare, 1 },
        { "match", sp_match, 1 }, { "matchAll", sp_match_all, 1 }, { "normalize", sp_normalize, 0 },
        { "padEnd", sp_pad_end, 1 }, { "padStart", sp_pad_start, 1 }, { "repeat", sp_repeat, 1 },
        { "replace", sp_replace, 2 }, { "replaceAll", sp_replace_all, 2 }, { "search", sp_search, 1 },
        { "slice", sp_slice, 2 }, { "split", sp_split, 2 }, { "startsWith", sp_starts_with, 1 },
        { "substr", sp_substr, 2 }, { "substring", sp_substring, 2 }, { "toLocaleLowerCase", sp_to_lower, 0 },
        { "toLocaleUpperCase", sp_to_upper, 0 }, { "toLowerCase", sp_to_lower, 0 }, { "toString", sp_to_string, 0 },
        { "toUpperCase", sp_to_upper, 0 }, { "toWellFormed", sp_to_well_formed, 0 }, { "trim", sp_trim, 0 },
        { "trimEnd", sp_trim_end, 0 }, { "trimStart", sp_trim_start, 0 }, { "valueOf", sp_to_string, 0 },
        { "anchor", sp_anchor, 1 }, { "link", sp_link, 1 }, { "bold", sp_bold, 0 }, { "italics", sp_italics, 0 },
    };
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++)
        obj_define_native(J, p, methods[i].name, methods[i].fn, methods[i].len, 0);
    obj_define_value(J, p, key_cstr(J, "trimLeft"), obj_get(J, p, key_cstr(J, "trimStart"), obj_val(p)),
                     PROP_WRITABLE | PROP_CONFIGURABLE);
    obj_define_value(J, p, key_cstr(J, "trimRight"), obj_get(J, p, key_cstr(J, "trimEnd"), obj_val(p)),
                     PROP_WRITABLE | PROP_CONFIGURABLE);
    struct jsobject *it = make_native(J, "[Symbol.iterator]", sp_iterator, 0, 0);
    obj_define_value(J, p, J->symbol[SYM_ITERATOR], obj_val(it), PROP_WRITABLE | PROP_CONFIGURABLE);
}
