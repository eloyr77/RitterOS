/*
 * Worlds (27o): js_new sets up the heap, the atoms, the interpreter's stacks
 * and the realm's standard objects (init_* of the b_*.c files, in an order
 * where each finds what it builds on); the global functions (parseInt and
 * friends, the URI functions, eval, print and console) are here.
 */
#include <math.h>
#include "jsint.h"

void gc_free_all(struct js *J);

struct jsobject *make_native(struct js *J, const char *name, js_native fn, int length, int magic)
{
    struct jsobject *f = obj_new(J, J->proto[P_FUNCTION], CLASS_NATIVE);
    f->h.flags |= OBJ_CALLABLE;
    f->u.native.fn = fn;
    f->u.native.length = (int16_t)length;
    f->u.native.magic = (int16_t)magic;
    f->u.native.data = JS_UNDEFINED;
    prop_add(J, f, str_val(J->atom[A_length]), js_number(length), PROP_CONFIGURABLE);
    prop_add(J, f, str_val(J->atom[A_name]), str_val(atom_cstr(J, name)), PROP_CONFIGURABLE);
    return f;
}

void obj_define_native(struct js *J, struct jsobject *o, const char *name, js_native fn, int length, int magic)
{
    struct jsobject *f = make_native(J, name, fn, length, magic);
    obj_define_value(J, o, key_cstr(J, name), obj_val(f), PROP_WRITABLE | PROP_CONFIGURABLE);
}

struct jsobject *make_ctor(struct js *J, const char *name, js_native fn, int length, struct jsobject *proto)
{
    struct jsobject *f = make_native(J, name, fn, length, 0);
    f->h.flags |= OBJ_CONSTRUCTOR;
    if (proto) {
        prop_add(J, f, str_val(J->atom[A_prototype]), obj_val(proto), 0);
        obj_define_value(J, proto, str_val(J->atom[A_constructor]), obj_val(f), PROP_WRITABLE | PROP_CONFIGURABLE);
    }
    obj_define_value(J, J->global, key_cstr(J, name), obj_val(f), PROP_WRITABLE | PROP_CONFIGURABLE);
    return f;
}

double arg_integer(struct js *J, jsval v, bool *threw)
{
    return js_to_integer(J, v, threw);
}

/* --- global functions -------------------------------------------------- */

static jsval write_args(struct js *J, int argc, jsval *argv, FILE *out)
{
    for (int i = 0; i < argc; i++) {
        struct jsstring *s = js_to_str(J, argv[i]);
        if (!s)
            return JS_EXCEPTION;
        size_t n;
        char *t = str_to_utf8(J, s, &n);
        if (i)
            fputc(' ', out);
        fwrite(t, 1, n, out);
        free(t);
    }
    fputc('\n', out);
    fflush(out);
    return JS_UNDEFINED;
}

static jsval g_print(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)nt;
    return write_args(J, argc, argv, stdout);
}

static jsval g_print_err(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)nt;
    return write_args(J, argc, argv, stderr);
}

static jsval g_parse_int(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    bool threw = false;
    int32_t radix = js_to_int32(js_to_number(J, argv[1], &threw));
    if (threw)
        return JS_EXCEPTION;
    struct jsstring *f = str_flat(J, s);
    const uint16_t *u = f->chars;
    uint32_t n = str_len(f), i = 0;
    while (i < n && js_is_space(u[i]))
        i++;
    bool neg = false;
    if (i < n && (u[i] == '+' || u[i] == '-'))
        neg = u[i++] == '-';
    bool strip = true;
    if (radix != 0) {
        if (radix < 2 || radix > 36)
            return js_number(NAN);
        if (radix != 16)
            strip = false;
    } else
        radix = 10;
    if (strip && i + 1 < n && u[i] == '0' && (u[i + 1] == 'x' || u[i + 1] == 'X')) {
        i += 2;
        radix = 16;
    }
    uint32_t start = i;
    while (i < n) {
        uint32_t c = u[i];
        int d = c >= '0' && c <= '9' ? (int)(c - '0') : c >= 'a' && c <= 'z' ? (int)(c - 'a' + 10)
                : c >= 'A' && c <= 'Z' ? (int)(c - 'A' + 10) : 99;
        if (d >= radix)
            break;
        i++;
    }
    if (i == start)
        return js_number(NAN);
    double v;
    if (radix == 10) {
        /* the decimal digits through strtod: correctly rounded */
        uint32_t used;
        v = parse_float_prefix(u + start, i - start, &used);
    } else if (radix == 2 || radix == 4 || radix == 8 || radix == 16 || radix == 32) {
        uint16_t buf[4096];
        uint32_t k = i - start < 4000 ? i - start : 4000;
        /* through string_to_number's power-of-two path when it can read the radix */
        v = 0;
        if (radix == 16 || radix == 8 || radix == 2) {
            buf[0] = '0';
            buf[1] = radix == 16 ? 'x' : radix == 8 ? 'o' : 'b';
            memcpy(buf + 2, u + start, (size_t)k * 2);
            v = string_to_number(buf, k + 2);
        } else
            for (uint32_t j = start; j < i; j++) {
                uint32_t c = u[j];
                int d = c >= '0' && c <= '9' ? (int)(c - '0') : (int)((c | 0x20) - 'a' + 10);
                v = v * radix + d;
            }
    } else {
        v = 0;
        for (uint32_t j = start; j < i; j++) {
            uint32_t c = u[j];
            int d = c >= '0' && c <= '9' ? (int)(c - '0') : (int)((c | 0x20) - 'a' + 10);
            v = v * radix + d;
        }
    }
    return js_number(neg ? -v : v);
}

