/*
 * Style sheets. A sheet is rules of selectors and declarations; the
 * cascade: the browser's own sheet, the page's sheets in order, style="",
 * each by specificity then order, and !important declarations after all
 * normal ones (27f); custom properties (--name) inherit and var() takes
 * them. Inherited: color, the font, text-align, white-space, list-style
 * and the text's decoration.
 *
 * Selectors (27f): compounds of a tag (or *), #id, .classes, attribute
 * tests ([a], [a=v], ~= |= ^= $= *=, an i for any case) and pseudo-classes
 * (:root, :first-child, :last-child, :only-child, :nth-child(An+B),
 * :nth-last-child, the -of-type forms, :empty, :not(), :is(), :where(),
 * :link and :any-link; :checked and :disabled by the controls; :hover,
 * :focus, :visited and the other states never match, as nothing is
 * hovered and no history is kept), joined by descendant, child (>),
 * next-sibling (+) and later-sibling (~) combinators, ::before and ::after
 * last. Specificity is CSS's (ids, then classes, attributes and
 * pseudo-classes, then types; :where 0, :is and :not their most specific
 * argument).
 *
 * @media (27f): its rules count while the query holds for the window (a
 * resize restyles): screen and all (not print), width and height with
 * min-/max- or the range syntax, orientation, prefers-color-scheme light,
 * prefers-reduced-motion no-preference, hover, pointer fine, resolution
 * 1x. @supports holds for the properties read here; @layer's rules are
 * plain rules; @import, @font-face, @keyframes and the like are skipped.
 * Values: calc(), min(), max() and clamp() of lengths (a width keeps a
 * percentage and pixels apart: calc(100% - 20px)), vw and vh of the window,
 * inherit, initial and unset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser.h"
#include "image.h"

#define COMPOUNDS 8
#define CLASSES 6

struct selector_list;

struct attr_test {
    char name[32];
    char op;                            /* 0 exists, '=', '~', '|', '^', '$', '*' */
    bool icase;
    char *value;
};

enum pseudo_kind { PC_ROOT, PC_FIRST, PC_LAST, PC_ONLY, PC_NTH, PC_NTH_LAST, PC_FIRST_TYPE,
                   PC_LAST_TYPE, PC_ONLY_TYPE, PC_NTH_TYPE, PC_NTH_LAST_TYPE, PC_EMPTY, PC_NOT,
                   PC_IS, PC_LINK, PC_CHECKED, PC_DISABLED, PC_ENABLED, PC_NEVER, PC_HAS };

struct pseudo {
    enum pseudo_kind kind;
    int a, b;                           /* :nth-*(An+B) */
    struct selector_list *list;         /* :not(), :is(), :where() */
};

struct compound {
    char tag[16];                       /* "" any */
    char id[32];
    char classes[CLASSES][32];
    int class_count;
    struct attr_test *attrs;
    int attr_count;
    struct pseudo *pseudos;
    int pseudo_count;
    char combinator;                    /* to the compound left of it: ' ', '>', '+', '~' (0: none) */
};

enum { PSEUDO_NONE, PSEUDO_BEFORE, PSEUDO_AFTER };

struct selector {
    struct compound parts[COMPOUNDS];   /* left to right */
    int count;
    int specificity;
    int pseudo_element;                 /* PSEUDO_BEFORE, PSEUDO_AFTER: ::before, ::after */
};

struct selector_list {
    struct selector *items;
    int count;
    char *relative;                     /* (27n) :has(): each one's leading combinator (' ', '>', '+', '~') */
};

struct decl {
    char name[48];
    char *value;
    bool important;
};

struct rule {
    struct selector sel;
    struct decl *decls;
    int decl_count;
    int order;
    int media;                          /* (27f) its @media queries (the sheet's medias[]), or -1 */
};

/* (27q) The rules filed by their last compound's id, else its first class,
 * else its tag ("#x", ".x", "x"); the others in `anyrules`. An element is
 * then compared only with the rules filed under its own id, classes and
 * tag, and those (a 1 MB sheet of thousands of rules took ~60 ms an element
 * before on the Mac). Built when first used after the sheet grew. */
struct bucket {
    char key[36];                       /* "" free */
    int *rules;                         /* rule numbers, in the sheet's order */
    int count, room;
};

struct sheet {
    struct rule *rules;
    int count, room;
    char **medias;                      /* the media texts the rules point to */
    int media_count;
    bool pseudo_rules;                  /* some rule is for ::before or ::after */
    struct bucket *buckets;             /* (27q) open addressing, bucket_size a power of two */
    int bucket_size;
    int *anyrules;
    int any_count, any_room;
    int indexed;                        /* rules filed (rebuilt when count differs) */
};

bool css_scripting;

/* The window the styles are for (css_apply's): @media, vw and vh. */
static int view_w = 800, view_h = 600;

/* The browser's own sheet (what pages look like unstyled). */
static const char UA_SHEET[] =
    "html,body,div,p,h1,h2,h3,h4,h5,h6,ul,ol,dl,dt,dd,blockquote,pre,hr,table,form,header,"
    "footer,nav,section,article,aside,main,figure,figcaption,address,center,fieldset,details,"
    "summary,legend,noscript,menu,dir,caption,tbody,thead,tfoot{display:block}"
    "li{display:list-item}"
    "table{display:table;border-spacing:2px}caption{display:table-caption}"
    "thead,tbody,tfoot{display:table-row-group}"
    "tr{display:table-row}td,th{display:table-cell;padding:1px 4px;vertical-align:middle}"
    "head,script,style,title,meta,link,base,template,datalist,param{display:none}"
    "body{margin:8px;color:#000;font-size:16px}"
    "p,dl,blockquote,figure,ul,ol,pre,table,fieldset{margin:16px 0}"
    "h1{font-size:32px;font-weight:bold;margin:21px 0}"
    "h2{font-size:24px;font-weight:bold;margin:20px 0}"
    "h3{font-size:19px;font-weight:bold;margin:18px 0}"
    "h4{font-size:16px;font-weight:bold;margin:21px 0}"
    "h5{font-size:13px;font-weight:bold;margin:22px 0}"
    "h6{font-size:11px;font-weight:bold;margin:25px 0}"
    "ul,ol,menu,dir{padding-left:40px}ol{list-style-type:decimal}"
    "ul ul,ol ul{list-style-type:circle}ul ul ul{list-style-type:square}"
    "li ul,li ol{margin:0}"
    "dd{margin-left:40px}blockquote{margin:16px 40px}figure{margin:16px 40px}"
    "pre{white-space:pre}"
    "a:link{color:#0000ee;text-decoration:underline}"
    "b,strong,th,dt{font-weight:bold}th{text-align:center}caption{text-align:center}"
    "i,em,cite,var,dfn,address{font-style:italic}"
    "u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
    "center{text-align:center}"
    "hr{border:1px solid #888;margin:8px 0}"
    "small,sub,sup{font-size:smaller}big{font-size:larger}"
    "pre,code,kbd,samp,tt,listing,xmp,plaintext{font-family:monospace}"
    "pre,code,kbd,samp,tt{font-size:0.8125em}"
    "sub{vertical-align:sub}sup{vertical-align:super}"
    "mark{background-color:#ff0}"
    "[hidden]{display:none}";

/* --- reading --- */

static const char *skip_space(const char *s)
{
    for (;;) {
        while (is_space(*s))
            s++;
        if (s[0] == '/' && s[1] == '*') {
            const char *end = strstr(s + 2, "*/");
            s = end ? end + 2 : s + strlen(s);
            continue;
        }
        return s;
    }
}

/* Past a {...} block, nested ones too (s just after its '{'); strings
 * and comments inside are skipped. */
static const char *skip_block(const char *s)
{
    int depth = 1;
    while (*s && depth) {
        if (*s == '"' || *s == '\'') {
            char q = *s++;
            while (*s && *s != q)
                s += *s == '\\' && s[1] ? 2 : 1;
            if (*s)
                s++;
            continue;
        }
        if (s[0] == '/' && s[1] == '*') {
            const char *end = strstr(s + 2, "*/");
            s = end ? end + 2 : s + strlen(s);
            continue;
        }
        if (*s == '{')
            depth++;
        else if (*s == '}')
            depth--;
        s++;
    }
    return s;
}

static bool name_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c >= 0x80 || c == '\\';
}

/* An identifier at text[*i..n) into out (lower case if fold): its length. */
static size_t ident(const char *text, size_t n, size_t *i, char *out, size_t size, bool fold)
{
    size_t o = 0;
    while (*i < n && name_char((unsigned char)text[*i])) {
        char c = text[*i];
        if (c == '\\' && *i + 1 < n) {                  /* (an escaped character: itself) */
            (*i)++;
            c = text[*i];
        }
        if (o + 1 < size)
            out[o++] = fold ? (char)lower((unsigned char)c) : c;
        (*i)++;
    }
    out[o] = 0;
    return o;
}

static bool parse_list(const char *text, size_t n, struct selector_list *out);
static bool parse_relative(const char *text, size_t n, struct selector_list *out);
static bool parse_list_items(const char *text, size_t n, struct selector_list *out);
static void free_selector(struct selector *s);

/* :nth-*'s argument: odd, even, B, An, An+B (in [s, e)). */
static bool parse_nth(const char *s, const char *e, int *a, int *b)
{
    while (s < e && is_space(*s))
        s++;
    while (e > s && is_space(e[-1]))
        e--;
    size_t n = (size_t)(e - s);
    if (n == 3 && starts_word(s, "odd")) {
        *a = 2, *b = 1;
        return true;
    }
    if (n == 4 && starts_word(s, "even")) {
        *a = 2, *b = 0;
        return true;
    }
    const char *p = s;
    int sign = 1, num = 0;
    bool digits = false;
    if (p < e && (*p == '+' || *p == '-'))
        sign = *p++ == '-' ? -1 : 1;
    while (p < e && *p >= '0' && *p <= '9')
        num = num * 10 + (*p++ - '0'), digits = true;
    if (p < e && lower(*p) == 'n') {
        *a = sign * (digits ? num : 1);
        p++;
        while (p < e && is_space(*p))
            p++;
        *b = 0;
        if (p < e) {
            int bs = *p == '-' ? -1 : *p == '+' ? 1 : 0;
            if (!bs)
                return false;
            p++;
            while (p < e && is_space(*p))
                p++;
            int v = 0;
            bool d = false;
            while (p < e && *p >= '0' && *p <= '9')
                v = v * 10 + (*p++ - '0'), d = true;
            if (!d)
                return false;
            *b = bs * v;
        }
        return p == e;
    }
    if (!digits || p != e)
        return false;
    *a = 0, *b = sign * num;
    return true;
}

/* The end of a (...) starting at text[i] == '(' (just after its ')'), or 0
 * if it is not closed before n. */
static size_t paren_end(const char *text, size_t n, size_t i)
{
    int depth = 0;
    for (; i < n; i++) {
        if (text[i] == '(')
            depth++;
        else if (text[i] == ')' && --depth == 0)
            return i + 1;
    }
    return 0;
}

/* One selector from text[0..n): false if it says what we do not read. */
static bool parse_selector(const char *text, size_t n, struct selector *out)
{
    memset(out, 0, sizeof(*out));
    size_t i = 0;
    char combinator = 0;
    int ids = 0, classes = 0, types = 0;
    while (i < n) {
        while (i < n && is_space(text[i])) {
            i++;
            if (out->count)
                combinator = combinator ? combinator : ' ';
        }
        if (i >= n)
            break;
        if (text[i] == '>' || text[i] == '+' || text[i] == '~') {
            combinator = text[i++];
            continue;
        }
        if (out->count == COMPOUNDS || out->pseudo_element)
            goto refused;                       /* (nothing after ::before) */
        struct compound *c = &out->parts[out->count];
        out->count++;
        c->combinator = out->count > 1 ? (combinator ? combinator : ' ') : 0;
        combinator = 0;
        bool any = false;
        while (i < n && !is_space(text[i]) && text[i] != '>' && text[i] != '+' && text[i] != '~') {
            char kind = text[i];
            if (kind == '*') {
                i++;
                any = true;
                continue;
            }
            if (kind == '[') {                  /* [name], [name op "value" i] */
                size_t end = i + 1;
                char q = 0;
                while (end < n && (q || text[end] != ']')) {
                    if (q && text[end] == q)
                        q = 0;
                    else if (!q && (text[end] == '"' || text[end] == '\''))
                        q = text[end];
                    end++;
                }
                if (end >= n)
                    goto refused;
                struct attr_test t;
                memset(&t, 0, sizeof(t));
                size_t k = i + 1;
                while (k < end && is_space(text[k]))
                    k++;
                if (!ident(text, end, &k, t.name, sizeof(t.name), true))
                    goto refused;
                while (k < end && is_space(text[k]))
                    k++;
                if (k < end) {
                    if (text[k] == '=')
                        t.op = '=', k++;
                    else if (k + 1 < end && text[k + 1] == '=' && strchr("~|^$*", text[k]))
                        t.op = text[k], k += 2;
                    else
                        goto refused;
                    while (k < end && is_space(text[k]))
                        k++;
                    size_t vs = k, ve;
                    if (k < end && (text[k] == '"' || text[k] == '\'')) {
                        char qq = text[k++];
                        vs = k;
                        while (k < end && text[k] != qq)
                            k++;
                        ve = k;
                        if (k < end)
                            k++;
                    } else {
                        while (k < end && !is_space(text[k]))
                            k++;
                        ve = k;
                    }
                    t.value = copy_text(text + vs, ve - vs);
                    while (k < end && is_space(text[k]))
                        k++;
                    if (k < end && lower(text[k]) == 'i')
                        t.icase = true;
                }
                struct attr_test *grown = realloc(c->attrs, sizeof(*grown) * (size_t)(c->attr_count + 1));
                if (!grown) {
                    free(t.value);
                    goto refused;
                }
                c->attrs = grown;
                c->attrs[c->attr_count++] = t;
                classes++;
                i = end + 1;
                any = true;
                continue;
            }
            if (kind == ':') {
                i++;
                if (i < n && text[i] == ':') {  /* ::before, ::after */
                    i++;
                    char name[32];
                    ident(text, n, &i, name, sizeof(name), true);
                    if (!strcmp(name, "before"))
                        out->pseudo_element = PSEUDO_BEFORE;
                    else if (!strcmp(name, "after"))
                        out->pseudo_element = PSEUDO_AFTER;
                    else
                        goto refused;           /* (::marker, ::placeholder, ...: not drawn) */
                    types++;
                    any = true;
                    continue;
                }
                char name[32];
                ident(text, n, &i, name, sizeof(name), true);
                struct pseudo p;
                memset(&p, 0, sizeof(p));
                const char *arg = 0;
                size_t arg_n = 0;
                if (i < n && text[i] == '(') {
                    size_t end = paren_end(text, n, i);
                    if (!end)
                        goto refused;
                    arg = text + i + 1;
                    arg_n = end - i - 2;
                    i = end;
                }
                int spec = 100;                 /* (a pseudo-class counts as a class) */
                if (!strcmp(name, "before") || !strcmp(name, "after")) {   /* (CSS 2's one colon) */
                    out->pseudo_element = name[0] == 'b' ? PSEUDO_BEFORE : PSEUDO_AFTER;
                    types++;
                    any = true;
                    continue;
                }
                static const struct { const char *name; enum pseudo_kind kind; } SIMPLE[] = {
                    { "root", PC_ROOT }, { "first-child", PC_FIRST }, { "last-child", PC_LAST },
                    { "only-child", PC_ONLY }, { "first-of-type", PC_FIRST_TYPE },
                    { "last-of-type", PC_LAST_TYPE }, { "only-of-type", PC_ONLY_TYPE },
                    { "empty", PC_EMPTY }, { "link", PC_LINK }, { "any-link", PC_LINK },
                    { "checked", PC_CHECKED }, { "disabled", PC_DISABLED }, { "enabled", PC_ENABLED },
                    { "hover", PC_NEVER }, { "focus", PC_NEVER }, { "active", PC_NEVER },
                    { "visited", PC_NEVER }, { "focus-within", PC_NEVER },
                    { "focus-visible", PC_NEVER }, { "target", PC_NEVER },
                    { "placeholder-shown", PC_NEVER }, { "invalid", PC_NEVER },
                };
                bool known = false;
                for (size_t k = 0; k < sizeof(SIMPLE) / sizeof(SIMPLE[0]) && !arg; k++)
                    if (!strcmp(name, SIMPLE[k].name))
                        p.kind = SIMPLE[k].kind, known = true;
                if (!known && arg) {
                    if (!strcmp(name, "nth-child") || !strcmp(name, "nth-last-child") ||
                        !strcmp(name, "nth-of-type") || !strcmp(name, "nth-last-of-type")) {
                        if (!parse_nth(arg, arg + arg_n, &p.a, &p.b))
                            goto refused;       /* (:nth-child(An+B of S): not read) */
                        p.kind = !strcmp(name, "nth-child") ? PC_NTH
                               : !strcmp(name, "nth-last-child") ? PC_NTH_LAST
                               : !strcmp(name, "nth-of-type") ? PC_NTH_TYPE : PC_NTH_LAST_TYPE;
                        known = true;
                    } else if (!strcmp(name, "has")) {      /* (27n) relative selectors */
                        p.list = calloc(1, sizeof(*p.list));
                        if (!p.list || !parse_relative(arg, arg_n, p.list)) {
                            free(p.list);
                            goto refused;
                        }
                        p.kind = PC_HAS;
                        spec = 0;
                        for (int k = 0; k < p.list->count; k++)
                            if (p.list->items[k].specificity > spec)
                                spec = p.list->items[k].specificity;
                        known = true;
                    } else if (!strcmp(name, "not") || !strcmp(name, "is") || !strcmp(name, "where") ||
                               !strcmp(name, "matches") || !strcmp(name, "-webkit-any")) {
                        p.list = calloc(1, sizeof(*p.list));
                        if (!p.list || !parse_list(arg, arg_n, p.list)) {
                            free(p.list);
                            goto refused;
                        }
                        p.kind = name[0] == 'n' ? PC_NOT : PC_IS;
                        spec = 0;
                        if (name[0] != 'w')     /* (:where counts nothing; the others their most) */
                            for (int k = 0; k < p.list->count; k++)
                                if (p.list->items[k].specificity > spec)
                                    spec = p.list->items[k].specificity;
                        known = true;
                    }
                }
                if (!known)
                    goto refused;
                struct pseudo *grown = realloc(c->pseudos, sizeof(*grown) * (size_t)(c->pseudo_count + 1));
                if (!grown)
                    goto refused;
                c->pseudos = grown;
                c->pseudos[c->pseudo_count++] = p;
                out->specificity += spec;
                any = true;
                continue;
            }
            if (kind == '#' || kind == '.')
                i++;
            else if (!name_char((unsigned char)kind))
                goto refused;
            char name[64];
            size_t len = ident(text, n, &i, name, sizeof(name), kind != '#' && kind != '.');
            if (len == 0)
                goto refused;
            if (kind == '#') {
                if (len >= sizeof(c->id))
                    goto refused;
                memcpy(c->id, name, len + 1);
                ids++;
            } else if (kind == '.') {
                if (c->class_count == CLASSES || len >= sizeof(c->classes[0]))
                    goto refused;
                memcpy(c->classes[c->class_count++], name, len + 1);
                classes++;
            } else {
                if (len >= sizeof(c->tag))
                    goto refused;
                memcpy(c->tag, name, len + 1);
                types++;
            }
            any = true;
        }
        if (!any)
            goto refused;
    }
    if (out->count == 0 || (combinator && combinator != ' '))
        goto refused;                           /* (a combinator with nothing after it) */
    out->specificity += ids * 10000 + classes * 100 + types;
    return true;
refused:
    free_selector(out);
    memset(out, 0, sizeof(*out));
    return false;
}

