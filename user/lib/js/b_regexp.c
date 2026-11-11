/*
 * RegExp and its prototype (27o): exec and the Symbol methods that
 * String.prototype's match, matchAll, replace, search and split use, as
 * the specification writes them (through `exec` and `lastIndex`, so a
 * subclass sees every step), on regexp.c's matcher.
 */
#include <math.h>
#include "jsint.h"

struct regex *regex_compile(struct js *J, const uint16_t *s, uint32_t n, uint32_t flags, const char **err);
int regex_exec(struct regex *re, const uint16_t *s, uint32_t n, uint32_t start, bool sticky, int32_t *caps);
uint32_t regex_ngroups(struct regex *re);
uint32_t regex_flags(struct regex *re);
const uint16_t *regex_group_name(struct regex *re, uint32_t g, uint32_t *len);
uint32_t regex_parse_flags(const uint16_t *f, uint32_t n, bool *ok);
struct jsstring *get_substitution(struct js *J, struct jsstring *matched, struct jsstring *str, uint32_t pos,
                                  jsval *captures, uint32_t ncap, jsval groups, struct jsstring *repl);

enum { RF_G = 1, RF_I = 2, RF_M = 4, RF_S = 8, RF_U = 16, RF_Y = 32, RF_D = 64, RF_V = 128 };

static struct jsobject *this_regexp(struct js *J, jsval v, const char *method)
{
    if (js_is_object(v) && obj_class(to_obj(v)) == CLASS_REGEXP)
        return to_obj(v);
    throw_type(J, "RegExp.prototype.%s requires that 'this' be a RegExp object", method);
    return 0;
}

static jsval initialize(struct js *J, struct jsobject *o, jsval pattern, jsval flags)
{
    struct jsstring *p = pattern == JS_UNDEFINED ? J->atom[A_empty] : js_to_str(J, pattern);
    if (!p)
        return JS_EXCEPTION;
    struct jsstring *f = flags == JS_UNDEFINED ? J->atom[A_empty] : js_to_str(J, flags);
    if (!f)
        return JS_EXCEPTION;
    struct jsstring *ff = str_flat(J, f), *pf = str_flat(J, p);
    bool ok;
    uint32_t fl = regex_parse_flags(ff->chars, str_len(ff), &ok);
    if (!ok) {
        char *t = str_to_utf8(J, f, 0);
        throw_syntax(J, "Invalid flags supplied to RegExp constructor '%s'", t);
        free(t);
        return JS_EXCEPTION;
    }
    const char *err;
    struct regex *re = regex_compile(J, pf->chars, str_len(pf), fl, &err);
    if (!re) {
        char *t = str_to_utf8(J, p, 0);
        throw_syntax(J, "Invalid regular expression: /%s/: %s", t, err);
        free(t);
        return JS_EXCEPTION;
    }
    if (o->u.re.prog) {
        extern void regex_free(struct regex *);
        regex_free(o->u.re.prog);
    }
    o->u.re.prog = re;
    o->u.re.source = p;
    o->u.re.flags = f;
    struct prop *li = prop_find(&o->props, str_val(J->atom[A_lastIndex]));
    if (li) {
        if (obj_set(J, o, str_val(J->atom[A_lastIndex]), js_number(0), obj_val(o), true) < 0)
            return JS_EXCEPTION;
    } else
        prop_add(J, o, str_val(J->atom[A_lastIndex]), js_number(0), PROP_WRITABLE);
    return obj_val(o);
}

jsval regexp_create(struct js *J, jsval pattern, jsval flags, jsval nt)
{
    struct jsobject *proto = nt == JS_UNDEFINED ? J->proto[P_REGEXP] : js_proto_from_ctor(J, nt, J->proto[P_REGEXP]);
    if (!proto)
        return JS_EXCEPTION;
    struct jsobject *o = obj_new(J, proto, CLASS_REGEXP);
    return initialize(J, o, pattern, flags);
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

static jsval re_ctor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc;
    jsval pattern = argv[0], flags = argv[1];
    int isr = is_regexp(J, pattern);
    if (isr < 0)
        return JS_EXCEPTION;
    if (nt == JS_UNDEFINED) {
        nt = obj_val(J->ctor[C_REGEXP]);
        if (isr && flags == JS_UNDEFINED) {
            jsval c = js_get_value(J, pattern, str_val(J->atom[A_constructor]));
            if (c == JS_EXCEPTION)
                return c;
            if (c == nt)
                return pattern;
        }
    }
    jsval p = pattern, f = flags;
    if (js_is_object(pattern) && obj_class(to_obj(pattern)) == CLASS_REGEXP) {
        p = str_val(to_obj(pattern)->u.re.source);
        if (flags == JS_UNDEFINED)
            f = str_val(to_obj(pattern)->u.re.flags);
    } else if (isr) {
        p = js_get_value(J, pattern, str_val(J->atom[A_source]));
        if (p == JS_EXCEPTION)
            return p;
        if (flags == JS_UNDEFINED) {
            f = js_get_value(J, pattern, str_val(J->atom[A_flags]));
            if (f == JS_EXCEPTION)
                return f;
        }
    }
    return regexp_create(J, p, f, nt);
}