static jsval g_parse_float(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    struct jsstring *f = str_flat(J, s);
    uint32_t i = 0, n = str_len(f);
    while (i < n && js_is_space(f->chars[i]))
        i++;
    uint32_t used;
    double d = parse_float_prefix(f->chars + i, n - i, &used);
    return js_number(used ? d : NAN);
}

static jsval g_is_nan(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    bool threw = false;
    double d = js_to_number(J, argv[0], &threw);
    return threw ? JS_EXCEPTION : js_bool(d != d);
}

static jsval g_is_finite(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    bool threw = false;
    double d = js_to_number(J, argv[0], &threw);
    return threw ? JS_EXCEPTION : js_bool(isfinite(d));
}

static const char uri_reserved[] = ";/?:@&=+$,";
static const char uri_unescaped_extra[] = "-_.!~*'()";

static bool uri_unescaped(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && c < 128 && strchr(uri_unescaped_extra, (int)c));
}

static jsval uri_encode(struct js *J, jsval v, bool component)
{
    struct jsstring *s = js_to_str(J, v);
    if (!s)
        return JS_EXCEPTION;
    struct jsstring *f = str_flat(J, s);
    struct sbuf b = { 0 };
    uint32_t n = str_len(f);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = f->chars[i];
        if (uri_unescaped(c) || (!component && c < 128 && (strchr(uri_reserved, (int)c) || c == '#'))) {
            sb_unit(&b, c);
            continue;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) {
            sb_free(&b);
            return throw_error(J, P_URI_ERROR, "URI malformed");
        }
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= n || f->chars[i + 1] < 0xDC00 || f->chars[i + 1] > 0xDFFF) {
                sb_free(&b);
                return throw_error(J, P_URI_ERROR, "URI malformed");
            }
            c = 0x10000 + ((c - 0xD800) << 10) + (f->chars[++i] - 0xDC00);
        }
        unsigned char bytes[4];
        int k;
        if (c < 0x80)
            bytes[0] = (unsigned char)c, k = 1;
        else if (c < 0x800)
            bytes[0] = (unsigned char)(0xC0 | c >> 6), bytes[1] = (unsigned char)(0x80 | (c & 0x3F)), k = 2;
        else if (c < 0x10000)
            bytes[0] = (unsigned char)(0xE0 | c >> 12), bytes[1] = (unsigned char)(0x80 | (c >> 6 & 0x3F)),
            bytes[2] = (unsigned char)(0x80 | (c & 0x3F)), k = 3;
        else
            bytes[0] = (unsigned char)(0xF0 | c >> 18), bytes[1] = (unsigned char)(0x80 | (c >> 12 & 0x3F)),
            bytes[2] = (unsigned char)(0x80 | (c >> 6 & 0x3F)), bytes[3] = (unsigned char)(0x80 | (c & 0x3F)), k = 4;
        for (int j = 0; j < k; j++) {
            char hex[4];
            snprintf(hex, sizeof hex, "%%%02X", bytes[j]);
            sb_ascii(&b, hex);
        }
    }
    struct jsstring *r = sb_finish(J, &b);
    return r ? str_val(r) : JS_EXCEPTION;
}