/* A selector list "a, b" (for :not and :is): false if one cannot be read
 * (as CSS: then the whole list is invalid). */
static bool parse_list(const char *text, size_t n, struct selector_list *out)
{
    static int depth;                   /* (:not(:is(...)) nested: at most 8 deep) */
    out->items = 0, out->count = 0;
    if (depth >= 8)
        return false;
    depth++;
    bool ok = parse_list_items(text, n, out);
    depth--;
    return ok;
}

static bool parse_list_items(const char *text, size_t n, struct selector_list *out)
{
    size_t i = 0;
    while (i <= n) {
        size_t start = i;
        int paren = 0;
        while (i < n && (text[i] != ',' || paren)) {
            paren += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
            i++;
        }
        struct selector s;
        if (!parse_selector(text + start, i - start, &s) || s.pseudo_element) {
            free_selector(&s);
            for (int k = 0; k < out->count; k++)
                free_selector(&out->items[k]);
            free(out->items);
            out->items = 0, out->count = 0;
            return false;
        }
        struct selector *grown = realloc(out->items, sizeof(*grown) * (size_t)(out->count + 1));
        if (!grown) {
            free_selector(&s);
            return false;
        }
        out->items = grown;
        out->items[out->count++] = s;
        i++;
    }
    return out->count > 0;
}

/* (27n) :has()'s list: relative selectors, each maybe beginning with a
 * combinator (none: a descendant). */
static bool parse_relative(const char *text, size_t n, struct selector_list *out)
{
    out->items = 0, out->count = 0, out->relative = 0;
    for (size_t i = 0; i <= n;) {
        size_t start = i;
        int paren = 0;
        while (i < n && (text[i] != ',' || paren)) {
            paren += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
            i++;
        }
        size_t a = start;
        while (a < i && is_space(text[a]))
            a++;
        char rel = ' ';
        if (a < i && (text[a] == '>' || text[a] == '+' || text[a] == '~'))
            rel = text[a++];
        struct selector s;
        bool ok = a < i && parse_selector(text + a, i - a, &s) && !s.pseudo_element;
        struct selector *grown = ok ? realloc(out->items, sizeof(*grown) * (size_t)(out->count + 1)) : 0;
        char *rels = ok ? realloc(out->relative, (size_t)(out->count + 1)) : 0;
        if (grown)
            out->items = grown;
        if (rels)
            out->relative = rels;
        if (!ok || !grown || !rels) {
            if (ok)
                free_selector(&s);
            for (int k = 0; k < out->count; k++)
                free_selector(&out->items[k]);
            free(out->items), free(out->relative);
            out->items = 0, out->relative = 0, out->count = 0;
            return false;
        }
        out->relative[out->count] = rel;
        out->items[out->count++] = s;
        i++;
    }
    return out->count > 0;
}

static void free_selector(struct selector *s)
{
    for (int i = 0; i < s->count; i++) {
        struct compound *c = &s->parts[i];
        for (int k = 0; k < c->attr_count; k++)
            free(c->attrs[k].value);
        free(c->attrs);
        for (int k = 0; k < c->pseudo_count; k++)
            if (c->pseudos[k].list) {
                for (int j = 0; j < c->pseudos[k].list->count; j++)
                    free_selector(&c->pseudos[k].list->items[j]);
                free(c->pseudos[k].list->items);
                free(c->pseudos[k].list->relative);
                free(c->pseudos[k].list);
            }
        free(c->pseudos);
        c->attrs = 0, c->pseudos = 0, c->attr_count = c->pseudo_count = 0;
    }
}

/* Declarations "name: value; ..." from text[0..n) into *decls. */
static int parse_decls(const char *text, size_t n, struct decl **decls)
{
    int count = 0;
    *decls = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && (is_space(text[i]) || text[i] == ';'))
            i++;
        size_t ns = i;
        while (i < n && text[i] != ':' && text[i] != ';')
            i++;
        if (i >= n || text[i] != ':') {
            continue;
        }
        size_t ne = i++;
        while (ne > ns && is_space(text[ne - 1]))
            ne--;
        size_t vs = i;
        int paren = 0;
        char q = 0;
        while (i < n && (q || paren || text[i] != ';')) {
            if (q) {
                if (text[i] == '\\' && i + 1 < n)
                    i++;
                else if (text[i] == q)
                    q = 0;
            } else if (text[i] == '"' || text[i] == '\'') {
                q = text[i];
            } else {
                paren += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
            }
            i++;
        }
        size_t ve = i;
        while (vs < ve && is_space(text[vs]))
            vs++;
        while (ve > vs && is_space(text[ve - 1]))
            ve--;
        bool important = false;
        if (ve - vs >= 10) {                    /* "... !important" */
            size_t b = ve;
            while (b > vs && text[b - 1] != '!')
                b--;
            if (b > vs && ve - b == 9 && starts_word(text + b, "important")) {
                important = true;
                ve = b - 1;
                while (ve > vs && is_space(text[ve - 1]))
                    ve--;
            }
        }
        if (ne == ns || ne - ns >= sizeof((*decls)->name) || count >= 256)
            continue;
        struct decl *d = realloc(*decls, sizeof(**decls) * (size_t)(count + 1));
        if (!d)
            break;
        *decls = d;
        memset(&d[count], 0, sizeof(d[count]));
        bool custom = ne - ns > 2 && text[ns] == '-' && text[ns + 1] == '-';
        for (size_t k = 0; k < ne - ns; k++)    /* (custom properties keep their case) */
            d[count].name[k] = custom ? text[ns + k] : (char)lower((unsigned char)text[ns + k]);
        d[count].value = copy_text(text + vs, ve - vs);
        d[count].important = important;
        if (d[count].value)
            count++;
    }
    return count;
}

/* --- @media and @supports (27f) --- */

/* A length in a media query (px, em and rem as 16px), or -1. */
static int media_length(const char *s, const char *e)
{
    while (s < e && is_space(*s))
        s++;
    long whole = 0, frac = 0, scale = 1;
    bool digits = false;
    while (s < e && *s >= '0' && *s <= '9')
        whole = whole * 10 + (*s++ - '0'), digits = true;
    if (s < e && *s == '.')
        for (s++; s < e && *s >= '0' && *s <= '9'; s++)
            if (scale < 1000)
                frac = frac * 10 + (*s - '0'), scale *= 10, digits = true;
    if (!digits)
        return -1;
    long milli = whole * 1000 + frac * 1000 / scale;
    while (s < e && is_space(*s))
        s++;
    if (e - s >= 2 && (starts_word(s, "em") || starts_word(s, "rem")))
        return (int)(milli * 16 / 1000);
    return (int)(milli / 1000);                         /* (px, or no unit) */
}

/* One media feature "(name: value)" or range "(width >= 600px)" in [s, e). */
static bool media_feature(const char *s, const char *e)
{
    while (s < e && is_space(*s))
        s++;
    while (e > s && is_space(e[-1]))
        e--;
    char name[40];
    size_t n = 0;
    const char *p = s;
    while (p < e && (name_char((unsigned char)*p)) && n + 1 < sizeof(name))
        name[n++] = (char)lower((unsigned char)*p++);
    name[n] = 0;
    while (p < e && is_space(*p))
        p++;
    if (p == e)                                         /* (name): the feature is there */
        return !strcmp(name, "color") || !strcmp(name, "hover") || !strcmp(name, "any-hover") ||
               !strcmp(name, "pointer") || !strcmp(name, "any-pointer") || !strcmp(name, "width") ||
               !strcmp(name, "height") || !strcmp(name, "orientation");
    int width = view_w, height = view_h;
    if (*p == ':') {
        p++;
        while (p < e && is_space(*p))
            p++;
        size_t vn = (size_t)(e - p);
        bool is_dims = !strcmp(name, "width") || !strcmp(name, "min-width") ||
                       !strcmp(name, "max-width") || !strcmp(name, "height") ||
                       !strcmp(name, "min-height") || !strcmp(name, "max-height");
        if (is_dims) {
            int v = media_length(p, e);
            if (v < 0)
                return false;
            int have = strstr(name, "width") ? width : height;
            return !strncmp(name, "min-", 4) ? have >= v : !strncmp(name, "max-", 4) ? have <= v
                                                                                     : have == v;
        }
        if (!strcmp(name, "orientation"))
            return vn == 8 ? starts_word(p, "portrait") == (height >= width)
                           : starts_word(p, "landscape") && width > height;
        if (!strcmp(name, "prefers-color-scheme"))
            return starts_word(p, "light");
        if (!strcmp(name, "prefers-reduced-motion") || !strcmp(name, "prefers-reduced-transparency") ||
            !strcmp(name, "prefers-contrast"))
            return starts_word(p, "no-preference");
        if (!strcmp(name, "hover") || !strcmp(name, "any-hover"))
            return starts_word(p, "hover");
        if (!strcmp(name, "pointer") || !strcmp(name, "any-pointer"))
            return starts_word(p, "fine");
        if (!strcmp(name, "min-resolution") || !strcmp(name, "-webkit-min-device-pixel-ratio") ||
            !strcmp(name, "min--moz-device-pixel-ratio"))
            return atof(p) <= 1.0 || (strstr(p, "dpi") && atof(p) <= 96.0);
        if (!strcmp(name, "max-resolution") || !strcmp(name, "-webkit-max-device-pixel-ratio"))
            return atof(p) >= 1.0 && !(strstr(p, "dpi") && atof(p) < 96.0);
        if (!strcmp(name, "min-aspect-ratio") || !strcmp(name, "max-aspect-ratio")) {
            double a = atof(p), b = strchr(p, '/') ? atof(strchr(p, '/') + 1) : 1.0;
            double ratio = height ? (double)width / height : 1.0;
            return b > 0 && (name[1] == 'i' ? ratio >= a / b : ratio <= a / b);
        }
        if (!strcmp(name, "scripting"))
            return starts_word(p, "none");
        if (!strcmp(name, "color") || !strcmp(name, "min-color"))
            return true;
        return false;                                   /* (what is not known: not this window) */
    }
    /* The range syntax: width >= 600px, 400px <= width < 700px, ... */
    const char *a = s;
    bool feature_first = name[0] && (!strcmp(name, "width") || !strcmp(name, "height"));
    if (feature_first) {
        int have = !strcmp(name, "width") ? width : height;
        const char *op = p;
        int v = media_length(op + (op[1] == '=' ? 2 : 1), e);
        if (v < 0)
            return false;
        if (op[0] == '>')
            return op[1] == '=' ? have >= v : have > v;
        if (op[0] == '<')
            return op[1] == '=' ? have <= v : have < v;
        return op[0] == '=' && have == v;
    }
    /* "A <op> width <op> B" */
    const char *w = strstr(a, "width");
    const char *h = strstr(a, "height");
    const char *f = w && w < e ? w : h && h < e ? h : 0;
    if (!f)
        return false;
    int have = f == w ? width : height;
    const char *op1 = f - 1;
    while (op1 > a && is_space(*op1))
        op1--;
    const char *op1s = op1[-1] == '<' || op1[-1] == '>' ? op1 - 1 : op1;
    int lo = media_length(a, op1s);
    bool ok = true;
    if (lo >= 0)
        ok = (*op1s == '<') ? (op1s[1] == '=' ? lo <= have : lo < have)
           : (*op1s == '>') ? (op1s[1] == '=' ? lo >= have : lo > have) : false;
    const char *q = f + (f == w ? 5 : 6);
    while (q < e && is_space(*q))
        q++;
    if (q < e) {
        int hi = media_length(q + (q[1] == '=' ? 2 : 1), e);
        if (hi < 0)
            return false;
        ok = ok && (q[0] == '<' ? (q[1] == '=' ? have <= hi : have < hi)
                                : (q[1] == '=' ? have >= hi : have > hi));
    }
    return ok;
}

static bool media_condition(const char *s, const char *e);

/* One query "[not|only] TYPE [and (feature)]..." or "(f) and (g)" in [s, e). */
static bool media_one(const char *s, const char *e)
{
    static int depth;                   /* (nested conditions: at most 8 deep) */
    if (depth >= 8)
        return false;
    depth++;
    bool ok = media_condition(s, e);
    depth--;
    return ok;
}

static bool media_condition(const char *s, const char *e)
{
    while (s < e && is_space(*s))
        s++;
    while (e > s && is_space(e[-1]))
        e--;
    bool negate = false;
    if (e - s > 4 && starts_word(s, "not") && is_space(s[3]))
        negate = true, s += 4;
    else if (e - s > 5 && starts_word(s, "only") && is_space(s[4]))
        s += 5;
    bool ok = true;
    bool or_mode = false, any = false;
    for (const char *p = s; p < e && (ok || or_mode);) {
        while (p < e && is_space(*p))
            p++;
        if (p >= e)
            break;
        if (*p == '(') {
            size_t end = paren_end(p, (size_t)(e - p), 0);
            if (!end)
                return false;
            const char *inner = p + 1, *inner_end = p + end - 1;
            const char *t = inner;
            while (t < inner_end && is_space(*t))
                t++;
            bool v;
            if (*t == '(' || starts_word(t, "not "))   /* (nested: a condition of its own) */
                v = media_one(inner, inner_end);
            else
                v = media_feature(inner, inner_end);
            if (or_mode)
                any = any || v;
            else
                ok = ok && v;
            p += end;
            continue;
        }
        char word[24];
        size_t n = 0;
        while (p < e && !is_space(*p) && *p != '(' && n + 1 < sizeof(word))
            word[n++] = (char)lower((unsigned char)*p++);
        word[n] = 0;
        if (!strcmp(word, "and"))
            continue;
        if (!strcmp(word, "or")) {
            or_mode = true, any = ok;
            continue;
        }
        if (!strcmp(word, "all") || !strcmp(word, "screen"))
            continue;
        ok = false;                                     /* print, speech, tv, ...: not this */
    }
    bool result = or_mode ? any : ok;
    return negate ? !result : result;
}

static bool media_ok(const char *media);

/* (27p) matchMedia: a query for a window of width x height. */
bool css_media_matches(const char *query, int width, int height)
{
    view_w = width;
    view_h = height;
    return media_ok(query);
}

/* A media query list (commas: any of them), ';' between nested ones (all). */
static bool media_ok(const char *media)
{
    if (!media)
        return true;
    for (const char *level = media; *level;) {
        const char *end = strchr(level, ';');
        if (!end)
            end = level + strlen(level);
        bool any = false;
        for (const char *q = level; q < end && !any;) {
            const char *comma = q;
            int paren = 0;
            while (comma < end && (*comma != ',' || paren)) {
                paren += *comma == '(' ? 1 : *comma == ')' ? -1 : 0;
                comma++;
            }
            any = media_one(q, comma);
            q = comma + 1;
        }
        if (!any)
            return false;
        level = *end ? end + 1 : end;
    }
    return true;
}

static bool known_property(const char *name);
static bool supports_condition(const char *s, const char *e);

/* @supports: (property: value) for the properties read here, not, and, or. */
static bool supports_ok(const char *s, const char *e)
{
    static int depth;
    if (depth >= 8)
        return false;
    depth++;
    bool ok = supports_condition(s, e);
    depth--;
    return ok;
}

static bool supports_condition(const char *s, const char *e)
{
    while (s < e && is_space(*s))
        s++;
    while (e > s && is_space(e[-1]))
        e--;
    if (e - s > 4 && starts_word(s, "not") && (is_space(s[3]) || s[3] == '('))
        return !supports_ok(s + 3, e);
    bool ok = true, or_mode = false, any = false;
    for (const char *p = s; p < e;) {
        while (p < e && is_space(*p))
            p++;
        if (p >= e)
            break;
        if (*p == '(') {
            size_t end = paren_end(p, (size_t)(e - p), 0);
            if (!end)
                return false;
            const char *inner = p + 1, *inner_end = p + end - 1;
            const char *colon = memchr(inner, ':', (size_t)(inner_end - inner));
            bool v;
            if (colon) {
                char name[48];
                size_t n = 0;
                const char *t = inner;
                while (t < colon && is_space(*t))
                    t++;
                while (t < colon && !is_space(*t) && n + 1 < sizeof(name))
                    name[n++] = (char)lower((unsigned char)*t++);
                name[n] = 0;
                const char *value = colon + 1;
                while (value < inner_end && is_space(*value))
                    value++;
                v = known_property(name) &&
                    !(strstr(name, "display") && (starts_word(value, "subgrid") ||
                                                  starts_word(value, "subgrid")));
            } else {
                v = supports_ok(inner, inner_end);
            }
            if (or_mode)
                any = any || v;
            else
                ok = ok && v;
            p += end;
            continue;
        }
        if (starts_word(p, "or"))
            or_mode = true, any = ok, p += 2;
        else if (starts_word(p, "and"))
            p += 3;
        else if (starts_word(p, "selector(")) {
            size_t end = paren_end(p + 8, (size_t)(e - p - 8), 0);
            if (!end)
                return false;
            struct selector sel;
            bool v = parse_selector(p + 9, end - 2, &sel);
            free_selector(&sel);
            if (or_mode)
                any = any || v;
            else
                ok = ok && v;
            p += 8 + end;
        } else
            return false;
    }
    return or_mode ? any : ok;
}

static void add_rule(struct sheet *s, const struct selector *sel, const struct decl *decls, int dn,
                     int media)
{
    if (s->count == s->room) {
        int room = s->room ? s->room * 2 : 64;
        struct rule *r = realloc(s->rules, sizeof(*r) * (size_t)room);
        if (!r)
            return;
        s->rules = r, s->room = room;
    }
    struct rule *r = &s->rules[s->count];
    r->sel = *sel;
    r->order = s->count;
    r->decl_count = dn;
    r->media = media;
    r->decls = malloc(sizeof(*decls) * (size_t)dn);
    if (!r->decls)
        return;
    for (int i = 0; i < dn; i++) {
        r->decls[i] = decls[i];
        r->decls[i].value = copy_text(decls[i].value, strlen(decls[i].value));
    }
    if (sel->pseudo_element)
        s->pseudo_rules = true;
    s->count++;
}

/* The media text of a nested @media: the outer one's and this, ';' between;
 * its index in the sheet's medias[]. */
static int keep_media(struct sheet *s, int outer_index, const char *q, size_t n)
{
    const char *outer = outer_index >= 0 ? s->medias[outer_index] : 0;
    size_t outer_n = outer ? strlen(outer) + 1 : 0;
    char *m = malloc(outer_n + n + 1);
    if (!m)
        return outer_index;
    if (outer) {
        memcpy(m, outer, outer_n - 1);
        m[outer_n - 1] = ';';
    }
    memcpy(m + outer_n, q, n);
    m[outer_n + n] = 0;
    for (size_t i = 0; m[i]; i++)
        if (m[i] == ';' && i >= outer_n)
            m[i] = ' ';                                 /* (a stray ';' in the query) */
    char **grown = realloc(s->medias, sizeof(*grown) * (size_t)(s->media_count + 1));
    if (!grown) {
        free(m);
        return outer_index;
    }
    s->medias = grown;
    s->medias[s->media_count] = m;
    return s->media_count++;
}

/* Rules from text up to its end or an unmatched '}' (inside @media); the
 * place after it. */
