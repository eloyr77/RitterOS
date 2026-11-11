/*
 * The parser (27o): tokens to a syntax tree, by recursive descent with the
 * binary operators by precedence. Where the grammar only decides later
 * (arrow parameters, destructuring assignment), an expression is parsed
 * first and turned into a pattern when `=>` or `=` comes (CoverParenthesized-
 * ExpressionAndArrowParameterList, AssignmentPattern). Scopes are not made
 * here: compile.c walks the tree for them.
 */
#include "jsparse.h"

/* --- the arena ------------------------------------------------------------ */

struct chunk {
    struct chunk *next;
    size_t used, size;
    char data[];
};
static struct chunk *arena;

void *arena_new(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (!arena || arena->used + n > arena->size) {
        size_t size = n > 65536 ? n : 65536;
        struct chunk *c = malloc(sizeof *c + size);
        if (!c)
            abort();
        c->next = arena;
        c->used = 0;
        c->size = size;
        arena = c;
    }
    void *p = arena->data + arena->used;
    arena->used += n;
    memset(p, 0, n);
    return p;
}

void arena_free_all(void)
{
    while (arena) {
        struct chunk *c = arena->next;
        free(arena);
        arena = c;
    }
}

/* --- helpers -------------------------------------------------------------- */

struct parser {
    struct lexer *L;
    struct js *J;
    bool in_function, in_async, in_generator, in_class_field, in_method, in_derived_ctor;
    bool in_static_block;
    int depth;
};

#define TOK (&P->L->tok)

static _Noreturn void error_at(struct parser *P, const struct token *t, const char *msg)
{
    lex_error(P->L, t, "%s", msg);
}

static _Noreturn void unexpected(struct parser *P)
{
    const struct token *t = TOK;
    char buf[128];
    switch (t->type) {
    case T_EOF:
        lex_error(P->L, t, "Unexpected end of script");
    case T_NUM:
        lex_error(P->L, t, "Unexpected number");
    case T_STR:
        lex_error(P->L, t, "Unexpected string literal");
    case T_TEMPLATE:
        lex_error(P->L, t, "Unexpected template string");
    case T_KEYWORD:
        lex_error(P->L, t, "Unexpected keyword '%s'", keyword_text(t->op));
    case T_PUNCT:
        lex_error(P->L, t, "Unexpected token '%s'", punct_text(t->op));
    case T_IDENT: {
        char *s = str_to_utf8(P->J, t->atom, 0);
        snprintf(buf, sizeof buf, "Unexpected identifier '%s'", s);
        free(s);
        error_at(P, t, buf);
    }
    default:
        lex_error(P->L, t, "Unexpected token");
    }
}

static void next(struct parser *P) { lex_next(P->L); }
static bool is_p(struct parser *P, int op) { return TOK->type == T_PUNCT && TOK->op == op; }
static bool is_kw(struct parser *P, int kw) { return TOK->type == T_KEYWORD && TOK->op == kw; }
static bool is_name(struct parser *P, int a)
{
    return TOK->type == T_IDENT && !TOK->escaped && TOK->atom == P->J->atom[a];
}

static bool accept(struct parser *P, int op)
{
    if (is_p(P, op)) {
        next(P);
        return true;
    }
    return false;
}

static void expect(struct parser *P, int op)
{
    if (!accept(P, op))
        unexpected(P);
}

static void semicolon(struct parser *P)
{
    if (accept(P, P_SEMI))
        return;
    if (is_p(P, P_RBRACE) || TOK->type == T_EOF || TOK->nl_before)
        return;
    unexpected(P);
}

static struct node *mk(struct parser *P, int type, const struct token *at)
{
    struct node *n = arena_new(sizeof *n);
    n->type = (uint16_t)type;
    n->line = at->line;
    n->col = at->col;
    n->pos = at->pos;
    n->end = at->end;
    (void)P;
    return n;
}

/* A growing list of nodes in the arena. */
struct nlist {
    struct node **v;
    int n, cap;
};

static void push(struct nlist *l, struct node *x)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        struct node **v = arena_new((size_t)cap * sizeof *v);
        if (l->n)
            memcpy(v, l->v, (size_t)l->n * sizeof *v);
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = x;
}

static void enter(struct parser *P)
{
    if (++P->depth > 400)
        error_at(P, TOK, "Too much nesting");
}

/* Peeks at the token after the current one. */
static struct token peek(struct parser *P)
{
    struct lexer saved;
    lex_save(P->L, &saved);
    lex_next(P->L);
    struct token t = P->L->tok;
    lex_restore(P->L, &saved);
    return t;
}

static bool is_reserved_strict(struct parser *P, struct jsstring *a)
{
    static const char *const words[] = { "implements", "interface", "let", "package", "private",
                                         "protected", "public", "static", "yield" };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (str_is_ascii_cstr(P->J, a, words[i]))
            return true;
    return false;
}

/* An identifier used as a binding or a reference. */
static struct jsstring *identifier(struct parser *P)
{
    if (TOK->type != T_IDENT) {
        if (TOK->type == T_KEYWORD && TOK->op == KW_ENUM)
            error_at(P, TOK, "Unexpected reserved word 'enum'");
        unexpected(P);
    }
    struct jsstring *a = TOK->atom;
    if (P->L->strict && is_reserved_strict(P, a))
        lex_error(P->L, TOK, "Unexpected strict mode reserved word");
    if (a == P->J->atom[A_await] && P->in_async)
        error_at(P, TOK, "Unexpected 'await'");
    if (a == P->J->atom[A_yield] && (P->in_generator || P->L->strict))
        error_at(P, TOK, "Unexpected 'yield'");
    next(P);
    return a;
}

static struct node *expression(struct parser *P, bool no_in);
static struct node *assignment(struct parser *P, bool no_in);
static struct node *statement(struct parser *P);
static struct node *statement_list_item(struct parser *P);
static struct node *function_rest(struct parser *P, const struct token *start, uint32_t flags,
                                  struct jsstring *name);
static struct node *class_tail(struct parser *P, const struct token *start, struct jsstring *name, bool decl);
static struct node *binding_target(struct parser *P);
static struct node *lhs_expression(struct parser *P);

/* --- patterns -------------------------------------------------------------- */

static struct node *to_pattern(struct parser *P, struct node *e, bool binding);

static struct node *to_pattern(struct parser *P, struct node *e, bool binding)
{
    struct token at = { .line = e->line, .col = e->col, .pos = e->pos };
    switch (e->type) {
    case N_IDENT:
        if (P->L->strict && (e->str == P->J->atom[A_eval] || e->str == P->J->atom[A_arguments]))
            error_at(P, &at, "Cannot assign to 'eval' or 'arguments' in strict mode");
        return e;
    case N_MEMBER: case N_INDEX: case N_SUPER_MEMBER: case N_SUPER_INDEX: case N_PRIVATE_MEMBER:
        if (binding || (e->flags & NF_OPTIONAL))
            error_at(P, &at, "Invalid destructuring target");
        return e;
    case N_ARRAY_PATTERN: case N_OBJECT_PATTERN: case N_ASSIGN_PATTERN:
        return e;
    case N_ARRAY: {
        if (e->flags & NF_PARENS)
            error_at(P, &at, "Invalid destructuring assignment target");
        struct node *p = mk(P, N_ARRAY_PATTERN, &at);
        p->list = e->list;
        p->n = e->n;
        for (int i = 0; i < p->n; i++) {
            struct node *x = p->list[i];
            if (x->type == N_HOLE)
                continue;
            if (x->type == N_SPREAD) {
                if (i != p->n - 1)
                    error_at(P, &at, "Rest element must be last element");
                struct node *r = mk(P, N_REST, &at);
                r->a = to_pattern(P, x->a, binding);
                if (r->a->type == N_ASSIGN_PATTERN)
                    error_at(P, &at, "Rest element may not have a default initializer");
                p->list[i] = r;
                continue;
            }
            p->list[i] = to_pattern(P, x, binding);
        }
        return p;
    }
    case N_OBJECT: {
        if (e->flags & NF_PARENS)
            error_at(P, &at, "Invalid destructuring assignment target");
        struct node *p = mk(P, N_OBJECT_PATTERN, &at);
        p->list = e->list;
        p->n = e->n;
        for (int i = 0; i < p->n; i++) {
            struct node *x = p->list[i];
            if (x->op == PK_SPREAD) {
                if (i != p->n - 1)
                    error_at(P, &at, "Rest element must be last element");
                struct node *r = mk(P, N_REST, &at);
                r->a = to_pattern(P, x->b, binding);
                if (r->a->type != N_IDENT && binding)
                    error_at(P, &at, "Invalid rest element");
                p->list[i] = r;
                continue;
            }
            if (x->op != PK_INIT && x->op != PK_PROTO)
                error_at(P, &at, "Invalid destructuring assignment target");
            x->op = PK_INIT;
            x->flags &= ~NF_COVER_INIT;
            x->b = to_pattern(P, x->b, binding);
        }
        return p;
    }
    case N_ASSIGN:
        if (e->op != P_ASSIGN)
            break;
        {
            struct node *p = mk(P, N_ASSIGN_PATTERN, &at);
            p->a = to_pattern(P, e->a, binding);
            p->b = e->b;
            return p;
        }
    }
    error_at(P, &at, binding ? "Invalid destructuring target" : "Invalid left-hand side in assignment");
}