static jsval re_species(struct js *J, jsval this_val, int c, jsval *a, jsval n)
{
    (void)J, (void)c, (void)a, (void)n;
    return this_val;
}

/* --- exec ---------------------------------------------------------------- */

static uint32_t advance(struct jsstring *s, uint32_t i, bool unicode)
{
    if (!unicode || i + 1 >= str_len(s))
        return i + 1;
    uint16_t a = s->chars[i], b = s->chars[i + 1];
    if (a >= 0xD800 && a <= 0xDBFF && b >= 0xDC00 && b <= 0xDFFF)
        return i + 2;
    return i + 1;
}

static int get_last_index(struct js *J, struct jsobject *r, double *li)
{
    jsval v = obj_get(J, r, str_val(J->atom[A_lastIndex]), obj_val(r));
    if (v == JS_EXCEPTION)
        return -1;
    return js_to_length(J, v, li);
}

static int set_last_index(struct js *J, struct jsobject *r, double v)
{
    return obj_set(J, r, str_val(J->atom[A_lastIndex]), js_number(v), obj_val(r), true) < 0 ? -1 : 0;
}

/* RegExpBuiltinExec */
static jsval builtin_exec(struct js *J, struct jsobject *r, struct jsstring *s)
{
    struct regex *re = r->u.re.prog;
    uint32_t fl = regex_flags(re);
    bool global = fl & RF_G, sticky = fl & RF_Y, unicode = fl & (RF_U | RF_V);
    double li;
    if (get_last_index(J, r, &li) < 0)
        return JS_EXCEPTION;
    if (!global && !sticky)
        li = 0;
    struct jsstring *f = str_flat(J, s);
    uint32_t n = str_len(f);
    if (li > n) {
        if ((global || sticky) && set_last_index(J, r, 0) < 0)
            return JS_EXCEPTION;
        return JS_NULL;
    }
    uint32_t ng = regex_ngroups(re);
    int32_t small[64], *caps = ng * 2 <= 64 ? small : malloc(ng * 2 * sizeof *caps);
    if (!caps)
        abort();
    int m = regex_exec(re, f->chars, n, (uint32_t)li, sticky, caps);
    if (m <= 0) {
        if (caps != small)
            free(caps);
        if (m < 0)
            return throw_range(J, "Regular expression is too complex");
        if ((global || sticky) && set_last_index(J, r, 0) < 0)
            return JS_EXCEPTION;
        return JS_NULL;
    }
    (void)unicode;
    if ((global || sticky) && set_last_index(J, r, caps[1]) < 0) {
        if (caps != small)
            free(caps);
        return JS_EXCEPTION;
    }
    struct jsobject *a = array_new(J, ng);
    for (uint32_t g = 0; g < ng; g++)
        a->elems[g] = caps[2 * g] >= 0 && caps[2 * g + 1] >= 0
                          ? str_val(str_sub(J, f, (uint32_t)caps[2 * g], (uint32_t)caps[2 * g + 1]))
                          : JS_UNDEFINED;
    prop_add(J, a, str_val(J->atom[A_index]), js_number(caps[0]), PROP_DEFAULT);
    prop_add(J, a, str_val(J->atom[A_input]), str_val(s), PROP_DEFAULT);
    bool named = false;
    for (uint32_t g = 1; g < ng; g++) {
        uint32_t len;
        if (regex_group_name(re, g, &len))
            named = true;
    }
    jsval groups = JS_UNDEFINED;
    if (named) {
        struct jsobject *go = obj_new(J, 0, CLASS_OBJECT);
        for (uint32_t g = 1; g < ng; g++) {
            uint32_t len;
            const uint16_t *nm = regex_group_name(re, g, &len);
            if (nm)
                obj_define_value(J, go, str_val(atom_u16(J, nm, len)), a->elems[g], PROP_DEFAULT);
        }
        groups = obj_val(go);
    }
    prop_add(J, a, str_val(J->atom[A_groups]), groups, PROP_DEFAULT);
    if (fl & RF_D) {
        struct jsobject *ind = array_new(J, ng);
        for (uint32_t g = 0; g < ng; g++) {
            if (caps[2 * g] >= 0) {
                jsval pair[2] = { js_number(caps[2 * g]), js_number(caps[2 * g + 1]) };
                ind->elems[g] = js_create_from_list(J, pair, 2);
            } else
                ind->elems[g] = JS_UNDEFINED;
        }
        jsval ig = JS_UNDEFINED;
        if (named) {
            struct jsobject *go = obj_new(J, 0, CLASS_OBJECT);
            for (uint32_t g = 1; g < ng; g++) {
                uint32_t len;
                const uint16_t *nm = regex_group_name(re, g, &len);
                if (nm)
                    obj_define_value(J, go, str_val(atom_u16(J, nm, len)), ind->elems[g], PROP_DEFAULT);
            }
            ig = obj_val(go);
        }
        prop_add(J, ind, str_val(J->atom[A_groups]), ig, PROP_DEFAULT);
        prop_add(J, a, str_val(J->atom[A_indices]), obj_val(ind), PROP_DEFAULT);
    }
    if (caps != small)
        free(caps);
    return obj_val(a);
}