static int hex1(uint32_t c)
{
    if (c >= '0' && c <= '9')
        return (int)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (int)(c - 'A' + 10);
    return -1;
}

static jsval uri_decode(struct js *J, jsval v, bool component)
{
    struct jsstring *s = js_to_str(J, v);
    if (!s)
        return JS_EXCEPTION;
    struct jsstring *f = str_flat(J, s);
    struct sbuf b = { 0 };
    uint32_t n = str_len(f);
    const uint16_t *u = f->chars;
    for (uint32_t i = 0; i < n; i++) {
        if (u[i] != '%') {
            sb_unit(&b, u[i]);
            continue;
        }
        if (i + 2 >= n || hex1(u[i + 1]) < 0 || hex1(u[i + 2]) < 0)
            goto bad;
        uint32_t byte = (uint32_t)(hex1(u[i + 1]) * 16 + hex1(u[i + 2]));
        uint32_t start = i;
        i += 2;
        if (byte < 0x80) {
            if (!component && (strchr(uri_reserved, (int)byte) || byte == '#') && byte)
                sb_units(&b, u + start, 3);
            else
                sb_unit(&b, byte);
            continue;
        }
        int need = (byte & 0xE0) == 0xC0 ? 1 : (byte & 0xF0) == 0xE0 ? 2 : (byte & 0xF8) == 0xF0 ? 3 : -1;
        if (need < 0)
            goto bad;
        uint32_t cp = byte & (0x3F >> need);
        for (int k = 0; k < need; k++) {
            if (i + 3 >= n || u[i + 1] != '%' || hex1(u[i + 2]) < 0 || hex1(u[i + 3]) < 0)
                goto bad;
            uint32_t cb = (uint32_t)(hex1(u[i + 2]) * 16 + hex1(u[i + 3]));
            if ((cb & 0xC0) != 0x80)
                goto bad;
            cp = cp << 6 | (cb & 0x3F);
            i += 3;
        }
        static const uint32_t min[] = { 0, 0x80, 0x800, 0x10000 };
        if (cp < min[need] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            goto bad;
        sb_code_point(&b, cp);
    }
    {
        struct jsstring *r = sb_finish(J, &b);
        return r ? str_val(r) : JS_EXCEPTION;
    }
bad:
    sb_free(&b);
    return throw_error(J, P_URI_ERROR, "URI malformed");
}

/* escape and unescape (Annex B) */
static jsval g_escape(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    struct jsstring *f = str_flat(J, s);
    struct sbuf b = { 0 };
    for (uint32_t i = 0; i < str_len(f); i++) {
        uint32_t c = f->chars[i];
        char buf[8];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && c < 128 && strchr("@*_+-./", (int)c)))
            sb_unit(&b, c);
        else {
            snprintf(buf, sizeof buf, c < 256 ? "%%%02X" : "%%u%04X", c);
            sb_ascii(&b, buf);
        }
    }
    struct jsstring *r = sb_finish(J, &b);
    return r ? str_val(r) : JS_EXCEPTION;
}

static jsval g_unescape(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    struct jsstring *f = str_flat(J, s);
    const uint16_t *u = f->chars;
    uint32_t n = str_len(f);
    struct sbuf b = { 0 };
    for (uint32_t i = 0; i < n; i++) {
        if (u[i] == '%') {
            if (i + 5 < n + 0 && u[i + 1] == 'u' && hex1(u[i + 2]) >= 0 && hex1(u[i + 3]) >= 0 && hex1(u[i + 4]) >= 0 &&
                hex1(u[i + 5]) >= 0) {
                sb_unit(&b, (uint32_t)(hex1(u[i + 2]) << 12 | hex1(u[i + 3]) << 8 | hex1(u[i + 4]) << 4 | hex1(u[i + 5])));
                i += 5;
                continue;
            }
            if (i + 2 < n && hex1(u[i + 1]) >= 0 && hex1(u[i + 2]) >= 0) {
                sb_unit(&b, (uint32_t)(hex1(u[i + 1]) << 4 | hex1(u[i + 2])));
                i += 2;
                continue;
            }
        }
        sb_unit(&b, u[i]);
    }
    struct jsstring *r = sb_finish(J, &b);
    return r ? str_val(r) : JS_EXCEPTION;
}