static struct node *binding_element(struct parser *P)
{
    const struct token at = *TOK;
    struct node *t = binding_target(P);
    if (accept(P, P_ASSIGN)) {
        struct node *d = mk(P, N_ASSIGN_PATTERN, &at);
        d->a = t;
        d->b = assignment(P, false);
        return d;
    }
    return t;
}

static struct node *property_key(struct parser *P, uint32_t *flags);

static struct node *binding_target(struct parser *P)
{
    const struct token at = *TOK;
    enter(P);
    if (accept(P, P_LBRACKET)) {
        struct node *p = mk(P, N_ARRAY_PATTERN, &at);
        struct nlist l = { 0 };
        while (!accept(P, P_RBRACKET)) {
            if (accept(P, P_COMMA)) {
                push(&l, mk(P, N_HOLE, TOK));
                continue;
            }
            if (is_p(P, P_ELLIPSIS)) {
                struct node *r = mk(P, N_REST, TOK);
                next(P);
                r->a = binding_target(P);
                push(&l, r);
                accept(P, P_COMMA);
                expect(P, P_RBRACKET);
                break;
            }
            push(&l, binding_element(P));
            if (!is_p(P, P_RBRACKET))
                expect(P, P_COMMA);
        }
        p->list = l.v;
        p->n = l.n;
        P->depth--;
        return p;
    }
    if (accept(P, P_LBRACE)) {
        struct node *p = mk(P, N_OBJECT_PATTERN, &at);
        struct nlist l = { 0 };
        while (!accept(P, P_RBRACE)) {
            if (is_p(P, P_ELLIPSIS)) {
                struct node *r = mk(P, N_REST, TOK);
                next(P);
                const struct token it = *TOK;
                r->a = mk(P, N_IDENT, &it);
                r->a->str = identifier(P);
                push(&l, r);
                accept(P, P_COMMA);
                expect(P, P_RBRACE);
                break;
            }
            struct node *x = mk(P, N_PROPERTY, TOK);
            x->op = PK_INIT;
            const struct token kt = *TOK;
            bool ident = TOK->type == T_IDENT;
            x->a = property_key(P, &x->flags);
            if (accept(P, P_COLON))
                x->b = binding_element(P);
            else {
                if (!ident)
                    unexpected(P);
                struct node *id = mk(P, N_IDENT, &kt);
                id->str = x->a->str;
                if (P->L->strict && is_reserved_strict(P, id->str))
                    error_at(P, &kt, "Unexpected strict mode reserved word");
                x->flags |= NF_SHORTHAND;
                if (accept(P, P_ASSIGN)) {
                    struct node *d = mk(P, N_ASSIGN_PATTERN, &kt);
                    d->a = id;
                    d->b = assignment(P, false);
                    x->b = d;
                } else
                    x->b = id;
            }
            push(&l, x);
            if (!is_p(P, P_RBRACE))
                expect(P, P_COMMA);
        }
        p->list = l.v;
        p->n = l.n;
        P->depth--;
        return p;
    }
    struct node *id = mk(P, N_IDENT, &at);
    id->str = identifier(P);
    if (P->L->strict && (id->str == P->J->atom[A_eval] || id->str == P->J->atom[A_arguments]))
        error_at(P, &at, "Cannot declare 'eval' or 'arguments' in strict mode");
    P->depth--;
    return id;
}

/* --- literals ------------------------------------------------------------ */

/* A property name: identifier names (keywords too), strings, numbers, [computed]. */
static struct node *property_key(struct parser *P, uint32_t *flags)
{
    const struct token at = *TOK;
    if (accept(P, P_LBRACKET)) {
        *flags |= NF_COMPUTED;
        struct node *e = assignment(P, false);
        expect(P, P_RBRACKET);
        return e;
    }
    struct node *k = mk(P, N_STR, &at);
    switch (TOK->type) {
    case T_IDENT:
        k->str = TOK->atom;
        break;
    case T_KEYWORD:
        k->str = atom_cstr(P->J, keyword_text(TOK->op));
        break;
    case T_STR:
        k->str = TOK->atom;
        break;
    case T_NUM:
        k->str = atom(P->J, number_to_string(P->J, TOK->num, 10));
        break;
    case T_PRIVATE:
        k->type = N_PRIVATE_MEMBER;
        k->str = TOK->atom;
        break;
    default:
        unexpected(P);
    }
    next(P);
    return k;
}

static struct node *method(struct parser *P, const struct token *start, uint32_t fnflags, struct jsstring *name)
{
    bool saved_m = P->in_method;
    P->in_method = true;
    struct node *f = function_rest(P, start, fnflags | FN_METHOD, name);
    P->in_method = saved_m;
    return f;
}

static struct node *object_literal(struct parser *P)
{
    const struct token at = *TOK;
    expect(P, P_LBRACE);
    struct node *o = mk(P, N_OBJECT, &at);
    struct nlist l = { 0 };
    bool has_proto = false;
    while (!accept(P, P_RBRACE)) {
        const struct token pt = *TOK;
        struct node *x = mk(P, N_PROPERTY, &pt);
        if (accept(P, P_ELLIPSIS)) {
            x->op = PK_SPREAD;
            x->b = assignment(P, false);
        } else {
            uint32_t fnflags = 0;
            int kind = PK_INIT;
            if ((is_name(P, A_get) || is_name(P, A_set) || is_name(P, A_async)) ) {
                struct token nt = peek(P);
                bool more = !(nt.type == T_PUNCT && (nt.op == P_COLON || nt.op == P_COMMA || nt.op == P_RBRACE ||
                                                      nt.op == P_LPAREN || nt.op == P_ASSIGN));
                if (more) {
                    if (is_name(P, A_get))
                        kind = PK_GET, fnflags = FN_GETTER;
                    else if (is_name(P, A_set))
                        kind = PK_SET, fnflags = FN_SETTER;
                    else {
                        if (nt.nl_before)
                            unexpected(P);
                        kind = PK_METHOD, fnflags = FN_ASYNC;
                    }
                    next(P);
                }
            }
            if (accept(P, P_STAR)) {
                fnflags |= FN_GENERATOR;
                kind = kind == PK_INIT ? PK_METHOD : kind;
            }
            bool ident = TOK->type == T_IDENT;
            const struct token kt = *TOK;
            x->a = property_key(P, &x->flags);
            if (x->a->type == N_PRIVATE_MEMBER)
                error_at(P, &kt, "Unexpected private name");
            if (kind == PK_GET || kind == PK_SET || kind == PK_METHOD || is_p(P, P_LPAREN)) {
                if (!is_p(P, P_LPAREN))
                    unexpected(P);
                x->op = kind == PK_INIT ? PK_METHOD : kind;
                x->b = method(P, &kt, fnflags, (x->flags & NF_COMPUTED) ? 0 : x->a->str);
            } else if (accept(P, P_COLON)) {
                x->op = PK_INIT;
                x->b = assignment(P, false);
                if (!(x->flags & NF_COMPUTED) && x->a->str == P->J->atom[A_proto]) {
                    if (has_proto)
                        error_at(P, &pt, "Duplicate __proto__ fields are not allowed in object literals");
                    has_proto = true;
                    x->op = PK_PROTO;
                }
            } else {
                if (!ident || kt.type != T_IDENT)
                    unexpected(P);
                struct node *id = mk(P, N_IDENT, &kt);
                id->str = x->a->str;
                if (kt.atom == P->J->atom[A_await] && P->in_async)
                    error_at(P, &kt, "Unexpected 'await'");
                x->op = PK_INIT;
                x->flags |= NF_SHORTHAND;
                if (is_p(P, P_ASSIGN)) {
                    next(P);
                    struct node *a = mk(P, N_ASSIGN, &kt);
                    a->op = P_ASSIGN;
                    a->a = id;
                    a->b = assignment(P, false);
                    x->b = a;
                    x->flags |= NF_COVER_INIT;
                } else
                    x->b = id;
            }
        }
        push(&l, x);
        if (!is_p(P, P_RBRACE))
            expect(P, P_COMMA);
    }
    o->list = l.v;
    o->n = l.n;
    return o;
}

