/*
 * JSON.parse and JSON.stringify (27o), by ECMA-404's grammar and the
 * language's SerializeJSONProperty (toJSON, replacers, gaps, lone
 * surrogates written as \uXXXX, cycles a TypeError).
 */
#include <math.h>
#include "jsint.h"

/* --- parse --------------------------------------------------------------- */

struct jparse {
    struct js *J;
    const uint16_t *s;
    uint32_t n, i;
    int depth;
};

static jsval jerr(struct jparse *P, const char *what)
{
    if (P->i >= P->n)
        return throw_syntax(P->J, "JSON Parse error: Unexpected EOF");
    return throw_syntax(P->J, "JSON Parse error: %s", what);
}

static void jspace(struct jparse *P)
{
    while (P->i < P->n && (P->s[P->i] == ' ' || P->s[P->i] == '\t' || P->s[P->i] == '\n' || P->s[P->i] == '\r'))
        P->i++;
}

static jsval jvalue(struct jparse *P);

static struct jsstring *jstring(struct jparse *P)
{
    struct js *J = P->J;
    P->i++;                                     /* the quote */
    struct sbuf b = { 0 };
    for (;;) {
        if (P->i >= P->n) {
            sb_free(&b);
            jerr(P, "Unterminated string");
            return 0;
        }
        uint32_t c = P->s[P->i++];
        if (c == '"')
            break;
        if (c < 0x20) {
            sb_free(&b);
            jerr(P, "Unterminated string");
            return 0;
        }
        if (c != '\\') {
            sb_unit(&b, c);
            continue;
        }
        if (P->i >= P->n) {
            sb_free(&b);
            jerr(P, "Unterminated string");
            return 0;
        }
        c = P->s[P->i++];
        switch (c) {
        case '"': case '\\': case '/': sb_unit(&b, c); break;
        case 'b': sb_unit(&b, '\b'); break;
        case 'f': sb_unit(&b, '\f'); break;
        case 'n': sb_unit(&b, '\n'); break;
        case 'r': sb_unit(&b, '\r'); break;
        case 't': sb_unit(&b, '\t'); break;
        case 'u': {
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t h = P->i < P->n ? P->s[P->i] : 0;
                int d = h >= '0' && h <= '9' ? (int)(h - '0') : h >= 'a' && h <= 'f' ? (int)(h - 'a' + 10)
                        : h >= 'A' && h <= 'F' ? (int)(h - 'A' + 10) : -1;
                if (d < 0) {
                    sb_free(&b);
                    jerr(P, "\\u must be followed by 4 hex digits");
                    return 0;
                }
                v = v * 16 + (uint32_t)d;
                P->i++;
            }
            sb_unit(&b, v);
            break;
        }
        default:
            sb_free(&b);
            jerr(P, "Invalid escape character");
            return 0;
        }
    }
    struct jsstring *s = sb_finish(J, &b);
    return s;
}

static jsval jnumber(struct jparse *P)
{
    uint32_t start = P->i;
    if (P->s[P->i] == '-')
        P->i++;
    if (P->i >= P->n)
        return jerr(P, "Invalid number");
    if (P->s[P->i] == '0')
        P->i++;
    else if (P->s[P->i] >= '1' && P->s[P->i] <= '9')
        while (P->i < P->n && P->s[P->i] >= '0' && P->s[P->i] <= '9')
            P->i++;
    else
        return jerr(P, "Invalid number");
    if (P->i < P->n && P->s[P->i] == '.') {
        P->i++;
        if (!(P->i < P->n && P->s[P->i] >= '0' && P->s[P->i] <= '9'))
            return jerr(P, "Invalid digits after decimal point");
        while (P->i < P->n && P->s[P->i] >= '0' && P->s[P->i] <= '9')
            P->i++;
    }
    if (P->i < P->n && (P->s[P->i] == 'e' || P->s[P->i] == 'E')) {
        P->i++;
        if (P->i < P->n && (P->s[P->i] == '+' || P->s[P->i] == '-'))
            P->i++;
        if (!(P->i < P->n && P->s[P->i] >= '0' && P->s[P->i] <= '9'))
            return jerr(P, "Exponent symbols should be followed by an optional '+' or '-' and then by at least one number");
        while (P->i < P->n && P->s[P->i] >= '0' && P->s[P->i] <= '9')
            P->i++;
    }
    uint32_t used;
    double d = parse_float_prefix(P->s + start, P->i - start, &used);
    return js_number(d);
}