static jsval g_eval(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    if (!js_is_string(argv[0]))
        return argv[0];
    size_t n;
    char *text = str_to_utf8(J, to_str(argv[0]), &n);
    struct funcdef *def = js_compile(J, text, n, "eval", true);
    free(text);
    if (!def)
        return JS_EXCEPTION;
    return vm_run_script(J, def);
}

static jsval g_gc(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)argv, (void)nt;
    js_gc(J);
    return JS_UNDEFINED;
}

static jsval uri_dispatch(struct js *J, int magic, jsval v)
{
    switch (magic) {
    case 0: return uri_encode(J, v, false);
    case 1: return uri_encode(J, v, true);
    case 2: return uri_decode(J, v, false);
    default: return uri_decode(J, v, true);
    }
}

static jsval g_encode_uri(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t, (void)c, (void)n; return uri_dispatch(J, 0, a[0]); }
static jsval g_encode_uri_c(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t, (void)c, (void)n; return uri_dispatch(J, 1, a[0]); }
static jsval g_decode_uri(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t, (void)c, (void)n; return uri_dispatch(J, 2, a[0]); }
static jsval g_decode_uri_c(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t, (void)c, (void)n; return uri_dispatch(J, 3, a[0]); }

void init_global(struct js *J)
{
    struct jsobject *g = J->global;
    obj_define_value(J, g, str_val(J->atom[A_globalThis]), obj_val(g), PROP_WRITABLE | PROP_CONFIGURABLE);
    obj_define_value(J, g, str_val(J->atom[A_NaN]), js_number(NAN), 0);
    obj_define_value(J, g, str_val(J->atom[A_Infinity]), js_number(INFINITY), 0);
    obj_define_value(J, g, str_val(J->atom[A_undefined]), JS_UNDEFINED, 0);
    obj_define_native(J, g, "parseInt", g_parse_int, 2, 0);
    obj_define_native(J, g, "parseFloat", g_parse_float, 1, 0);
    obj_define_native(J, g, "isNaN", g_is_nan, 1, 0);
    obj_define_native(J, g, "isFinite", g_is_finite, 1, 0);
    obj_define_native(J, g, "encodeURI", g_encode_uri, 1, 0);
    obj_define_native(J, g, "encodeURIComponent", g_encode_uri_c, 1, 0);
    obj_define_native(J, g, "decodeURI", g_decode_uri, 1, 0);
    obj_define_native(J, g, "decodeURIComponent", g_decode_uri_c, 1, 0);
    obj_define_native(J, g, "eval", g_eval, 1, 0);
    obj_define_native(J, g, "escape", g_escape, 1, 0);
    obj_define_native(J, g, "unescape", g_unescape, 1, 0);
    J->eval_fn = obj_get(J, g, str_val(J->atom[A_eval]), obj_val(g));
    /* Number.parseFloat and Number.parseInt are the same functions */
    obj_define_value(J, J->ctor[C_NUMBER], key_cstr(J, "parseFloat"), obj_get(J, g, key_cstr(J, "parseFloat"), obj_val(g)),
                     PROP_WRITABLE | PROP_CONFIGURABLE);
    obj_define_value(J, J->ctor[C_NUMBER], key_cstr(J, "parseInt"), obj_get(J, g, key_cstr(J, "parseInt"), obj_val(g)),
                     PROP_WRITABLE | PROP_CONFIGURABLE);
    obj_define_native(J, g, "print", g_print, 0, 0);
    obj_define_native(J, g, "gc", g_gc, 0, 0);
    struct jsobject *console = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    obj_define_native(J, console, "log", g_print, 0, 0);
    obj_define_native(J, console, "info", g_print, 0, 0);
    obj_define_native(J, console, "debug", g_print, 0, 0);
    obj_define_native(J, console, "warn", g_print_err, 0, 0);
    obj_define_native(J, console, "error", g_print_err, 0, 0);
    obj_define_value(J, g, key_cstr(J, "console"), obj_val(console), PROP_WRITABLE | PROP_CONFIGURABLE);
}

/* --- the world ------------------------------------------------------------ */

static const char *const atom_texts[] = {
#define A(name, text) text,
#include "atoms.def"
#undef A
};

static jsval proto_function_call(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)J, (void)this_val, (void)argc, (void)argv, (void)nt;
    return JS_UNDEFINED;
}