static struct node *array_literal(struct parser *P)
{
    const struct token at = *TOK;
    expect(P, P_LBRACKET);
    struct node *a = mk(P, N_ARRAY, &at);
    struct nlist l = { 0 };
    while (!accept(P, P_RBRACKET)) {
        if (is_p(P, P_COMMA)) {
            push(&l, mk(P, N_HOLE, TOK));
            next(P);
            continue;
        }
        if (is_p(P, P_ELLIPSIS)) {
            struct node *s = mk(P, N_SPREAD, TOK);
            next(P);
            s->a = assignment(P, false);
            push(&l, s);
        } else
            push(&l, assignment(P, false));
        if (!is_p(P, P_RBRACKET))
            expect(P, P_COMMA);
    }
    a->list = l.v;
    a->n = l.n;
    return a;
}

/* A template from its first part (the current token); tagged ones keep the
 * raw strings and allow invalid escapes (their cooked string undefined). */
static struct node *template_literal(struct parser *P, bool tagged)
{
    const struct token at = *TOK;
    struct node *t = mk(P, tagged ? N_TAGGED : N_TEMPLATE, &at);
    struct nlist parts = { 0 }, exprs = { 0 }, raws = { 0 };
    for (;;) {
        if (TOK->type != T_TEMPLATE)
            unexpected(P);
        if (!TOK->atom && !tagged)
            error_at(P, TOK, "Invalid escape sequence in template");
        struct node *s = mk(P, N_STR, TOK);
        s->str = TOK->atom;                     /* 0: undefined (tagged only) */
        push(&parts, s);
        struct node *r = mk(P, N_STR, TOK);
        r->str = TOK->raw;
        push(&raws, r);
        if (TOK->tail) {
            next(P);
            break;
        }
        next(P);
        push(&exprs, expression(P, false));
        if (!is_p(P, P_RBRACE))
            unexpected(P);
        lex_template_continue(P->L);
    }
    t->raws = parts.v;                          /* cooked */
    t->n = exprs.n;
    t->list = exprs.v;
    t->d = mk(P, N_ARRAY, &at);                 /* the raw strings */
    t->d->list = raws.v;
    t->d->n = raws.n;
    return t;
}

/* --- functions and classes ----------------------------------------------- */

static bool directive_is_strict(struct parser *P, const struct token *t)
{
    if (t->type != T_STR || t->end - t->pos != 12)
        return false;
    const uint16_t *s = P->L->s + t->pos + 1;
    static const char us[] = "use strict";
    for (int i = 0; i < 10; i++)
        if (s[i] != (uint16_t)us[i])
            return false;
    return true;
}

/* Statements up to `}` (or the end), with the directive prologue. */
static struct node **body_statements(struct parser *P, int *n, bool *strict, bool to_eof)
{
    struct nlist l = { 0 };
    bool prologue = true;
    while (to_eof ? TOK->type != T_EOF : !is_p(P, P_RBRACE)) {
        if (TOK->type == T_EOF)
            unexpected(P);
        if (prologue) {
            if (TOK->type == T_STR) {
                struct token st = *TOK;
                struct token nt = peek(P);
                bool alone = (nt.type == T_PUNCT && (nt.op == P_SEMI || nt.op == P_RBRACE)) || nt.nl_before ||
                             nt.type == T_EOF;
                if (alone && directive_is_strict(P, &st)) {
                    *strict = true;
                    P->L->strict = true;
                }
                if (!alone)
                    prologue = false;
            } else
                prologue = false;
        }
        push(&l, statement_list_item(P));
    }
    *n = l.n;
    return l.v;
}

static bool simple_params(struct node **p, int n)
{
    for (int i = 0; i < n; i++)
        if (p[i]->type != N_IDENT)
            return false;
    return true;
}

static int param_length(struct node **p, int n)
{
    int k = 0;
    for (; k < n; k++)
        if (p[k]->type == N_ASSIGN_PATTERN || p[k]->type == N_REST)
            break;
    return k;
}

struct saved_ctx {
    bool in_function, in_async, in_generator, in_class_field, in_method, in_derived_ctor, in_static_block, strict;
};

static struct saved_ctx save_ctx(struct parser *P)
{
    struct saved_ctx s = { P->in_function, P->in_async, P->in_generator, P->in_class_field,
                           P->in_method, P->in_derived_ctor, P->in_static_block, P->L->strict };
    return s;
}

static void restore_ctx(struct parser *P, struct saved_ctx s)
{
    P->in_function = s.in_function;
    P->in_async = s.in_async;
    P->in_generator = s.in_generator;
    P->in_class_field = s.in_class_field;
    P->in_method = s.in_method;
    P->in_derived_ctor = s.in_derived_ctor;
    P->in_static_block = s.in_static_block;
    P->L->strict = s.strict;
}

/* Parameters and body from '(' on. */
static struct node *function_rest(struct parser *P, const struct token *start, uint32_t flags, struct jsstring *name)
{
    struct node *f = mk(P, N_FUNCTION, start);
    struct cfunc *fn = arena_new(sizeof *fn);
    f->fn = fn;
    fn->name = name;
    fn->start = start->pos;
    fn->line = start->line;
    struct saved_ctx saved = save_ctx(P);
    P->in_function = true;
    P->in_async = (flags & FN_ASYNC) != 0;
    P->in_generator = (flags & FN_GENERATOR) != 0;
    P->in_class_field = false;
    P->in_static_block = false;
    if (!(flags & FN_METHOD))
        P->in_method = false;
    if (!(flags & FN_CTOR))
        P->in_derived_ctor = false;
    enter(P);
    expect(P, P_LPAREN);
    struct nlist params = { 0 };
    while (!accept(P, P_RPAREN)) {
        if (is_p(P, P_ELLIPSIS)) {
            struct node *r = mk(P, N_REST, TOK);
            next(P);
            r->a = binding_target(P);
            push(&params, r);
            expect(P, P_RPAREN);
            break;
        }
        push(&params, binding_element(P));
        if (!is_p(P, P_RPAREN))
            expect(P, P_COMMA);
    }
    if ((flags & FN_GETTER) && params.n != 0)
        error_at(P, start, "Getter must not have any formal parameters.");
    if ((flags & FN_SETTER) && (params.n != 1 || params.v[0]->type == N_REST))
        error_at(P, start, "Setter must have exactly one formal parameter.");
    fn->params = params.v;
    fn->nparams = params.n;
    expect(P, P_LBRACE);
    bool strict = P->L->strict;
    fn->body = body_statements(P, &fn->nbody, &strict, false);
    if (strict && !saved.strict && !simple_params(fn->params, fn->nparams))
        error_at(P, start, "'use strict' not allowed in function with non-simple parameters");
    fn->end = TOK->end;
    f->end = TOK->end;
    expect(P, P_RBRACE);
    if (strict)
        flags |= FN_STRICT;
    if (simple_params(fn->params, fn->nparams))
        flags |= FN_SIMPLE_PARAMS;
    fn->flags = flags;
    fn->length = param_length(fn->params, fn->nparams);
    P->depth--;
    restore_ctx(P, saved);
    return f;
}