static const char *parse_rules(struct sheet *s, const char *p, int media, int depth)
{
    for (;;) {
        p = skip_space(p);
        if (!*p)
            return p;
        if (*p == '}')
            return p + 1;
        if (*p == '@') {
            const char *name = p + 1;
            const char *q = name;
            while (name_char((unsigned char)*q))
                q++;
            size_t name_n = (size_t)(q - name);
            const char *stop = q;
            while (*stop && *stop != '{' && *stop != ';')
                stop++;
            bool block = *stop == '{';
            if (block && depth < 8 && name_n == 5 && starts_word(name, "media")) {
                p = parse_rules(s, stop + 1, keep_media(s, media, q, (size_t)(stop - q)), depth + 1);
                continue;
            }
            if (block && depth < 8 && name_n == 8 && starts_word(name, "supports")) {
                if (supports_ok(q, stop))
                    p = parse_rules(s, stop + 1, media, depth + 1);
                else
                    p = skip_block(stop + 1);
                continue;
            }
            if (block && depth < 8 && ((name_n == 5 && starts_word(name, "layer")) ||
                                       (name_n == 9 && starts_word(name, "container")))) {
                p = parse_rules(s, stop + 1, media, depth + 1);     /* (layers: plain rules) */
                continue;
            }
            p = block ? skip_block(stop + 1) : *stop ? stop + 1 : stop;
            continue;
        }
        if (!strncmp(p, "<!--", 4) || !strncmp(p, "-->", 3)) {
            p += *p == '<' ? 4 : 3;
            continue;
        }
        const char *open = p;
        while (*open && *open != '{' && *open != '}')
            open++;
        if (*open != '{')
            return *open ? open + 1 : open;             /* (a broken rule: up to the block's end) */
        const char *close = skip_block(open + 1);       /* (just after its '}') */
        const char *body_end = close > open + 1 && close[-1] == '}' ? close - 1 : close;
        struct decl *decls;
        int dn = parse_decls(open + 1, (size_t)(body_end - open - 1), &decls);
        /* Each selector of the list "a, b" is its own rule. */
        for (const char *sel = p; sel < open;) {
            const char *comma = sel;
            int paren = 0;
            while (comma < open && (*comma != ',' || paren)) {
                paren += *comma == '(' ? 1 : *comma == ')' ? -1 : 0;
                comma++;
            }
            struct selector parsed;
            if (dn && parse_selector(sel, (size_t)(comma - sel), &parsed))
                add_rule(s, &parsed, decls, dn, media);
            sel = comma + 1;
        }
        for (int i = 0; i < dn; i++)
            free(decls[i].value);
        free(decls);
        p = close;
    }
}

struct sheet *css_parse(const char *text, struct sheet *s)
{
    if (!s && !(s = calloc(1, sizeof(*s))))
        return 0;
    for (const char *p = text; *p;)
        p = parse_rules(s, p, -1, 0);                   /* (a stray '}' at the top: go on) */
    return s;
}

void css_free(struct sheet *s)
{
    if (!s)
        return;
    for (int i = 0; i < s->count; i++) {
        for (int k = 0; k < s->rules[i].decl_count; k++)
            free(s->rules[i].decls[k].value);
        free(s->rules[i].decls);
        free_selector(&s->rules[i].sel);
    }
    free(s->rules);
    for (int i = 0; i < s->media_count; i++)
        free(s->medias[i]);
    free(s->medias);
    for (int i = 0; i < s->bucket_size; i++)
        free(s->buckets[i].rules);
    free(s->buckets);
    free(s->anyrules);
    free(s);
}

/* --- (27q) the rules' index --- */

static uint32_t key_hash(const char *k)
{
    uint32_t h = 2166136261u;                   /* (FNV-1a) */
    for (; *k; k++)
        h = (h ^ (unsigned char)*k) * 16777619u;
    return h;
}

static struct bucket *bucket_at(const struct sheet *s, const char *key, bool make)
{
    if (!s->bucket_size)
        return 0;
    for (uint32_t i = key_hash(key) & (uint32_t)(s->bucket_size - 1);; i = (i + 1) & (uint32_t)(s->bucket_size - 1)) {
        struct bucket *b = &s->buckets[i];
        if (!b->key[0]) {
            if (!make)
                return 0;
            snprintf(b->key, sizeof(b->key), "%s", key);
            return b;
        }
        if (!strcmp(b->key, key))
            return b;
    }
}

static bool add_number(int **list, int *count, int *room, int v)
{
    if (*count == *room) {
        int r = *room ? *room * 2 : 4;
        int *l = realloc(*list, sizeof(int) * (size_t)r);
        if (!l)
            return false;
        *list = l, *room = r;
    }
    (*list)[(*count)++] = v;
    return true;
}

/* The key a rule is filed under ("" for none). */
static void rule_key(const struct rule *r, char *key, size_t size)
{
    const struct compound *c = &r->sel.parts[r->sel.count - 1];
    if (c->id[0])
        snprintf(key, size, "#%s", c->id);
    else if (c->class_count)
        snprintf(key, size, ".%s", c->classes[0]);
    else if (c->tag[0])
        snprintf(key, size, "%s", c->tag);
    else
        key[0] = 0;
}

static bool index_sheet(struct sheet *s)
{
    for (int i = 0; i < s->bucket_size; i++)
        free(s->buckets[i].rules);
    free(s->buckets);
    s->buckets = 0, s->bucket_size = 0, s->any_count = 0, s->indexed = 0;
    int size = 16;
    while (size < s->count * 2)
        size *= 2;
    s->buckets = calloc((size_t)size, sizeof(*s->buckets));
    if (!s->buckets)
        return false;
    s->bucket_size = size;
    for (int i = 0; i < s->count; i++) {
        char key[36];
        rule_key(&s->rules[i], key, sizeof(key));
        struct bucket *b = key[0] ? bucket_at(s, key, true) : 0;
        if (!(b ? add_number(&b->rules, &b->count, &b->room, i)
                : add_number(&s->anyrules, &s->any_count, &s->any_room, i)))
            return false;
    }
    s->indexed = s->count;
    return true;
}

/* --- matching --- */

static bool has_class(const struct node *e, const char *name)
{
    const char *list = node_attr(e, "class");
    if (!list)
        return false;
    size_t n = strlen(name);
    for (const char *c = list; *c;) {
        while (is_space(*c))
            c++;
        const char *start = c;
        while (*c && !is_space(*c))
            c++;
        if ((size_t)(c - start) == n && !memcmp(start, name, n))
            return true;
    }
    return false;
}

static bool chars_equal(const char *a, const char *b, size_t n, bool icase)
{
    for (size_t i = 0; i < n; i++)
        if (icase ? lower((unsigned char)a[i]) != lower((unsigned char)b[i]) : a[i] != b[i])
            return false;
    return true;
}

static bool attr_matches(const struct attr_test *t, const struct node *e)
{
    const char *v = node_attr(e, t->name);
    if (!v)
        return false;
    if (!t->op)
        return true;
    const char *want = t->value ? t->value : "";
    size_t vn = strlen(v), wn = strlen(want);
    switch (t->op) {
    case '=':
        return vn == wn && chars_equal(v, want, vn, t->icase);
    case '^':
        return wn && vn >= wn && chars_equal(v, want, wn, t->icase);
    case '$':
        return wn && vn >= wn && chars_equal(v + vn - wn, want, wn, t->icase);
    case '*':
        for (size_t i = 0; wn && i + wn <= vn; i++)
            if (chars_equal(v + i, want, wn, t->icase))
                return true;
        return false;
    case '|':
        return vn >= wn && chars_equal(v, want, wn, t->icase) && (vn == wn || v[wn] == '-');
    case '~':
        for (const char *c = v; *c;) {
            while (is_space(*c))
                c++;
            const char *s = c;
            while (*c && !is_space(*c))
                c++;
            if ((size_t)(c - s) == wn && wn && chars_equal(s, want, wn, t->icase))
                return true;
        }
        return false;
    }
    return false;
}

static bool is_element(const struct node *n)
{
    return n && n->kind == NODE_ELEMENT && n->tag[0] != ':';   /* (not ::before or ::after) */
}

/* e's place among its parent's element children (1 first), by its kind
 * too (of_type); from the end with last. */
static int position(const struct node *e, bool of_type, bool last)
{
    int k = 1;
    for (const struct node *s = last ? e->next : e->prev; s; s = last ? s->next : s->prev)
        if (is_element(s) && (!of_type || !strcmp(s->tag, e->tag)))
            k++;
    return k;
}

static bool nth(int a, int b, int k)
{
    if (a == 0)
        return k == b;
    return (k - b) % a == 0 && (k - b) / a >= 0;
}

static bool matches(const struct selector *s, const struct node *e);

static bool pseudo_matches(const struct pseudo *p, const struct node *e)
{
    switch (p->kind) {
    case PC_ROOT:
        return !e->parent || !is_element(e->parent);
    case PC_FIRST:
        return position(e, false, false) == 1;
    case PC_LAST:
        return position(e, false, true) == 1;
    case PC_ONLY:
        return position(e, false, false) == 1 && position(e, false, true) == 1;
    case PC_FIRST_TYPE:
        return position(e, true, false) == 1;
    case PC_LAST_TYPE:
        return position(e, true, true) == 1;
    case PC_ONLY_TYPE:
        return position(e, true, false) == 1 && position(e, true, true) == 1;
    case PC_NTH:
        return nth(p->a, p->b, position(e, false, false));
    case PC_NTH_LAST:
        return nth(p->a, p->b, position(e, false, true));
    case PC_NTH_TYPE:
        return nth(p->a, p->b, position(e, true, false));
    case PC_NTH_LAST_TYPE:
        return nth(p->a, p->b, position(e, true, true));
    case PC_EMPTY:
        for (const struct node *c = e->first; c; c = c->next)
            if (c->kind == NODE_ELEMENT || (c->kind == NODE_TEXT && c->text && c->text[0]))
                return false;
        return true;
    case PC_NOT:
        for (int i = 0; i < p->list->count; i++)
            if (matches(&p->list->items[i], e))
                return false;
        return true;
    case PC_IS:
        for (int i = 0; i < p->list->count; i++)
            if (matches(&p->list->items[i], e))
                return true;
        return false;
    case PC_LINK:
        return (!strcmp(e->tag, "a") || !strcmp(e->tag, "area")) && node_attr(e, "href");
    case PC_CHECKED: {
        struct control *k = is_control(e) ? control_of((struct node *)e) : 0;
        return (k && k->checked) || (!strcmp(e->tag, "option") && node_attr(e, "selected"));
    }
    case PC_DISABLED:
        return is_control(e) && node_attr(e, "disabled");
    case PC_ENABLED:
        return is_control(e) && !node_attr(e, "disabled");
    case PC_NEVER:
        return false;
    case PC_HAS:                                /* (27n) */
        for (int i = 0; i < p->list->count; i++) {
            const struct selector *sel = &p->list->items[i];
            char rel = p->list->relative[i];
            if (rel == '+') {
                const struct node *next = e->next;
                while (next && !is_element(next))
                    next = next->next;
                if (next && matches(sel, next))
                    return true;
            } else if (rel == '~') {
                for (const struct node *s = e->next; s; s = s->next)
                    if (is_element(s) && matches(sel, s))
                        return true;
            } else {
                for (const struct node *d = e->first; d; d = rel == '>' ? d->next : node_next((struct node *)d, (struct node *)e))
                    if (is_element(d) && matches(sel, d))
                        return true;
            }
        }
        return false;
    }
    return false;
}

static bool compound_matches(const struct compound *c, const struct node *e)
{
    if (!is_element(e))
        return false;
    if (c->tag[0] && strcmp(c->tag, e->tag))
        return false;
    if (c->id[0]) {
        const char *id = node_attr(e, "id");
        if (!id || strcmp(id, c->id))
            return false;
    }
    for (int i = 0; i < c->class_count; i++)
        if (!has_class(e, c->classes[i]))
            return false;
    for (int i = 0; i < c->attr_count; i++)
        if (!attr_matches(&c->attrs[i], e))
            return false;
    for (int i = 0; i < c->pseudo_count; i++)
        if (!pseudo_matches(&c->pseudos[i], e))
            return false;
    return true;
}

static const struct node *element_before(const struct node *e)
{
    for (const struct node *s = e->prev; s; s = s->prev)
        if (is_element(s))
            return s;
    return 0;
}

/* Does the selector's part `at` (and all left of it) match e? */
static bool matches_from(const struct selector *s, int at, const struct node *e)
{
    if (!compound_matches(&s->parts[at], e))
        return false;
    if (at == 0)
        return true;
    switch (s->parts[at].combinator) {
    case '>':
        return e->parent && matches_from(s, at - 1, e->parent);
    case '+': {
        const struct node *b = element_before(e);
        return b && matches_from(s, at - 1, b);
    }
    case '~':
        for (const struct node *b = element_before(e); b; b = element_before(b))
            if (matches_from(s, at - 1, b))
                return true;
        return false;
    default:
        for (const struct node *a = e->parent; a; a = a->parent)
            if (matches_from(s, at - 1, a))
                return true;
        return false;
    }
}

static bool matches(const struct selector *s, const struct node *e)
{
    return matches_from(s, s->count - 1, e);
}

/* (27p) Selector lists for scripts: querySelector, matches, closest. */
struct selector_list *css_selectors(const char *text)
{
    struct selector_list *l = calloc(1, sizeof(*l));
    if (!l)
        return 0;
    if (!parse_list(text, strlen(text), l)) {
        free(l);
        return 0;
    }
    return l;
}

bool css_selectors_match(const struct selector_list *l, const struct node *e)
{
    if (e->kind != NODE_ELEMENT || e->tag[0] == ':' || e->tag[0] == '#')
        return false;
    for (int i = 0; i < l->count; i++)
        if (matches(&l->items[i], e))
            return true;
    return false;
}

void css_selectors_free(struct selector_list *l)
{
    if (!l)
        return;
    for (int i = 0; i < l->count; i++)
        free_selector(&l->items[i]);
    free(l->items);
    free(l->relative);
    free(l);
}

/* --- values --- */

static const struct { const char *name; uint32_t rgb; } COLORS[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
    { "blue", 0x0000FF }, { "yellow", 0xFFFF00 }, { "gray", 0x808080 }, { "grey", 0x808080 },
    { "silver", 0xC0C0C0 }, { "maroon", 0x800000 }, { "purple", 0x800080 },
    { "fuchsia", 0xFF00FF }, { "magenta", 0xFF00FF }, { "lime", 0x00FF00 }, { "olive", 0x808000 },
    { "navy", 0x000080 }, { "teal", 0x008080 }, { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF },
    { "orange", 0xFFA500 }, { "pink", 0xFFC0CB }, { "brown", 0xA52A2A }, { "gold", 0xFFD700 },
    { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 }, { "lightgray", 0xD3D3D3 },
    { "lightgrey", 0xD3D3D3 }, { "darkblue", 0x00008B }, { "darkred", 0x8B0000 },
    { "darkgreen", 0x006400 }, { "lightblue", 0xADD8E6 }, { "lightgreen", 0x90EE90 },
    { "lightyellow", 0xFFFFE0 }, { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 },
    { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC }, { "crimson", 0xDC143C },
    { "indigo", 0x4B0082 }, { "violet", 0xEE82EE }, { "tomato", 0xFF6347 },
    { "coral", 0xFF7F50 }, { "salmon", 0xFA8072 }, { "khaki", 0xF0E68C }, { "tan", 0xD2B48C },
    { "steelblue", 0x4682B4 }, { "skyblue", 0x87CEEB }, { "royalblue", 0x4169E1 },
    { "slategray", 0x708090 }, { "dimgray", 0x696969 }, { "darkorange", 0xFF8C00 },
    { "seagreen", 0x2E8B57 }, { "forestgreen", 0x228B22 }, { "chocolate", 0xD2691E },
    { "aliceblue", 0xF0F8FF }, { "lavender", 0xE6E6FA }, { "linen", 0xFAF0E6 },
    { "snow", 0xFFFAFA }, { "honeydew", 0xF0FFF0 }, { "mintcream", 0xF5FFFA },
    { "rebeccapurple", 0x663399 }, { "darkslategray", 0x2F4F4F }, { "darkslategrey", 0x2F4F4F },
    { "lightslategray", 0x778899 }, { "midnightblue", 0x191970 }, { "dodgerblue", 0x1E90FF },
    { "deepskyblue", 0x00BFFF }, { "cornflowerblue", 0x6495ED }, { "firebrick", 0xB22222 },
    { "orangered", 0xFF4500 }, { "goldenrod", 0xDAA520 }, { "limegreen", 0x32CD32 },
    { "darkcyan", 0x008B8B }, { "lightcyan", 0xE0FFFF }, { "hotpink", 0xFF69B4 },
    { "deeppink", 0xFF1493 }, { "plum", 0xDDA0DD }, { "orchid", 0xDA70D6 }, { "sienna", 0xA0522D },
    { "peru", 0xCD853F }, { "wheat", 0xF5DEB3 }, { "moccasin", 0xFFE4B5 }, { "bisque", 0xFFE4C4 },
    { "papayawhip", 0xFFEFD5 }, { "oldlace", 0xFDF5E6 }, { "seashell", 0xFFF5EE },
    { "ghostwhite", 0xF8F8FF }, { "floralwhite", 0xFFFAF0 }, { "azure", 0xF0FFFF },
    { "lightpink", 0xFFB6C1 }, { "lightcoral", 0xF08080 }, { "lightsalmon", 0xFFA07A },
    { "darkviolet", 0x9400D3 }, { "mediumpurple", 0x9370DB }, { "slateblue", 0x6A5ACD },
    { "teal", 0x008080 }, { "turquoise", 0x40E0D0 }, { "aquamarine", 0x7FFFD4 },
    { "olivedrab", 0x6B8E23 }, { "yellowgreen", 0x9ACD32 }, { "springgreen", 0x00FF7F },
    { "darkkhaki", 0xBDB76B }, { "rosybrown", 0xBC8F8F }, { "indianred", 0xCD5C5C },
    { "cadetblue", 0x5F9EA0 }, { "powderblue", 0xB0E0E6 }, { "lightsteelblue", 0xB0C4DE },
    { "navajowhite", 0xFFDEAD }, { "blanchedalmond", 0xFFEBCD }, { "lemonchiffon", 0xFFFACD },
    { "darkmagenta", 0x8B008B }, { "darkolivegreen", 0x556B2F }, { "mediumseagreen", 0x3CB371 },
    { 0, 0 }
};

/* A number of a colour function: 0-255 (or a percentage of it); alpha and
 * hues are taken but the alpha is not drawn. */
static const char *color_part(const char *p, double *out)
{
    while (is_space(*p) || *p == ',' || *p == '/')
        p++;
    char *end;
    double v = strtod(p, &end);
    if (end == p || v != v)
        return 0;                                       /* (none, or NaN) */
    p = end;
    if (*p == '%')
        v = v * 255.0 / 100.0, p++;
    else if (starts_word(p, "deg"))
        p += 3;
    *out = v;
    return p;
}

static int channel(double v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5);
}