struct js *js_new(void)
{
    struct js *J = calloc(1, sizeof *J);
    if (!J)
        return 0;
    J->threshold = 8u << 20;
    J->gc_inhibit = 1;
    J->exception = JS_UNDEFINED;
    J->jobs = JS_UNDEFINED;
    for (int i = 0; i < A_COUNT; i++)
        J->atom[i] = atom_cstr(J, atom_texts[i]);
    J->stack = malloc((size_t)VALUE_STACK * sizeof(jsval));
    J->frames = malloc((size_t)(FRAME_MAX + 1) * sizeof *J->frames);
    if (!J->stack || !J->frames)
        abort();
    J->stack_end = J->stack + VALUE_STACK;
    J->sp_high = J->stack;
    J->proto[P_OBJECT] = obj_new(J, 0, CLASS_OBJECT);
    struct jsobject *fp = obj_new(J, J->proto[P_OBJECT], CLASS_NATIVE);
    fp->h.flags |= OBJ_CALLABLE;
    fp->u.native.fn = proto_function_call;
    fp->u.native.data = JS_UNDEFINED;
    J->proto[P_FUNCTION] = fp;
    prop_add(J, fp, str_val(J->atom[A_length]), js_number(0), PROP_CONFIGURABLE);
    prop_add(J, fp, str_val(J->atom[A_name]), str_val(J->atom[A_empty]), PROP_CONFIGURABLE);
    J->global = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    J->global_lex = obj_new(J, 0, CLASS_OBJECT);
    J->symbol_registry = obj_new(J, 0, CLASS_OBJECT);
    init_symbol(J);
    init_object(J);
    init_function(J);
    init_error(J);
    init_iterator(J);
    init_array(J);
    init_string(J);
    init_number(J);
    init_math(J);
    init_json(J);
    init_regexp(J);
    init_map(J);
    init_date(J);
    init_generator(J);                          /* (27r) */
    init_proxy(J);                              /* (64r) */
    init_global(J);
    init_intl(J);                               /* (27s) */
    J->gc_inhibit = 0;
    return J;
}

void js_free(struct js *J)
{
    if (!J)
        return;
    gc_free_all(J);
    free(J->atoms);
    free(J->stack);
    free(J->frames);
    free(J->handlers);
    free(J->visiting);
    free(J);
}

jsval js_eval(struct js *J, const char *text, size_t len, const char *file)
{
    struct funcdef *def = js_compile(J, text, len, file, false);
    if (!def)
        return JS_EXCEPTION;
    return vm_run_script(J, def);
}

jsval js_global(struct js *J)
{
    return obj_val(J->global);
}

jsval js_new_object(struct js *J)
{
    return obj_val(obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT));
}

jsval js_new_array(struct js *J, uint32_t length)
{
    return obj_val(array_new(J, length));
}

jsval js_new_function(struct js *J, const char *name, js_native fn, int length)
{
    return obj_val(make_native(J, name, fn, length, 0));
}

jsval js_new_error(struct js *J, const char *kind, const char *message)
{
    int p = P_ERROR;
    if (!strcmp(kind, "TypeError")) p = P_TYPE_ERROR;
    else if (!strcmp(kind, "RangeError")) p = P_RANGE_ERROR;
    else if (!strcmp(kind, "ReferenceError")) p = P_REFERENCE_ERROR;
    else if (!strcmp(kind, "SyntaxError")) p = P_SYNTAX_ERROR;
    return error_create(J, J->proto[p], str_val(str_from_utf8(J, message, strlen(message))));
}

jsval js_string(struct js *J, const char *utf8, size_t len)
{
    return str_val(str_from_utf8(J, utf8, len));
}

jsval js_cstring(struct js *J, const char *utf8)
{
    return str_val(str_from_utf8(J, utf8, strlen(utf8)));
}

char *js_to_utf8(struct js *J, jsval v, size_t *len)
{
    struct jsstring *s = js_to_str(J, v);
    if (!s)
        return 0;
    return str_to_utf8(J, s, len);
}

jsval js_get(struct js *J, jsval obj, const char *name)
{
    return js_get_value(J, obj, str_val(atom(J, str_from_utf8(J, name, strlen(name)))));
}

int js_set(struct js *J, jsval obj, const char *name, jsval value)
{
    return js_set_value(J, obj, str_val(atom(J, str_from_utf8(J, name, strlen(name)))), value, true) < 0 ? -1 : 0;
}

jsval js_get_index(struct js *J, jsval obj, uint32_t index)
{
    return js_get_value(J, obj, str_val(atom_index(J, index)));
}