/* An arrow function from its parameters (already patterns) at `=>`. */
static struct node *arrow_rest(struct parser *P, const struct token *start, struct node **params, int nparams,
                               bool async)
{
    struct node *f = mk(P, N_FUNCTION, start);
    struct cfunc *fn = arena_new(sizeof *fn);
    f->fn = fn;
    fn->start = start->pos;
    fn->line = start->line;
    fn->params = params;
    fn->nparams = nparams;
    if (TOK->nl_before)
        unexpected(P);
    expect(P, P_ARROW);
    struct saved_ctx saved = save_ctx(P);
    P->in_async = async;
    P->in_generator = false;
    uint32_t flags = FN_ARROW | (async ? FN_ASYNC : 0);
    enter(P);
    if (is_p(P, P_LBRACE)) {
        next(P);
        bool strict = P->L->strict;
        bool saved_fn = P->in_function;
        P->in_function = true;
        fn->body = body_statements(P, &fn->nbody, &strict, false);
        P->in_function = saved_fn;
        if (strict && !saved.strict && !simple_params(params, nparams))
            error_at(P, start, "'use strict' not allowed in function with non-simple parameters");
        if (strict)
            flags |= FN_STRICT;
        fn->end = TOK->end;
        expect(P, P_RBRACE);
    } else {
        bool saved_fn = P->in_function;
        P->in_function = true;
        fn->expr = assignment(P, false);
        P->in_function = saved_fn;
        fn->end = P->L->tok.pos;
        /* the expression's end: the previous token's (approximately the next token's start) */
        flags |= FN_EXPRESSION;
        if (P->L->strict)
            flags |= FN_STRICT;
    }
    if (simple_params(params, nparams))
        flags |= FN_SIMPLE_PARAMS;
    fn->flags = flags;
    fn->length = param_length(params, nparams);
    f->end = fn->end;
    P->depth--;
    restore_ctx(P, saved);
    return f;
}

static struct node *function_expression(struct parser *P, bool async, const struct token *start)
{
    /* at `function` */
    next(P);
    uint32_t flags = async ? FN_ASYNC : 0;
    if (accept(P, P_STAR))
        flags |= FN_GENERATOR;
    struct jsstring *name = 0;
    if (TOK->type == T_IDENT) {
        bool sa = P->in_async, sg = P->in_generator;
        P->in_async = async;
        P->in_generator = (flags & FN_GENERATOR) != 0;
        name = identifier(P);
        P->in_async = sa;
        P->in_generator = sg;
        flags |= FN_HAS_NAME_BINDING;
    }
    struct node *f = function_rest(P, start, flags | FN_EXPRESSION, name);
    return f;
}

static struct node *class_tail(struct parser *P, const struct token *start, struct jsstring *name, bool decl)
{
    struct node *c = mk(P, N_CLASS, start);
    c->str = name;
    struct saved_ctx saved = save_ctx(P);
    P->L->strict = true;
    if (is_kw(P, KW_EXTENDS)) {
        next(P);
        c->a = lhs_expression(P);
    }
    expect(P, P_LBRACE);
    struct nlist members = { 0 };
    while (!accept(P, P_RBRACE)) {
        if (accept(P, P_SEMI))
            continue;
        const struct token mt = *TOK;
        struct node *m = mk(P, N_CLASS_MEMBER, &mt);
        if (is_name(P, A_static)) {
            struct token nt = peek(P);
            if (!(nt.type == T_PUNCT && (nt.op == P_LPAREN || nt.op == P_ASSIGN || nt.op == P_SEMI ||
                                          nt.op == P_RBRACE))) {
                next(P);
                m->flags |= NF_STATIC;
                if (is_p(P, P_LBRACE)) {
                    /* static { ... } */
                    m->type = N_STATIC_BLOCK;
                    const struct token bt = *TOK;
                    next(P);
                    struct node *f = mk(P, N_FUNCTION, &bt);
                    struct cfunc *fn = arena_new(sizeof *fn);
                    f->fn = fn;
                    fn->start = bt.pos;
                    fn->line = bt.line;
                    struct saved_ctx s2 = save_ctx(P);
                    P->in_function = true;
                    P->in_async = P->in_generator = false;
                    P->in_method = true;
                    P->in_static_block = true;
                    struct nlist body = { 0 };
                    while (!accept(P, P_RBRACE)) {
                        if (TOK->type == T_EOF)
                            unexpected(P);
                        push(&body, statement_list_item(P));
                    }
                    restore_ctx(P, s2);
                    fn->body = body.v;
                    fn->nbody = body.n;
                    fn->flags = FN_METHOD | FN_STRICT | FN_STATIC_BLOCK | FN_SIMPLE_PARAMS;
                    m->b = f;
                    push(&members, m);
                    continue;
                }
            }
        }
        uint32_t fnflags = 0;
        int kind = PK_METHOD;
        if (is_name(P, A_get) || is_name(P, A_set) || is_name(P, A_async)) {
            struct token nt = peek(P);
            bool more = !(nt.type == T_PUNCT && (nt.op == P_LPAREN || nt.op == P_ASSIGN || nt.op == P_SEMI ||
                                                  nt.op == P_RBRACE)) && !(nt.nl_before && is_name(P, A_async));
            if (more && !nt.nl_before) {
                if (is_name(P, A_get))
                    kind = PK_GET, fnflags = FN_GETTER;
                else if (is_name(P, A_set))
                    kind = PK_SET, fnflags = FN_SETTER;
                else
                    fnflags = FN_ASYNC;
                next(P);
            } else if (more && (is_name(P, A_get) || is_name(P, A_set))) {
                kind = is_name(P, A_get) ? PK_GET : PK_SET;
                fnflags = kind == PK_GET ? FN_GETTER : FN_SETTER;
                next(P);
            }
        }
        if (accept(P, P_STAR))
            fnflags |= FN_GENERATOR;
        const struct token kt = *TOK;
        m->a = property_key(P, &m->flags);
        bool is_ctor = !(m->flags & (NF_COMPUTED | NF_STATIC)) && m->a->type == N_STR &&
                       m->a->str == P->J->atom[A_constructor];
        if (is_p(P, P_LPAREN)) {
            m->op = (uint16_t)kind;
            if (is_ctor) {
                if (kind != PK_METHOD || fnflags)
                    error_at(P, &kt, "Class constructor may not be an accessor, generator or async");
                if (c->b)
                    error_at(P, &kt, "A class may only have one constructor");
                bool sd = P->in_derived_ctor;
                P->in_derived_ctor = c->a != 0;
                m->b = method(P, &kt, FN_CTOR | FN_CLASS_CTOR | (c->a ? FN_DERIVED : 0), name);
                P->in_derived_ctor = sd;
                c->b = m->b;
                continue;
            }
            if (m->a->type == N_PRIVATE_MEMBER && m->a->str == P->J->atom[A_constructor])
                error_at(P, &kt, "Classes may not have a private field named '#constructor'");
            m->b = method(P, &kt, fnflags, (m->flags & NF_COMPUTED) ? 0 : m->a->str);
        } else {
            if (kind != PK_METHOD || fnflags)
                unexpected(P);
            if (is_ctor || (!(m->flags & NF_COMPUTED) && m->a->type == N_STR &&
                            m->a->str == P->J->atom[A_constructor]))
                error_at(P, &kt, "Classes may not have a field named 'constructor'");
            m->op = PK_FIELD;
            if (accept(P, P_ASSIGN)) {
                /* the initializer is a method-like function of its own (its `this`) */
                const struct token it = *TOK;
                struct node *f = mk(P, N_FUNCTION, &it);
                struct cfunc *fn = arena_new(sizeof *fn);
                f->fn = fn;
                fn->start = it.pos;
                fn->line = it.line;
                struct saved_ctx s2 = save_ctx(P);
                P->in_function = true;
                P->in_async = P->in_generator = false;
                P->in_method = true;
                P->in_class_field = true;
                fn->expr = assignment(P, false);
                restore_ctx(P, s2);
                fn->flags = FN_METHOD | FN_STRICT | FN_EXPRESSION | FN_FIELD_INIT | FN_SIMPLE_PARAMS;
                fn->end = TOK->pos;
                m->b = f;
            }
            semicolon(P);
        }
        push(&members, m);
    }
    c->list = members.v;
    c->n = members.n;
    c->end = P->L->tok.pos;
    restore_ctx(P, saved);
    (void)decl;
    return c;
}

/* --- expressions --------------------------------------------------------- */