static bool jword(struct jparse *P, const char *w)
{
    size_t n = strlen(w);
    if (P->i + n > P->n)
        return false;
    for (size_t k = 0; k < n; k++)
        if (P->s[P->i + k] != (uint16_t)w[k])
            return false;
    P->i += (uint32_t)n;
    return true;
}

static jsval jvalue(struct jparse *P)
{
    struct js *J = P->J;
    jspace(P);
    if (P->i >= P->n)
        return jerr(P, "Unexpected EOF");
    if (++P->depth > 4000)
        return throw_range(J, "JSON nesting too deep");
    uint32_t c = P->s[P->i];
    jsval r;
    if (c == '{') {
        P->i++;
        struct jsobject *o = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
        r = obj_val(o);
        jspace(P);
        if (P->i < P->n && P->s[P->i] == '}')
            P->i++;
        else
            for (;;) {
                jspace(P);
                if (P->i >= P->n || P->s[P->i] != '"') {
                    r = jerr(P, "Expected '\"' before a property name");
                    break;
                }
                struct jsstring *k = jstring(P);
                if (!k) {
                    r = JS_EXCEPTION;
                    break;
                }
                jspace(P);
                if (P->i >= P->n || P->s[P->i] != ':') {
                    r = jerr(P, "Expected ':' before value in object property definition");
                    break;
                }
                P->i++;
                jsval v = jvalue(P);
                if (v == JS_EXCEPTION) {
                    r = v;
                    break;
                }
                obj_define_value(J, o, str_val(atom(J, k)), v, PROP_DEFAULT);
                jspace(P);
                if (P->i < P->n && P->s[P->i] == ',') {
                    P->i++;
                    continue;
                }
                if (P->i < P->n && P->s[P->i] == '}') {
                    P->i++;
                    break;
                }
                r = jerr(P, "Expected '}'");
                break;
            }
    } else if (c == '[') {
        P->i++;
        struct jsobject *a = array_new(J, 0);
        r = obj_val(a);
        jspace(P);
        if (P->i < P->n && P->s[P->i] == ']')
            P->i++;
        else
            for (;;) {
                jsval v = jvalue(P);
                if (v == JS_EXCEPTION) {
                    r = v;
                    break;
                }
                if (a->length >= a->cap)
                    array_grow(J, a, a->length + 1);
                a->elems[a->length++] = v;
                jspace(P);
                if (P->i < P->n && P->s[P->i] == ',') {
                    P->i++;
                    continue;
                }
                if (P->i < P->n && P->s[P->i] == ']') {
                    P->i++;
                    break;
                }
                r = jerr(P, "Expected ']'");
                break;
            }
    } else if (c == '"') {
        struct jsstring *s = jstring(P);
        r = s ? str_val(s) : JS_EXCEPTION;
    } else if (c == '-' || (c >= '0' && c <= '9'))
        r = jnumber(P);
    else if (jword(P, "true"))
        r = JS_TRUE;
    else if (jword(P, "false"))
        r = JS_FALSE;
    else if (jword(P, "null"))
        r = JS_NULL;
    else
        r = jerr(P, "Unexpected identifier");
    P->depth--;
    return r;
}

static jsval revive(struct js *J, struct jsobject *holder, jsval key, jsval reviver)
{
    jsval val = obj_get(J, holder, key, obj_val(holder));
    if (val == JS_EXCEPTION)
        return val;
    if (js_is_object(val)) {
        struct jsobject *o = to_obj(val);
        if (J->native_depth > NATIVE_DEPTH_MAX)
            return throw_range(J, "Maximum call stack size exceeded.");
        J->native_depth++;
        jsval *keys;
        uint32_t n;
        if (obj_class(o) == CLASS_ARRAY) {
            double len = o->length;
            n = (uint32_t)len;
            keys = malloc(((size_t)n + 1) * sizeof *keys);
            for (uint32_t i = 0; i < n; i++)
                keys[i] = str_val(atom_index(J, i));
        } else if (obj_own_keys(J, o, 5, &keys, &n) < 0) {
            free(keys);
            J->native_depth--;
            return JS_EXCEPTION;
        }
        js_root_push(J, keys, n);
        for (uint32_t i = 0; i < n; i++) {
            jsval nv = revive(J, o, keys[i], reviver);
            if (nv == JS_EXCEPTION) {
                js_root_pop(J);
                free(keys);
                J->native_depth--;
                return nv;
            }
            if (nv == JS_UNDEFINED)
                obj_delete(J, o, keys[i], false);
            else
                obj_define_value(J, o, keys[i], nv, PROP_DEFAULT);
        }
        js_root_pop(J);
        free(keys);
        J->native_depth--;
    }
    jsval args[2] = { key, val };
    return vm_call(J, reviver, obj_val(holder), 2, args, JS_UNDEFINED);
}