int js_set_index(struct js *J, jsval obj, uint32_t index, jsval value)
{
    return js_set_value(J, obj, str_val(atom_index(J, index)), value, true) < 0 ? -1 : 0;
}

void js_define_method(struct js *J, jsval obj, const char *name, js_native fn, int length)
{
    if (js_is_object(obj))
        obj_define_native(J, to_obj(obj), name, fn, length, 0);
}

void js_define_value(struct js *J, jsval obj, const char *name, jsval value)
{
    if (js_is_object(obj))
        obj_define_value(J, to_obj(obj), str_val(atom(J, str_from_utf8(J, name, strlen(name)))), value,
                         PROP_WRITABLE | PROP_CONFIGURABLE);
}

char *js_exception_text(struct js *J, jsval e)
{
    struct sbuf b = { 0 };
    J->gc_inhibit++;
    if (js_is_object(e) && obj_class(to_obj(e)) == CLASS_ERROR) {
        struct jsobject *o = to_obj(e);
        jsval name = obj_get(J, o, str_val(J->atom[A_name]), e);
        jsval msg = obj_get(J, o, str_val(J->atom[A_message]), e);
        struct jsstring *n = name == JS_EXCEPTION ? 0 : js_to_str_noexcept(J, name);
        struct jsstring *m = msg == JS_EXCEPTION ? 0 : js_to_str_noexcept(J, msg);
        if (n)
            sb_str(J, &b, n);
        if (m && str_len(m)) {
            sb_ascii(&b, ": ");
            sb_str(J, &b, m);
        }
        if (o->u.err.file && o->u.err.line) {
            char buf[32];
            sb_ascii(&b, " at ");
            sb_str(J, &b, o->u.err.file);
            snprintf(buf, sizeof buf, ":%d", o->u.err.line);
            sb_ascii(&b, buf);
        }
    } else {
        struct jsstring *s = js_to_str_noexcept(J, e);
        if (s)
            sb_str(J, &b, s);
    }
    J->has_exception = false;
    J->exception = JS_UNDEFINED;
    struct jsstring *s = str_from_u16(J, b.s ? b.s : (uint16_t *)"", b.n);
    sb_free(&b);
    char *t = str_to_utf8(J, s, 0);
    J->gc_inhibit--;
    return t;
}

/* --- (27p) what programs build their own objects with ------------------- */

jsval js_new_host(struct js *J, jsval proto, void *data, const struct js_host_class *cls)
{
    struct jsobject *o = obj_new(J, js_is_object(proto) ? to_obj(proto) : 0, CLASS_HOST);
    o->u.host.data = data;
    o->u.host.cls = cls;
    return obj_val(o);
}

void *js_host_data(jsval v, const struct js_host_class *cls)
{
    if (!js_is_object(v) || obj_class(to_obj(v)) != CLASS_HOST || to_obj(v)->u.host.cls != cls)
        return 0;
    return to_obj(v)->u.host.data;
}

void js_mark(struct js *J, jsval v)
{
    gc_mark_value(J, v);
}

jsval js_new_object_proto(struct js *J, jsval proto)
{
    return obj_val(obj_new(J, js_is_object(proto) ? to_obj(proto) : 0, CLASS_OBJECT));
}

jsval js_get_proto(struct js *J, jsval obj)
{
    (void)J;
    if (!js_is_object(obj) || !to_obj(obj)->proto)
        return JS_NULL;
    return obj_val(to_obj(obj)->proto);
}

void js_define_accessor_magic(struct js *J, jsval obj, const char *name, js_native get, js_native set, int magic)
{
    if (!js_is_object(obj))
        return;
    char buf[80];
    snprintf(buf, sizeof buf, "get %s", name);
    jsval g = get ? obj_val(make_native(J, buf, get, 0, magic)) : JS_UNDEFINED;
    snprintf(buf, sizeof buf, "set %s", name);
    jsval s = set ? obj_val(make_native(J, buf, set, 1, magic)) : JS_UNDEFINED;
    obj_define_accessor(J, to_obj(obj), str_val(atom(J, str_from_utf8(J, name, strlen(name)))), g, s,
                        PROP_ENUMERABLE | PROP_CONFIGURABLE);
}

void js_define_accessor(struct js *J, jsval obj, const char *name, js_native get, js_native set)
{
    js_define_accessor_magic(J, obj, name, get, set, 0);
}