static struct node *primary(struct parser *P)
{
    const struct token at = *TOK;
    struct node *n;
    switch (TOK->type) {
    case T_NUM:
        n = mk(P, N_NUM, &at);
        n->num = TOK->num;
        next(P);
        return n;
    case T_STR:
        n = mk(P, N_STR, &at);
        n->str = TOK->atom;
        next(P);
        return n;
    case T_TEMPLATE:
        return template_literal(P, false);
    case T_PRIVATE: {
        /* #x in obj */
        n = mk(P, N_PRIVATE_IN, &at);
        n->str = TOK->atom;
        next(P);
        if (!is_kw(P, KW_IN))
            unexpected(P);
        return n;
    }
    case T_IDENT: {
        if (is_name(P, A_async)) {
            struct token nt = peek(P);
            if (!nt.nl_before && nt.type == T_KEYWORD && nt.op == KW_FUNCTION) {
                next(P);
                return function_expression(P, true, &at);
            }
            if (!nt.nl_before && nt.type == T_IDENT) {
                /* async x => ... */
                next(P);
                struct node *id = mk(P, N_IDENT, TOK);
                bool sa = P->in_async;
                P->in_async = true;
                id->str = identifier(P);
                P->in_async = sa;
                struct node **ps = arena_new(sizeof *ps);
                ps[0] = id;
                return arrow_rest(P, &at, ps, 1, true);
            }
            if (!nt.nl_before && nt.type == T_PUNCT && nt.op == P_LPAREN) {
                /* async(...) : a call or an async arrow's parameters */
                next(P);
                struct node *call = mk(P, N_CALL, &at);
                struct node *callee = mk(P, N_IDENT, &at);
                callee->str = P->J->atom[A_async];
                call->a = callee;
                next(P);
                struct nlist args = { 0 };
                bool trailing = false;
                while (!accept(P, P_RPAREN)) {
                    if (is_p(P, P_ELLIPSIS)) {
                        struct node *s = mk(P, N_SPREAD, TOK);
                        next(P);
                        s->a = assignment(P, false);
                        push(&args, s);
                    } else
                        push(&args, assignment(P, false));
                    trailing = false;
                    if (!is_p(P, P_RPAREN)) {
                        expect(P, P_COMMA);
                        trailing = true;
                    }
                }
                (void)trailing;
                call->list = args.v;
                call->n = args.n;
                if (is_p(P, P_ARROW) && !TOK->nl_before) {
                    for (int i = 0; i < call->n; i++) {
                        struct node *x = call->list[i];
                        if (x->type == N_SPREAD) {
                            struct node *r = mk(P, N_REST, &at);
                            r->a = to_pattern(P, x->a, true);
                            call->list[i] = r;
                        } else
                            call->list[i] = to_pattern(P, x, true);
                    }
                    return arrow_rest(P, &at, call->list, call->n, true);
                }
                return call;
            }
        }
        struct token nt = peek(P);
        if (nt.type == T_PUNCT && nt.op == P_ARROW && !nt.nl_before) {
            struct node *id = mk(P, N_IDENT, &at);
            id->str = identifier(P);
            if (P->L->strict && (id->str == P->J->atom[A_eval] || id->str == P->J->atom[A_arguments]))
                error_at(P, &at, "Cannot declare 'eval' or 'arguments' in strict mode");
            struct node **ps = arena_new(sizeof *ps);
            ps[0] = id;
            return arrow_rest(P, &at, ps, 1, false);
        }
        n = mk(P, N_IDENT, &at);
        n->str = identifier(P);
        return n;
    }
    case T_KEYWORD:
        switch (TOK->op) {
        case KW_THIS:
            next(P);
            return mk(P, N_THIS, &at);
        case KW_NULL:
            next(P);
            return mk(P, N_NULL, &at);
        case KW_TRUE:
            next(P);
            return mk(P, N_TRUE, &at);
        case KW_FALSE:
            next(P);
            return mk(P, N_FALSE, &at);
        case KW_FUNCTION:
            return function_expression(P, false, &at);
        case KW_CLASS: {
            next(P);
            struct jsstring *name = 0;
            if (TOK->type == T_IDENT && !is_kw(P, KW_EXTENDS)) {
                bool s = P->L->strict;
                P->L->strict = true;
                name = identifier(P);
                P->L->strict = s;
            }
            return class_tail(P, &at, name, false);
        }
        }
        unexpected(P);
    case T_PUNCT:
        switch (TOK->op) {
        case P_LBRACKET:
            return array_literal(P);
        case P_LBRACE:
            return object_literal(P);
        case P_SLASH: case P_DIV_ASSIGN: {
            lex_regexp(P->L);
            n = mk(P, N_REGEXP, TOK);
            n->str = TOK->atom;
            n->raw = TOK->raw;
            extern const char *regex_check(struct js *J, struct jsstring *src, struct jsstring *flags);
            const char *err = regex_check(P->J, n->str, n->raw);
            if (err)
                lex_error(P->L, &at, "Invalid regular expression: %s", err);
            next(P);
            return n;
        }
        case P_LPAREN: {
            next(P);
            if (is_p(P, P_RPAREN)) {
                next(P);
                if (!is_p(P, P_ARROW))
                    unexpected(P);
                return arrow_rest(P, &at, 0, 0, false);
            }
            struct nlist items = { 0 };
            bool rest = false, trailing = false;
            for (;;) {
                if (is_p(P, P_ELLIPSIS)) {
                    struct node *r = mk(P, N_REST, TOK);
                    next(P);
                    r->a = binding_target(P);
                    push(&items, r);
                    rest = true;
                    expect(P, P_RPAREN);
                    break;
                }
                push(&items, assignment(P, false));
                if (accept(P, P_RPAREN))
                    break;
                expect(P, P_COMMA);
                if (accept(P, P_RPAREN)) {
                    trailing = true;
                    break;
                }
            }
            if (is_p(P, P_ARROW) && !TOK->nl_before) {
                for (int i = 0; i < items.n; i++)
                    if (items.v[i]->type != N_REST)
                        items.v[i] = to_pattern(P, items.v[i], true);
                return arrow_rest(P, &at, items.v, items.n, false);
            }
            if (rest || trailing)
                unexpected(P);
            if (items.n == 1) {
                items.v[0]->flags |= NF_PARENS;
                return items.v[0];
            }
            n = mk(P, N_SEQ, &at);
            n->list = items.v;
            n->n = items.n;
            n->flags |= NF_PARENS;
            return n;
        }
        }
    default:
        break;
    }
    unexpected(P);
}

static struct node *arguments(struct parser *P, struct node *call)
{
    expect(P, P_LPAREN);
    struct nlist args = { 0 };
    while (!accept(P, P_RPAREN)) {
        if (is_p(P, P_ELLIPSIS)) {
            struct node *s = mk(P, N_SPREAD, TOK);
            next(P);
            s->a = assignment(P, false);
            push(&args, s);
        } else
            push(&args, assignment(P, false));
        if (!is_p(P, P_RPAREN))
            expect(P, P_COMMA);
    }
    call->list = args.v;
    call->n = args.n;
    return call;
}

static struct jsstring *member_name(struct parser *P)
{
    struct jsstring *a;
    if (TOK->type == T_IDENT)
        a = TOK->atom;
    else if (TOK->type == T_KEYWORD)
        a = atom_cstr(P->J, keyword_text(TOK->op));
    else
        unexpected(P);
    next(P);
    return a;
}

/* Member accesses, calls and templates after `base`; calls only if allowed. */
static struct node *call_tail(struct parser *P, struct node *base, bool allow_call)
{
    bool chain = false;
    for (;;) {
        const struct token at = *TOK;
        if (accept(P, P_DOT)) {
            if (TOK->type == T_PRIVATE) {
                struct node *m = mk(P, N_PRIVATE_MEMBER, &at);
                m->a = base;
                m->str = TOK->atom;
                next(P);
                base = m;
                continue;
            }
            struct node *m = mk(P, N_MEMBER, &at);
            m->a = base;
            m->str = member_name(P);
            base = m;
        } else if (is_p(P, P_OPTIONAL)) {
            if (!allow_call)
                break;                          /* (the full tail takes it) */
            next(P);
            chain = true;
            struct node *m;
            if (is_p(P, P_LPAREN)) {
                m = mk(P, N_CALL, &at);
                m->a = base;
                arguments(P, m);
            } else if (accept(P, P_LBRACKET)) {
                m = mk(P, N_INDEX, &at);
                m->a = base;
                m->b = expression(P, false);
                expect(P, P_RBRACKET);
            } else if (TOK->type == T_PRIVATE) {
                m = mk(P, N_PRIVATE_MEMBER, &at);
                m->a = base;
                m->str = TOK->atom;
                next(P);
            } else {
                m = mk(P, N_MEMBER, &at);
                m->a = base;
                m->str = member_name(P);
            }
            m->flags |= NF_OPTIONAL;
            base = m;
        } else if (accept(P, P_LBRACKET)) {
            struct node *m = mk(P, N_INDEX, &at);
            m->a = base;
            m->b = expression(P, false);
            expect(P, P_RBRACKET);
            base = m;
        } else if (allow_call && is_p(P, P_LPAREN)) {
            struct node *c = mk(P, N_CALL, &at);
            c->a = base;
            arguments(P, c);
            base = c;
        } else if (TOK->type == T_TEMPLATE) {
            if (chain)
                error_at(P, &at, "Invalid tagged template on optional chain");
            struct node *t = template_literal(P, true);
            t->a = base;
            base = t;
        } else
            break;
    }
    if (chain) {
        struct node *c = mk(P, N_CHAIN, TOK);
        c->line = base->line;
        c->pos = base->pos;
        c->a = base;
        return c;
    }
    return base;
}