jsval regexp_exec(struct js *J, jsval rv, struct jsstring *s)
{
    jsval exec = js_get_value(J, rv, str_val(J->atom[A_exec]));
    if (exec == JS_EXCEPTION)
        return exec;
    if (js_is_callable(exec) &&
        !(obj_class(to_obj(exec)) == CLASS_NATIVE && to_obj(exec)->u.native.magic == 77)) {
        jsval sv = str_val(s);
        jsval r = vm_call(J, exec, rv, 1, &sv, JS_UNDEFINED);
        if (r == JS_EXCEPTION)
            return r;
        if (!js_is_object(r) && r != JS_NULL)
            return throw_type(J, "The result of RegExp exec must be an object or null");
        return r;
    }
    if (!js_is_object(rv) || obj_class(to_obj(rv)) != CLASS_REGEXP)
        return throw_type(J, "RegExp exec called on an incompatible receiver");
    return builtin_exec(J, to_obj(rv), s);
}

static jsval rp_exec(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    struct jsobject *r = this_regexp(J, this_val, "exec");
    if (!r)
        return JS_EXCEPTION;
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    return builtin_exec(J, r, s);
}

static jsval rp_test(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "RegExp.prototype.test called on a non-object");
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    jsval m = regexp_exec(J, this_val, s);
    if (m == JS_EXCEPTION)
        return m;
    return js_bool(m != JS_NULL);
}

static jsval rp_to_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "RegExp.prototype.toString called on a non-object");
    jsval src = js_get_value(J, this_val, str_val(J->atom[A_source]));
    if (src == JS_EXCEPTION)
        return src;
    struct jsstring *ss = js_to_str(J, src);
    if (!ss)
        return JS_EXCEPTION;
    jsval fl = js_get_value(J, this_val, str_val(J->atom[A_flags]));
    if (fl == JS_EXCEPTION)
        return fl;
    struct jsstring *fs = js_to_str(J, fl);
    if (!fs)
        return JS_EXCEPTION;
    struct sbuf b = { 0 };
    sb_unit(&b, '/');
    sb_str(J, &b, ss);
    sb_unit(&b, '/');
    sb_str(J, &b, fs);
    struct jsstring *r = sb_finish(J, &b);
    return r ? str_val(r) : JS_EXCEPTION;
}

static jsval rp_compile(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    struct jsobject *r = this_regexp(J, this_val, "compile");
    if (!r)
        return JS_EXCEPTION;
    jsval p = argv[0], f = argv[1];
    if (js_is_object(p) && obj_class(to_obj(p)) == CLASS_REGEXP) {
        if (f != JS_UNDEFINED)
            return throw_type(J, "Cannot supply flags when constructing one RegExp from another");
        f = str_val(to_obj(p)->u.re.flags);
        p = str_val(to_obj(p)->u.re.source);
    }
    return initialize(J, r, p, f);
}