jsval js_new_function_magic(struct js *J, const char *name, js_native fn, int length, int magic)
{
    return obj_val(make_native(J, name, fn, length, magic));
}

void js_define_method_magic(struct js *J, jsval obj, const char *name, js_native fn, int length, int magic)
{
    if (js_is_object(obj))
        obj_define_native(J, to_obj(obj), name, fn, length, magic);
}

int js_magic(struct js *J)
{
    return J->native_magic;
}

jsval js_new_constructor(struct js *J, const char *name, js_native fn, int length, jsval proto)
{
    struct jsobject *f = make_native(J, name, fn, length, 0);
    f->h.flags |= OBJ_CONSTRUCTOR;
    if (js_is_object(proto)) {
        prop_add(J, f, str_val(J->atom[A_prototype]), proto, 0);
        obj_define_value(J, to_obj(proto), str_val(J->atom[A_constructor]), obj_val(f),
                         PROP_WRITABLE | PROP_CONFIGURABLE);
    }
    return obj_val(f);
}

int js_delete(struct js *J, jsval obj, const char *name)
{
    if (!js_is_object(obj))
        return 0;
    return obj_delete(J, to_obj(obj), str_val(atom(J, str_from_utf8(J, name, strlen(name)))), false);
}

bool js_is_array(jsval v)
{
    return js_is_object(v) && obj_class(to_obj(v)) == CLASS_ARRAY;
}

uint32_t js_array_length(jsval v)
{
    return js_is_array(v) ? to_obj(v)->length : 0;
}

jsval js_throw_named(struct js *J, const char *name, const char *message)
{
    jsval e = error_create(J, J->proto[P_ERROR], str_val(str_from_utf8(J, message, strlen(message))));
    if (js_is_object(e))
        obj_define_value(J, to_obj(e), str_val(J->atom[A_name]), str_val(str_from_utf8(J, name, strlen(name))),
                         PROP_WRITABLE | PROP_CONFIGURABLE);
    return js_throw(J, e);
}

/* --- 27q: keys of the program's own, and jobs ----------------------------- */

jsval js_new_private_key(struct js *J, const char *description)
{
    struct jsstring *d = str_from_utf8(J, description, strlen(description));
    struct jssymbol *s = gc_alloc(J, CELL_SYMBOL, sizeof *s);
    if (!s)
        abort();
    s->description = d;
    s->hash = ++J->symbol_count * 2654435761u;
    s->h.flags = 2;                             /* (a private name: in no list of keys) */
    return js_tagged(JS_T_SYMBOL, s);
}

jsval js_get_key(struct js *J, jsval obj, jsval key)
{
    return js_get_value(J, obj, key);
}

int js_set_key(struct js *J, jsval obj, jsval key, jsval value)
{
    return js_set_value(J, obj, key, value, true) < 0 ? -1 : 0;
}

void js_queue_job(struct js *J, jsval fn, int argc, jsval *argv)
{
    jsval job = js_new_array(J, 0);
    js_set_index(J, job, 0, fn);
    for (int i = 0; i < argc; i++)
        js_set_index(J, job, (uint32_t)i + 1, argv[i]);
    if (!js_is_array(J->jobs)) {
        J->jobs = js_new_array(J, 0);
        J->jobs_head = 0;
    }
    js_set_index(J, J->jobs, js_array_length(J->jobs), job);
}

bool js_jobs_pending(struct js *J)
{
    return js_is_array(J->jobs) && J->jobs_head < js_array_length(J->jobs);
}

jsval js_run_jobs(struct js *J)
{
    while (js_jobs_pending(J)) {
        jsval job = js_get_index(J, J->jobs, J->jobs_head);
        js_set_index(J, J->jobs, J->jobs_head, JS_UNDEFINED);
        J->jobs_head++;
        if (J->jobs_head == js_array_length(J->jobs))
            J->jobs = JS_UNDEFINED, J->jobs_head = 0;   /* (all taken: a fresh queue for what comes) */
        uint32_t n = js_array_length(job);
        jsval args[16];
        int argc = 0;
        for (uint32_t i = 1; i < n && argc < 16; i++)
            args[argc++] = js_get_index(J, job, i);
        jsval r = js_call(J, js_get_index(J, job, 0), JS_UNDEFINED, argc, args);
        if (r == JS_EXCEPTION)
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}