static struct node *member_expression(struct parser *P)
{
    const struct token at = *TOK;
    enter(P);
    struct node *base;
    if (is_kw(P, KW_NEW)) {
        next(P);
        if (accept(P, P_DOT)) {
            if (!(TOK->type == T_IDENT && TOK->atom == P->J->atom[A_target]))
                unexpected(P);
            if (!P->in_function && !P->in_class_field)
                error_at(P, &at, "new.target is only valid inside functions");
            next(P);
            base = mk(P, N_NEW_TARGET, &at);
        } else {
            struct node *callee = member_expression(P);
            struct node *n = mk(P, N_NEW, &at);
            n->a = callee;
            if (is_p(P, P_LPAREN))
                arguments(P, n);
            base = n;
        }
        base = call_tail(P, base, false);
    } else if (is_kw(P, KW_SUPER)) {
        next(P);
        if (is_p(P, P_LPAREN)) {
            if (!P->in_derived_ctor)
                error_at(P, &at, "super() is only valid in derived class constructors");
            base = mk(P, N_SUPER_CALL, &at);
            arguments(P, base);
        } else if (accept(P, P_DOT)) {
            if (!P->in_method)
                error_at(P, &at, "super is only valid inside methods");
            base = mk(P, N_SUPER_MEMBER, &at);
            base->str = member_name(P);
        } else if (accept(P, P_LBRACKET)) {
            if (!P->in_method)
                error_at(P, &at, "super is only valid inside methods");
            base = mk(P, N_SUPER_INDEX, &at);
            base->b = expression(P, false);
            expect(P, P_RBRACKET);
        } else
            error_at(P, &at, "'super' keyword unexpected here");
        base = call_tail(P, base, false);
    } else {
        base = primary(P);
        base = call_tail(P, base, false);
    }
    P->depth--;
    return base;
}

static struct node *lhs_expression(struct parser *P)
{
    struct node *m = member_expression(P);
    return call_tail(P, m, true);
}

static struct node *unary(struct parser *P);

static struct node *update(struct parser *P)
{
    const struct token at = *TOK;
    if (is_p(P, P_INC) || is_p(P, P_DEC)) {
        int op = TOK->op;
        next(P);
        struct node *e = unary(P);
        if (e->type != N_IDENT && e->type != N_MEMBER && e->type != N_INDEX && e->type != N_SUPER_MEMBER &&
            e->type != N_SUPER_INDEX && e->type != N_PRIVATE_MEMBER)
            error_at(P, &at, "Invalid left-hand side expression in prefix operation");
        if (e->type == N_IDENT && P->L->strict && (e->str == P->J->atom[A_eval] || e->str == P->J->atom[A_arguments]))
            error_at(P, &at, "Cannot modify 'eval' or 'arguments' in strict mode");
        struct node *u = mk(P, N_UPDATE, &at);
        u->op = (uint16_t)op;
        u->flags |= NF_PREFIX;
        u->a = e;
        return u;
    }
    struct node *e = lhs_expression(P);
    if ((is_p(P, P_INC) || is_p(P, P_DEC)) && !TOK->nl_before) {
        if (e->type != N_IDENT && e->type != N_MEMBER && e->type != N_INDEX && e->type != N_SUPER_MEMBER &&
            e->type != N_SUPER_INDEX && e->type != N_PRIVATE_MEMBER)
            error_at(P, &at, "Invalid left-hand side expression in postfix operation");
        struct node *u = mk(P, N_UPDATE, &at);
        u->op = (uint16_t)TOK->op;
        u->a = e;
        next(P);
        return u;
    }
    return e;
}

static struct node *unary(struct parser *P)
{
    const struct token at = *TOK;
    enter(P);
    struct node *r;
    if ((TOK->type == T_PUNCT && (TOK->op == P_PLUS || TOK->op == P_MINUS || TOK->op == P_BANG ||
                                  TOK->op == P_TILDE)) ||
        (TOK->type == T_KEYWORD && (TOK->op == KW_TYPEOF || TOK->op == KW_VOID || TOK->op == KW_DELETE))) {
        struct node *u = mk(P, N_UNARY, &at);
        u->op = (uint16_t)(TOK->type == T_KEYWORD ? 1000 + TOK->op : TOK->op);
        next(P);
        u->a = unary(P);
        if (u->op == 1000 + KW_DELETE && P->L->strict && u->a->type == N_IDENT)
            error_at(P, &at, "Cannot delete unqualified property in strict mode");
        if (u->op == 1000 + KW_DELETE && u->a->type == N_PRIVATE_MEMBER)
            error_at(P, &at, "Private fields can not be deleted");
        r = u;
    } else if (P->in_async && is_name(P, A_await)) {
        struct node *u = mk(P, N_AWAIT, &at);
        next(P);
        u->a = unary(P);
        r = u;
    } else
        r = update(P);
    P->depth--;
    return r;
}

static int binary_prec(struct parser *P, bool no_in)
{
    if (TOK->type == T_KEYWORD) {
        if (TOK->op == KW_INSTANCEOF)
            return 8;
        if (TOK->op == KW_IN && !no_in)
            return 8;
        return 0;
    }
    if (TOK->type != T_PUNCT)
        return 0;
    switch (TOK->op) {
    case P_NULLISH: return 1;
    case P_OR: return 2;
    case P_AND: return 3;
    case P_BAR: return 4;
    case P_CARET: return 5;
    case P_AMP: return 6;
    case P_EQ: case P_NE: case P_SEQ: case P_SNE: return 7;
    case P_LT: case P_GT: case P_LE: case P_GE: return 8;
    case P_SHL: case P_SAR: case P_SHR: return 9;
    case P_PLUS: case P_MINUS: return 10;
    case P_STAR: case P_SLASH: case P_PERCENT: return 11;
    case P_STARSTAR: return 12;
    }
    return 0;
}

static struct node *binary(struct parser *P, int min_prec, bool no_in)
{
    const struct token at = *TOK;
    struct node *left = unary(P);
    for (;;) {
        int prec = binary_prec(P, no_in);
        if (!prec || prec < min_prec)
            break;
        int op = TOK->type == T_KEYWORD ? 1000 + TOK->op : TOK->op;
        const struct token ot = *TOK;
        if (op == P_STARSTAR && left->type == N_UNARY && !(left->flags & NF_PARENS))
            error_at(P, &ot, "Unary operator used immediately before exponentiation expression");
        next(P);
        struct node *right;
        if (op == P_STARSTAR)
            right = binary(P, prec, no_in);     /* right associative */
        else
            right = binary(P, prec + 1, no_in);
        struct node *b;
        if (op == P_AND || op == P_OR || op == P_NULLISH) {
            b = mk(P, N_LOGICAL, &at);
            if (op == P_NULLISH) {
                if ((left->type == N_LOGICAL && left->op != P_NULLISH && !(left->flags & NF_PARENS)) ||
                    (right->type == N_LOGICAL && right->op != P_NULLISH && !(right->flags & NF_PARENS)))
                    error_at(P, &ot, "Cannot mix ?? with && or || without parentheses");
            } else if ((left->type == N_LOGICAL && left->op == P_NULLISH && !(left->flags & NF_PARENS)) ||
                       (right->type == N_LOGICAL && right->op == P_NULLISH && !(right->flags & NF_PARENS)))
                error_at(P, &ot, "Cannot mix ?? with && or || without parentheses");
        } else
            b = mk(P, N_BINARY, &at);
        b->op = (uint16_t)op;
        b->line = ot.line;
        b->a = left;
        b->b = right;
        left = b;
    }
    return left;
}