/* flag getters: magic = the flag's bit; 0 for `flags` itself */
static jsval rp_flag(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt);

static jsval flag_getter(struct js *J, jsval this_val, uint32_t bit, const char *name)
{
    if (!js_is_object(this_val))
        return throw_type(J, "RegExp.prototype.%s getter called on a non-object", name);
    struct jsobject *o = to_obj(this_val);
    if (obj_class(o) != CLASS_REGEXP) {
        if (o == J->proto[P_REGEXP])
            return JS_UNDEFINED;
        return throw_type(J, "RegExp.prototype.%s getter called on a non-RegExp object", name);
    }
    return js_bool(regex_flags(o->u.re.prog) & bit);
}

#define FLAG_GETTER(fname, bit, name) \
    static jsval fname(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return flag_getter(J, t, bit, name); }
FLAG_GETTER(rp_global, RF_G, "global")
FLAG_GETTER(rp_ignore_case, RF_I, "ignoreCase")
FLAG_GETTER(rp_multiline, RF_M, "multiline")
FLAG_GETTER(rp_dot_all, RF_S, "dotAll")
FLAG_GETTER(rp_unicode, RF_U, "unicode")
FLAG_GETTER(rp_unicode_sets, RF_V, "unicodeSets")
FLAG_GETTER(rp_sticky, RF_Y, "sticky")
FLAG_GETTER(rp_has_indices, RF_D, "hasIndices")

static jsval rp_flag(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)J, (void)this_val, (void)argc, (void)argv, (void)nt;
    return JS_UNDEFINED;
}

static jsval rp_flags(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "RegExp.prototype.flags getter called on a non-object");
    static const struct { const char *name; char c; } order[] = {
        { "hasIndices", 'd' }, { "global", 'g' }, { "ignoreCase", 'i' }, { "multiline", 'm' },
        { "dotAll", 's' }, { "unicode", 'u' }, { "unicodeSets", 'v' }, { "sticky", 'y' },
    };
    char out[9];
    int k = 0;
    for (int i = 0; i < 8; i++) {
        jsval v = js_get_cstr(J, this_val, order[i].name);
        if (v == JS_EXCEPTION)
            return v;
        if (js_to_boolean(v))
            out[k++] = order[i].c;
    }
    out[k] = 0;
    return str_val(str_from_ascii(J, out));
}

static jsval rp_source(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "RegExp.prototype.source getter called on a non-object");
    struct jsobject *o = to_obj(this_val);
    if (obj_class(o) != CLASS_REGEXP) {
        if (o == J->proto[P_REGEXP])
            return str_val(atom_cstr(J, "(?:)"));
        return throw_type(J, "RegExp.prototype.source getter called on a non-RegExp object");
    }
    struct jsstring *src = str_flat(J, o->u.re.source);
    uint32_t n = str_len(src);
    if (n == 0)
        return str_val(atom_cstr(J, "(?:)"));
    struct sbuf b = { 0 };
    bool in_class = false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = src->chars[i];
        if (c == '\\' && i + 1 < n) {
            sb_unit(&b, c);
            sb_unit(&b, src->chars[++i]);
            continue;
        }
        if (c == '[')
            in_class = true;
        else if (c == ']')
            in_class = false;
        if (c == '/' && !in_class)
            sb_ascii(&b, "\\/");
        else if (c == '\n')
            sb_ascii(&b, "\\n");
        else if (c == '\r')
            sb_ascii(&b, "\\r");
        else if (c == 0x2028)
            sb_ascii(&b, "\\u2028");
        else if (c == 0x2029)
            sb_ascii(&b, "\\u2029");
        else
            sb_unit(&b, c);
    }
    struct jsstring *r = sb_finish(J, &b);
    return r ? str_val(r) : JS_EXCEPTION;
}

/* --- the Symbol methods --------------------------------------------------- */

static bool flags_have(struct js *J, jsval rv, char c, bool *threw)
{
    jsval f = js_get_value(J, rv, str_val(J->atom[A_flags]));
    if (f == JS_EXCEPTION) {
        *threw = true;
        return false;
    }
    struct jsstring *fs = js_to_str(J, f);
    if (!fs) {
        *threw = true;
        return false;
    }
    return str_index_of(J, fs, str_from_char(J, c), 0) >= 0;
}