static jsval json_parse(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    struct jsstring *f = str_flat(J, s);
    struct jparse P = { J, f->chars, str_len(f), 0, 0 };
    jsval v = jvalue(&P);
    if (v == JS_EXCEPTION)
        return v;
    jspace(&P);
    if (P.i < P.n)
        return throw_syntax(J, "JSON Parse error: Unable to parse JSON string");
    if (js_is_callable(argv[1])) {
        struct jsobject *root = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
        prop_add(J, root, str_val(J->atom[A_empty]), v, PROP_DEFAULT);
        return revive(J, root, str_val(J->atom[A_empty]), argv[1]);
    }
    return v;
}

/* --- stringify ------------------------------------------------------------ */

struct jout {
    struct js *J;
    struct sbuf b;
    jsval replacer;                             /* a function or undefined */
    jsval *allow;                               /* the property list (keys), or 0 */
    uint32_t nallow;
    struct jsstring *gap;
    struct sbuf indent;
};

static void quote_into(struct js *J, struct sbuf *b, struct jsstring *s)
{
    struct jsstring *f = str_flat(J, s);
    uint32_t n = str_len(f);
    sb_unit(b, '"');
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = f->chars[i];
        switch (c) {
        case '"': sb_ascii(b, "\\\""); continue;
        case '\\': sb_ascii(b, "\\\\"); continue;
        case '\b': sb_ascii(b, "\\b"); continue;
        case '\f': sb_ascii(b, "\\f"); continue;
        case '\n': sb_ascii(b, "\\n"); continue;
        case '\r': sb_ascii(b, "\\r"); continue;
        case '\t': sb_ascii(b, "\\t"); continue;
        }
        bool lone = false;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < n && f->chars[i + 1] >= 0xDC00 && f->chars[i + 1] <= 0xDFFF) {
                sb_unit(b, c);
                sb_unit(b, f->chars[++i]);
                continue;
            }
            lone = true;
        } else if (c >= 0xDC00 && c <= 0xDFFF)
            lone = true;
        if (c < 0x20 || lone) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", c);
            sb_ascii(b, buf);
        } else
            sb_unit(b, c);
    }
    sb_unit(b, '"');
}

jsval json_quote(struct js *J, struct jsstring *s)
{
    struct sbuf b = { 0 };
    quote_into(J, &b, s);
    struct jsstring *r = sb_finish(J, &b);
    return r ? str_val(r) : JS_EXCEPTION;
}

static int serialize(struct jout *O, struct jsobject *holder, jsval key, jsval value);

static void newline_indent(struct jout *O)
{
    if (!O->gap || !str_len(O->gap))
        return;
    sb_unit(&O->b, '\n');
    sb_units(&O->b, O->indent.s, O->indent.n);
}