/* HSL (h in degrees, s and l 0-1) to 0xRRGGBB. */
static uint32_t hsl(double h, double s, double l)
{
    h = h - 360.0 * (double)(long)(h / 360.0);
    if (h < 0)
        h += 360.0;
    double c = (1.0 - (2.0 * l - 1.0 < 0 ? 1.0 - 2.0 * l : 2.0 * l - 1.0)) * s;
    double hh = h / 60.0;
    double mod = hh - 2.0 * (double)(long)(hh / 2.0);
    double x = c * (1.0 - (mod - 1.0 < 0 ? 1.0 - mod : mod - 1.0));
    double r = 0, g = 0, b = 0;
    if (hh < 1) r = c, g = x;
    else if (hh < 2) r = x, g = c;
    else if (hh < 3) g = c, b = x;
    else if (hh < 4) g = x, b = c;
    else if (hh < 5) r = x, b = c;
    else r = c, b = x;
    double m = l - c / 2.0;
    return (uint32_t)channel((r + m) * 255.0) << 16 | (uint32_t)channel((g + m) * 255.0) << 8 |
           (uint32_t)channel((b + m) * 255.0);
}

bool css_color_alpha(const char *text, uint32_t *out, int *alpha)
{
    while (is_space(*text))
        text++;
    size_t n = strcspn(text, " \t;");
    *alpha = 255;
    if (text[0] == '#') {
        int d[8];
        size_t k = n - 1;
        if (k != 3 && k != 6 && k != 4 && k != 8)
            return false;
        for (size_t i = 0; i < k; i++)
            if ((d[i] = hex_digit((unsigned char)text[1 + i])) < 0)
                return false;
        if (k == 3 || k == 4)
            *out = (uint32_t)(d[0] * 17) << 16 | (uint32_t)(d[1] * 17) << 8 | (uint32_t)(d[2] * 17);
        else
            *out = (uint32_t)(d[0] << 20 | d[1] << 16 | d[2] << 12 | d[3] << 8 | d[4] << 4 | d[5]);
        if (k == 4)
            *alpha = d[3] * 17;
        else if (k == 8)
            *alpha = d[6] * 16 + d[7];
        return true;
    }
    if (starts_word(text, "rgb") || starts_word(text, "hsl")) {
        bool is_hsl = lower(text[0]) == 'h';
        const char *p = strchr(text, '(');
        if (!p)
            return false;
        p++;
        double v[4] = { 0, 0, 0, 255 };             /* (no alpha given: opaque) */
        for (int i = 0; i < 4; i++) {
            const char *q = color_part(p, &v[i]);
            if (!q) {
                if (i < 3)
                    return false;
                break;
            }
            if (i == 3 && q[-1] != '%')
                v[3] *= 255.0;                  /* (an alpha of 0-1; a percentage is already 0-255) */
            p = q;
        }
        double a = v[3] > 255 ? 255 : v[3] < 0 ? 0 : v[3];
        *alpha = (int)(a + 0.5);
        if (is_hsl)
            *out = hsl(v[0], v[1] / 255.0, v[2] / 255.0);
        else
            *out = (uint32_t)channel(v[0]) << 16 | (uint32_t)channel(v[1]) << 8 | (uint32_t)channel(v[2]);
        return true;
    }
    char name[24];
    if (n >= sizeof(name))
        return false;
    for (size_t i = 0; i < n; i++)
        name[i] = (char)lower((unsigned char)text[i]);
    name[n] = 0;
    if (!strcmp(name, "transparent")) {
        *out = 0, *alpha = 0;
        return true;
    }
    for (int i = 0; COLORS[i].name; i++)
        if (!strcmp(COLORS[i].name, name)) {
            *out = COLORS[i].rgb;
            return true;
        }
    return false;
}

bool css_color(const char *text, uint32_t *out)
{
    int alpha;
    return css_color_alpha(text, out, &alpha) && alpha > 0;
}

/* --- lengths and calc() (27f) --- */

/* A length's two parts: pixels, and a percentage of what it is relative to. */
struct amount {
    double px, pct;
    bool number;                        /* a plain number (a factor in calc) */
};

static bool expr(const char **p, int em, struct amount *out);
static bool term_inside(const char **pp, int em, struct amount *out);

/* One term: a number with its unit, or a function, or ( expr ). */
static bool term(const char **pp, int em, struct amount *out)
{
    static int depth;                   /* (calc's parentheses: at most 16 deep) */
    if (depth >= 16)
        return false;
    depth++;
    bool ok = term_inside(pp, em, out);
    depth--;
    return ok;
}

static bool term_inside(const char **pp, int em, struct amount *out)
{
    const char *p = *pp;
    while (is_space(*p))
        p++;
    memset(out, 0, sizeof(*out));
    if (*p == '(') {
        p++;
        if (!expr(&p, em, out))
            return false;
        while (is_space(*p))
            p++;
        if (*p != ')')
            return false;
        *pp = p + 1;
        return true;
    }
    if (starts_word(p, "calc(") || starts_word(p, "min(") || starts_word(p, "max(") ||
        starts_word(p, "clamp(")) {
        char fn = (char)lower(p[0]) == 'c' && lower(p[1]) == 'a' ? 'c' : (char)lower(p[1]) == 'i' ? 'n'
                : (char)lower(p[1]) == 'a' ? 'x' : 'l';
        p = strchr(p, '(') + 1;
        struct amount v[3];
        int n = 0;
        for (;;) {
            if (n == 3 || !expr(&p, em, &v[n]))
                return false;
            n++;
            while (is_space(*p))
                p++;
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p != ')')
                return false;
            p++;
            break;
        }
        /* min/max/clamp of mixed percentages and pixels: compared as pixels of
         * the window's width (what they are almost always relative to). */
        #define AS_PX(a) ((a).px + (a).pct * view_w / 100.0)
        if (fn == 'c') {
            if (n != 1)
                return false;
            *out = v[0];
        } else if (fn == 'n' || fn == 'x') {
            *out = v[0];
            for (int i = 1; i < n; i++)
                if (fn == 'n' ? AS_PX(v[i]) < AS_PX(*out) : AS_PX(v[i]) > AS_PX(*out))
                    *out = v[i];
        } else {
            if (n != 3)
                return false;
            *out = AS_PX(v[1]) < AS_PX(v[0]) ? v[0] : AS_PX(v[1]) > AS_PX(v[2]) ? v[2] : v[1];
        }
        #undef AS_PX
        *pp = p;
        return true;
    }
    char *end;
    double num = strtod(p, &end);
    if (end == p)
        return false;
    p = end;
    if (*p == '%')
        out->pct = num, p++;
    else if (starts_word(p, "px"))
        out->px = num, p += 2;
    else if (starts_word(p, "rem"))
        out->px = num * 16, p += 3;
    else if (starts_word(p, "em"))
        out->px = num * em, p += 2;
    else if (starts_word(p, "ex") || starts_word(p, "ch"))
        out->px = num * em / 2, p += 2;
    else if (starts_word(p, "pt"))
        out->px = num * 4 / 3, p += 2;
    else if (starts_word(p, "pc"))
        out->px = num * 16, p += 2;
    else if (starts_word(p, "in"))
        out->px = num * 96, p += 2;
    else if (starts_word(p, "cm"))
        out->px = num * 96 / 2.54, p += 2;
    else if (starts_word(p, "mm"))
        out->px = num * 96 / 25.4, p += 2;
    else if (starts_word(p, "vw") || starts_word(p, "dvw") || starts_word(p, "svw") || starts_word(p, "lvw"))
        out->px = num * view_w / 100, p += (lower(*p) == 'v' ? 2 : 3);
    else if (starts_word(p, "vh") || starts_word(p, "dvh") || starts_word(p, "svh") || starts_word(p, "lvh"))
        out->px = num * view_h / 100, p += (lower(*p) == 'v' ? 2 : 3);
    else if (starts_word(p, "vmin"))
        out->px = num * (view_w < view_h ? view_w : view_h) / 100, p += 4;
    else if (starts_word(p, "vmax"))
        out->px = num * (view_w > view_h ? view_w : view_h) / 100, p += 4;
    else if (name_char((unsigned char)*p) && *p != '-')
        return false;                                   /* (a unit not known) */
    else
        out->px = num, out->number = true;              /* (0, or a factor) */
    *pp = p;
    return true;
}

/* product := term (('*' | '/') term)* */
static bool product(const char **pp, int em, struct amount *out)
{
    struct amount a;
    if (!term(pp, em, &a))
        return false;
    for (;;) {
        const char *p = *pp;
        while (is_space(*p))
            p++;
        if (*p != '*' && *p != '/')
            break;
        char op = *p++;
        struct amount b;
        if (!term(&p, em, &b))
            return false;
        if (op == '*') {
            if (!b.number && !a.number)
                return false;                           /* (a length times a length) */
            double f = b.number ? b.px : a.px;
            if (a.number)
                a = b;
            a.px *= f, a.pct *= f;
        } else {
            if (!b.number || b.px == 0)
                return false;
            a.px /= b.px, a.pct /= b.px;
        }
        *pp = p;
    }
    *out = a;
    return true;
}

/* expr := product (('+' | '-') product)*, left to right */
static bool expr(const char **pp, int em, struct amount *out)
{
    struct amount a;
    if (!product(pp, em, &a))
        return false;
    for (;;) {
        const char *p = *pp;
        while (is_space(*p))
            p++;
        if ((*p != '+' && *p != '-') || !is_space(p[1]))
            break;                                      /* (CSS wants spaces around + and -) */
        char op = *p++;
        struct amount b;
        if (!product(&p, em, &b))
            return false;
        a.px += op == '+' ? b.px : -b.px;
        a.pct += op == '+' ? b.pct : -b.pct;
        a.number = a.number && b.number;
        *pp = p;
    }
    *out = a;
    return true;
}

/* A length in its two parts (px and a percentage); false if it is none.
 * "auto" sets *is_auto. */
static bool length_parts(const char *text, int em, double *px, double *pct, bool *is_auto)
{
    while (is_space(*text))
        text++;
    *is_auto = false;
    if (starts_word(text, "auto")) {
        *is_auto = true;
        return true;
    }
    struct amount a;
    const char *p = text;
    if (!term(&p, em, &a))
        return false;
    while (is_space(*p))
        p++;
    if (*p && *p != ';' && *p != '!')
        return false;
    if (a.number && a.px != 0)
        return false;                                   /* (a plain number but 0: not a length) */
    if (a.px != a.px || a.pct != a.pct)
        return false;                                   /* (NaN: 0/0 somewhere) */
    *px = a.px > 100000 ? 100000 : a.px < -100000 ? -100000 : a.px;     /* (1e309px: as far as any) */
    *pct = a.pct > 100000 ? 100000 : a.pct < -100000 ? -100000 : a.pct;
    return true;
}

/* A length: "12px", "1.5em", "50%" (of `percent_of`), "0", "auto",
 * calc(...); `em` is the font size it is relative to. false if it is none. */
static bool length(const char *text, int em, int percent_of, int *out)
{
    double px, pct;
    bool is_auto;
    if (!length_parts(text, em, &px, &pct, &is_auto))
        return false;
    if (is_auto) {
        *out = LENGTH_AUTO;
        return true;
    }
    double v = px + (percent_of < 0 ? 0 : pct * percent_of / 100.0);
    if (v > 100000)
        v = 100000;
    if (v < -100000)
        v = -100000;
    *out = (int)(v < 0 ? v - 0.5 : v + 0.5);
    return true;
}

/* The words of a value, e.g. "1px solid #ccc": up to `max` of them. */
static int words(const char *value, char out[][64], int max)
{
    int n = 0;
    for (const char *p = value; *p && n < max;) {
        while (is_space(*p))
            p++;
        if (!*p)
            break;
        const char *s = p;
        int paren = 0;
        while (*p && (!is_space(*p) || paren)) {
            paren += *p == '(' ? 1 : *p == ')' ? -1 : 0;
            p++;
        }
        size_t len = (size_t)(p - s) < 63 ? (size_t)(p - s) : 63;
        memcpy(out[n], s, len);
        out[n][len] = 0;
        n++;
    }
    return n;
}

/* margin: 1 to 4 lengths into box[4]. */
static void box_sides(const char *value, int em, int *box)
{
    char w[4][64];
    int n = words(value, w, 4), v[4];
    for (int i = 0; i < n; i++)
        if (!length(w[i], em, -1, &v[i]))
            return;
    if (n == 1)
        box[0] = box[1] = box[2] = box[3] = v[0];
    else if (n == 2)
        box[0] = box[2] = v[0], box[1] = box[3] = v[1];
    else if (n == 3)
        box[0] = v[0], box[1] = box[3] = v[1], box[2] = v[2];
    else if (n == 4)
        box[0] = v[0], box[1] = v[1], box[2] = v[2], box[3] = v[3];
}

static int font_size_of(const char *value, int parent)
{
    static const struct { const char *name; int px; } SIZES[] = {
        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
        { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 }, { 0, 0 }
    };
    for (int i = 0; SIZES[i].name; i++)
        if (same_word(value, SIZES[i].name))
            return SIZES[i].px;
    if (same_word(value, "smaller"))
        return parent * 5 / 6;
    if (same_word(value, "larger"))
        return parent * 6 / 5;
    int px;
    if (length(value, parent, parent, &px) && px > 0)
        return px > 200 ? 200 : px;
    return parent;
}

/* A border width: thin, medium, thick or a length; -1 if none. */
static int border_width_of(const char *w, int em)
{
    int px;
    if (same_word(w, "thin"))
        return 1;
    if (same_word(w, "medium"))
        return 3;
    if (same_word(w, "thick"))
        return 5;
    if (length(w, em, -1, &px) && px >= 0)
        return px > 40 ? 40 : px;
    return -1;
}

static bool border_style_of(const char *w, bool *on)
{
    static const char *const STYLES[] = { "solid", "dashed", "dotted", "double", "groove", "ridge",
                                          "inset", "outset", 0 };
    if (same_word(w, "none") || same_word(w, "hidden")) {
        *on = false;
        return true;
    }
    for (int i = 0; STYLES[i]; i++)
        if (same_word(w, STYLES[i])) {
            *on = true;
            return true;
        }
    return false;
}

/* border, border-top, ...: width, style and colour of the sides in mask
 * (bit 0 top, 1 right, 2 bottom, 3 left); what is not given is reset. */
static void border(const char *value, struct style *s, int mask)
{
    char w[4][64];
    int n = words(value, w, 4), width = 3, alpha;
    bool on = false;
    uint32_t color = COLOR_NONE, c;
    for (int i = 0; i < n; i++) {
        int px = border_width_of(w[i], s->font_size);
        if (px >= 0)
            width = px;
        else if (border_style_of(w[i], &on))
            ;
        else if (css_color_alpha(w[i], &c, &alpha))
            color = alpha ? c : COLOR_CLEAR;
        else if (same_word(w[i], "currentcolor"))
            color = COLOR_NONE;
    }
    for (int k = 0; k < 4; k++)
        if (mask & 1 << k)
            s->border_set[k] = width, s->border_style[k] = on, s->border_colors[k] = color;
}

/* 1 to 4 words to the four sides (top, right, bottom, left), as margin. */
static int four_sides(const char *value, char out[4][64])
{
    char w[4][64];
    int n = words(value, w, 4);
    if (n == 0)
        return 0;
    int from[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
    for (int k = 0; k < 4; k++)
        memcpy(out[k], w[from[n - 1][k]], 64);
    return n;
}

/* A width-like value: px, or a percentage p (as -p - 1, up to 1000%) with pixels added
 * (*add, for calc(100% - 20px)), or -1 for none. */
static bool width_value(const char *value, int em, int *where, int *add, bool percent_ok)
{
    double px, pct;
    bool is_auto;
    if (same_word(value, "none") || same_word(value, "auto") || same_word(value, "fit-content") ||
        same_word(value, "max-content") || same_word(value, "min-content")) {
        *where = -1, *add = 0;
        return true;
    }
    if (!length_parts(value, em, &px, &pct, &is_auto))
        return false;
    if (is_auto) {
        *where = -1, *add = 0;
        return true;
    }
    if (pct != 0) {
        if (!percent_ok || pct <= 0 || pct > 1000)
            return false;
        *where = -(int)(pct + 0.5) - 1;
        *add = (int)(px < 0 ? px - 0.5 : px + 0.5);
        return true;
    }
    if (px < 0)
        return false;
    *where = (int)(px + 0.5);
    *add = 0;
    return true;
}

/* The properties read here (for @supports). */
static const char *const KNOWN[] = {
    "color", "background", "background-color", "display", "font-weight", "font-style", "font-size",
    "font", "text-align", "text-decoration", "text-decoration-line", "white-space",
    "list-style", "list-style-type", "margin", "padding", "margin-top", "margin-right",
    "margin-bottom", "margin-left", "padding-top", "padding-right", "padding-bottom",
    "padding-left", "border", "border-top", "border-right", "border-bottom", "border-left",
    "border-width", "border-color", "border-style", "float", "clear", "position", "top", "right",
    "bottom", "left", "z-index", "overflow", "overflow-x", "overflow-y", "width", "height",
    "max-width", "content", "flex", "flex-direction", "flex-wrap", "flex-flow", "flex-grow",
    "flex-shrink", "flex-basis", "justify-content", "align-items", "align-self", "align-content",
    "order", "gap", "row-gap", "column-gap", "min-width", "box-sizing", "box-shadow", "opacity",
    "visibility", "text-transform", "line-height", "vertical-align", "border-spacing", "font-family",
    "border-collapse", "transform", "min-height", "max-height", "grid-template-columns",
    "grid-template-rows", "grid-template-areas", "grid-column", "grid-row", "grid-area", "grid-auto-flow",
    "justify-items", "justify-self", "fill", "stroke", "stroke-width", "fill-opacity", "stroke-opacity", "fill-rule", 0
};

static bool known_property(const char *name)
{
    for (int i = 0; KNOWN[i]; i++)
        if (!strcmp(KNOWN[i], name))
            return true;
    if (!strncmp(name, "border", 6) || !strncmp(name, "background", 10))
        return true;                                    /* (27h: all their parts) */
    return name[0] == '-' && name[1] == '-';            /* (custom properties) */
}

/* rgb at alpha over under (as the layout's mix). */
static uint32_t mix_rgb(uint32_t rgb, int alpha, uint32_t under)
{
    if (alpha >= 255 || under & 0xFF000000u)
        return rgb;
    if (alpha <= 0)
        return under;
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int a = (int)(rgb >> shift & 255), b = (int)(under >> shift & 255);
        out |= (uint32_t)((a * alpha + b * (255 - alpha)) / 255) << shift;
    }
    return out;
}

/* (27n) grid-column / grid-row: "a", "a / b", "span n", "a / span n",
 * "auto": the start and end lines (0: auto) and the span. */
static void grid_lines(const char *value, int *start, int *end, int *span)
{
    *start = *end = 0, *span = 1;
    for (int part = 0; part < 2; part++) {
        const char *slash = strchr(value, '/');
        char text[64];
        size_t len = part == 0 ? (slash ? (size_t)(slash - value) : strlen(value)) : slash ? strlen(slash + 1) : 0;
        if (part == 1 && !slash)
            break;
        snprintf(text, sizeof(text), "%.*s", (int)(len < 63 ? len : 63), part == 0 ? value : slash + 1);
        char w[2][64];
        int n = words(text, w, 2);
        int *line = part == 0 ? start : end;
        if (n == 2 && same_word(w[0], "span"))
            *span = atoi(w[1]) > 0 ? (atoi(w[1]) > 1000 ? 1000 : atoi(w[1])) : 1;
        else if (n == 1 && !same_word(w[0], "auto"))
            *line = atoi(w[0]) > 1000 ? 1000 : atoi(w[0]) < -1000 ? -1000 : atoi(w[0]);
    }
}

bool css_length_px(const char *text, int em, int percent_of, int *out)
{
    return length(text, em, percent_of, out);
}

/* --- backgrounds, corners, shadows (27h) --- */