static jsval rp_match(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "RegExp.prototype[Symbol.match] called on a non-object");
    struct jsobject *r = to_obj(this_val);
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    bool threw = false;
    bool global = flags_have(J, this_val, 'g', &threw);
    if (threw)
        return JS_EXCEPTION;
    if (!global)
        return regexp_exec(J, this_val, s);
    bool unicode = flags_have(J, this_val, 'u', &threw) || flags_have(J, this_val, 'v', &threw);
    if (threw)
        return JS_EXCEPTION;
    if (set_last_index(J, r, 0) < 0)
        return JS_EXCEPTION;
    struct jsobject *a = array_new(J, 0);
    struct jsstring *f = str_flat(J, s);
    for (;;) {
        jsval m = regexp_exec(J, this_val, s);
        if (m == JS_EXCEPTION)
            return m;
        if (m == JS_NULL)
            return a->length ? obj_val(a) : JS_NULL;
        jsval mv = js_get_value(J, m, str_val(J->atom[A_zero]));
        if (mv == JS_EXCEPTION)
            return mv;
        struct jsstring *ms = js_to_str(J, mv);
        if (!ms)
            return JS_EXCEPTION;
        array_grow(J, a, a->length + 1);
        a->elems[a->length++] = str_val(ms);
        if (str_len(ms) == 0) {
            double li;
            if (get_last_index(J, r, &li) < 0)
                return JS_EXCEPTION;
            if (set_last_index(J, r, li < 4294967295.0 ? advance(f, (uint32_t)li, unicode) : li + 1) < 0)
                return JS_EXCEPTION;
        }
    }
}

static jsval rp_search(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "RegExp.prototype[Symbol.search] called on a non-object");
    struct jsobject *r = to_obj(this_val);
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    jsval prev = obj_get(J, r, str_val(J->atom[A_lastIndex]), this_val);
    if (prev == JS_EXCEPTION)
        return prev;
    if (!js_same_value(J, prev, js_number(0), false) && set_last_index(J, r, 0) < 0)
        return JS_EXCEPTION;
    jsval m = regexp_exec(J, this_val, s);
    if (m == JS_EXCEPTION)
        return m;
    jsval cur = obj_get(J, r, str_val(J->atom[A_lastIndex]), this_val);
    if (cur == JS_EXCEPTION)
        return cur;
    if (!js_same_value(J, cur, prev, false) &&
        obj_set(J, r, str_val(J->atom[A_lastIndex]), prev, this_val, true) < 0)
        return JS_EXCEPTION;
    if (m == JS_NULL)
        return js_number(-1);
    return js_get_value(J, m, str_val(J->atom[A_index]));
}