static struct node *conditional(struct parser *P, bool no_in)
{
    const struct token at = *TOK;
    struct node *test = binary(P, 1, no_in);
    if (!accept(P, P_QUESTION))
        return test;
    struct node *c = mk(P, N_COND, &at);
    c->a = test;
    c->b = assignment(P, false);
    expect(P, P_COLON);
    c->c = assignment(P, no_in);
    return c;
}

static bool is_assign_op(int op)
{
    return op >= P_ASSIGN && op <= P_NULLISH_ASSIGN;
}

static struct node *assignment(struct parser *P, bool no_in)
{
    const struct token at = *TOK;
    enter(P);
    if (P->in_generator && is_name(P, A_yield)) {
        struct node *y = mk(P, N_YIELD, &at);
        next(P);
        if (!TOK->nl_before) {
            if (accept(P, P_STAR)) {
                y->flags |= NF_PREFIX;          /* yield* */
                y->a = assignment(P, no_in);
            } else if (!(TOK->type == T_PUNCT && (TOK->op == P_RPAREN || TOK->op == P_RBRACKET ||
                                                  TOK->op == P_RBRACE || TOK->op == P_COMMA ||
                                                  TOK->op == P_SEMI || TOK->op == P_COLON)) &&
                       TOK->type != T_EOF && !is_kw(P, KW_IN))
                y->a = assignment(P, no_in);
        }
        P->depth--;
        return y;
    }
    struct node *left = conditional(P, no_in);
    if (left->type == N_FUNCTION && (left->fn->flags & FN_ARROW)) {
        P->depth--;
        return left;
    }
    if (TOK->type == T_PUNCT && is_assign_op(TOK->op)) {
        int op = TOK->op;
        const struct token ot = *TOK;
        if (op == P_ASSIGN)
            left = to_pattern(P, left, false);
        else if (left->type != N_IDENT && left->type != N_MEMBER && left->type != N_INDEX &&
                 left->type != N_SUPER_MEMBER && left->type != N_SUPER_INDEX && left->type != N_PRIVATE_MEMBER)
            error_at(P, &ot, "Invalid left-hand side in assignment");
        else if (left->type == N_IDENT && P->L->strict &&
                 (left->str == P->J->atom[A_eval] || left->str == P->J->atom[A_arguments]))
            error_at(P, &ot, "Cannot assign to 'eval' or 'arguments' in strict mode");
        next(P);
        struct node *a = mk(P, N_ASSIGN, &at);
        a->op = (uint16_t)op;
        a->a = left;
        a->b = assignment(P, no_in);
        P->depth--;
        return a;
    }
    P->depth--;
    return left;
}

static struct node *expression(struct parser *P, bool no_in)
{
    const struct token at = *TOK;
    struct node *e = assignment(P, no_in);
    if (!is_p(P, P_COMMA))
        return e;
    struct nlist l = { 0 };
    push(&l, e);
    while (accept(P, P_COMMA))
        push(&l, assignment(P, no_in));
    struct node *s = mk(P, N_SEQ, &at);
    s->list = l.v;
    s->n = l.n;
    return s;
}

/* --- statements ------------------------------------------------------------ */

static struct node *block(struct parser *P)
{
    const struct token at = *TOK;
    expect(P, P_LBRACE);
    struct node *b = mk(P, N_BLOCK, &at);
    struct nlist l = { 0 };
    enter(P);
    while (!accept(P, P_RBRACE)) {
        if (TOK->type == T_EOF)
            unexpected(P);
        push(&l, statement_list_item(P));
    }
    P->depth--;
    b->list = l.v;
    b->n = l.n;
    return b;
}

/* var/let/const declarations (the keyword already read). */
static struct node *declarations(struct parser *P, const struct token *at, int kind, bool no_in, bool in_for)
{
    struct node *v = mk(P, N_VAR, at);
    v->op = (uint16_t)kind;
    struct nlist l = { 0 };
    do {
        const struct token dt = *TOK;
        struct node *d = mk(P, N_DECLARATOR, &dt);
        d->a = binding_target(P);
        if (kind != VK_VAR && d->a->type == N_IDENT && d->a->str == P->J->atom[A_let])
            error_at(P, &dt, "let is disallowed as a lexically bound name");
        if (accept(P, P_ASSIGN))
            d->b = assignment(P, no_in);
        else if (!in_for) {
            if (kind == VK_CONST)
                error_at(P, &dt, "Missing initializer in const declaration");
            if (d->a->type != N_IDENT)
                error_at(P, &dt, "Missing initializer in destructuring declaration");
        }
        push(&l, d);
    } while (accept(P, P_COMMA));
    v->list = l.v;
    v->n = l.n;
    return v;
}

static bool let_is_declaration(struct parser *P)
{
    if (!is_name(P, A_let))
        return false;
    struct token nt = peek(P);
    if (nt.type == T_IDENT || (nt.type == T_PUNCT && (nt.op == P_LBRACKET || nt.op == P_LBRACE)))
        return true;
    if (nt.type == T_KEYWORD && (nt.op == KW_ENUM))
        return true;
    return false;
}

static struct node *for_statement(struct parser *P)
{
    const struct token at = *TOK;
    next(P);
    if (is_name(P, A_await))
        error_at(P, TOK, "for await is not supported yet");
    expect(P, P_LPAREN);
    struct node *init = 0;
    const struct token it = *TOK;
    bool decl = false;
    if (is_p(P, P_SEMI)) {
    } else if (is_kw(P, KW_VAR) || is_kw(P, KW_CONST) || let_is_declaration(P)) {
        int kind = is_kw(P, KW_VAR) ? VK_VAR : is_kw(P, KW_CONST) ? VK_CONST : VK_LET;
        next(P);
        init = declarations(P, &it, kind, true, true);
        decl = true;
    } else
        init = expression(P, true);
    if (init && (is_kw(P, KW_IN) || is_name(P, A_of))) {
        bool of = is_name(P, A_of);
        if (decl) {
            if (init->n != 1)
                error_at(P, &it, "Only one variable may be declared in a for-in/of loop");
            struct node *d = init->list[0];
            if (d->b && !(!of && init->op == VK_VAR && !P->L->strict && d->a->type == N_IDENT))
                error_at(P, &it, "for-in/of loop variable declaration may not have an initializer");
        } else {
            if (of && init->type == N_IDENT && init->str == P->J->atom[A_async] && !(init->flags & NF_PARENS))
                error_at(P, &it, "The left-hand side of a for-of loop may not be 'async'");
            init = to_pattern(P, init, false);
        }
        next(P);
        struct node *f = mk(P, of ? N_FOR_OF : N_FOR_IN, &at);
        f->a = init;
        f->b = of ? assignment(P, false) : expression(P, false);
        expect(P, P_RPAREN);
        f->c = statement(P);
        return f;
    }
    if (decl)
        for (int i = 0; i < init->n; i++) {
            struct node *d = init->list[i];
            if (!d->b && (init->op == VK_CONST || d->a->type != N_IDENT))
                error_at(P, &it, "Missing initializer in declaration");
        }
    struct node *f = mk(P, N_FOR, &at);
    f->a = init;
    expect(P, P_SEMI);
    if (!is_p(P, P_SEMI))
        f->b = expression(P, false);
    expect(P, P_SEMI);
    if (!is_p(P, P_RPAREN))
        f->c = expression(P, false);
    expect(P, P_RPAREN);
    f->d = statement(P);
    return f;
}

static struct node *function_declaration(struct parser *P, bool async, const struct token *start)
{
    next(P);                                    /* function */
    uint32_t flags = async ? FN_ASYNC : 0;
    if (accept(P, P_STAR))
        flags |= FN_GENERATOR;
    struct jsstring *name = identifier(P);
    struct node *f = function_rest(P, start, flags | FN_DECLARATION, name);
    struct node *d = mk(P, N_FUNC_DECL, start);
    d->a = f;
    d->str = name;
    return d;
}