/* The first layer of a comma-separated list (outside parentheses): malloc'ed. */
static char *first_layer(const char *value)
{
    int paren = 0;
    size_t i = 0;
    for (; value[i] && (paren || value[i] != ','); i++)
        paren += value[i] == '(' ? 1 : value[i] == ')' ? -1 : 0;
    return copy_text(value, i);
}

static void background_repeat(struct style *s, const char *value)
{
    char w[2][64];
    int n = words(value, w, 2);
    if (n == 1 && same_word(w[0], "repeat-x"))
        s->bg_repeat = REPEAT_X;
    else if (n == 1 && same_word(w[0], "repeat-y"))
        s->bg_repeat = REPEAT_Y;
    else if (n >= 1 && same_word(w[0], "no-repeat") && (n == 1 || same_word(w[1], "no-repeat")))
        s->bg_repeat = REPEAT_NONE;
    else if (n == 2 && same_word(w[1], "no-repeat"))
        s->bg_repeat = REPEAT_X;
    else if (n == 2 && same_word(w[0], "no-repeat"))
        s->bg_repeat = REPEAT_Y;
    else
        s->bg_repeat = REPEAT_BOTH;                     /* (repeat, space, round) */
}

/* A position or size part: a length or a percentage. */
static bool bg_length(const char *w, int em, int *v, bool *pct)
{
    double px, p;
    bool is_auto;
    if (!length_parts(w, em, &px, &p, &is_auto) || is_auto)
        return false;
    *pct = p != 0;
    *v = (int)(*pct ? p : px);
    return true;
}

static void background_position(struct style *s, const char *value)
{
    char w[4][64];
    int n = words(value, w, 4), x = 0, y = 0;
    bool xp = true, yp = true, have_x = false, have_y = false;
    for (int i = 0; i < n; i++) {
        int v;
        bool pct;
        if (same_word(w[i], "left") || same_word(w[i], "right")) {
            x = w[i][0] == 'l' || w[i][0] == 'L' ? 0 : 100, xp = true, have_x = true;
        } else if (same_word(w[i], "top") || same_word(w[i], "bottom")) {
            y = w[i][0] == 't' || w[i][0] == 'T' ? 0 : 100, yp = true, have_y = true;
        } else if (same_word(w[i], "center")) {
            if (!have_x)
                x = 50, xp = true, have_x = true;
            else
                y = 50, yp = true, have_y = true;
        } else if (bg_length(w[i], s->font_size, &v, &pct)) {
            if (!have_x)
                x = v, xp = pct, have_x = true;
            else
                y = v, yp = pct, have_y = true;
        }
    }
    if (have_x && !have_y)
        y = 50, yp = true;                              /* (one value: the other centred) */
    s->bg_x = x, s->bg_x_pct = xp, s->bg_y = y, s->bg_y_pct = yp;
}

static void background_size(struct style *s, const char *value)
{
    char w[2][64];
    int n = words(value, w, 2);
    s->bg_size = BG_SIZE_AUTO;
    if (n == 1 && same_word(w[0], "cover"))
        s->bg_size = BG_SIZE_COVER;
    else if (n == 1 && same_word(w[0], "contain"))
        s->bg_size = BG_SIZE_CONTAIN;
    else if (n >= 1) {
        s->bg_w = s->bg_h = LENGTH_AUTO;
        s->bg_w_pct = s->bg_h_pct = false;
        bool any = false;
        if (bg_length(w[0], s->font_size, &s->bg_w, &s->bg_w_pct))
            any = true;
        if (n == 2 && bg_length(w[1], s->font_size, &s->bg_h, &s->bg_h_pct))
            any = true;
        if (any)
            s->bg_size = BG_SIZE_GIVEN;
    }
}

/* The background shorthand: its last layer gives the colour, its first the
 * picture; what is not given goes back to its initial value. */
static void background(struct style *s, const char *value)
{
    free(s->background_image);
    s->background_image = 0;
    s->background = COLOR_NONE, s->background_alpha = 255;
    s->bg_repeat = REPEAT_BOTH, s->bg_size = BG_SIZE_AUTO;
    s->bg_x = s->bg_y = 0, s->bg_x_pct = s->bg_y_pct = true;
    char *layer = first_layer(value);
    if (!layer)
        return;
    const char *last = value;
    for (const char *p = value; *p;) {                  /* (the last layer's colour) */
        int paren = 0;
        const char *q = p;
        while (*q && (paren || *q != ','))
            paren += *q == '(' ? 1 : *q == ')' ? -1 : 0, q++;
        last = p;
        p = *q ? q + 1 : q;
    }
    char w[12][64];
    int n = words(last, w, 12), alpha;
    uint32_t c;
    for (int i = 0; i < n; i++)
        if (css_color_alpha(w[i], &c, &alpha))
            s->background = alpha ? c : COLOR_NONE, s->background_alpha = alpha;
        else if (same_word(w[i], "currentcolor"))
            s->background = s->color;
    /* The first layer: picture, repeat, position [/ size]. */
    const char *u = strstr(layer, "url(");
    const char *g = strstr(layer, "gradient(");
    if (u || g) {
        const char *start = u ? u : g;
        while (start > layer && !is_space(start[-1]))
            start--;                                    /* (linear-gradient: its name) */
        size_t end = paren_end(start, strlen(start), (size_t)(strchr(start, '(') - start));
        if (end)
            s->background_image = copy_text(start, end);
    }
    char rest[512];
    size_t o = 0;
    int paren = 0;
    for (const char *p = layer; *p && o + 1 < sizeof(rest); p++) {     /* (without the functions) */
        if (*p == '(')
            paren++;
        if (!paren)
            rest[o++] = *p;
        if (*p == ')')
            paren--;
    }
    rest[o] = 0;
    char *slash = strchr(rest, '/');
    if (slash) {
        *slash = 0;
        background_size(s, slash + 1);
    }
    char pos[256] = "", rep[64] = "";
    n = words(rest, w, 12);
    for (int i = 0; i < n; i++) {
        if (starts_word(w[i], "repeat") || starts_word(w[i], "no-repeat") || same_word(w[i], "space") ||
            same_word(w[i], "round"))
            snprintf(rep + strlen(rep), sizeof(rep) - strlen(rep), "%s ", w[i]);
        else if (same_word(w[i], "left") || same_word(w[i], "right") || same_word(w[i], "top") ||
                 same_word(w[i], "bottom") || same_word(w[i], "center") ||
                 (w[i][0] >= '0' && w[i][0] <= '9') || w[i][0] == '-' || w[i][0] == '.')
            snprintf(pos + strlen(pos), sizeof(pos) - strlen(pos), "%s ", w[i]);
    }
    if (rep[0])
        background_repeat(s, rep);
    if (pos[0])
        background_position(s, pos);
    free(layer);
}

static void radius_of(struct style *s, const char *w, int *out)
{
    double px, pct;
    bool is_auto;
    if (length_parts(w, s->font_size, &px, &pct, &is_auto) && !is_auto && px >= 0 && pct >= 0)
        *out = pct > 0 ? -(int)(pct > 50 ? 50 : pct + 0.5) : (int)(px > 5000 ? 5000 : px + 0.5);
}

/* box-shadow: the first shadow that is not inset (offsets, blur, spread,
 * colour; the colour by default the text's). */
static void box_shadow(struct style *s, const char *value)
{
    s->shadow = false;
    for (const char *p = value; *p;) {
        int paren = 0;
        const char *q = p;
        while (*q && (paren || *q != ','))
            paren += *q == '(' ? 1 : *q == ')' ? -1 : 0, q++;
        char one[256];
        snprintf(one, sizeof(one), "%.*s", (int)(q - p < 255 ? q - p : 255), p);
        p = *q ? q + 1 : q;
        char w[8][64];
        int n = words(one, w, 8), lens[4], count = 0, alpha = 255;
        uint32_t color = s->color;
        bool inset = false;
        for (int i = 0; i < n; i++) {
            uint32_t c;
            int a;
            if (same_word(w[i], "inset"))
                inset = true;
            else if (count < 4 && length(w[i], s->font_size, -1, &lens[count]))
                count++;
            else if (css_color_alpha(w[i], &c, &a))
                color = c, alpha = a;
        }
        if (inset || count < 2 || alpha == 0)
            continue;
        s->shadow = true;
        s->shadow_x = lens[0], s->shadow_y = lens[1];
        s->shadow_blur = count > 2 && lens[2] > 0 ? (lens[2] > 100 ? 100 : lens[2]) : 0;
        s->shadow_spread = count > 3 ? (lens[3] > 100 ? 100 : lens[3] < -100 ? -100 : lens[3]) : 0;
        s->shadow_color = color, s->shadow_alpha = alpha;
        return;
    }
}

char *css_url(const char *value)
{
    const char *u = strstr(value, "url(");
    if (!u)
        return 0;
    u += 4;
    while (is_space(*u))
        u++;
    char q = *u == '"' || *u == '\'' ? *u++ : 0;
    const char *end = u;
    while (*end && (q ? *end != q : *end != ')' && !is_space(*end)))
        end++;
    if (end == u)
        return 0;
    return copy_text(u, (size_t)(end - u));
}

char *css_absolute_urls(const char *text, const char *base)
{
    char *out = copy_text("", 0);
    for (const char *p = text; *p && out;) {
        const char *u = strstr(p, "url(");
        if (!u) {
            out = join_text(out, p);
            break;
        }
        char *head = copy_text(p, (size_t)(u - p + 4));
        out = head ? join_text(out, head) : out;
        free(head);
        const char *a = u + 4;
        while (is_space(*a))
            a++;
        char q = *a == '"' || *a == '\'' ? *a++ : 0;
        const char *end = a;
        while (*end && (q ? *end != q : *end != ')' && !is_space(*end)))
            end++;
        char *ref = copy_text(a, (size_t)(end - a));
        char absolute[URL_MAX];
        if (ref && !starts_word(ref, "data:") && url_resolve(base, ref, absolute, sizeof(absolute)))
            out = join_text(out, absolute);
        else if (ref)
            out = join_text(out, ref);
        free(ref);
        p = q && *end == q ? end + 1 : end;
    }
    return out;
}

/* A gradient's colour stop: COLOR [POSITION%]. */
bool css_gradient(const char *text, struct gradient *g, uint32_t under)
{
    memset(g, 0, sizeof(*g));
    const char *open = strchr(text, '(');
    if (!open)
        return false;
    bool radial = strstr(text, "radial") != 0 && strstr(text, "radial") < open;
    size_t end = paren_end(open, strlen(open), 0);
    if (!end)
        return false;
    char inner[1024];
    snprintf(inner, sizeof(inner), "%.*s", (int)(end - 2 < 1023 ? end - 2 : 1023), open + 1);
    g->across = false, g->reverse = false;              /* (to bottom by default) */
    int part = 0;
    for (char *p = inner; *p && g->count < GRADIENT_STOPS; part++) {
        int paren = 0;
        char *q = p;
        while (*q && (paren || *q != ','))
            paren += *q == '(' ? 1 : *q == ')' ? -1 : 0, q++;
        char one[256];
        snprintf(one, sizeof(one), "%.*s", (int)(q - p < 255 ? q - p : 255), p);
        p = *q ? q + 1 : q;
        char w[4][64];
        int n = words(one, w, 4), alpha;
        uint32_t c;
        if (part == 0 && n && !css_color_alpha(w[0], &c, &alpha)) {      /* the direction */
            if (radial)
                continue;
            if (same_word(w[0], "to")) {
                for (int i = 1; i < n; i++) {
                    if (same_word(w[i], "right"))
                        g->across = true, g->reverse = false;
                    else if (same_word(w[i], "left"))
                        g->across = true, g->reverse = true;
                    else if (same_word(w[i], "top") && n == 2)
                        g->reverse = true;
                }
            } else {
                double deg = atof(w[0]);
                if (strstr(w[0], "turn"))
                    deg *= 360;
                else if (strstr(w[0], "rad"))
                    deg *= 57.29578;
                deg -= 360.0 * (double)(long)(deg / 360.0);
                if (deg < 0)
                    deg += 360;
                int quarter = (int)((deg + 45) / 90) % 4;           /* (the nearest of the four) */
                g->across = quarter == 1 || quarter == 3;
                g->reverse = quarter == 0 || quarter == 3;
            }
            continue;
        }
        if (!n || !css_color_alpha(w[0], &c, &alpha))
            continue;
        g->colors[g->count] = mix_rgb(c, alpha, under);
        g->at[g->count] = n > 1 && strchr(w[1], '%') ? (int)(atof(w[1]) * 10) : -1;
        g->count++;
    }
    if (g->count == 0)
        return false;
    for (int i = 0; i < g->count; i++)                  /* positions not given: spread evenly */
        if (g->at[i] < 0)
            g->at[i] = g->count == 1 ? 0 : i * 1000 / (g->count - 1);
    for (int i = 1; i < g->count; i++)
        if (g->at[i] < g->at[i - 1])
            g->at[i] = g->at[i - 1];
    return true;                                        /* (a radial one: top to bottom, roughly) */
}

/* (27k) font-family: the first family in the list that maps to one of
 * ours (a name we know, or a generic one); 0 if none does. */
static bool family_of(const char *value, enum font_family *out)
{
    static const struct { const char *name; enum font_family f; } NAMES[] = {
        { "serif", FAMILY_SERIF }, { "sans-serif", FAMILY_SANS }, { "monospace", FAMILY_MONO },
        { "system-ui", FAMILY_SANS }, { "ui-sans-serif", FAMILY_SANS }, { "ui-serif", FAMILY_SERIF },
        { "ui-monospace", FAMILY_MONO }, { "-apple-system", FAMILY_SANS },
        { "blinkmacsystemfont", FAMILY_SANS }, { "cursive", FAMILY_SERIF }, { "fantasy", FAMILY_SANS },
        { "times", FAMILY_SERIF }, { "times new roman", FAMILY_SERIF }, { "georgia", FAMILY_SERIF },
        { "garamond", FAMILY_SERIF }, { "palatino", FAMILY_SERIF }, { "palatino linotype", FAMILY_SERIF },
        { "cambria", FAMILY_SERIF }, { "book antiqua", FAMILY_SERIF }, { "baskerville", FAMILY_SERIF },
        { "charter", FAMILY_SERIF }, { "merriweather", FAMILY_SERIF }, { "noto serif", FAMILY_SERIF },
        { "source serif pro", FAMILY_SERIF }, { "pt serif", FAMILY_SERIF }, { "dejavu serif", FAMILY_SERIF },
        { "liberation serif", FAMILY_SERIF },
        { "helvetica", FAMILY_SANS }, { "helvetica neue", FAMILY_SANS }, { "arial", FAMILY_SANS },
        { "verdana", FAMILY_SANS }, { "tahoma", FAMILY_SANS }, { "segoe ui", FAMILY_SANS },
        { "roboto", FAMILY_SANS }, { "inter", FAMILY_SANS }, { "open sans", FAMILY_SANS },
        { "lato", FAMILY_SANS }, { "ubuntu", FAMILY_SANS }, { "noto sans", FAMILY_SANS },
        { "trebuchet ms", FAMILY_SANS }, { "geneva", FAMILY_SANS }, { "lucida grande", FAMILY_SANS },
        { "dejavu sans", FAMILY_SANS }, { "liberation sans", FAMILY_SANS }, { "sf pro text", FAMILY_SANS },
        { "courier", FAMILY_MONO }, { "courier new", FAMILY_MONO }, { "consolas", FAMILY_MONO },
        { "menlo", FAMILY_MONO }, { "monaco", FAMILY_MONO }, { "lucida console", FAMILY_MONO },
        { "dejavu sans mono", FAMILY_MONO }, { "liberation mono", FAMILY_MONO },
        { "source code pro", FAMILY_MONO }, { "fira code", FAMILY_MONO }, { "fira mono", FAMILY_MONO },
        { "jetbrains mono", FAMILY_MONO }, { "sf mono", FAMILY_MONO }, { "sfmono-regular", FAMILY_MONO },
        { "roboto mono", FAMILY_MONO }, { "andale mono", FAMILY_MONO }, { "noto sans mono", FAMILY_MONO },
        { "fixed", FAMILY_FIXED }, { "misc-fixed", FAMILY_FIXED },     /* (X11's names: the system's own) */
        { 0, 0 }
    };
    bool unknown = false;
    for (const char *p = value; *p;) {
        while (is_space(*p) || *p == ',')
            p++;
        char name[48];
        size_t n = 0;
        char q = *p == '"' || *p == '\'' ? *p++ : 0;
        while (*p && (q ? *p != q : *p != ',')) {
            if (n + 1 < sizeof(name))
                name[n++] = (char)lower((unsigned char)*p);
            p++;
        }
        if (q && *p)
            p++;
        while (n && is_space(name[n - 1]))
            n--;
        name[n] = 0;
        if (!n)
            continue;
        for (int i = 0; NAMES[i].name; i++)
            if (!strcmp(NAMES[i].name, name)) {
                *out = NAMES[i].f;
                return true;
            }
        unknown = true;
    }
    if (unknown) {                                      /* (a web font alone: a sans face looks most like) */
        *out = FAMILY_SANS;
        return true;
    }
    return false;
}

/* `inherit`, `initial` and `unset` for the properties kept in a style. */
static bool keyword(struct style *s, const struct style *parent, const char *name, const char *value)
{
    bool inherit = same_word(value, "inherit"), initial = same_word(value, "initial"),
         unset = same_word(value, "unset") || same_word(value, "revert") ||
                 same_word(value, "revert-layer");
    if (!inherit && !initial && !unset)
        return false;
    static const char *const INHERITED[] = { "color", "font-size", "font-weight", "font-style",
                                             "text-align", "white-space", "list-style",
                                             "list-style-type", "font", "visibility", "font-family",
                                             "text-transform", "line-height", 0 };
    bool inherited = false;
    for (int i = 0; INHERITED[i]; i++)
        inherited = inherited || !strcmp(name, INHERITED[i]);
    bool from_parent = inherit || (unset && inherited);
    struct style def;
    memset(&def, 0, sizeof(def));
    def.font_size = 16, def.background = COLOR_NONE;
    def.width = def.height = def.max_width = -1;
    def.min_width = def.basis = -1, def.shrink = 1, def.align_self = FLEX_AUTO, def.max_height = -1;
    def.opacity = def.background_alpha = def.color_alpha = 255;
    for (int k = 0; k < 4; k++)
        def.border_set[k] = 3, def.border_colors[k] = COLOR_NONE;
    const struct style *src = from_parent && parent ? parent : &def;
    if (!strcmp(name, "color"))
        s->color = src->color;
    else if (!strcmp(name, "font-size"))
        s->font_size = src->font_size;
    else if (!strcmp(name, "font-weight"))
        s->bold = src->bold;
    else if (!strcmp(name, "font-style"))
        s->italic = src->italic;
    else if (!strcmp(name, "font"))
        s->font_size = src->font_size, s->bold = src->bold, s->italic = src->italic, s->family = src->family;
    else if (!strcmp(name, "font-family"))
        s->family = src->family;
    else if (!strcmp(name, "text-align"))
        s->align = src->align;
    else if (!strcmp(name, "white-space"))
        s->pre = src->pre, s->nowrap = src->nowrap;
    else if (!strcmp(name, "list-style") || !strcmp(name, "list-style-type"))
        s->list = src->list;
    else if (!strcmp(name, "background") || !strcmp(name, "background-color"))
        s->background = src->background, s->background_alpha = src->background_alpha;
    else if (!strcmp(name, "display"))
        s->display = from_parent && parent ? parent->display : DISPLAY_INLINE,
        s->flex = from_parent && parent ? parent->flex : false;
    else if (!strcmp(name, "text-decoration") || !strcmp(name, "text-decoration-line"))
        s->underline = src->underline, s->strike = src->strike;
    else if (!strcmp(name, "width"))
        s->width = src->width, s->width_add = src->width_add;
    else if (!strcmp(name, "max-width"))
        s->max_width = src->max_width, s->max_width_add = src->max_width_add;
    else if (!strcmp(name, "height"))
        s->height = src->height, s->height_pct = src->height_pct, s->height_add = src->height_add;
    else if (!strcmp(name, "transform"))
        s->translated = false;
    else if (!strcmp(name, "margin"))
        memcpy(s->margin, src->margin, sizeof(s->margin));
    else if (!strcmp(name, "padding"))
        memcpy(s->padding, src->padding, sizeof(s->padding));
    else if (!strcmp(name, "border") || !strcmp(name, "border-width") || !strcmp(name, "border-style"))
        memcpy(s->border_set, src->border_set, sizeof(s->border_set)),
        memcpy(s->border_style, src->border_style, sizeof(s->border_style));
    else if (!strcmp(name, "border-color"))
        memcpy(s->border_colors, src->border_colors, sizeof(s->border_colors));
    else if (!strcmp(name, "opacity"))
        s->opacity = from_parent && parent ? parent->opacity : 255;
    else if (!strcmp(name, "visibility"))
        s->hidden = from_parent && parent ? parent->hidden : false;
    else if (!strcmp(name, "text-transform"))
        s->transform = src->transform;
    else if (!strcmp(name, "line-height"))
        s->line_factor = src->line_factor, s->line_px = src->line_px;
    else if (!strcmp(name, "border-radius"))
        memcpy(s->radius, src->radius, sizeof(s->radius));
    else if (!strcmp(name, "box-shadow"))
        s->shadow = from_parent && parent ? parent->shadow : false;
    return true;
}