static jsval rp_replace(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "RegExp.prototype[Symbol.replace] called on a non-object");
    struct jsobject *r = to_obj(this_val);
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    struct jsstring *f = str_flat(J, s);
    uint32_t n = str_len(f);
    jsval rv = argv[1];
    bool functional = js_is_callable(rv);
    struct jsstring *repl = 0;
    if (!functional && !(repl = js_to_str(J, rv)))
        return JS_EXCEPTION;
    bool threw = false;
    bool global = flags_have(J, this_val, 'g', &threw);
    bool unicode = false;
    if (!threw && global)
        unicode = flags_have(J, this_val, 'u', &threw) || flags_have(J, this_val, 'v', &threw);
    if (threw)
        return JS_EXCEPTION;
    if (global && set_last_index(J, r, 0) < 0)
        return JS_EXCEPTION;
    /* the results first (rooted in an array), then the text */
    struct jsobject *results = array_new(J, 0);
    for (;;) {
        jsval m = regexp_exec(J, this_val, s);
        if (m == JS_EXCEPTION)
            return m;
        if (m == JS_NULL)
            break;
        array_grow(J, results, results->length + 1);
        results->elems[results->length++] = m;
        if (!global)
            break;
        jsval mv = js_get_value(J, m, str_val(J->atom[A_zero]));
        if (mv == JS_EXCEPTION)
            return mv;
        struct jsstring *ms = js_to_str(J, mv);
        if (!ms)
            return JS_EXCEPTION;
        if (str_len(ms) == 0) {
            double li;
            if (get_last_index(J, r, &li) < 0)
                return JS_EXCEPTION;
            if (set_last_index(J, r, li < 4294967295.0 ? advance(f, (uint32_t)li, unicode) : li + 1) < 0)
                return JS_EXCEPTION;
        }
    }
    struct sbuf b = { 0 };
    uint32_t next = 0;
    for (uint32_t k = 0; k < results->length; k++) {
        jsval m = results->elems[k];
        double ncap;
        jsval lv = js_get_value(J, m, str_val(J->atom[A_length]));
        if (lv == JS_EXCEPTION || js_to_length(J, lv, &ncap) < 0)
            goto fail;
        ncap = ncap > 1 ? ncap - 1 : 0;
        jsval mv = js_get_value(J, m, str_val(J->atom[A_zero]));
        struct jsstring *matched = mv == JS_EXCEPTION ? 0 : js_to_str(J, mv);
        if (!matched)
            goto fail;
        jsval pv = js_get_value(J, m, str_val(J->atom[A_index]));
        if (pv == JS_EXCEPTION)
            goto fail;
        bool t = false;
        double pos = js_to_integer(J, pv, &t);
        if (t)
            goto fail;
        pos = fmax(fmin(pos, n), 0);
        uint32_t nc = ncap > 1000 ? 1000 : (uint32_t)ncap;
        jsval *caps = calloc(nc + 4, sizeof *caps);
        if (!caps)
            abort();
        js_root_push(J, caps, nc + 4);
        bool bad = false;
        for (uint32_t i = 0; i < nc && !bad; i++) {
            jsval c = js_get_value(J, m, str_val(atom_index(J, i + 1)));
            if (c == JS_EXCEPTION)
                bad = true;
            else if (c != JS_UNDEFINED) {
                struct jsstring *cs = js_to_str(J, c);
                if (!cs)
                    bad = true;
                else
                    caps[i] = str_val(cs);
            } else
                caps[i] = JS_UNDEFINED;
        }
        jsval groups = bad ? JS_EXCEPTION : js_get_value(J, m, str_val(J->atom[A_groups]));
        if (groups == JS_EXCEPTION) {
            js_root_pop(J);
            free(caps);
            goto fail;
        }
        struct jsstring *replacement;
        if (functional) {
            uint32_t na = nc + 3 + (groups != JS_UNDEFINED);
            jsval *args = calloc(na + 1, sizeof *args);
            args[0] = str_val(matched);
            for (uint32_t i = 0; i < nc; i++)
                args[1 + i] = caps[i];
            args[nc + 1] = js_number(pos);
            args[nc + 2] = str_val(s);
            if (groups != JS_UNDEFINED)
                args[nc + 3] = groups;
            js_root_push(J, args, na);
            jsval rr = vm_call(J, rv, JS_UNDEFINED, (int)na, args, JS_UNDEFINED);
            js_root_pop(J);
            free(args);
            replacement = rr == JS_EXCEPTION ? 0 : js_to_str(J, rr);
        } else {
            if (groups != JS_UNDEFINED) {
                struct jsobject *go = js_to_object(J, groups);
                groups = go ? obj_val(go) : JS_EXCEPTION;
            }
            replacement = groups == JS_EXCEPTION ? 0
                          : get_substitution(J, matched, s, (uint32_t)pos, caps, nc, groups, repl);
        }
        js_root_pop(J);
        free(caps);
        if (!replacement)
            goto fail;
        if (pos >= next) {
            sb_units(&b, f->chars + next, (uint32_t)pos - next);
            sb_str(J, &b, replacement);
            next = (uint32_t)pos + str_len(matched);
        }
    }
    if (next < n)
        sb_units(&b, f->chars + next, n - next);
    {
        struct jsstring *res = sb_finish(J, &b);
        return res ? str_val(res) : JS_EXCEPTION;
    }
fail:
    sb_free(&b);
    return JS_EXCEPTION;
}