static struct node *statement_list_item(struct parser *P)
{
    const struct token at = *TOK;
    if (is_kw(P, KW_FUNCTION))
        return function_declaration(P, false, &at);
    if (is_name(P, A_async)) {
        struct token nt = peek(P);
        if (nt.type == T_KEYWORD && nt.op == KW_FUNCTION && !nt.nl_before) {
            next(P);
            return function_declaration(P, true, &at);
        }
    }
    if (is_kw(P, KW_CLASS)) {
        next(P);
        bool s = P->L->strict;
        P->L->strict = true;
        struct jsstring *name = identifier(P);
        P->L->strict = s;
        struct node *c = class_tail(P, &at, name, true);
        struct node *d = mk(P, N_CLASS_DECL, &at);
        d->a = c;
        d->str = name;
        return d;
    }
    if (is_kw(P, KW_CONST) || let_is_declaration(P)) {
        int kind = is_kw(P, KW_CONST) ? VK_CONST : VK_LET;
        next(P);
        struct node *v = declarations(P, &at, kind, false, false);
        semicolon(P);
        return v;
    }
    return statement(P);
}

static struct node *statement(struct parser *P)
{
    const struct token at = *TOK;
    struct node *s;
    enter(P);
    if (TOK->type == T_PUNCT) {
        switch (TOK->op) {
        case P_LBRACE:
            s = block(P);
            P->depth--;
            return s;
        case P_SEMI:
            next(P);
            P->depth--;
            return mk(P, N_EMPTY, &at);
        }
    }
    if (TOK->type == T_KEYWORD) {
        switch (TOK->op) {
        case KW_VAR:
            next(P);
            s = declarations(P, &at, VK_VAR, false, false);
            semicolon(P);
            break;
        case KW_IF:
            next(P);
            s = mk(P, N_IF, &at);
            expect(P, P_LPAREN);
            s->a = expression(P, false);
            expect(P, P_RPAREN);
            s->b = statement(P);
            if (is_kw(P, KW_ELSE)) {
                next(P);
                s->c = statement(P);
            }
            break;
        case KW_FOR:
            s = for_statement(P);
            break;
        case KW_WHILE:
            next(P);
            s = mk(P, N_WHILE, &at);
            expect(P, P_LPAREN);
            s->a = expression(P, false);
            expect(P, P_RPAREN);
            s->b = statement(P);
            break;
        case KW_DO:
            next(P);
            s = mk(P, N_DO_WHILE, &at);
            s->b = statement(P);
            if (!is_kw(P, KW_WHILE))
                unexpected(P);
            next(P);
            expect(P, P_LPAREN);
            s->a = expression(P, false);
            expect(P, P_RPAREN);
            accept(P, P_SEMI);
            break;
        case KW_CONTINUE: case KW_BREAK: {
            bool brk = TOK->op == KW_BREAK;
            next(P);
            s = mk(P, brk ? N_BREAK : N_CONTINUE, &at);
            if (TOK->type == T_IDENT && !TOK->nl_before)
                s->str = identifier(P);
            semicolon(P);
            break;
        }
        case KW_RETURN:
            if (!P->in_function || P->in_class_field || P->in_static_block)
                error_at(P, &at, "Return statements are only valid inside functions");
            next(P);
            s = mk(P, N_RETURN, &at);
            if (!is_p(P, P_SEMI) && !is_p(P, P_RBRACE) && TOK->type != T_EOF && !TOK->nl_before)
                s->a = expression(P, false);
            semicolon(P);
            break;
        case KW_THROW:
            next(P);
            if (TOK->nl_before)
                error_at(P, &at, "Illegal newline after throw");
            s = mk(P, N_THROW, &at);
            s->a = expression(P, false);
            semicolon(P);
            break;
        case KW_TRY:
            next(P);
            s = mk(P, N_TRY, &at);
            s->a = block(P);
            if (is_kw(P, KW_CATCH)) {
                next(P);
                if (accept(P, P_LPAREN)) {
                    s->b = binding_target(P);
                    expect(P, P_RPAREN);
                }
                s->c = block(P);
                s->flags |= NF_PREFIX;          /* has a catch clause */
            }
            if (is_kw(P, KW_FINALLY)) {
                next(P);
                s->d = block(P);
            }
            if (!(s->flags & NF_PREFIX) && !s->d)
                error_at(P, &at, "Missing catch or finally after try");
            break;
        case KW_SWITCH: {
            next(P);
            s = mk(P, N_SWITCH, &at);
            expect(P, P_LPAREN);
            s->a = expression(P, false);
            expect(P, P_RPAREN);
            expect(P, P_LBRACE);
            struct nlist cases = { 0 };
            bool has_default = false;
            while (!accept(P, P_RBRACE)) {
                const struct token ct = *TOK;
                struct node *c = mk(P, N_CASE, &ct);
                if (is_kw(P, KW_CASE)) {
                    next(P);
                    c->a = expression(P, false);
                } else if (is_kw(P, KW_DEFAULT)) {
                    if (has_default)
                        error_at(P, &ct, "More than one default clause in switch statement");
                    has_default = true;
                    next(P);
                } else
                    unexpected(P);
                expect(P, P_COLON);
                struct nlist body = { 0 };
                while (!is_kw(P, KW_CASE) && !is_kw(P, KW_DEFAULT) && !is_p(P, P_RBRACE)) {
                    if (TOK->type == T_EOF)
                        unexpected(P);
                    push(&body, statement_list_item(P));
                }
                c->list = body.v;
                c->n = body.n;
                push(&cases, c);
            }
            s->list = cases.v;
            s->n = cases.n;
            break;
        }
        case KW_DEBUGGER:
            next(P);
            semicolon(P);
            s = mk(P, N_DEBUGGER, &at);
            break;
        case KW_WITH:
            if (P->L->strict)
                error_at(P, &at, "Strict mode code may not include a with statement");
            error_at(P, &at, "with statements are not supported");
        case KW_FUNCTION:
            if (P->L->strict)
                error_at(P, &at, "In strict mode code, functions can only be declared at top level or inside a block");
            s = function_declaration(P, false, &at);    /* (Annex B: if (x) function f(){}) */
            break;
        case KW_CLASS:
            error_at(P, &at, "Unexpected keyword 'class'");
        case KW_IMPORT: case KW_EXPORT:
            error_at(P, &at, "Modules are not supported yet");
        default:
            goto expression_statement;
        }
        P->depth--;
        return s;
    }
    if (TOK->type == T_IDENT) {
        struct token nt = peek(P);
        if (nt.type == T_PUNCT && nt.op == P_COLON && !TOK->escaped) {
            s = mk(P, N_LABELED, &at);
            s->str = identifier(P);
            next(P);
            if (is_kw(P, KW_FUNCTION)) {
                if (P->L->strict)
                    error_at(P, TOK, "In strict mode code, functions can only be declared at top level or inside a block");
                s->a = function_declaration(P, false, TOK);
            } else
                s->a = statement(P);
            P->depth--;
            return s;
        }
        if (is_name(P, A_let) && nt.type == T_PUNCT && nt.op == P_LBRACKET)
            error_at(P, &at, "Lexical declaration cannot appear in a single-statement context");
    }
expression_statement:
    s = mk(P, N_EXPR_STMT, &at);
    s->a = expression(P, false);
    semicolon(P);
    P->depth--;
    return s;
}

struct node *parse_script(struct lexer *L, bool strict)
{
    struct parser P0 = { .L = L, .J = L->J }, *P = &P0;
    L->strict = strict;
    lex_next(L);
    struct node *s = mk(P, N_SCRIPT, TOK);
    bool st = strict;
    s->list = body_statements(P, &s->n, &st, true);
    if (st)
        s->flags |= 1;
    return s;
}

/* new Function(params, body): the text is "(params\n) {\nbody\n}" after the
 * keyword; kind 0 plain. */
struct node *parse_function_text(struct lexer *L, int kind)
{
    struct parser P0 = { .L = L, .J = L->J }, *P = &P0;
    (void)kind;
    lex_next(L);
    const struct token at = *TOK;
    if (!is_kw(P, KW_FUNCTION))
        unexpected(P);
    next(P);
    if (TOK->type == T_IDENT)
        next(P);                                /* anonymous */
    struct node *f = function_rest(P, &at, FN_EXPRESSION, L->J->atom[A_anonymous]);
    if (TOK->type != T_EOF)
        unexpected(P);
    return f;
}