static void declare(struct style *s, const struct style *parent, const char *name, const char *value)
{
    uint32_t c;
    int px;
    int em = parent ? parent->font_size : 16;
    if (keyword(s, parent, name, value))
        return;
    int alpha;
    if (name[0] == '-' && name[1] != '-') {             /* -webkit-box-shadow and the like */
        static const char *const VENDORS[] = { "-webkit-", "-moz-", "-ms-", "-o-", 0 };
        for (int i = 0; VENDORS[i]; i++)
            if (!strncmp(name, VENDORS[i], strlen(VENDORS[i])) && known_property(name + strlen(VENDORS[i]))) {
                declare(s, parent, name + strlen(VENDORS[i]), value);
                return;
            }
        return;
    }
    if (!strcmp(name, "color")) {
        if (css_color_alpha(value, &c, &alpha) && alpha > 0)
            s->color = c, s->color_alpha = alpha;
        else if (same_word(value, "currentcolor") && parent)
            s->color = parent->color, s->color_alpha = parent->color_alpha;
    } else if (!strcmp(name, "background-color")) {
        if (css_color_alpha(value, &c, &alpha))
            s->background = alpha ? c : COLOR_NONE, s->background_alpha = alpha;
        else if (same_word(value, "currentcolor"))
            s->background = s->color, s->background_alpha = 255;
    } else if (!strcmp(name, "background")) {
        background(s, value);
    } else if (!strcmp(name, "background-image")) {
        free(s->background_image);
        s->background_image = 0;
        char w[1][64];
        if (words(value, w, 1) && (starts_word(value, "url(") || strstr(value, "gradient(")))
            s->background_image = first_layer(value);
    } else if (!strcmp(name, "background-repeat")) {
        background_repeat(s, value);
    } else if (!strcmp(name, "background-position")) {
        char *layer = first_layer(value);
        if (layer)
            background_position(s, layer);
        free(layer);
    } else if (!strcmp(name, "background-size")) {
        char *layer = first_layer(value);
        if (layer)
            background_size(s, layer);
        free(layer);
    } else if (!strcmp(name, "display")) {
        s->flex = same_word(value, "flex") || same_word(value, "inline-flex");
        s->table = same_word(value, "table") || same_word(value, "inline-table");      /* (27i) */
        s->grid = same_word(value, "grid") || same_word(value, "inline-grid");          /* (27n) */
        if (same_word(value, "none"))
            s->display = DISPLAY_NONE;
        else if (same_word(value, "contents"))
            s->display = DISPLAY_CONTENTS;
        else if (same_word(value, "block") || same_word(value, "flex") || same_word(value, "grid") ||
                 same_word(value, "table") || starts_word(value, "table-") ||
                 same_word(value, "flow-root"))
            s->display = DISPLAY_BLOCK;
        else if (same_word(value, "list-item"))
            s->display = DISPLAY_LIST_ITEM;
        else if (same_word(value, "table-row"))
            s->display = DISPLAY_TABLE_ROW;
        else if (same_word(value, "table-cell"))
            s->display = DISPLAY_TABLE_CELL;
        else if (same_word(value, "inline-block") || same_word(value, "inline-flex") ||
                 same_word(value, "inline-grid") || same_word(value, "inline-table"))
            s->display = DISPLAY_INLINE_BLOCK;
        else if (starts_word(value, "inline"))
            s->display = DISPLAY_INLINE;
        if (same_word(value, "flow-root"))
            s->holds_floats = true;
        if (same_word(value, "table-row"))
            s->display = DISPLAY_TABLE_ROW;
        else if (same_word(value, "table-cell"))
            s->display = DISPLAY_TABLE_CELL;
    } else if (!strcmp(name, "font-weight")) {
        s->bold = same_word(value, "bold") || same_word(value, "bolder") ||
                  (value[0] >= '6' && value[0] <= '9' && value[1] == '0' && value[2] == '0');
    } else if (!strcmp(name, "font-style")) {
        s->italic = same_word(value, "italic") || same_word(value, "oblique");
    } else if (!strcmp(name, "font-family")) {                 /* (27k) */
        family_of(value, &s->family);
    } else if (!strcmp(name, "font-size")) {
        s->font_size = font_size_of(value, em);
    } else if (!strcmp(name, "font")) {
        char w[8][64];
        int n = words(value, w, 8);
        for (int i = 0; i < n; i++) {
            if (same_word(w[i], "bold"))
                s->bold = true;
            else if (same_word(w[i], "italic"))
                s->italic = true;
            else if ((w[i][0] >= '0' && w[i][0] <= '9') || w[i][0] == '.' || starts_word(w[i], "calc(") ||
                     starts_word(w[i], "clamp(")) {
                char *slash = strchr(w[i], '/');
                if (slash)
                    *slash = 0;
                if (!(w[i][0] >= '1' && w[i][0] <= '9' && w[i][1] == '0' && w[i][2] == '0' && !w[i][3])) {
                    s->font_size = font_size_of(w[i], em);
                    /* (27k) what follows the size is the family list */
                    const char *at = strstr(value, w[i]);
                    if (at) {
                        at += strlen(w[i]);
                        while (*at && !is_space(*at))
                            at++;
                        family_of(at, &s->family);
                    }
                }
            }
        }
    } else if (!strcmp(name, "text-align")) {
        s->align = same_word(value, "center") ? ALIGN_CENTER
                 : same_word(value, "right") || same_word(value, "end") ? ALIGN_RIGHT : ALIGN_LEFT;
    } else if (!strcmp(name, "text-decoration") || !strcmp(name, "text-decoration-line")) {
        s->underline = strstr(value, "underline") != 0;
        s->strike = strstr(value, "line-through") != 0;
    } else if (!strcmp(name, "white-space")) {
        s->pre = starts_word(value, "pre");
        s->nowrap = same_word(value, "nowrap") || same_word(value, "pre");     /* (27n) */
    } else if (!strcmp(name, "list-style-type") || !strcmp(name, "list-style")) {
        if (strstr(value, "none"))
            s->list = LIST_NONE;
        else if (strstr(value, "decimal"))
            s->list = LIST_DECIMAL;
        else if (strstr(value, "circle"))
            s->list = LIST_CIRCLE;
        else if (strstr(value, "square"))
            s->list = LIST_SQUARE;
        else if (strstr(value, "disc"))
            s->list = LIST_DISC;
    } else if (!strcmp(name, "margin")) {
        box_sides(value, s->font_size, s->margin);
    } else if (!strcmp(name, "padding")) {
        box_sides(value, s->font_size, s->padding);
    } else if (!strncmp(name, "margin-", 7) || !strncmp(name, "padding-", 8)) {
        int *box = name[0] == 'm' ? s->margin : s->padding;
        const char *side = strchr(name, '-') + 1;
        int i = !strcmp(side, "top") || !strcmp(side, "block-start") ? 0
              : !strcmp(side, "right") || !strcmp(side, "inline-end") ? 1
              : !strcmp(side, "bottom") || !strcmp(side, "block-end") ? 2
              : !strcmp(side, "left") || !strcmp(side, "inline-start") ? 3 : -1;
        if (i >= 0 && length(value, s->font_size, -1, &px))
            box[i] = px;
        else if (!strcmp(side, "inline") || !strcmp(side, "block")) {   /* (two sides at once) */
            int two[4] = { box[0], box[1], box[2], box[3] };
            char w[2][64];
            int n = words(value, w, 2), a, b;
            if (n >= 1 && length(w[0], s->font_size, -1, &a)) {
                b = n == 2 && length(w[1], s->font_size, -1, &b) ? b : a;
                if (side[0] == 'i')
                    two[3] = a, two[1] = b;
                else
                    two[0] = a, two[2] = b;
                memcpy(box, two, sizeof(two));
            }
        }
    } else if (!strncmp(name, "border", 6) && !strstr(name, "radius") && !strstr(name, "collapse") &&
               !strstr(name, "spacing") && !strstr(name, "image")) {
        /* border, border-SIDE, border-SIDE-PART, border-PART (1-4 sides) */
        static const char *const SIDES[] = { "top", "right", "bottom", "left" };
        const char *rest = name + 6;
        int mask = 15;
        for (int k = 0; k < 4; k++)
            if (rest[0] == '-' && !strncmp(rest + 1, SIDES[k], strlen(SIDES[k]))) {
                mask = 1 << k;
                rest += 1 + strlen(SIDES[k]);
            }
        if (!strncmp(rest, "-block", 6) || !strncmp(rest, "-inline", 7)) {
            mask = rest[2] == 'l' ? 5 : 10;     /* (block: top and bottom; inline: the sides) */
            rest += rest[2] == 'l' ? 6 : 7;
            if (!strncmp(rest, "-start", 6) || !strncmp(rest, "-end", 4)) {
                bool start = rest[1] == 's';
                mask = mask == 5 ? (start ? 1 : 4) : (start ? 8 : 2);
                rest += start ? 6 : 4;
            }
        }
        if (!*rest) {
            border(value, s, mask);
        } else if (!strcmp(rest, "-width") || !strcmp(rest, "-style") || !strcmp(rest, "-color")) {
            char v[4][64];
            if (mask == 15 ? !four_sides(value, v) : !words(value, v, 1))
                return;
            for (int k = 0; k < 4; k++) {
                if (!(mask & 1 << k))
                    continue;
                const char *one = mask == 15 ? v[k] : v[0];
                int px;
                bool on;
                if (rest[1] == 'w' && (px = border_width_of(one, s->font_size)) >= 0)
                    s->border_set[k] = px;
                else if (rest[1] == 's' && border_style_of(one, &on))
                    s->border_style[k] = on;
                else if (rest[1] == 'c' && css_color_alpha(one, &c, &alpha))
                    s->border_colors[k] = alpha ? c : COLOR_CLEAR;
                else if (rest[1] == 'c' && same_word(one, "currentcolor"))
                    s->border_colors[k] = COLOR_NONE;
            }
        }
    } else if (!strcmp(name, "border-radius")) {
        char v[4][64], first[256];
        size_t cut = strcspn(value, "/");               /* (elliptic corners: their first radii) */
        snprintf(first, sizeof(first), "%.*s", (int)(cut < 255 ? cut : 255), value);
        if (four_sides(first, v))
            for (int k = 0; k < 4; k++)
                radius_of(s, v[k], &s->radius[k]);
    } else if (!strcmp(name, "border-top-left-radius") || !strcmp(name, "border-top-right-radius") ||
               !strcmp(name, "border-bottom-right-radius") || !strcmp(name, "border-bottom-left-radius")) {
        int k = name[7] == 't' ? (name[11] == 'l' ? 0 : 1) : (name[14] == 'r' ? 2 : 3);
        char v[1][64];
        if (words(value, v, 1))
            radius_of(s, v[0], &s->radius[k]);
    } else if (!strcmp(name, "box-shadow")) {
        box_shadow(s, value);
    } else if (!strcmp(name, "opacity")) {
        char *end;
        double v = strtod(value, &end);
        if (end != value) {
            if (*end == '%')
                v /= 100;
            s->opacity = !(v > 0) ? 0 : v >= 1 ? 255 : (int)(v * 255 + 0.5);
        }
    } else if (!strcmp(name, "visibility")) {
        s->hidden = same_word(value, "hidden") || same_word(value, "collapse");
    } else if (!strcmp(name, "text-transform")) {
        s->transform = same_word(value, "uppercase") ? TRANSFORM_UPPER
                     : same_word(value, "lowercase") ? TRANSFORM_LOWER
                     : same_word(value, "capitalize") ? TRANSFORM_CAPITALIZE : TRANSFORM_NONE;
    } else if (!strcmp(name, "line-height")) {
        char *end;
        double v = strtod(value, &end);
        if (same_word(value, "normal"))
            s->line_factor = 0, s->line_px = 0;
        else if (end != value && (!*end || is_space(*end)) && v >= 0)
            s->line_factor = (float)(v > 10 ? 10 : v), s->line_px = 0;     /* (a number: inherited as one) */
        else if (length(value, s->font_size, s->font_size, &px) && px >= 0)
            s->line_factor = 0, s->line_px = px > 1000 ? 1000 : px;
    } else if (!strcmp(name, "vertical-align")) {
        s->valign = same_word(value, "sub") ? VALIGN_SUB : same_word(value, "super") ? VALIGN_SUPER
                  : same_word(value, "top") || same_word(value, "text-top") ? VALIGN_TOP
                  : same_word(value, "middle") ? VALIGN_MIDDLE
                  : same_word(value, "bottom") || same_word(value, "text-bottom") ? VALIGN_BOTTOM
                  : VALIGN_BASELINE;
        if (s->valign == VALIGN_BASELINE && length(value, s->font_size, s->font_size, &px))
            s->valign = VALIGN_LENGTH, s->raise = px > 500 ? 500 : px < -500 ? -500 : px;
    } else if (!strcmp(name, "float")) {                    /* (27d) */
        s->floating = same_word(value, "left") || same_word(value, "inline-start") ? FLOAT_LEFT
                    : same_word(value, "right") || same_word(value, "inline-end") ? FLOAT_RIGHT
                    : FLOAT_NONE;
    } else if (!strcmp(name, "clear")) {
        s->clear = same_word(value, "left") ? CLEAR_LEFT : same_word(value, "right") ? CLEAR_RIGHT
                 : same_word(value, "both") ? CLEAR_LEFT | CLEAR_RIGHT : 0;
    } else if (!strcmp(name, "position")) {
        s->position = same_word(value, "relative") ? POSITION_RELATIVE
                    : same_word(value, "absolute") ? POSITION_ABSOLUTE
                    : same_word(value, "fixed") ? POSITION_FIXED
                    : same_word(value, "sticky") ? POSITION_RELATIVE     /* (sticks nowhere here) */
                    : POSITION_STATIC;
    } else if (!strncmp(name, "inset-", 6)) {                   /* (27m) the logical names, left to right */
        const char *side = !strcmp(name, "inset-inline-start") ? "left" : !strcmp(name, "inset-inline-end") ? "right"
                         : !strcmp(name, "inset-block-start") ? "top" : !strcmp(name, "inset-block-end") ? "bottom" : 0;
        if (side)
            declare(s, parent, side, value);
        else if (!strcmp(name, "inset-inline") || !strcmp(name, "inset-block")) {
            char w[2][64];
            int n = words(value, w, 2);
            if (n) {
                bool inl = name[6] == 'i';
                declare(s, parent, inl ? "left" : "top", w[0]);
                declare(s, parent, inl ? "right" : "bottom", n == 2 ? w[1] : w[0]);
            }
        }
    } else if (!strcmp(name, "top") || !strcmp(name, "right") || !strcmp(name, "bottom") ||
               !strcmp(name, "left") || !strcmp(name, "inset")) {
        int i = name[0] == 't' ? 0 : name[0] == 'r' ? 1 : name[0] == 'b' ? 2 : 3;
        double lpx, pct;
        bool is_auto;
        if (!strcmp(name, "inset")) {
            int v[4] = { LENGTH_AUTO, LENGTH_AUTO, LENGTH_AUTO, LENGTH_AUTO };
            box_sides(value, s->font_size, v);
            memcpy(s->offset, v, sizeof(v));
        } else if (length_parts(value, s->font_size, &lpx, &pct, &is_auto)) {
            if (is_auto)
                s->offset[i] = LENGTH_AUTO;
            else if (pct != 0 && (i == 1 || i == 3) && pct >= -100 && pct <= 100)
                s->offset[i] = OFFSET_PERCENT((int)pct);
            else
                s->offset[i] = (int)lpx;
        }
    } else if (!strcmp(name, "z-index")) {
        bool minus = value[0] == '-';
        if ((value[minus] >= '0' && value[minus] <= '9')) {
            long z = atol(value);
            s->z_index = (int)(z > 9999 ? 9999 : z < -9999 ? -9999 : z);
            s->z_auto = false;
        } else if (same_word(value, "auto")) {
            s->z_auto = true;
        }
    } else if (!strcmp(name, "overflow") || !strcmp(name, "overflow-x") || !strcmp(name, "overflow-y")) {
        s->holds_floats = !same_word(value, "visible");
        s->clips = !same_word(value, "visible");
    } else if (!strcmp(name, "width") || !strcmp(name, "inline-size")) {
        width_value(value, s->font_size, &s->width, &s->width_add, true);
    } else if (!strcmp(name, "max-width") || !strcmp(name, "max-inline-size")) {
        width_value(value, s->font_size, &s->max_width, &s->max_width_add, true);
    } else if (!strcmp(name, "height") || !strcmp(name, "block-size")) {
        int where, add;                                 /* (27m: a percentage too) */
        if (width_value(value, s->font_size, &where, &add, true)) {
            s->height_pct = where < -1 ? -where - 1 : 0;
            s->height_add = where < -1 ? add : 0;
            s->height = where < -1 ? -1 : where;
        }
    } else if (!strcmp(name, "min-height") || !strcmp(name, "min-block-size") ||
               !strcmp(name, "max-height") || !strcmp(name, "max-block-size")) {      /* (27m) */
        int where, add;
        bool max = name[1] == 'a';
        if (width_value(value, s->font_size, &where, &add, true)) {
            int px = where < -1 ? add : where, pct = where < -1 ? -where - 1 : 0;
            if (max)
                s->max_height = where == -1 ? -1 : px, s->max_height_pct = pct;
            else
                s->min_height = where == -1 ? 0 : px, s->min_height_pct = pct;
        }
    } else if (!strcmp(name, "grid-template-columns") || !strcmp(name, "grid-template-rows") ||
               !strcmp(name, "grid-template-areas") || !strcmp(name, "grid-auto-columns") ||
               !strcmp(name, "grid-auto-rows")) {                                       /* (27n) */
        char **text = !strcmp(name, "grid-template-columns") ? &s->grid_cols
                    : !strcmp(name, "grid-template-rows") ? &s->grid_rows
                    : !strcmp(name, "grid-template-areas") ? &s->grid_areas
                    : !strcmp(name, "grid-auto-columns") ? &s->grid_auto_cols : &s->grid_auto_rows;
        free(*text);
        *text = same_word(value, "none") || same_word(value, "auto") ? 0 : copy_text(value, strlen(value));
    } else if (!strcmp(name, "grid-template")) {
        if (same_word(value, "none")) {
            declare(s, parent, "grid-template-rows", "none");
            declare(s, parent, "grid-template-columns", "none");
            declare(s, parent, "grid-template-areas", "none");
        } else if (!strchr(value, '"') && !strchr(value, '\'')) {  /* rows / columns */
            const char *slash = strchr(value, '/');
            if (slash) {
                char *rows = copy_text(value, (size_t)(slash - value));
                if (rows)
                    declare(s, parent, "grid-template-rows", rows);
                free(rows);
                declare(s, parent, "grid-template-columns", slash + 1);
            }
        } else {                                        /* (areas with sizes: the areas, and the columns) */
            char areas[512] = "";
            size_t o = 0;
            for (const char *p = value; *p && o + 4 < sizeof(areas); p++)
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    areas[o++] = '"';
                    while (*p && *p != q && o + 3 < sizeof(areas))
                        areas[o++] = *p++;
                    areas[o++] = '"', areas[o++] = ' ';
                    if (!*p)
                        break;
                }
            areas[o] = 0;
            declare(s, parent, "grid-template-areas", areas);
            const char *slash = strrchr(value, '/');
            if (slash && slash > strrchr(value, '"'))
                declare(s, parent, "grid-template-columns", slash + 1);
        }
    } else if (!strcmp(name, "grid-auto-flow")) {
        s->grid_by_column = strstr(value, "column") != 0;
    } else if (!strcmp(name, "justify-items") || !strcmp(name, "justify-self")) {
        char w[3][64];
        int n = words(value, w, 3);
        const char *v = n ? w[n - 1] : "";
        enum flex_align a = same_word(v, "start") || same_word(v, "flex-start") || same_word(v, "left") ||
                                    same_word(v, "self-start") ? FLEX_START
                          : same_word(v, "end") || same_word(v, "flex-end") || same_word(v, "right") ||
                                    same_word(v, "self-end") ? FLEX_END
                          : same_word(v, "center") ? FLEX_CENTER : same_word(v, "stretch") ? FLEX_STRETCH
                          : same_word(v, "auto") ? FLEX_AUTO : FLEX_NORMAL;
        if (name[8] == 'i')
            s->justify_items = a == FLEX_AUTO ? FLEX_NORMAL : a;
        else
            s->justify_self = a == FLEX_NORMAL ? FLEX_AUTO : a;
    } else if (!strcmp(name, "grid-column") || !strcmp(name, "grid-row")) {
        bool col = name[5] == 'c';
        grid_lines(value, col ? &s->col_start : &s->row_start, col ? &s->col_end : &s->row_end,
                   col ? &s->col_span : &s->row_span);
    } else if (!strcmp(name, "grid-column-start") || !strcmp(name, "grid-column-end") ||
               !strcmp(name, "grid-row-start") || !strcmp(name, "grid-row-end")) {
        bool col = name[5] == 'c', start = strstr(name, "-start") != 0;
        int *line = col ? (start ? &s->col_start : &s->col_end) : (start ? &s->row_start : &s->row_end);
        int *span = col ? &s->col_span : &s->row_span;
        char w[2][64];
        int n = words(value, w, 2);
        *line = 0;
        if (n == 2 && same_word(w[0], "span"))
            *span = atoi(w[1]) > 0 ? (atoi(w[1]) > 1000 ? 1000 : atoi(w[1])) : 1;
        else if (n == 1 && !same_word(w[0], "auto"))
            *line = atoi(w[0]) > 1000 ? 1000 : atoi(w[0]) < -1000 ? -1000 : atoi(w[0]);
    } else if (!strcmp(name, "grid-area")) {
        free(s->grid_area);
        s->grid_area = 0;
        if (strchr(value, '/') || (value[0] >= '0' && value[0] <= '9') || value[0] == '-' ||
            starts_word(value, "span")) {               /* row-start / column-start / row-end / column-end */
            char part[4][64] = { "", "", "", "" };
            int k = 0;
            for (const char *p = value; *p && k < 4; k++) {
                const char *slash = strchr(p, '/');
                size_t len = slash ? (size_t)(slash - p) : strlen(p);
                snprintf(part[k], sizeof(part[k]), "%.*s", (int)(len < 63 ? len : 63), p);
                p = slash ? slash + 1 : p + len;
            }
            char both[130];
            snprintf(both, sizeof(both), "%s / %s", part[0], part[2]);
            grid_lines(both, &s->row_start, &s->row_end, &s->row_span);
            snprintf(both, sizeof(both), "%s / %s", part[1], part[3]);
            grid_lines(both, &s->col_start, &s->col_end, &s->col_span);
        } else if (!same_word(value, "auto")) {
            char w[1][64];
            if (words(value, w, 1))
                s->grid_area = copy_text(w[0], strlen(w[0]));
        }
    } else if (!strcmp(name, "transform")) {                    /* (27m) the translations */
        s->translated = false;
        s->tx = s->ty = s->tx_pct = s->ty_pct = 0;
        for (const char *p = value; (p = strstr(p, "translate")) != 0;) {
            const char *open = strchr(p, '(');
            if (!open)
                break;
            char kind = (char)lower((unsigned char)p[9]);   /* 'x', 'y', '3', or '(' */
            size_t end = paren_end(open, strlen(open), 0);
            if (!end)
                break;
            char args[128];
            snprintf(args, sizeof(args), "%.*s", (int)(end - 2 < 127 ? end - 2 : 127), open + 1);
            for (char *q = args; *q; q++)
                if (*q == ',')
                    *q = ' ';
            char w[3][64];
            int n = words(args, w, 3);
            for (int i = 0; i < n && i < 2; i++) {
                double px, pct;
                bool is_auto;
                if (!length_parts(w[i], s->font_size, &px, &pct, &is_auto) || is_auto)
                    continue;
                bool along_y = kind == 'y' || (i == 1 && kind != 'x');
                if (kind == 'x' && i > 0)
                    break;
                if (along_y)
                    s->ty += (int)px, s->ty_pct += (int)pct;
                else
                    s->tx += (int)px, s->tx_pct += (int)pct;
                s->translated = true;
            }
            p = open + end;
        }
    } else if (!strcmp(name, "flex-direction")) {             /* (27g) */
        s->direction = same_word(value, "row-reverse") ? FLEX_ROW_REVERSE
                     : same_word(value, "column") ? FLEX_COLUMN
                     : same_word(value, "column-reverse") ? FLEX_COLUMN_REVERSE : FLEX_ROW;
    } else if (!strcmp(name, "flex-wrap")) {
        s->wrap = same_word(value, "wrap") ? FLEX_WRAP : same_word(value, "wrap-reverse") ? FLEX_WRAP_REVERSE
                                                                                        : FLEX_NOWRAP;
    } else if (!strcmp(name, "flex-flow")) {
        char w[2][64];
        int n = words(value, w, 2);
        for (int i = 0; i < n; i++)
            declare(s, parent, strstr(w[i], "wrap") ? "flex-wrap" : "flex-direction", w[i]);
    } else if (!strcmp(name, "justify-content") || !strcmp(name, "align-items") ||
               !strcmp(name, "align-self") || !strcmp(name, "align-content") ||
               !strcmp(name, "place-content") || !strcmp(name, "place-items")) {
        char w[3][64];
        int n = words(value, w, 3);
        const char *v = n ? w[n - 1] : "";              /* ("safe center": the last word) */
        if (n >= 2 && (same_word(w[0], "first") || same_word(w[0], "last")))
            v = w[0][0] == 'l' ? "flex-end" : "flex-start";     /* (first/last baseline) */
        enum flex_align a = same_word(v, "flex-start") || same_word(v, "start") || same_word(v, "left") ||
                                    same_word(v, "self-start") ? FLEX_START
                          : same_word(v, "flex-end") || same_word(v, "end") || same_word(v, "right") ||
                                    same_word(v, "self-end") ? FLEX_END
                          : same_word(v, "center") ? FLEX_CENTER
                          : same_word(v, "stretch") ? FLEX_STRETCH
                          : same_word(v, "baseline") ? FLEX_BASELINE
                          : same_word(v, "space-between") ? FLEX_BETWEEN
                          : same_word(v, "space-around") ? FLEX_AROUND
                          : same_word(v, "space-evenly") ? FLEX_EVENLY
                          : same_word(v, "auto") ? FLEX_AUTO : FLEX_NORMAL;
        if (!strcmp(name, "place-content")) {
            const char *first = n ? w[0] : "";
            declare(s, parent, "align-content", first);
            declare(s, parent, "justify-content", n > 1 ? w[1] : first);
        } else if (!strcmp(name, "place-items")) {
            s->align_items = a == FLEX_AUTO ? FLEX_NORMAL : a;
            if (n > 1)
                declare(s, parent, "align-items", w[0]);
        } else if (name[0] == 'j')
            s->justify = a;
        else if (!strcmp(name, "align-items"))
            s->align_items = a == FLEX_AUTO ? FLEX_NORMAL : a;
        else if (!strcmp(name, "align-self"))
            s->align_self = a == FLEX_NORMAL ? FLEX_AUTO : a;
        else
            s->align_content = a;
    } else if (!strcmp(name, "flex-grow") || !strcmp(name, "flex-shrink")) {
        char *end;
        double v = strtod(value, &end);
        if (end != value && v >= 0 && v < 1e6) {
            if (name[5] == 'g')
                s->grow = (float)v;
            else
                s->shrink = (float)v;
        }
    } else if (!strcmp(name, "flex-basis")) {
        if (same_word(value, "content"))
            s->basis = -1, s->basis_add = 0;
        else
            width_value(value, s->font_size, &s->basis, &s->basis_add, true);
    } else if (!strcmp(name, "flex")) {
        /* none: 0 0 auto; auto: 1 1 auto; initial: 0 1 auto; N: N 1 0;
         * a length: 1 1 it; N M; N basis; N M basis. */
        char w[3][64];
        int n = words(value, w, 3);
        if (n == 1 && same_word(w[0], "none"))
            s->grow = 0, s->shrink = 0, s->basis = -1, s->basis_add = 0;
        else if (n == 1 && same_word(w[0], "auto"))
            s->grow = 1, s->shrink = 1, s->basis = -1, s->basis_add = 0;
        else if (n == 1 && same_word(w[0], "initial"))
            s->grow = 0, s->shrink = 1, s->basis = -1, s->basis_add = 0;
        else if (n >= 1) {
            float grow = 1, shrink = 1;
            int basis = 0, add = 0, numbers = 0;
            bool have_basis = false;
            for (int i = 0; i < n; i++) {
                char *end;
                double v = strtod(w[i], &end);
                if (end != w[i] && !*end && numbers < 2 && v == v && v >= 0 && v < 1e6) {     /* (a plain number) */
                    if (numbers++ == 0)
                        grow = (float)v;
                    else
                        shrink = (float)v;
                } else if (width_value(w[i], s->font_size, &basis, &add, true)) {
                    have_basis = true;
                }
            }
            if (!have_basis && numbers == 0)
                return;
            s->grow = grow, s->shrink = shrink;
            if (have_basis)
                s->basis = basis, s->basis_add = add;
            else
                s->basis = 0, s->basis_add = 0;                 /* (flex: N means a basis of 0) */
        }
    } else if (!strcmp(name, "order")) {
        bool minus = value[0] == '-';
        if (value[minus] >= '0' && value[minus] <= '9') {
            long o = atol(value);
            s->order = (int)(o > 9999 ? 9999 : o < -9999 ? -9999 : o);
        }
    } else if (!strcmp(name, "gap") || !strcmp(name, "grid-gap") || !strcmp(name, "row-gap") ||
               !strcmp(name, "column-gap") || !strcmp(name, "grid-row-gap") ||
               !strcmp(name, "grid-column-gap")) {
        char w[2][64];
        int n = words(value, w, 2), a = 0, b;
        if (n >= 1 && (same_word(w[0], "normal") || length(w[0], s->font_size, -1, &a))) {
            if (same_word(w[0], "normal"))
                a = 0;
            b = a;
            if (n == 2 && !length(w[1], s->font_size, -1, &b))
                b = a;
            if (a < 0 || b < 0)
                return;
            if (strstr(name, "row"))
                s->row_gap = a;
            else if (strstr(name, "column"))
                s->column_gap = a;
            else
                s->row_gap = a, s->column_gap = b;
        }
    } else if (!strcmp(name, "min-width") || !strcmp(name, "min-inline-size")) {
        width_value(value, s->font_size, &s->min_width, &s->min_width_add, true);
    } else if (!strcmp(name, "border-spacing")) {               /* (27i) */
        char w[2][64];
        int n = words(value, w, 2), a, b;
        if (n >= 1 && length(w[0], s->font_size, -1, &a) && a >= 0) {
            b = n == 2 && length(w[1], s->font_size, -1, &b) && b >= 0 ? b : a;
            s->spacing_x = a > 100 ? 100 : a, s->spacing_y = b > 100 ? 100 : b;
        }
    } else if (!strcmp(name, "fill") || !strcmp(name, "stroke")) {        /* (27j) */
        bool f = name[0] == 'f';
        uint32_t *color = f ? &s->fill : &s->stroke;
        int *a = f ? &s->fill_alpha : &s->stroke_alpha;
        bool *cur = f ? &s->fill_current : &s->stroke_current;
        if (same_word(value, "none") || (starts_word(value, "url(") && strstr(value, ") none")))
            *a = 0, *cur = false;
        else if (same_word(value, "currentcolor"))
            *color = s->color, *a = 255, *cur = true;
        else if (css_color_alpha(value, &c, &alpha))
            *color = c, *a = alpha, *cur = false;
        else if (starts_word(value, "url(")) {          /* (a gradient or pattern: its fallback) */
            const char *after = strchr(value, ')');
            if (after && css_color_alpha(after + 1, &c, &alpha))
                *color = c, *a = alpha, *cur = false;
            else
                *color = 0x808080, *a = 255, *cur = false;
        }
    } else if (!strcmp(name, "stroke-width")) {
        double px, pct;
        bool is_auto;
        if (length_parts(value, s->font_size, &px, &pct, &is_auto) && !is_auto && px >= 0)
            s->stroke_width = (float)(px > 1000 ? 1000 : px);
    } else if (!strcmp(name, "fill-opacity") || !strcmp(name, "stroke-opacity")) {
        char *end;
        double v = strtod(value, &end);
        if (end != value) {
            if (*end == '%')
                v /= 100;
            int *a = name[0] == 'f' ? &s->fill_alpha : &s->stroke_alpha;
            *a = (int)(*a * (!(v > 0) ? 0 : v > 1 ? 1 : v) + 0.5);      /* (NaN: 0) */
        }
    } else if (!strcmp(name, "fill-rule")) {
        s->evenodd = same_word(value, "evenodd");
    } else if (!strcmp(name, "border-collapse")) {
        s->collapse = same_word(value, "collapse");
    } else if (!strcmp(name, "box-sizing")) {
        s->border_box = same_word(value, "border-box");
    } else if (!strcmp(name, "content")) {
        free(s->content);
        s->content = copy_text(value, strlen(value));
    }
}