static jsval rp_split(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "RegExp.prototype[Symbol.split] called on a non-object");
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    jsval c = js_species_constructor(J, to_obj(this_val), obj_val(J->ctor[C_REGEXP]));
    if (c == JS_EXCEPTION)
        return c;
    jsval fl = js_get_value(J, this_val, str_val(J->atom[A_flags]));
    if (fl == JS_EXCEPTION)
        return fl;
    struct jsstring *fs = js_to_str(J, fl);
    if (!fs)
        return JS_EXCEPTION;
    bool unicode = str_index_of(J, fs, str_from_char(J, 'u'), 0) >= 0 || str_index_of(J, fs, str_from_char(J, 'v'), 0) >= 0;
    struct jsstring *nf = fs;
    if (str_index_of(J, fs, str_from_char(J, 'y'), 0) < 0)
        nf = str_concat(J, fs, str_from_char(J, 'y'));
    jsval args[2] = { this_val, str_val(nf) };
    jsval splitter = vm_call(J, c, JS_UNDEFINED, 2, args, c);
    if (splitter == JS_EXCEPTION)
        return splitter;
    struct jsobject *a = array_new(J, 0);
    uint32_t lim = 0xFFFFFFFF;
    if (argv[1] != JS_UNDEFINED) {
        bool threw = false;
        lim = js_to_uint32(js_to_number(J, argv[1], &threw));
        if (threw)
            return JS_EXCEPTION;
    }
    if (lim == 0)
        return obj_val(a);
    struct jsstring *f = str_flat(J, s);
    uint32_t size = str_len(f);
    struct jsobject *sp = to_obj(splitter);
    if (size == 0) {
        jsval z = regexp_exec(J, splitter, s);
        if (z == JS_EXCEPTION)
            return z;
        if (z == JS_NULL) {
            array_grow(J, a, 1);
            a->elems[a->length++] = str_val(s);
        }
        return obj_val(a);
    }
    uint32_t p = 0, q = 0;
    while (q < size) {
        if (set_last_index(J, sp, q) < 0)
            return JS_EXCEPTION;
        jsval z = regexp_exec(J, splitter, s);
        if (z == JS_EXCEPTION)
            return z;
        if (z == JS_NULL) {
            q = advance(f, q, unicode);
            continue;
        }
        double e;
        if (get_last_index(J, sp, &e) < 0)
            return JS_EXCEPTION;
        if (e > size)
            e = size;
        if (e == p) {
            q = advance(f, q, unicode);
            continue;
        }
        array_grow(J, a, a->length + 1);
        a->elems[a->length++] = str_val(str_sub(J, f, p, q));
        if (a->length == lim)
            return obj_val(a);
        p = (uint32_t)e;
        double nc;
        jsval lv = js_get_value(J, z, str_val(J->atom[A_length]));
        if (lv == JS_EXCEPTION || js_to_length(J, lv, &nc) < 0)
            return JS_EXCEPTION;
        for (double i = 1; i < nc; i++) {
            jsval cap = js_get_value(J, z, str_val(atom_index(J, (uint32_t)i)));
            if (cap == JS_EXCEPTION)
                return cap;
            array_grow(J, a, a->length + 1);
            a->elems[a->length++] = cap;
            if (a->length == lim)
                return obj_val(a);
        }
        q = p;
    }
    array_grow(J, a, a->length + 1);
    a->elems[a->length++] = str_val(str_sub(J, f, p, size));
    return obj_val(a);
}

static jsval rp_match_all(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "RegExp.prototype[Symbol.matchAll] called on a non-object");
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    jsval c = js_species_constructor(J, to_obj(this_val), obj_val(J->ctor[C_REGEXP]));
    if (c == JS_EXCEPTION)
        return c;
    jsval fl = js_get_value(J, this_val, str_val(J->atom[A_flags]));
    if (fl == JS_EXCEPTION)
        return fl;
    struct jsstring *fs = js_to_str(J, fl);
    if (!fs)
        return JS_EXCEPTION;
    jsval args[2] = { this_val, str_val(fs) };
    jsval matcher = vm_call(J, c, JS_UNDEFINED, 2, args, c);
    if (matcher == JS_EXCEPTION)
        return matcher;
    double li;
    if (get_last_index(J, to_obj(this_val), &li) < 0 || set_last_index(J, to_obj(matcher), li) < 0)
        return JS_EXCEPTION;
    jsval it = make_iterator(J, J->proto[P_REGEXP_STRING_ITERATOR], matcher, ITER_REGEXP_STRING);
    struct jsobject *io = to_obj(it);
    io->u.iter.extra = str_val(s);
    io->u.iter.index = (str_index_of(J, fs, str_from_char(J, 'g'), 0) >= 0 ? 1 : 0) |
                       (str_index_of(J, fs, str_from_char(J, 'u'), 0) >= 0 || str_index_of(J, fs, str_from_char(J, 'v'), 0) >= 0 ? 2 : 0);
    return it;
}