static int serialize_object(struct jout *O, struct jsobject *o)
{
    struct js *J = O->J;
    if (!visiting_push(J, o)) {
        throw_type(J, "JSON.stringify cannot serialize cyclic structures.");
        return -1;
    }
    uint32_t saved = O->indent.n;
    if (O->gap)
        sb_str(J, &O->indent, O->gap);
    jsval *keys;
    uint32_t n;
    bool own = !O->allow;
    if (own && obj_own_keys(J, o, 5, &keys, &n) < 0) {      /* (64r: a proxy's trap threw) */
        free(keys);
        O->indent.n = saved;
        visiting_pop(J);
        return -1;
    }
    if (!own) {
        keys = O->allow;
        n = O->nallow;
    }
    if (own)
        js_root_push(J, keys, n);
    sb_unit(&O->b, '{');
    bool any = false;
    int r = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t mark = O->b.n;
        if (any)
            sb_unit(&O->b, ',');
        newline_indent(O);
        quote_into(J, &O->b, to_str(keys[i]));
        sb_unit(&O->b, ':');
        if (O->gap && str_len(O->gap))
            sb_unit(&O->b, ' ');
        jsval v = obj_get(J, o, keys[i], obj_val(o));
        if (v == JS_EXCEPTION) {
            r = -1;
            break;
        }
        int s = serialize(O, o, keys[i], v);
        if (s < 0) {
            r = -1;
            break;
        }
        if (s == 0) {
            O->b.n = mark;                      /* undefined: the member is left out */
            continue;
        }
        any = true;
    }
    if (own) {
        js_root_pop(J);
        free(keys);
    }
    O->indent.n = saved;
    if (r == 0) {
        if (any)
            newline_indent(O);
        sb_unit(&O->b, '}');
    }
    visiting_pop(J);
    return r;
}

static int serialize_array(struct jout *O, struct jsobject *a)
{
    struct js *J = O->J;
    if (!visiting_push(J, a)) {
        throw_type(J, "JSON.stringify cannot serialize cyclic structures.");
        return -1;
    }
    uint32_t saved = O->indent.n;
    if (O->gap)
        sb_str(J, &O->indent, O->gap);
    double len;
    jsval l = obj_get(J, a, str_val(J->atom[A_length]), obj_val(a));
    if (l == JS_EXCEPTION || js_to_length(J, l, &len) < 0) {
        visiting_pop(J);
        return -1;
    }
    sb_unit(&O->b, '[');
    int r = 0;
    for (double i = 0; i < len; i++) {
        if (i > 0)
            sb_unit(&O->b, ',');
        newline_indent(O);
        jsval key = str_val(atom_index(J, (uint32_t)i));
        jsval v = obj_get(J, a, key, obj_val(a));
        if (v == JS_EXCEPTION) {
            r = -1;
            break;
        }
        int s = serialize(O, a, key, v);
        if (s < 0) {
            r = -1;
            break;
        }
        if (s == 0)
            sb_ascii(&O->b, "null");
    }
    O->indent.n = saved;
    if (r == 0) {
        if (len > 0)
            newline_indent(O);
        sb_unit(&O->b, ']');
    }
    visiting_pop(J);
    return r;
}

/* 1 written, 0 undefined (nothing written), -1 threw */
static int serialize(struct jout *O, struct jsobject *holder, jsval key, jsval value)
{
    struct js *J = O->J;
    if (js_is_object(value)) {
        jsval tj = obj_get(J, to_obj(value), str_val(J->atom[A_toJSON]), value);
        if (tj == JS_EXCEPTION)
            return -1;
        if (js_is_callable(tj)) {
            value = vm_call(J, tj, value, 1, &key, JS_UNDEFINED);
            if (value == JS_EXCEPTION)
                return -1;
        }
    }
    if (O->replacer != JS_UNDEFINED) {
        jsval args[2] = { key, value };
        value = vm_call(J, O->replacer, obj_val(holder), 2, args, JS_UNDEFINED);
        if (value == JS_EXCEPTION)
            return -1;
    }
    if (js_is_object(value)) {
        struct jsobject *o = to_obj(value);
        switch (obj_class(o)) {
        case CLASS_NUMBER: {
            bool threw = false;
            double d = js_to_number(J, value, &threw);
            if (threw)
                return -1;
            value = js_number(d);
            break;
        }
        case CLASS_STRING: {
            struct jsstring *s = js_to_str(J, value);
            if (!s)
                return -1;
            value = str_val(s);
            break;
        }
        case CLASS_BOOLEAN:
            value = o->u.prim;
            break;
        }
    }
    switch (js_tag(value)) {
    case JS_T_NULL:
        sb_ascii(&O->b, "null");
        return 1;
    case JS_T_BOOL:
        sb_ascii(&O->b, value == JS_TRUE ? "true" : "false");
        return 1;
    case JS_T_STRING:
        quote_into(J, &O->b, to_str(value));
        return 1;
    case JS_T_NUMBER: {
        double d = js_number_value(value);
        if (!isfinite(d))
            sb_ascii(&O->b, "null");
        else {
            char buf[32];
            number_to_ascii(d, buf);
            sb_ascii(&O->b, buf);
        }
        return 1;
    }
    case JS_T_OBJECT:
        if (js_is_callable(value))
            return 0;
        if (J->native_depth > NATIVE_DEPTH_MAX * 4) {
            throw_range(J, "Maximum call stack size exceeded.");
            return -1;
        }
        J->native_depth++;
        int a = value_is_array(J, value);      /* (64r: a proxy of an array too) */
        int r = a < 0 ? -1 : a ? serialize_array(O, to_obj(value)) : serialize_object(O, to_obj(value));
        J->native_depth--;
        return r < 0 ? -1 : 1;
    }
    return 0;
}