/* --- custom properties and var() (27f) --- */

struct css_var {
    char *name, *value;
    struct css_var *next;
};

static const char *find_var(const struct node *e, const struct css_var *own, const char *name, size_t n)
{
    for (const struct css_var *v = own; v; v = v->next)
        if (strlen(v->name) == n && !memcmp(v->name, name, n))
            return v->value;
    for (const struct node *a = e ? e->parent : 0; a; a = a->parent)
        if (a->style)
            for (const struct css_var *v = a->style->vars; v; v = v->next)
                if (strlen(v->name) == n && !memcmp(v->name, name, n))
                    return v->value;
    return 0;
}

/* value with its var(--name[, fallback]) replaced: malloc'ed, or 0 if one
 * has no value and no fallback (the declaration is then not used). */
static char *substitute(const struct node *e, const struct css_var *own, const char *value, int depth)
{
    if (!strstr(value, "var("))
        return copy_text(value, strlen(value));
    if (depth > 16)
        return 0;                                       /* (a cycle, or too deep) */
    char *out = copy_text("", 0);
    for (const char *p = value; *p && out;) {
        const char *v = strstr(p, "var(");
        if (!v) {
            out = join_text(out, p);
            break;
        }
        char *head = copy_text(p, (size_t)(v - p));
        out = head ? join_text(out, head) : out;
        free(head);
        size_t end = paren_end(v + 3, strlen(v + 3), 0);        /* (just after its ')') */
        if (!end) {
            free(out);
            return 0;                                   /* (not closed: not used) */
        }
        const char *in = v + 4, *in_end = v + 3 + end - 1;
        while (in < in_end && is_space(*in))
            in++;
        const char *ne = in;
        while (ne < in_end && *ne != ',' && !is_space(*ne))
            ne++;
        const char *comma = memchr(ne, ',', (size_t)(in_end - ne));
        const char *found = find_var(e, own, in, (size_t)(ne - in));
        char *piece = 0;
        if (found)
            piece = substitute(e, own, found, depth + 1);
        else if (comma) {
            char *fallback = copy_text(comma + 1, (size_t)(in_end - comma - 1));
            piece = fallback ? substitute(e, own, fallback, depth + 1) : 0;
            free(fallback);
        }
        if (!piece) {
            free(out);
            return 0;
        }
        out = join_text(out, piece);
        free(piece);
        p = v + 3 + end;
    }
    return out;
}

/* --- the cascade --- */

/* Which of the page's media texts hold for the window (css_apply's). */
static const bool *media_now;

struct applied {
    const char *name, *value;
};

/* The declarations of the rules that match e (for pseudo ::before or
 * ::after, or none), in cascade order: normal ones (the browser's, the
 * page's, style="") then the !important ones (the page's, style=""). */
/* (27q) The rule lists e's rules may be in: the index's buckets of its id,
 * classes and tag, and the rules filed under none of those; -1 when that
 * cannot be told (no memory for the index, more classes than LISTS): then
 * every rule. */
#define LISTS 34

static int candidate_lists(const struct sheet *sh, const struct node *e, const int **lists, int *lens)
{
    struct sheet *s = (struct sheet *)sh;       /* (the index is a cache: made here when stale) */
    if (s->indexed != s->count && !index_sheet(s))
        return -1;
    int n = 0;
    char key[36];
    lists[n] = s->anyrules, lens[n++] = s->any_count;
    const char *id = node_attr(e, "id");
    struct bucket *b;
    if (id && strlen(id) < sizeof(key) - 1) {
        snprintf(key, sizeof(key), "#%s", id);
        if ((b = bucket_at(s, key, false)))
            lists[n] = b->rules, lens[n++] = b->count;
    }
    if ((b = bucket_at(s, e->tag, false)))
        lists[n] = b->rules, lens[n++] = b->count;
    const char *list = node_attr(e, "class");
    for (const char *c = list; c && *c;) {
        while (is_space(*c))
            c++;
        if (*c && n == LISTS)
            return -1;
        const char *start = c;
        while (*c && !is_space(*c))
            c++;
        size_t len = (size_t)(c - start);
        if (!len || len >= sizeof(key) - 1)
            continue;
        key[0] = '.';
        memcpy(key + 1, start, len);
        key[len + 1] = 0;
        bool twice = false;                     /* (class="a a": one list) */
        for (const char *p = list; p < start; p++)
            if ((p == list || is_space(p[-1])) && !strncmp(p, start, len) && (is_space(p[len]) || !p[len]))
                twice = true;
        if (!twice && (b = bucket_at(s, key, false)))
            lists[n] = b->rules, lens[n++] = b->count;
    }
    return n;
}