static jsval rsi_next(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    if (!js_is_object(this_val) || obj_class(to_obj(this_val)) != CLASS_ITERATOR ||
        to_obj(this_val)->u.iter.kind != ITER_REGEXP_STRING)
        return throw_type(J, "%%RegExpStringIterator%%.next called on an incompatible receiver");
    struct jsobject *it = to_obj(this_val);
    if (it->u.iter.done)
        return iter_result(J, JS_UNDEFINED, true);
    struct jsstring *s = to_str(it->u.iter.extra);
    jsval m = regexp_exec(J, it->u.iter.target, s);
    if (m == JS_EXCEPTION)
        return m;
    if (m == JS_NULL) {
        it->u.iter.done = true;
        return iter_result(J, JS_UNDEFINED, true);
    }
    if (!(it->u.iter.index & 1)) {
        it->u.iter.done = true;
        return iter_result(J, m, false);
    }
    jsval mv = js_get_value(J, m, str_val(J->atom[A_zero]));
    if (mv == JS_EXCEPTION)
        return mv;
    struct jsstring *ms = js_to_str(J, mv);
    if (!ms)
        return JS_EXCEPTION;
    if (str_len(ms) == 0) {
        struct jsobject *r = to_obj(it->u.iter.target);
        double li;
        if (get_last_index(J, r, &li) < 0 ||
            set_last_index(J, r, advance(str_flat(J, s), (uint32_t)li, it->u.iter.index & 2)) < 0)
            return JS_EXCEPTION;
    }
    return iter_result(J, m, false);
}

void init_regexp(struct js *J)
{
    struct jsobject *p = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    J->proto[P_REGEXP] = p;
    struct jsobject *c = make_ctor(J, "RegExp", re_ctor, 2, p);
    J->ctor[C_REGEXP] = c;
    struct jsobject *sg = make_native(J, "get [Symbol.species]", re_species, 0, 0);
    obj_define_accessor(J, c, J->symbol[SYM_SPECIES], obj_val(sg), JS_UNDEFINED, PROP_CONFIGURABLE);
    obj_define_native(J, p, "compile", rp_compile, 2, 0);
    obj_define_native(J, p, "exec", rp_exec, 1, 77);
    obj_define_native(J, p, "test", rp_test, 1, 0);
    obj_define_native(J, p, "toString", rp_to_string, 0, 0);
    static const struct { const char *name; js_native fn; } getters[] = {
        { "dotAll", rp_dot_all }, { "flags", rp_flags }, { "global", rp_global }, { "hasIndices", rp_has_indices },
        { "ignoreCase", rp_ignore_case }, { "multiline", rp_multiline }, { "source", rp_source },
        { "sticky", rp_sticky }, { "unicode", rp_unicode }, { "unicodeSets", rp_unicode_sets },
    };
    for (size_t i = 0; i < sizeof getters / sizeof getters[0]; i++) {
        char name[40];
        snprintf(name, sizeof name, "get %s", getters[i].name);
        struct jsobject *g = make_native(J, name, getters[i].fn, 0, 0);
        obj_define_accessor(J, p, key_cstr(J, getters[i].name), obj_val(g), JS_UNDEFINED, PROP_CONFIGURABLE);
    }
    static const struct { int sym; const char *name; js_native fn; int len; } syms[] = {
        { SYM_MATCH, "[Symbol.match]", rp_match, 1 }, { SYM_MATCH_ALL, "[Symbol.matchAll]", rp_match_all, 1 },
        { SYM_REPLACE, "[Symbol.replace]", rp_replace, 2 }, { SYM_SEARCH, "[Symbol.search]", rp_search, 1 },
        { SYM_SPLIT, "[Symbol.split]", rp_split, 2 },
    };
    for (size_t i = 0; i < sizeof syms / sizeof syms[0]; i++) {
        struct jsobject *f = make_native(J, syms[i].name, syms[i].fn, syms[i].len, 0);
        obj_define_value(J, p, J->symbol[syms[i].sym], obj_val(f), PROP_WRITABLE | PROP_CONFIGURABLE);
    }
    struct jsobject *rsi = obj_new(J, J->proto[P_ITERATOR], CLASS_OBJECT);
    J->proto[P_REGEXP_STRING_ITERATOR] = rsi;
    obj_define_native(J, rsi, "next", rsi_next, 0, 0);
    obj_define_value(J, rsi, J->symbol[SYM_TO_STRING_TAG], str_val(atom_cstr(J, "RegExp String Iterator")),
                     PROP_CONFIGURABLE);
    (void)rp_flag;
}