static jsval json_stringify(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    struct jout O = { .J = J, .replacer = JS_UNDEFINED };
    jsval rep = argv[1], space = argv[2];
    jsval *allow = 0;
    uint32_t nallow = 0;
    if (js_is_callable(rep))
        O.replacer = rep;
    else if (js_is_object(rep) && obj_class(to_obj(rep)) == CLASS_ARRAY) {
        struct jsobject *ra = to_obj(rep);
        allow = malloc(((size_t)ra->length + 1) * sizeof *allow);
        for (uint32_t i = 0; i < ra->length; i++) {
            jsval v = obj_get(J, ra, str_val(atom_index(J, i)), rep);
            if (v == JS_EXCEPTION) {
                free(allow);
                return v;
            }
            jsval item = JS_UNDEFINED;
            if (js_is_string(v) || js_is_number(v))
                item = key_from_value(J, v);
            else if (js_is_object(v) && (obj_class(to_obj(v)) == CLASS_STRING || obj_class(to_obj(v)) == CLASS_NUMBER)) {
                struct jsstring *s = js_to_str(J, v);
                if (!s) {
                    free(allow);
                    return JS_EXCEPTION;
                }
                item = str_val(atom(J, s));
            }
            if (item == JS_UNDEFINED || item == JS_EXCEPTION)
                continue;
            bool dup = false;
            for (uint32_t k = 0; k < nallow; k++)
                dup |= allow[k] == item;
            if (!dup)
                allow[nallow++] = item;
        }
        O.allow = allow;
        O.nallow = nallow;
        js_root_push(J, allow, nallow);
    }
    if (js_is_object(space)) {
        int c = obj_class(to_obj(space));
        if (c == CLASS_NUMBER) {
            bool threw = false;
            space = js_number(js_to_number(J, space, &threw));
        } else if (c == CLASS_STRING)
            space = str_val(js_to_str(J, space));
    }
    if (js_is_number(space)) {
        double d = js_number_value(space);
        int k = d != d ? 0 : d > 10 ? 10 : d < 1 ? 0 : (int)d;
        char buf[12];
        memset(buf, ' ', (size_t)k);
        buf[k] = 0;
        O.gap = str_from_ascii(J, buf);
    } else if (js_is_string(space)) {
        struct jsstring *s = to_str(space);
        O.gap = str_len(s) > 10 ? str_sub(J, s, 0, 10) : s;
    }
    struct jsobject *wrapper = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    prop_add(J, wrapper, str_val(J->atom[A_empty]), argv[0], PROP_DEFAULT);
    int r = serialize(&O, wrapper, str_val(J->atom[A_empty]), argv[0]);
    if (allow) {
        js_root_pop(J);
        free(allow);
    }
    sb_free(&O.indent);
    if (r <= 0) {
        sb_free(&O.b);
        return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    struct jsstring *s = sb_finish(J, &O.b);
    return s ? str_val(s) : JS_EXCEPTION;
}

void init_json(struct js *J)
{
    struct jsobject *j = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    obj_define_native(J, j, "parse", json_parse, 2, 0);
    obj_define_native(J, j, "stringify", json_stringify, 3, 0);
    obj_define_value(J, j, J->symbol[SYM_TO_STRING_TAG], str_val(atom_cstr(J, "JSON")), PROP_CONFIGURABLE);
    obj_define_value(J, J->global, key_cstr(J, "JSON"), obj_val(j), PROP_WRITABLE | PROP_CONFIGURABLE);
}