static int cascade(struct node *e, const struct sheet *ua, const struct sheet *author, int pseudo,
                   struct decl *hint_decls, int hint_n, struct decl *inline_decls, int inline_n,
                   struct applied *out, int room)
{
    int n = 0;
    const struct sheet *sheets[2] = { ua, author };
    const struct rule *found[2][512];
    int counts[2] = { 0, 0 };
    for (int o = 0; o < 2; o++) {
        const struct sheet *sh = sheets[o];
        if (!sh)
            continue;
        const int *lists[LISTS];
        int lens[LISTS], at[LISTS] = { 0 }, nlists = candidate_lists(sh, e, lists, lens);
        for (int every = 0; counts[o] < 512;) {     /* (the lists merged: the sheet's order) */
            int i;
            if (nlists < 0) {
                if (every == sh->count)
                    break;
                i = every++;
            } else {
                int best = -1;
                for (int l = 0; l < nlists; l++)
                    if (at[l] < lens[l] && (best < 0 || lists[l][at[l]] < lists[best][at[best]]))
                        best = l;
                if (best < 0)
                    break;
                i = lists[best][at[best]++];
            }
            if (sh->rules[i].sel.pseudo_element == pseudo &&
                (sh->rules[i].media < 0 || (o == 1 && media_now && media_now[sh->rules[i].media])) &&
                matches(&sh->rules[i].sel, e))
                found[o][counts[o]++] = &sh->rules[i];
        }
        for (int i = 1; i < counts[o]; i++)             /* (insertion sort: stable) */
            for (int k = i; k > 0 && found[o][k - 1]->sel.specificity > found[o][k]->sel.specificity; k--) {
                const struct rule *t = found[o][k];
                found[o][k] = found[o][k - 1], found[o][k - 1] = t;
            }
    }
    for (int pass = 0; pass < 2; pass++) {              /* normal, then !important */
        for (int o = 0; o < 2; o++) {
            for (int i = 0; i < counts[o]; i++)
                for (int k = 0; k < found[o][i]->decl_count && n < room; k++)
                    if (found[o][i]->decls[k].important == (pass == 1))
                        out[n++] = (struct applied){ found[o][i]->decls[k].name,
                                                     found[o][i]->decls[k].value };
            for (int k = 0; o == 0 && pass == 0 && k < hint_n && n < room; k++)     /* (27i) */
                out[n++] = (struct applied){ hint_decls[k].name, hint_decls[k].value };
        }
        for (int k = 0; k < inline_n && n < room; k++)
            if (inline_decls[k].important == (pass == 1))
                out[n++] = (struct applied){ inline_decls[k].name, inline_decls[k].value };
    }
    return n;
}

/* (27i) A number in an attribute: its digits (and a %), as CSS, into out;
 * false if there is none. */
static bool attr_length(const char *v, char *out, size_t size)
{
    if (!v)
        return false;
    while (is_space(*v))
        v++;
    int n = 0, digits = 0;
    while (*v >= '0' && *v <= '9' && digits < 6)       /* (at most six digits) */
        n = n * 10 + (*v++ - '0'), digits++;
    if (!digits)
        return false;
    snprintf(out, size, "%d%s", n, *v == '%' ? "%" : "px");
    return true;
}

static const struct node *enclosing(const struct node *e, const char *tag)
{
    for (const struct node *a = e->parent; a; a = a->parent)
        if (a->kind == NODE_ELEMENT && !strcmp(a->tag, tag))
            return a;
    return 0;
}

/* (27i) What old HTML attributes say about e, as CSS (browsers' "presentational
 * hints": after the browser's own rules, before the page's): a table's
 * border, cellspacing, width and align=center; its cells' borders and
 * cellpadding; width, height and valign on cells. */
static bool hints(const struct node *e, char *out, size_t size)
{
    out[0] = 0;
    char len[16];
    size_t o = 0;
    #define HINT(...) (o += (size_t)snprintf(out + o, o < size ? size - o : 0, __VA_ARGS__), o = o < size ? o : size - 1)
    bool table = !strcmp(e->tag, "table"), cell = !strcmp(e->tag, "td") || !strcmp(e->tag, "th");
    /* (27j) SVG's presentation attributes are CSS too */
    static const char *const SVG_PAINT[] = { "fill", "stroke", "stroke-width", "fill-opacity",
                                             "stroke-opacity", "fill-rule", "opacity", "display",
                                             "visibility", "color", "font-size", 0 };
    bool svg = false;
    for (const struct node *a = e; a && !svg; a = a->parent)
        svg = !strcmp(a->tag, "svg");
    if (svg) {
        for (int i = 0; SVG_PAINT[i]; i++) {
            const char *v = node_attr(e, SVG_PAINT[i]);
            if (!v || !*v || strchr(v, ';') || strchr(v, '{'))
                continue;
            size_t vn = strlen(v);
            bool number = (v[vn - 1] >= '0' && v[vn - 1] <= '9') || v[vn - 1] == '.';
            bool length_kind = !strcmp(SVG_PAINT[i], "stroke-width") || !strcmp(SVG_PAINT[i], "font-size");
            HINT("%s:%s%s;", SVG_PAINT[i], v, number && length_kind ? "px" : "");     /* (SVG's user units) */
        }
        if (strcmp(e->tag, "svg"))
            return out[0] != 0;                 /* (an <svg>'s own width and height are its size) */
    }
    if (!table && !cell && strcmp(e->tag, "col"))
        return out[0] != 0;
    if (table) {
        const char *b = node_attr(e, "border");
        if (b) {
            int n = atoi(b);
            if (!*b || n > 0)
                HINT("border:%dpx outset #808080;", *b ? (n > 20 ? 20 : n) : 1);
        }
        if (attr_length(node_attr(e, "cellspacing"), len, sizeof(len)) && !strchr(len, '%'))
            HINT("border-spacing:%s;", len);
        const char *align = node_attr(e, "align");
        if (align && same_word(align, "center"))
            HINT("margin-left:auto;margin-right:auto;");
        else if (align && (same_word(align, "right") || same_word(align, "left")))
            HINT("float:%s;", align);
    }
    if (cell) {
        const struct node *t = enclosing(e, "table");
        const char *b = t ? node_attr(t, "border") : 0;
        if (b && (!*b || atoi(b) > 0))
            HINT("border:1px inset #808080;");
        if (t && attr_length(node_attr(t, "cellpadding"), len, sizeof(len)) && !strchr(len, '%'))
            HINT("padding:%s;", len);
        const char *va = node_attr(e, "valign");
        if (!va && e->parent && !strcmp(e->parent->tag, "tr"))
            va = node_attr(e->parent, "valign");
        if (va && (same_word(va, "top") || same_word(va, "middle") || same_word(va, "bottom")))
            HINT("vertical-align:%s;", va);
        if (node_attr(e, "nowrap"))
            HINT("white-space:nowrap;");
    }
    if (table || cell || !strcmp(e->tag, "col")) {
        if (attr_length(node_attr(e, "width"), len, sizeof(len)))
            HINT("width:%s;", len);
        if (attr_length(node_attr(e, "height"), len, sizeof(len)) && !strchr(len, '%'))
            HINT("height:%s;", len);
    }
    #undef HINT
    return out[0] != 0;
}

static void style_defaults(struct style *s, const struct style *parent)
{
    if (parent) {
        s->color = parent->color;
        s->font_size = parent->font_size;
        s->bold = parent->bold, s->italic = parent->italic;
        s->underline = parent->underline, s->strike = parent->strike;
        s->pre = parent->pre;
        s->align = parent->align;
        s->list = parent->list;
        s->color_alpha = parent->color_alpha;       /* (27h) inherited */
        s->hidden = parent->hidden, s->transform = parent->transform;
        s->line_factor = parent->line_factor, s->line_px = parent->line_px;
        s->spacing_x = parent->spacing_x, s->spacing_y = parent->spacing_y;     /* (27i) */
        s->collapse = parent->collapse;
        s->fill = parent->fill, s->fill_alpha = parent->fill_alpha;     /* (27j) */
        s->stroke = parent->stroke, s->stroke_alpha = parent->stroke_alpha;
        s->stroke_width = parent->stroke_width, s->evenodd = parent->evenodd;
        s->fill_current = parent->fill_current, s->stroke_current = parent->stroke_current;
        s->family = parent->family;                 /* (27k) */
        s->nowrap = parent->nowrap;                 /* (27n) */
    } else {
        s->color = 0x000000, s->font_size = 16, s->color_alpha = 255;
        s->fill_alpha = 255, s->stroke_width = 1;       /* (SVG: black fill, no stroke) */
    }
    s->display = DISPLAY_INLINE;
    s->background = COLOR_NONE;
    for (int k = 0; k < 4; k++)                     /* (27h) medium, no style, the text's colour */
        s->border_set[k] = 3, s->border_colors[k] = COLOR_NONE;
    s->opacity = s->background_alpha = 255;
    s->bg_x_pct = s->bg_y_pct = true;
    s->width = s->height = s->max_width = -1;
    s->min_width = s->basis = -1;                   /* (27g) */
    s->max_height = -1;                             /* (27m) */
    s->shrink = 1;
    s->align_self = FLEX_AUTO;
    for (int i = 0; i < 4; i++)
        s->offset[i] = LENGTH_AUTO;
    s->z_auto = true;
}

#define APPLIED_MAX 1024

/* The computed style of e (pseudo: of its ::before or ::after; parent is
 * then e's own style): inherited from its parent, then its rules. */
static struct style *style_of(struct node *e, const struct sheet *ua, const struct sheet *author,
                              int pseudo, const struct style *parent)
{
    struct style *s = calloc(1, sizeof(*s));
    struct applied *list = malloc(sizeof(*list) * APPLIED_MAX);
    if (!s || !list) {
        free(s);
        free(list);
        return 0;
    }
    style_defaults(s, parent);
    struct decl *inline_decls = 0, *hint_decls = 0;
    int inline_n = 0, hint_n = 0;
    const char *inline_style = pseudo ? 0 : node_attr(e, "style");
    if (inline_style)
        inline_n = parse_decls(inline_style, strlen(inline_style), &inline_decls);
    char hint_text[512];
    if (!pseudo && hints(e, hint_text, sizeof(hint_text)))
        hint_n = parse_decls(hint_text, strlen(hint_text), &hint_decls);
    int n = cascade(e, ua, author, pseudo, hint_decls, hint_n, inline_decls, inline_n, list, APPLIED_MAX);
    /* Custom properties first (later ones win), then var() in the others. */
    for (int i = 0; i < n; i++)
        if (list[i].name[0] == '-' && list[i].name[1] == '-') {
            struct css_var *v = s->vars;
            while (v && strcmp(v->name, list[i].name))
                v = v->next;
            if (!v && (v = calloc(1, sizeof(*v)))) {
                v->name = copy_text(list[i].name, strlen(list[i].name));
                v->next = s->vars;
                s->vars = v;
            }
            if (v) {
                free(v->value);
                v->value = copy_text(list[i].value, strlen(list[i].value));
            }
        }
    for (int pass = 0; pass < 2; pass++)                /* (font sizes first: em lengths use them) */
        for (int i = 0; i < n; i++) {
            const char *name = list[i].name;
            if (name[0] == '-' && name[1] == '-')
                continue;
            bool font = !strcmp(name, "font-size") || !strcmp(name, "font");
            if (font != (pass == 0))
                continue;
            char *value = substitute(pseudo ? 0 : e, s->vars, list[i].value, 0);
            if (value)
                declare(s, parent, name, value);
            free(value);
        }
    for (int i = 0; i < inline_n; i++)
        free(inline_decls[i].value);
    free(inline_decls);
    for (int i = 0; i < hint_n; i++)
        free(hint_decls[i].value);
    free(hint_decls);
    free(list);
    for (int k = 0; k < 4; k++)                     /* (27h) a side is drawn with a style */
        s->border[k] = s->border_style[k] ? s->border_set[k] : 0;
    if (pseudo)
        return s;
    /* Old presentational attributes, still common. */
    const char *a;
    uint32_t c;
    if ((a = node_attr(e, "bgcolor")) && css_color(a, &c))
        s->background = c;
    if (!strcmp(e->tag, "font") && (a = node_attr(e, "color")) && css_color(a, &c))
        s->color = c;
    if ((a = node_attr(e, "align")) && strcmp(e->tag, "table")) {     /* (a table's: its place, 27i) */
        if (same_word(a, "center"))
            s->align = ALIGN_CENTER;
        else if (same_word(a, "right"))
            s->align = ALIGN_RIGHT;
    }
    if (!strcmp(e->tag, "body") && (a = node_attr(e, "text")) && css_color(a, &c))
        s->color = c;
    if (css_scripting && !strcmp(e->tag, "noscript"))
        s->display = DISPLAY_NONE;              /* (27q: the page's scripts run: its <noscript> is not shown) */
    return s;
}

void css_style_free(struct style *s)
{
    if (!s)
        return;
    for (struct css_var *v = s->vars; v;) {
        struct css_var *next = v->next;
        free(v->name);
        free(v->value);
        free(v);
        v = next;
    }
    free(s->content);
    free(s->background_image);
    free(s->grid_cols), free(s->grid_rows), free(s->grid_areas);           /* (27n) */
    free(s->grid_auto_cols), free(s->grid_auto_rows), free(s->grid_area);
    free(s);
}

/* content's text: strings ("..." with escapes), attr(NAME), quotes;
 * 0 for none and normal. malloc'ed. */
static char *content_text(const char *value, const struct node *e)
{
    if (!value || same_word(value, "none") || same_word(value, "normal"))
        return 0;
    char *out = copy_text("", 0);
    for (const char *p = value; *p && out;) {
        while (is_space(*p))
            p++;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            char piece[512];
            size_t o = 0;
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) {
                    p++;
                    int v = 0, k = 0;
                    while (k < 6 && hex_digit((unsigned char)*p) >= 0)
                        v = v * 16 + hex_digit((unsigned char)*p++), k++;
                    if (k) {                            /* \2014 and the like: UTF-8 */
                        if (*p == ' ')
                            p++;
                        if (v < 0x80 && o + 1 < sizeof(piece))
                            piece[o++] = (char)v;
                        else if (v < 0x800 && o + 2 < sizeof(piece))
                            piece[o++] = (char)(0xC0 | v >> 6), piece[o++] = (char)(0x80 | (v & 63));
                        else if (v < 0x10000 && o + 3 < sizeof(piece))
                            piece[o++] = (char)(0xE0 | v >> 12), piece[o++] = (char)(0x80 | (v >> 6 & 63)),
                            piece[o++] = (char)(0x80 | (v & 63));
                        continue;
                    }
                }
                if (o + 1 < sizeof(piece))
                    piece[o++] = *p;
                p++;
            }
            piece[o] = 0;
            if (*p)
                p++;
            out = join_text(out, piece);
        } else if (starts_word(p, "attr(")) {
            const char *s = p + 5, *end = strchr(s, ')');
            if (!end)
                break;
            char name[32];
            size_t n = 0;
            while (s < end && n + 1 < sizeof(name))
                if (!is_space(*s))
                    name[n++] = (char)lower((unsigned char)*s++);
                else
                    s++;
            name[n] = 0;
            const char *v = node_attr(e, name);
            out = join_text(out, v ? v : "");
            p = end + 1;
        } else if (starts_word(p, "open-quote") || starts_word(p, "close-quote")) {
            out = join_text(out, lower(p[0]) == 'o' ? "\xE2\x80\x9C" : "\xE2\x80\x9D");
            p += lower(p[0]) == 'o' ? 10 : 11;
        } else {
            while (*p && !is_space(*p))
                p++;                                    /* (counter(), url(): not drawn) */
        }
    }
    return out;
}

/* ::before and ::after of e: generated children (tag "::before",
 * "::after", one text child), made again at each restyle. */
static void generated(struct node *e, const struct sheet *ua, const struct sheet *author)
{
    for (int pseudo = PSEUDO_BEFORE; pseudo <= PSEUDO_AFTER; pseudo++) {
        struct style *s = style_of(e, ua, author, pseudo, e->style);
        char *text = s ? content_text(s->content, e) : 0;
        if (!text || s->display == DISPLAY_NONE) {
            free(text);
            css_style_free(s);
            continue;
        }
        struct node *g = calloc(1, sizeof(*g)), *t = calloc(1, sizeof(*t));
        if (!g || !t) {
            free(g), free(t), free(text);
            css_style_free(s);
            continue;
        }
        g->kind = NODE_ELEMENT;
        strcpy(g->tag, pseudo == PSEUDO_BEFORE ? "::before" : "::after");
        g->style = s;
        g->parent = e;
        t->kind = NODE_TEXT;
        t->text = text;
        t->parent = g;
        g->first = g->last = t;
        if (pseudo == PSEUDO_BEFORE) {                  /* (its first child) */
            g->next = e->first;
            if (e->first)
                e->first->prev = g;
            else
                e->last = g;
            e->first = g;
        } else {                                        /* (its last child) */
            g->prev = e->last;
            if (e->last)
                e->last->next = g;
            else
                e->first = g;
            e->last = g;
        }
    }
}

/* Take away the ::before and ::after children an earlier restyle made. */
static void drop_generated(struct node *root)
{
    for (struct node *n = root; n; n = node_next(n, root)) {
        for (struct node *c = n->first; c;) {
            struct node *next = c->next;
            if (c->kind == NODE_ELEMENT && c->tag[0] == ':') {
                if (c->prev)
                    c->prev->next = c->next;
                else
                    n->first = c->next;
                if (c->next)
                    c->next->prev = c->prev;
                else
                    n->last = c->prev;
                c->next = c->prev = 0;
                c->parent = 0;
                html_free(c);
            }
            c = next;
        }
    }
}

void css_apply(struct node *root, const struct sheet *author, int width, int height)
{
    static struct sheet *ua;
    if (!ua)
        ua = css_parse(UA_SHEET, 0);
    if (width > 0)
        view_w = width;
    if (height > 0)
        view_h = height;
    drop_generated(root);
    bool *media = author && author->media_count ? calloc((size_t)author->media_count, sizeof(bool)) : 0;
    for (int i = 0; media && i < author->media_count; i++)
        media[i] = media_ok(author->medias[i]);
    media_now = media;
    bool pseudo = author && author->pseudo_rules;
    for (struct node *n = root; n; n = node_next(n, root)) {
        if (n->kind != NODE_ELEMENT || n->tag[0] == ':')
            continue;                                   /* (generated ones have theirs) */
        css_style_free(n->style);
        n->style = style_of(n, ua, author, PSEUDO_NONE, n->parent ? n->parent->style : 0);
        if (n->svg_drawn && n->picture) {       /* (27j: drawn in the old style: again) */
            image_free(n->picture);
            free(n->picture);
            n->picture = 0;
            n->svg_drawn = false;
        }
        if (pseudo && n->style && n->style->display != DISPLAY_NONE)
            generated(n, ua, author);
    }
    media_now = 0;
    free(media);
}
