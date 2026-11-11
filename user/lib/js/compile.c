/*
 * The compiler (27o): a parsed script to compiled functions (struct funcdef).
 *
 * Two walks over the tree. The first (resolve) makes the scopes: each
 * function's parameters, `var`s and function declarations, each block's
 * `let`, `const`, `class` and function declarations, and binds every
 * identifier to its declaration, marking a binding "captured" when a
 * function inside uses it. `this`, `new.target`, the home object of
 * `super` and `arguments` are bindings too (made when first used), so an
 * arrow function reaches its parent's through the same scopes. Names a
 * script declares at its top level, and names declared nowhere, are the
 * global object's or the global lexical scope's, found by name at run time.
 *
 * The second walk (gen) writes the code. A binding no inner function uses
 * lives in a register of its function's frame; captured ones live in a
 * scope cell made when their block is entered (PUSH_SCOPE), reached by how
 * many scope cells lie between (hops). `finally` blocks are written out at
 * each way out of their `try` (an exception, a break, a return), as are the
 * closing of iterators and the popping of scope cells (the "actions").
 */
#include <math.h>
#include "jsparse.h"

static const signed char op_effect[] = {
#define OP(name, n, e) e,
#include "opcodes.h"
#undef OP
};

/* --- resolving ------------------------------------------------------------ */

struct script_decls {
    struct jsstring **vars;
    int nvars, cvars;
    struct node **funcs;
    int nfuncs, cfuncs;
    struct jsstring **lex;
    uint8_t *lex_const;
    int nlex, clex;
};

struct resolver {
    struct js *J;
    struct lexer *L;
    struct script_decls *script;
    int hidden;
};

static _Noreturn void node_error(struct resolver *R, const struct node *n, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void node_error(struct resolver *R, const struct node *n, const char *fmt, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    struct token t = { .line = n ? n->line : 0, .col = n ? n->col : 0 };
    lex_error(R->L, &t, "%s", buf);
}

static struct cscope *new_scope(struct cscope *parent, struct cfunc *fn, enum scope_kind kind)
{
    struct cscope *s = arena_new(sizeof *s);
    s->parent = parent;
    s->fn = fn;
    s->kind = kind;
    return s;
}

static struct binding *find_local(struct cscope *s, struct jsstring *name)
{
    for (int i = 0; i < s->nb; i++)
        if (s->b[i]->name == name)
            return s->b[i];
    return 0;
}

static bool is_lexical(int kind)
{
    return kind == B_LET || kind == B_CONST || kind == B_CLASS || kind == B_CLASS_INNER;
}

static struct binding *add_binding(struct cscope *s, struct jsstring *name, int kind)
{
    if (s->nb == s->cap) {
        int cap = s->cap ? s->cap * 2 : 8;
        struct binding **b = arena_new((size_t)cap * sizeof *b);
        if (s->nb)
            memcpy(b, s->b, (size_t)s->nb * sizeof *b);
        s->b = b;
        s->cap = cap;
    }
    struct binding *b = arena_new(sizeof *b);
    b->name = name;
    b->kind = (uint8_t)kind;
    b->scope = s;
    b->tdz = is_lexical(kind);
    s->b[s->nb++] = b;
    return b;
}

static struct binding *declare(struct resolver *R, struct cscope *s, struct jsstring *name, int kind, struct node *at)
{
    struct binding *b = find_local(s, name);
    if (b) {
        if (is_lexical(kind) || is_lexical(b->kind)) {
            char *n = str_to_utf8(R->J, name, 0);
            char msg[300];
            snprintf(msg, sizeof msg, "Cannot declare a %s variable twice: '%s'.",
                     kind == B_CONST ? "const" : kind == B_CLASS ? "class" : "let", n);
            free(n);
            node_error(R, at, "%s", msg);
        }
        if (kind == B_FUNCTION) {
            if (b->kind == B_VAR || b->kind == B_FUNCTION)
                b->kind = B_FUNCTION;
            b->func = at;
        }
        return b;
    }
    b = add_binding(s, name, kind);
    if (kind == B_FUNCTION)
        b->func = at;
    return b;
}

static void script_var(struct script_decls *d, struct jsstring *name)
{
    for (int i = 0; i < d->nvars; i++)
        if (d->vars[i] == name)
            return;
    if (d->nvars == d->cvars) {
        int cap = d->cvars ? d->cvars * 2 : 16;
        struct jsstring **v = arena_new((size_t)cap * sizeof *v);
        if (d->nvars)
            memcpy(v, d->vars, (size_t)d->nvars * sizeof *v);
        d->vars = v;
        d->cvars = cap;
    }
    d->vars[d->nvars++] = name;
}

static void script_func(struct script_decls *d, struct node *f)
{
    for (int i = 0; i < d->nfuncs; i++)
        if (d->funcs[i]->str == f->str) {
            d->funcs[i] = f;                    /* the last one wins */
            return;
        }
    if (d->nfuncs == d->cfuncs) {
        int cap = d->cfuncs ? d->cfuncs * 2 : 16;
        struct node **v = arena_new((size_t)cap * sizeof *v);
        if (d->nfuncs)
            memcpy(v, d->funcs, (size_t)d->nfuncs * sizeof *v);
        d->funcs = v;
        d->cfuncs = cap;
    }
    d->funcs[d->nfuncs++] = f;
}

static void script_lex(struct resolver *R, struct node *at, struct jsstring *name, bool is_const)
{
    struct script_decls *d = R->script;
    for (int i = 0; i < d->nlex; i++)
        if (d->lex[i] == name)
            node_error(R, at, "Cannot declare a let variable twice");
    for (int i = 0; i < d->nvars; i++)
        if (d->vars[i] == name)
            node_error(R, at, "Cannot declare a let variable twice");
    for (int i = 0; i < d->nfuncs; i++)
        if (d->funcs[i]->str == name)
            node_error(R, at, "Cannot declare a let variable twice");
    if (d->nlex == d->clex) {
        int cap = d->clex ? d->clex * 2 : 16;
        struct jsstring **v = arena_new((size_t)cap * sizeof *v);
        uint8_t *c = arena_new((size_t)cap);
        if (d->nlex) {
            memcpy(v, d->lex, (size_t)d->nlex * sizeof *v);
            memcpy(c, d->lex_const, (size_t)d->nlex);
        }
        d->lex = v;
        d->lex_const = c;
        d->clex = cap;
    }
    d->lex[d->nlex] = name;
    d->lex_const[d->nlex] = is_const;
    d->nlex++;
}

typedef void (*name_fn)(struct resolver *R, struct cscope *s, struct node *id, int kind);

static void walk_pattern_names(struct resolver *R, struct cscope *s, struct node *p, int kind, name_fn fn)
{
    if (!p)
        return;
    switch (p->type) {
    case N_IDENT:
        fn(R, s, p, kind);
        break;
    case N_ASSIGN_PATTERN:
        walk_pattern_names(R, s, p->a, kind, fn);
        break;
    case N_REST:
        walk_pattern_names(R, s, p->a, kind, fn);
        break;
    case N_ARRAY_PATTERN:
        for (int i = 0; i < p->n; i++)
            if (p->list[i]->type != N_HOLE)
                walk_pattern_names(R, s, p->list[i], kind, fn);
        break;
    case N_OBJECT_PATTERN:
        for (int i = 0; i < p->n; i++) {
            struct node *x = p->list[i];
            walk_pattern_names(R, s, x->type == N_REST ? x->a : x->b, kind, fn);
        }
        break;
    }
}

static void declare_name(struct resolver *R, struct cscope *s, struct node *id, int kind)
{
    if (s->kind == S_SCRIPT) {
        if (kind == B_VAR)
            script_var(R->script, id->str);
        else
            script_lex(R, id, id->str, kind == B_CONST);
        return;
    }
    struct binding *b = declare(R, s, id->str, kind, id);
    if (kind == B_VAR || kind == B_PARAM) {
        /* a var named like a lexical binding of the same function scope */
        if (is_lexical(b->kind))
            node_error(R, id, "Cannot declare a var variable that shadows a let/const/class variable");
    }
}

static void hoist(struct resolver *R, struct node *n, struct cscope *fs, bool top);

static void hoist_list(struct resolver *R, struct node **l, int n, struct cscope *fs, bool top)
{
    for (int i = 0; i < n; i++)
        hoist(R, l[i], fs, top);
}

/* var declarations and function declarations, into the function scope fs. */
static void hoist(struct resolver *R, struct node *n, struct cscope *fs, bool top)
{
    if (!n)
        return;
    switch (n->type) {
    case N_VAR:
        if (n->op == VK_VAR)
            for (int i = 0; i < n->n; i++)
                walk_pattern_names(R, fs, n->list[i]->a, B_VAR, declare_name);
        break;
    case N_FOR:
        hoist(R, n->a, fs, false);
        hoist(R, n->d, fs, false);
        break;
    case N_FOR_IN: case N_FOR_OF:
        hoist(R, n->a, fs, false);
        hoist(R, n->c, fs, false);
        break;
    case N_BLOCK:
        hoist_list(R, n->list, n->n, fs, false);
        break;
    case N_IF:
        hoist(R, n->b, fs, false);
        hoist(R, n->c, fs, false);
        break;
    case N_WHILE: case N_DO_WHILE:
        hoist(R, n->b, fs, false);
        break;
    case N_TRY:
        hoist(R, n->a, fs, false);
        hoist(R, n->c, fs, false);
        hoist(R, n->d, fs, false);
        break;
    case N_SWITCH:
        for (int i = 0; i < n->n; i++)
            hoist_list(R, n->list[i]->list, n->list[i]->n, fs, false);
        break;
    case N_LABELED:
        hoist(R, n->a, fs, top);
        break;
    case N_FUNC_DECL:
        if (top) {
            if (fs->kind == S_SCRIPT)
                script_func(R->script, n);
            else
                declare(R, fs, n->str, B_FUNCTION, n);
        } else if (!(n->a->fn->flags & FN_STRICT) && !R->L->strict && !(n->a->fn->flags & (FN_ASYNC | FN_GENERATOR))) {
            /* Annex B: a function in a block is also a var of its function */
            if (fs->kind == S_SCRIPT) {
                bool lex = false;
                for (int i = 0; i < R->script->nlex; i++)
                    if (R->script->lex[i] == n->str)
                        lex = true;
                if (!lex) {
                    script_var(R->script, n->str);
                    n->flags |= NF_STATIC;      /* (marks "copy to the global at the statement") */
                }
            } else {
                struct binding *b = find_local(fs, n->str);
                if (!b || !is_lexical(b->kind)) {
                    if (!b)
                        b = add_binding(fs, n->str, B_VAR);
                    n->binding2 = b;
                }
            }
        }
        break;
    }
}

/* let, const, class and (in blocks) function declarations of a statement list. */
static void declare_lexical(struct resolver *R, struct node **l, int n, struct cscope *s, bool function_top)
{
    for (int i = 0; i < n; i++) {
        struct node *x = l[i];
        while (x->type == N_LABELED && x->a && x->a->type == N_FUNC_DECL)
            x = x->a;
        if (x->type == N_VAR && x->op != VK_VAR)
            for (int k = 0; k < x->n; k++)
                walk_pattern_names(R, s, x->list[k]->a, x->op == VK_CONST ? B_CONST : B_LET, declare_name);
        else if (x->type == N_CLASS_DECL) {
            struct node id = *x;
            id.type = N_IDENT;
            declare_name(R, s, &id, B_CLASS);
        } else if (x->type == N_FUNC_DECL && !function_top) {
            if (s->kind == S_SCRIPT)
                continue;
            struct binding *b = find_local(s, x->str);
            if (b && b->kind == B_FUNCTION) {
                if (R->L->strict)
                    node_error(R, x, "Cannot declare a function twice in a block in strict mode");
                b->func = x;
            } else
                declare(R, s, x->str, B_FUNCTION, x);
        }
    }
}

static void resolve(struct resolver *R, struct node *n, struct cscope *s);
static void resolve_function(struct resolver *R, struct node *f, struct cscope *outer);

static void resolve_list(struct resolver *R, struct node **l, int n, struct cscope *s)
{
    for (int i = 0; i < n; i++)
        resolve(R, l[i], s);
}

static void resolve_ident(struct resolver *R, struct node *n, struct cscope *s)
{
    struct cfunc *fn = s->fn;
    for (struct cscope *c = s; c; c = c->parent) {
        struct binding *b = find_local(c, n->str);
        if (b) {
            if (c->fn != fn)
                b->captured = true;
            n->binding = b;
            return;
        }
        if (c->kind == S_FUNCTION && n->str == R->J->atom[A_arguments] &&
            !(c->fn->flags & (FN_ARROW | FN_FIELD_INIT | FN_STATIC_BLOCK))) {
            b = add_binding(c, n->str, B_ARGUMENTS);
            b->tdz = false;
            c->fn->uses_arguments = true;
            if (c->fn != fn)
                b->captured = true;
            n->binding = b;
            return;
        }
    }
    n->binding = 0;
}

/* this, new.target, the home object and the active function: bindings of the
 * nearest function that is not an arrow (0 at the top of a script). */
static struct binding *special(struct resolver *R, struct cscope *s, int kind, int atom_name)
{
    struct cfunc *fn = s->fn;
    for (struct cscope *c = s; c; c = c->parent) {
        if (c->kind == S_FUNCTION && !(c->fn->flags & FN_ARROW)) {
            struct binding *b = find_local(c, R->J->atom[atom_name]);
            if (!b) {
                b = add_binding(c, R->J->atom[atom_name], kind);
                b->tdz = kind == B_THIS && (c->fn->flags & FN_DERIVED);
            }
            if (c->fn != fn)
                b->captured = true;
            return b;
        }
    }
    return 0;
}

static struct jsstring *hidden_name(struct resolver *R, const char *prefix)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%%%s%d", prefix, ++R->hidden);
    return atom_cstr(R->J, buf);
}

static struct jsstring *private_atom(struct resolver *R, struct jsstring *name)
{
    struct sbuf b = { 0 };
    sb_unit(&b, '#');
    sb_str(R->J, &b, name);
    struct jsstring *a = atom_u16(R->J, b.s, b.n);
    sb_free(&b);
    return a;
}

static void resolve_private(struct resolver *R, struct node *n, struct cscope *s)
{
    struct jsstring *name = private_atom(R, n->str);
    struct node tmp = *n;
    tmp.str = name;
    resolve_ident(R, &tmp, s);
    if (!tmp.binding) {
        char *t = str_to_utf8(R->J, name, 0);
        char msg[300];
        snprintf(msg, sizeof msg, "Cannot reference undeclared private names: \"%s\"", t);
        free(t);
        node_error(R, n, "%s", msg);
    }
    n->binding = tmp.binding;
}

static struct cfunc *synthetic_function(struct cscope *outer, uint32_t flags)
{
    struct cfunc *fn = arena_new(sizeof *fn);
    fn->flags = flags | FN_METHOD | FN_STRICT | FN_SIMPLE_PARAMS;
    fn->scope = new_scope(outer, fn, S_FUNCTION);
    fn->parent = outer->fn;
    return fn;
}

static void resolve_class(struct resolver *R, struct node *c, struct cscope *outer)
{
    struct cscope *s = new_scope(outer, outer->fn, S_CLASS);
    c->scope = s;
    if (c->str)
        add_binding(s, c->str, B_CLASS_INNER);
    for (int i = 0; i < c->n; i++) {
        struct node *m = c->list[i];
        if (m->type == N_CLASS_MEMBER && m->a && m->a->type == N_PRIVATE_MEMBER) {
            struct jsstring *pn = private_atom(R, m->a->str);
            struct binding *b = find_local(s, pn);
            if (b) {
                /* a getter and setter pair may share a name; nothing else */
                if (!((m->op == PK_GET || m->op == PK_SET) && b->func && b->func->op != m->op &&
                      (b->func->op == PK_GET || b->func->op == PK_SET) &&
                      ((b->func->flags ^ m->flags) & NF_STATIC) == 0))
                    node_error(R, m, "Cannot declare a private name twice");
            } else {
                b = add_binding(s, pn, B_CONST);
                b->tdz = false;
                b->func = m;
            }
            m->a->binding = b;
        }
    }
    if (c->a)
        resolve(R, c->a, s);
    struct cfunc *fields = synthetic_function(s, FN_FIELD_INIT);
    struct cfunc *statics = synthetic_function(s, FN_FIELD_INIT | FN_STATIC_BLOCK);
    c->fn = fields;
    c->fn2 = statics;
    for (int i = 0; i < c->n; i++) {
        struct node *m = c->list[i];
        bool is_static = m->flags & NF_STATIC;
        if (m->type == N_STATIC_BLOCK) {
            struct cscope *bs = new_scope(statics->scope, statics, S_BLOCK);
            m->scope = bs;
            hoist_list(R, m->b->fn->body, m->b->fn->nbody, statics->scope, true);
            declare_lexical(R, m->b->fn->body, m->b->fn->nbody, bs, false);
            resolve_list(R, m->b->fn->body, m->b->fn->nbody, bs);
            continue;
        }
        if (m->flags & NF_COMPUTED) {
            resolve(R, m->a, s);
            if (m->op == PK_FIELD) {
                struct binding *k = add_binding(s, hidden_name(R, "fk"), B_CONST);
                k->tdz = false;
                k->captured = true;             /* the initializer function reads it */
                m->binding = k;
            }
        }
        if (m->op == PK_FIELD) {
            if (m->b)
                resolve(R, m->b->fn->expr, is_static ? statics->scope : fields->scope);
        } else {
            resolve_function(R, m->b, s);
            if (m->a->type == N_PRIVATE_MEMBER && !is_static) {
                struct binding *k = add_binding(s, hidden_name(R, "pm"), B_CONST);
                k->tdz = false;
                k->captured = true;
                m->binding2 = k;
            }
        }
    }
    if (c->b)
        resolve_function(R, c->b, s);
}

static void resolve_pattern(struct resolver *R, struct node *p, struct cscope *s)
{
    if (!p)
        return;
    switch (p->type) {
    case N_IDENT:
        resolve_ident(R, p, s);
        break;
    case N_ASSIGN_PATTERN:
        resolve_pattern(R, p->a, s);
        resolve(R, p->b, s);
        break;
    case N_REST:
        resolve_pattern(R, p->a, s);
        break;
    case N_ARRAY_PATTERN:
        for (int i = 0; i < p->n; i++)
            resolve_pattern(R, p->list[i], s);
        break;
    case N_OBJECT_PATTERN:
        for (int i = 0; i < p->n; i++) {
            struct node *x = p->list[i];
            if (x->type == N_REST)
                resolve_pattern(R, x->a, s);
            else {
                if (x->flags & NF_COMPUTED)
                    resolve(R, x->a, s);
                resolve_pattern(R, x->b, s);
            }
        }
        break;
    default:
        resolve(R, p, s);                       /* member targets */
    }
}

static void resolve_function(struct resolver *R, struct node *f, struct cscope *outer)
{
    struct cfunc *fn = f->fn;
    struct cscope *s = new_scope(outer, fn, S_FUNCTION);
    fn->scope = s;
    fn->parent = outer ? outer->fn : 0;
    bool saved = R->L->strict;
    R->L->strict = (fn->flags & FN_STRICT) != 0;
    for (int i = 0; i < fn->nparams; i++)
        walk_pattern_names(R, s, fn->params[i], B_PARAM, declare_name);
    hoist_list(R, fn->body, fn->nbody, s, true);
    if ((fn->flags & FN_HAS_NAME_BINDING) && fn->name && !find_local(s, fn->name)) {
        struct binding *b = add_binding(s, fn->name, B_FUNC_NAME);
        b->tdz = false;
    }
    if (fn->flags & FN_CLASS_CTOR) {
        special(R, s, B_THIS, A_this);
        special(R, s, B_HOME, A_func);
    }
    declare_lexical(R, fn->body, fn->nbody, s, true);
    for (int i = 0; i < fn->nparams; i++)
        resolve_pattern(R, fn->params[i], s);
    if (fn->expr)
        resolve(R, fn->expr, s);
    else
        resolve_list(R, fn->body, fn->nbody, s);
    R->L->strict = saved;
}

static void resolve(struct resolver *R, struct node *n, struct cscope *s)
{
    if (!n)
        return;
    switch (n->type) {
    case N_IDENT:
        resolve_ident(R, n, s);
        return;
    case N_THIS:
        n->binding = special(R, s, B_THIS, A_this);
        return;
    case N_NEW_TARGET:
        n->binding = special(R, s, B_NEW_TARGET, A_new_target);
        return;
    case N_SUPER_MEMBER: case N_SUPER_INDEX:
        n->binding = special(R, s, B_HOME, A_home);
        n->binding2 = special(R, s, B_THIS, A_this);
        resolve(R, n->b, s);
        return;
    case N_SUPER_CALL:
        n->binding = special(R, s, B_HOME, A_func);
        n->binding2 = special(R, s, B_THIS, A_this);
        n->binding3 = special(R, s, B_NEW_TARGET, A_new_target);
        resolve_list(R, n->list, n->n, s);
        return;
    case N_FUNCTION:
        resolve_function(R, n, s);
        return;
    case N_CLASS:
        resolve_class(R, n, s);
        return;
    case N_PRIVATE_MEMBER:
        resolve(R, n->a, s);
        resolve_private(R, n, s);
        return;
    case N_PRIVATE_IN:
        resolve_private(R, n, s);
        return;
    case N_BLOCK: {
        struct cscope *b = new_scope(s, s->fn, S_BLOCK);
        n->scope = b;
        declare_lexical(R, n->list, n->n, b, false);
        resolve_list(R, n->list, n->n, b);
        return;
    }
    case N_VAR:
        for (int i = 0; i < n->n; i++) {
            resolve_pattern(R, n->list[i]->a, s);
            resolve(R, n->list[i]->b, s);
        }
        return;
    case N_FOR: {
        struct cscope *f = s;
        if (n->a && n->a->type == N_VAR && n->a->op != VK_VAR) {
            f = new_scope(s, s->fn, S_FOR);
            n->scope = f;
            declare_lexical(R, &n->a, 1, f, false);
        }
        resolve(R, n->a, f);
        resolve(R, n->b, f);
        resolve(R, n->c, f);
        resolve(R, n->d, f);
        return;
    }
    case N_FOR_IN: case N_FOR_OF: {
        resolve(R, n->b, s);
        struct cscope *f = s;
        if (n->a->type == N_VAR && n->a->op != VK_VAR) {
            f = new_scope(s, s->fn, S_FOR);
            n->scope = f;
            declare_lexical(R, &n->a, 1, f, false);
        }
        if (n->a->type == N_VAR) {
            resolve_pattern(R, n->a->list[0]->a, f);
            resolve(R, n->a->list[0]->b, f);
        } else
            resolve_pattern(R, n->a, f);
        resolve(R, n->c, f);
        return;
    }
    case N_TRY:
        resolve(R, n->a, s);
        if (n->flags & NF_PREFIX) {
            struct cscope *c = new_scope(s, s->fn, S_CATCH);
            n->scope = c;
            walk_pattern_names(R, c, n->b, B_CATCH, declare_name);
            resolve_pattern(R, n->b, c);
            resolve(R, n->c, c);
        }
        resolve(R, n->d, s);
        return;
    case N_SWITCH: {
        resolve(R, n->a, s);
        struct cscope *b = new_scope(s, s->fn, S_SWITCH);
        n->scope = b;
        for (int i = 0; i < n->n; i++)
            declare_lexical(R, n->list[i]->list, n->list[i]->n, b, false);
        for (int i = 0; i < n->n; i++) {
            resolve(R, n->list[i]->a, b);
            resolve_list(R, n->list[i]->list, n->list[i]->n, b);
        }
        return;
    }
    case N_FUNC_DECL: {
        resolve_function(R, n->a, s);
        struct node tmp = { .type = N_IDENT, .str = n->str };
        resolve_ident(R, &tmp, s);
        n->binding = tmp.binding;
        return;
    }
    case N_CLASS_DECL: {
        resolve_class(R, n->a, s);
        struct node tmp = { .type = N_IDENT, .str = n->str };
        resolve_ident(R, &tmp, s);
        n->binding = tmp.binding;
        return;
    }
    case N_ASSIGN:
        resolve_pattern(R, n->a, s);
        resolve(R, n->b, s);
        return;
    case N_OBJECT:
        for (int i = 0; i < n->n; i++) {
            struct node *x = n->list[i];
            if (x->flags & NF_COVER_INIT)
                node_error(R, x, "Invalid shorthand property initializer");
            if (x->flags & NF_COMPUTED)
                resolve(R, x->a, s);
            resolve(R, x->b, s);
        }
        return;
    case N_ARRAY_PATTERN: case N_OBJECT_PATTERN: case N_ASSIGN_PATTERN: case N_REST:
        resolve_pattern(R, n, s);
        return;
    case N_MEMBER:
        resolve(R, n->a, s);
        return;
    case N_TEMPLATE: case N_TAGGED:
        resolve(R, n->a, s);
        resolve_list(R, n->list, n->n, s);
        return;
    case N_LABELED:
        resolve(R, n->a, s);
        return;
    case N_STR: case N_NUM: case N_REGEXP: case N_NULL: case N_TRUE: case N_FALSE: case N_HOLE:
    case N_EMPTY: case N_DEBUGGER: case N_BREAK: case N_CONTINUE:
        return;
    }
    resolve(R, n->a, s);
    resolve(R, n->b, s);
    resolve(R, n->c, s);
    resolve(R, n->d, s);
    resolve_list(R, n->list, n->n, s);
}

/* --- generating code -------------------------------------------------- */

enum { ACT_SCOPE, ACT_TRY, ACT_FINALLY, ACT_ITER, ACT_POP };

struct action {
    int kind;
    struct node *fin;                           /* ACT_FINALLY */
    struct cscope *scope;                       /* ACT_FINALLY: the scope the finally block runs in */
};

struct patches {
    uint32_t *at;
    int n, cap;
};

struct target {
    struct target *prev;
    struct jsstring *labels[8];
    int nlabels;
    bool loop;                                  /* takes continue */
    bool plain_break;                           /* takes an unlabeled break (loops, switch) */
    int actions;                                /* actions below it */
    int sp;                                     /* the stack's depth at the break label */
    int cont_sp;
    struct patches breaks, conts;
};

struct gen {
    struct js *J;
    struct resolver *R;
    struct cfunc *fn;
    struct gen *parent;
    uint8_t *code;
    uint32_t len, cap;
    jsval *consts;
    uint32_t nconsts, cconsts;
    struct linepc *lines;
    uint32_t nlines, clines;
    int last_line;
    int nregs;
    int sp, max_sp;
    struct cscope *scope;
    struct target *targets;
    struct action *actions;
    int nactions, cactions;
    struct patches *chain;                      /* the innermost ?. chain's jumps */
    struct jsstring *pending[8];                /* labels for the next statement */
    int npending;
    int completion;                             /* scripts: the register of the last value, else -1 */
    struct jsstring *file, *source;
    bool strict;
    int stmt_sp;                                /* (27r) the stack's depth where the running statement began */
};

static void grow(struct gen *g, uint32_t more)
{
    if (g->len + more <= g->cap)
        return;
    uint32_t cap = g->cap ? g->cap * 2 : 256;
    while (cap < g->len + more)
        cap *= 2;
    g->code = realloc(g->code, cap);
    if (!g->code)
        abort();
    g->cap = cap;
}

static void emit1(struct gen *g, unsigned b)
{
    grow(g, 1);
    g->code[g->len++] = (uint8_t)b;
}

static void emit16(struct gen *g, unsigned v)
{
    grow(g, 2);
    g->code[g->len++] = (uint8_t)v;
    g->code[g->len++] = (uint8_t)(v >> 8);
}

static void emit32(struct gen *g, uint32_t v)
{
    grow(g, 4);
    memcpy(g->code + g->len, &v, 4);
    g->len += 4;
}

static void adjust(struct gen *g, int delta)
{
    g->sp += delta;
    if (g->sp > g->max_sp)
        g->max_sp = g->sp;
}

static void op(struct gen *g, int o)
{
    emit1(g, (unsigned)o);
    if (op_effect[o] != 99)
        adjust(g, op_effect[o]);
}

static void op16(struct gen *g, int o, unsigned v)
{
    op(g, o);
    emit16(g, v);
}

static void op32(struct gen *g, int o, uint32_t v)
{
    op(g, o);
    emit32(g, v);
}

static uint32_t add_const(struct gen *g, jsval v)
{
    for (uint32_t i = g->nconsts; i-- > 0 && i + 64 >= g->nconsts;)
        if (g->consts[i] == v)
            return i;
    if (g->nconsts == g->cconsts) {
        uint32_t cap = g->cconsts ? g->cconsts * 2 : 16;
        g->consts = realloc(g->consts, cap * sizeof *g->consts);
        if (!g->consts)
            abort();
        g->cconsts = cap;
    }
    g->consts[g->nconsts] = v;
    return g->nconsts++;
}

static uint32_t new_const(struct gen *g, jsval v)
{
    if (g->nconsts == g->cconsts) {
        uint32_t cap = g->cconsts ? g->cconsts * 2 : 16;
        g->consts = realloc(g->consts, cap * sizeof *g->consts);
        if (!g->consts)
            abort();
        g->cconsts = cap;
    }
    g->consts[g->nconsts] = v;
    return g->nconsts++;
}

static uint32_t atom_const(struct gen *g, struct jsstring *a)
{
    return add_const(g, str_val(a));
}

static void op_atom(struct gen *g, int o, struct jsstring *a)
{
    op32(g, o, atom_const(g, a));
}

static uint32_t jump(struct gen *g, int o)
{
    op(g, o);
    uint32_t at = g->len;
    emit32(g, 0);
    return at;
}

static void patch_to(struct gen *g, uint32_t at, uint32_t target)
{
    int32_t off = (int32_t)(target - (at + 4));
    memcpy(g->code + at, &off, 4);
}

static void patch(struct gen *g, uint32_t at)
{
    patch_to(g, at, g->len);
}

static void jump_to(struct gen *g, int o, uint32_t target)
{
    op(g, o);
    int32_t off = (int32_t)(target - (g->len + 4));
    emit32(g, (uint32_t)off);
}

static void add_patch(struct patches *p, uint32_t at)
{
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 8;
        uint32_t *v = arena_new((size_t)cap * sizeof *v);
        if (p->n)
            memcpy(v, p->at, (size_t)p->n * sizeof *v);
        p->at = v;
        p->cap = cap;
    }
    p->at[p->n++] = at;
}

static void line(struct gen *g, const struct node *n)
{
    if (!n || n->line == g->last_line || n->line == 0)
        return;
    g->last_line = n->line;
    if (g->nlines == g->clines) {
        uint32_t cap = g->clines ? g->clines * 2 : 16;
        g->lines = realloc(g->lines, cap * sizeof *g->lines);
        if (!g->lines)
            abort();
        g->clines = cap;
    }
    g->lines[g->nlines].pc = g->len;
    g->lines[g->nlines].line = (uint32_t)n->line;
    g->nlines++;
}

static int new_reg(struct gen *g)
{
    if (g->nregs >= 65000)
        node_error(g->R, 0, "Too many variables in one function");
    return g->nregs++;
}

static void push_action(struct gen *g, int kind, struct node *fin)
{
    if (g->nactions == g->cactions) {
        int cap = g->cactions ? g->cactions * 2 : 16;
        struct action *a = arena_new((size_t)cap * sizeof *a);
        if (g->nactions)
            memcpy(a, g->actions, (size_t)g->nactions * sizeof *a);
        g->actions = a;
        g->cactions = cap;
    }
    g->actions[g->nactions].kind = kind;
    g->actions[g->nactions].fin = fin;
    g->actions[g->nactions].scope = g->scope;
    g->nactions++;
}

static void gen_stmt(struct gen *g, struct node *n);
static void gen_expr(struct gen *g, struct node *n);
static void gen_effect(struct gen *g, struct node *n);
static void gen_yield(struct gen *g, struct node *n);
static struct funcdef *gen_function(struct gen *parent, struct cfunc *fn, struct node *class_node);

/* Writes the actions above `down_to` (innermost first), for leaving them. */
static void emit_actions(struct gen *g, int down_to)
{
    for (int i = g->nactions - 1; i >= down_to; i--) {
        struct action *a = &g->actions[i];
        switch (a->kind) {
        case ACT_SCOPE:
            op(g, OP_POP_SCOPE);
            break;
        case ACT_TRY:
            op(g, OP_END_TRY);
            break;
        case ACT_ITER:
            op(g, OP_ITER_CLOSE);
            break;
        case ACT_POP:
            op(g, OP_POP);
            break;
        case ACT_FINALLY: {
            op(g, OP_END_TRY);
            int saved_n = g->nactions;
            struct cscope *saved_scope = g->scope;
            struct target *saved_t = g->targets;
            g->nactions = i;                    /* the finally block runs outside its own try */
            g->scope = a->scope;
            gen_stmt(g, a->fin);
            g->nactions = saved_n;
            g->scope = saved_scope;
            g->targets = saved_t;
            break;
        }
        }
    }
}

/* hops to the scope cell of binding b from the current scope */
static unsigned hops_to(struct gen *g, struct cscope *target)
{
    unsigned h = 0;
    for (struct cscope *s = g->scope; s && s != target; s = s->parent)
        if (s->heap)
            h++;
    return h;
}

static void get_binding(struct gen *g, struct binding *b, struct jsstring *name)
{
    if (!b) {
        op_atom(g, OP_GET_GLOBAL, name);
        return;
    }
    if (b->in_heap) {
        unsigned h = hops_to(g, b->scope);
        op(g, b->tdz ? OP_GET_SCOPE_CHECK : OP_GET_SCOPE);
        emit16(g, h);
        emit16(g, (unsigned)b->slot);
        if (b->tdz)
            emit32(g, atom_const(g, b->name));
    } else {
        op(g, b->tdz ? OP_GET_REG_CHECK : OP_GET_REG);
        emit16(g, (unsigned)b->slot);
        if (b->tdz)
            emit32(g, atom_const(g, b->name));
    }
}

/* the binding's value without the initialized check */
static void get_binding_raw(struct gen *g, struct binding *b)
{
    if (b->in_heap) {
        op(g, OP_GET_SCOPE);
        emit16(g, hops_to(g, b->scope));
        emit16(g, (unsigned)b->slot);
    } else
        op16(g, OP_GET_REG, (unsigned)b->slot);
}

enum { STORE_ASSIGN, STORE_INIT, STORE_INIT_LEX };

/* Stores the top of the stack into b; `keep` leaves the value there. */
static void set_binding(struct gen *g, struct binding *b, struct jsstring *name, int mode, bool keep)
{
    if (!b) {
        if (mode == STORE_INIT_LEX) {
            if (keep)
                op(g, OP_DUP);
            op_atom(g, OP_INIT_GLOBAL_LEX, name);
            return;
        }
        op(g, OP_SET_GLOBAL);
        emit32(g, atom_const(g, name));
        emit1(g, g->strict);
        if (!keep)
            op(g, OP_POP);
        return;
    }
    if (mode == STORE_ASSIGN && (b->kind == B_CONST || b->kind == B_CLASS_INNER)) {
        if (b->tdz)
            get_binding(g, b, name), op(g, OP_POP);   /* (TDZ first: "before initialization") */
        op_atom(g, OP_THROW_CONST, b->name);
        if (!keep)
            op(g, OP_POP);
        return;
    }
    if (mode == STORE_ASSIGN && b->kind == B_FUNC_NAME) {
        if (g->strict)
            op_atom(g, OP_THROW_CONST, b->name);
        if (!keep)
            op(g, OP_POP);
        return;
    }
    bool check = mode == STORE_ASSIGN && b->tdz;
    if (b->in_heap) {
        unsigned h = hops_to(g, b->scope);
        if (check) {
            op(g, OP_SET_SCOPE_CHECK);
            emit16(g, h);
            emit16(g, (unsigned)b->slot);
            emit32(g, atom_const(g, b->name));
            if (!keep)
                op(g, OP_POP);
        } else {
            op(g, keep ? OP_SET_SCOPE : OP_PUT_SCOPE);
            emit16(g, h);
            emit16(g, (unsigned)b->slot);
        }
    } else {
        if (check) {
            op(g, OP_SET_REG_CHECK);
            emit16(g, (unsigned)b->slot);
            emit32(g, atom_const(g, b->name));
            if (!keep)
                op(g, OP_POP);
        } else
            op16(g, keep ? OP_SET_REG : OP_PUT_REG, (unsigned)b->slot);
    }
}

static void alloc_scope(struct gen *g, struct cscope *s)
{
    s->heap = false;
    s->heap_size = 0;
    for (int i = 0; i < s->nb; i++)
        if (s->b[i]->captured)
            s->heap = true;
    for (int i = 0; i < s->nb; i++) {
        struct binding *b = s->b[i];
        if (b->captured) {
            b->in_heap = true;
            b->slot = s->heap_size++;
        } else {
            b->in_heap = false;
            b->slot = new_reg(g);
        }
    }
}

static void init_lexicals(struct gen *g, struct cscope *s)
{
    for (int i = 0; i < s->nb; i++) {
        struct binding *b = s->b[i];
        if (b->tdz) {
            op(g, OP_EMPTY);
            set_binding(g, b, b->name, STORE_INIT, false);
        }
    }
}

static void closure(struct gen *g, struct node *f)
{
    struct funcdef *d = gen_function(g, f->fn, 0);
    op32(g, OP_CLOSURE, new_const(g, cell_val(d)));
}

/* Block-level function declarations: made when the block is entered. */
static void init_block_functions(struct gen *g, struct cscope *s)
{
    for (int i = 0; i < s->nb; i++) {
        struct binding *b = s->b[i];
        if (b->kind == B_FUNCTION && b->func) {
            closure(g, b->func->a);
            set_binding(g, b, b->name, STORE_INIT, false);
        }
    }
}

static void enter_scope(struct gen *g, struct cscope *s)
{
    alloc_scope(g, s);
    g->scope = s;
    if (s->heap) {
        op16(g, OP_PUSH_SCOPE, (unsigned)s->heap_size);
        push_action(g, ACT_SCOPE, 0);
    }
    init_lexicals(g, s);
    init_block_functions(g, s);
}

static void leave_scope(struct gen *g, struct cscope *s)
{
    if (s->heap) {
        op(g, OP_POP_SCOPE);
        g->nactions--;
    }
    g->scope = s->parent;
}

/* --- expressions ------------------------------------------------------- */

static bool is_anonymous_fn(struct node *n)
{
    if (n->type == N_FUNCTION)
        return !n->fn->name;
    if (n->type == N_CLASS)
        return !n->str;
    return false;
}

static void gen_class(struct gen *g, struct node *c, struct jsstring *name);

/* An expression whose function or class gets `name` if it has none. */
static void gen_named(struct gen *g, struct node *n, struct jsstring *name)
{
    if (n->type == N_FUNCTION && !n->fn->name && !(n->flags & NF_PARENS)) {
        n->fn->name = name;
        gen_expr(g, n);
        return;
    }
    if (n->type == N_CLASS && !n->str && !(n->flags & NF_PARENS)) {
        gen_class(g, n, name);
        return;
    }
    gen_expr(g, n);
}

static void gen_args(struct gen *g, struct node **args, int n, bool *spread)
{
    *spread = false;
    for (int i = 0; i < n; i++)
        if (args[i]->type == N_SPREAD)
            *spread = true;
    if (*spread) {
        op(g, OP_ARRAY);
        for (int i = 0; i < n; i++) {
            if (args[i]->type == N_SPREAD) {
                gen_expr(g, args[i]->a);
                op(g, OP_ARRAY_SPREAD);
            } else {
                gen_expr(g, args[i]);
                op(g, OP_ARRAY_APPEND);
            }
        }
        return;
    }
    for (int i = 0; i < n; i++)
        gen_expr(g, args[i]);
}

static void call_op(struct gen *g, int o, int argc)
{
    op16(g, o, (unsigned)argc);
    adjust(g, -(argc + 1));
}

static void gen_super_base(struct gen *g, struct node *n)
{
    /* home this */
    if (!n->binding)
        node_error(g->R, n, "'super' keyword unexpected here");
    get_binding_raw(g, n->binding);
    get_binding(g, n->binding2, g->J->atom[A_this]);
}

/* The callee as `f this` for a call. */
static void gen_callee(struct gen *g, struct node *c)
{
    switch (c->type) {
    case N_MEMBER:
        gen_expr(g, c->a);
        if (c->flags & NF_OPTIONAL)
            add_patch(g->chain, jump(g, OP_JMP_NULLISH_UNDEF));
        op_atom(g, OP_GET_METHOD, c->str);
        return;
    case N_INDEX:
        gen_expr(g, c->a);
        if (c->flags & NF_OPTIONAL)
            add_patch(g->chain, jump(g, OP_JMP_NULLISH_UNDEF));
        gen_expr(g, c->b);
        op(g, OP_GET_METHOD_ELEM);
        return;
    case N_PRIVATE_MEMBER:
        gen_expr(g, c->a);
        if (c->flags & NF_OPTIONAL)
            add_patch(g->chain, jump(g, OP_JMP_NULLISH_UNDEF));
        op(g, OP_DUP);
        get_binding_raw(g, c->binding);
        op(g, OP_GET_PRIVATE);
        op(g, OP_SWAP);
        return;
    case N_SUPER_MEMBER:
        gen_super_base(g, c);
        op_atom(g, OP_GET_SUPER, c->str);
        get_binding(g, c->binding2, g->J->atom[A_this]);
        return;
    case N_SUPER_INDEX:
        gen_super_base(g, c);
        gen_expr(g, c->b);
        op(g, OP_GET_SUPER_ELEM);
        get_binding(g, c->binding2, g->J->atom[A_this]);
        return;
    }
    gen_expr(g, c);
    op(g, OP_UNDEF);
}

static void gen_call(struct gen *g, struct node *n)
{
    struct node *c = n->a;
    if (c->type == N_CHAIN) {
        /* (a?.b)() : the chain's value with its this, as one callee */
        struct patches *saved = g->chain, mine = { 0 };
        g->chain = &mine;
        gen_callee(g, c->a);
        uint32_t skip = jump(g, OP_JMP);
        for (int i = 0; i < mine.n; i++)
            patch(g, mine.at[i]);
        adjust(g, -1);
        op(g, OP_UNDEF);                        /* nullish: undefined this */
        patch(g, skip);
        g->chain = saved;
    } else
        gen_callee(g, c);
    if (n->flags & NF_OPTIONAL)
        add_patch(g->chain, jump(g, OP_JMP_NULLISH_CALLEE));
    bool spread;
    gen_args(g, n->list, n->n, &spread);
    line(g, n);
    if (spread)
        op(g, OP_CALL_SPREAD);
    else
        call_op(g, OP_CALL, n->n);
}

static void gen_template(struct gen *g, struct node *t)
{
    bool have = false;
    for (int i = 0; i <= t->n; i++) {
        struct jsstring *s = t->raws[i]->str;
        if (s && str_len(s)) {
            op32(g, OP_CONST, atom_const(g, s));
            if (have)
                op(g, OP_ADD);
            have = true;
        } else if (!have && i == t->n) {
            op32(g, OP_CONST, atom_const(g, g->J->atom[A_empty]));
            have = true;
        }
        if (i < t->n) {
            gen_expr(g, t->list[i]);
            op(g, OP_TO_STRING);
            if (have)
                op(g, OP_ADD);
            have = true;
        }
    }
}

static void gen_tagged(struct gen *g, struct node *t)
{
    gen_callee(g, t->a);
    /* the site's strings: cooked (undefined if invalid) then raw */
    int n = t->n + 1;
    struct jsobject *a = array_new(g->J, (uint32_t)(2 * n));
    for (int i = 0; i < n; i++) {
        struct jsstring *c = t->raws[i]->str;
        a->elems[i] = c ? str_val(c) : JS_UNDEFINED;
        a->elems[n + i] = str_val(t->d->list[i]->str);
    }
    uint32_t k = new_const(g, obj_val(a));
    new_const(g, JS_UNDEFINED);                 /* the template object, made at its first use */
    op32(g, OP_TEMPLATE_OBJECT, k);
    for (int i = 0; i < t->n; i++)
        gen_expr(g, t->list[i]);
    line(g, t);
    call_op(g, OP_CALL, t->n + 1);
}

static void gen_object(struct gen *g, struct node *o)
{
    op(g, OP_OBJECT);
    for (int i = 0; i < o->n; i++) {
        struct node *p = o->list[i];
        bool computed = p->flags & NF_COMPUTED;
        switch (p->op) {
        case PK_SPREAD:
            gen_expr(g, p->b);
            op(g, OP_COPY_DATA_PROPS);
            break;
        case PK_PROTO:
            gen_expr(g, p->b);
            op(g, OP_SET_PROTO_LIT);
            break;
        case PK_INIT:
            if (!computed) {
                gen_named(g, p->b, p->a->str);
                op_atom(g, OP_DEFINE_FIELD, p->a->str);
            } else {
                gen_expr(g, p->a);
                op(g, OP_TO_PROPKEY);
                gen_expr(g, p->b);
                if (is_anonymous_fn(p->b)) {
                    op(g, OP_SET_NAME_ELEM);
                    emit1(g, 0);
                }
                op(g, OP_DEFINE_ELEM);
            }
            break;
        default: {
            int kind = p->op == PK_GET ? 1 : p->op == PK_SET ? 2 : 0;
            if (!computed) {
                closure(g, p->b);
                op(g, OP_DEFINE_METHOD);
                emit32(g, atom_const(g, p->a->str));
                emit1(g, (unsigned)kind);
                emit1(g, 1);
            } else {
                gen_expr(g, p->a);
                op(g, OP_TO_PROPKEY);
                closure(g, p->b);
                op(g, OP_DEFINE_METHOD_ELEM);
                emit1(g, (unsigned)kind);
                emit1(g, 1);
            }
        }
        }
    }
}

static void gen_array(struct gen *g, struct node *a)
{
    op(g, OP_ARRAY);
    for (int i = 0; i < a->n; i++) {
        struct node *e = a->list[i];
        if (e->type == N_HOLE)
            op(g, OP_ARRAY_HOLE);
        else if (e->type == N_SPREAD) {
            gen_expr(g, e->a);
            op(g, OP_ARRAY_SPREAD);
        } else {
            gen_expr(g, e);
            op(g, OP_ARRAY_APPEND);
        }
    }
}

/* Assigning the top of the stack to a pattern (it is consumed). */
static void assign_pattern(struct gen *g, struct node *p, int mode);

static void assign_default(struct gen *g, struct node *def, struct node *target)
{
    uint32_t j = jump(g, OP_JMP_NOT_UNDEF);
    op(g, OP_POP);
    if (target->type == N_IDENT)
        gen_named(g, def, target->str);
    else
        gen_expr(g, def);
    patch(g, j);
}

static void assign_pattern(struct gen *g, struct node *p, int mode)
{
    switch (p->type) {
    case N_IDENT:
        set_binding(g, p->binding, p->str, mode, false);
        return;
    case N_MEMBER:
        gen_expr(g, p->a);
        op(g, OP_SWAP);
        op(g, OP_SET_PROP);
        emit32(g, atom_const(g, p->str));
        emit1(g, g->strict);
        op(g, OP_POP);
        return;
    case N_INDEX:
        gen_expr(g, p->a);
        gen_expr(g, p->b);
        op(g, OP_ROT3R);                        /* v obj key -> obj key v (two turns) */
        op(g, OP_ROT3R);
        op(g, OP_SET_ELEM);
        emit1(g, g->strict);
        op(g, OP_POP);
        return;
    case N_PRIVATE_MEMBER:
        gen_expr(g, p->a);
        get_binding_raw(g, p->binding);
        op(g, OP_ROT3R);
        op(g, OP_ROT3R);
        op(g, OP_SET_PRIVATE);
        op(g, OP_POP);
        return;
    case N_SUPER_MEMBER:
        gen_super_base(g, p);
        op(g, OP_ROT3R);
        op(g, OP_ROT3R);
        op(g, OP_SET_SUPER);
        emit32(g, atom_const(g, p->str));
        emit1(g, g->strict);
        op(g, OP_POP);
        return;
    case N_ASSIGN_PATTERN:
        assign_default(g, p->b, p->a);
        assign_pattern(g, p->a, mode);
        return;
    case N_ARRAY_PATTERN: {
        op(g, OP_GET_ITER);
        /* an exception while assigning closes the iterator */
        uint32_t t = jump(g, OP_TRY);
        push_action(g, ACT_TRY, 0);
        for (int i = 0; i < p->n; i++) {
            struct node *e = p->list[i];
            if (e->type == N_HOLE) {
                op(g, OP_ITER_STEP);
                op(g, OP_POP);
            } else if (e->type == N_REST) {
                op(g, OP_ITER_REST);
                assign_pattern(g, e->a, mode);
            } else {
                op(g, OP_ITER_STEP);
                assign_pattern(g, e, mode);
            }
        }
        g->nactions--;
        op(g, OP_END_TRY);
        op(g, OP_ITER_CLOSE);
        uint32_t done = jump(g, OP_JMP);
        patch(g, t);
        adjust(g, 4);                           /* it next at exc */
        op(g, OP_ITER_CLOSE_EXC);
        op(g, OP_THROW);
        patch(g, done);
        return;
    }
    case N_OBJECT_PATTERN: {
        op(g, OP_CHECK_COERCIBLE);
        bool rest = p->n && p->list[p->n - 1]->type == N_REST;
        int keyregs[64];
        int nkeys = 0;
        bool too_many = rest && p->n > 65;
        if (too_many)
            node_error(g->R, p, "Too many properties before a rest element");
        for (int i = 0; i < p->n; i++) {
            struct node *x = p->list[i];
            if (x->type == N_REST) {
                /* source key... -> rest */
                for (int k = 0; k < nkeys; k++)
                    op16(g, OP_GET_REG, (unsigned)keyregs[k]);
                op16(g, OP_OBJECT_REST, (unsigned)nkeys);
                adjust(g, -nkeys);
                assign_pattern(g, x->a, mode);
                return;
            }
            op(g, OP_DUP);
            if (x->flags & NF_COMPUTED) {
                gen_expr(g, x->a);
                op(g, OP_TO_PROPKEY);
                if (rest) {
                    int r = new_reg(g);
                    op16(g, OP_SET_REG, (unsigned)r);
                    keyregs[nkeys++] = r;
                }
                op(g, OP_GET_ELEM);
            } else {
                if (rest) {
                    int r = new_reg(g);
                    op32(g, OP_CONST, atom_const(g, x->a->str));
                    op16(g, OP_PUT_REG, (unsigned)r);
                    keyregs[nkeys++] = r;
                }
                op_atom(g, OP_GET_PROP, x->a->str);
            }
            assign_pattern(g, x->b, mode);
        }
        op(g, OP_POP);
        return;
    }
    }
    node_error(g->R, p, "Invalid assignment target");
}

/* a reference's current value, keeping what is needed to store into it:
 * ident: -; member: obj; index: obj key; private: obj sym; super: home this (key) */
static void gen_ref_get(struct gen *g, struct node *t)
{
    switch (t->type) {
    case N_IDENT:
        get_binding(g, t->binding, t->str);
        return;
    case N_MEMBER:
        gen_expr(g, t->a);
        op(g, OP_DUP);
        op_atom(g, OP_GET_PROP, t->str);
        return;
    case N_INDEX:
        gen_expr(g, t->a);
        gen_expr(g, t->b);
        op(g, OP_TO_PROPKEY);
        op(g, OP_DUP2);
        op(g, OP_GET_ELEM);
        return;
    case N_PRIVATE_MEMBER:
        gen_expr(g, t->a);
        get_binding_raw(g, t->binding);
        op(g, OP_DUP2);
        op(g, OP_GET_PRIVATE);
        return;
    case N_SUPER_MEMBER:
        gen_super_base(g, t);
        op(g, OP_DUP2);
        op_atom(g, OP_GET_SUPER, t->str);
        return;
    case N_SUPER_INDEX:
        gen_super_base(g, t);
        gen_expr(g, t->b);
        op(g, OP_TO_PROPKEY);
        /* home this key -> home this key value */
        op(g, OP_ROT3R);                        /* key home this */
        op(g, OP_DUP2);                         /* key home this home this */
        op(g, OP_ROT4R);                        /* key home this this home -- wrong order fixed below */
        node_error(g->R, t, "Compound assignment to super[...] is not supported");
    }
    node_error(g->R, t, "Invalid assignment target");
}

/* stores the value on top into the reference left by gen_ref_get; leaves the value */
static void gen_ref_set(struct gen *g, struct node *t)
{
    switch (t->type) {
    case N_IDENT:
        set_binding(g, t->binding, t->str, STORE_ASSIGN, true);
        return;
    case N_MEMBER:
        op(g, OP_SET_PROP);
        emit32(g, atom_const(g, t->str));
        emit1(g, g->strict);
        return;
    case N_INDEX:
        op(g, OP_SET_ELEM);
        emit1(g, g->strict);
        return;
    case N_PRIVATE_MEMBER:
        op(g, OP_SET_PRIVATE);
        return;
    case N_SUPER_MEMBER:
        op(g, OP_SET_SUPER);
        emit32(g, atom_const(g, t->str));
        emit1(g, g->strict);
        return;
    }
}

static int ref_slots(struct node *t)
{
    switch (t->type) {
    case N_IDENT: return 0;
    case N_MEMBER: return 1;
    default: return 2;
    }
}

static int binop(int p)
{
    switch (p) {
    case P_PLUS: case P_ADD_ASSIGN: return OP_ADD;
    case P_MINUS: case P_SUB_ASSIGN: return OP_SUB;
    case P_STAR: case P_MUL_ASSIGN: return OP_MUL;
    case P_SLASH: case P_DIV_ASSIGN: return OP_DIV;
    case P_PERCENT: case P_MOD_ASSIGN: return OP_MOD;
    case P_STARSTAR: case P_POW_ASSIGN: return OP_POW;
    case P_AMP: case P_AND_ASSIGN: return OP_BAND;
    case P_BAR: case P_OR_ASSIGN: return OP_BOR;
    case P_CARET: case P_XOR_ASSIGN: return OP_BXOR;
    case P_SHL: case P_SHL_ASSIGN: return OP_SHL;
    case P_SAR: case P_SAR_ASSIGN: return OP_SAR;
    case P_SHR: case P_SHR_ASSIGN: return OP_SHR;
    case P_EQ: return OP_EQ;
    case P_NE: return OP_NE;
    case P_SEQ: return OP_SEQ;
    case P_SNE: return OP_SNE;
    case P_LT: return OP_LT;
    case P_LE: return OP_LE;
    case P_GT: return OP_GT;
    case P_GE: return OP_GE;
    case 1000 + KW_INSTANCEOF: return OP_INSTANCEOF;
    case 1000 + KW_IN: return OP_IN;
    }
    return OP_NOP;
}

static void gen_assign(struct gen *g, struct node *n)
{
    struct node *t = n->a;
    if (n->op == P_ASSIGN) {
        switch (t->type) {
        case N_IDENT:
            gen_named(g, n->b, t->str);
            set_binding(g, t->binding, t->str, STORE_ASSIGN, true);
            return;
        case N_MEMBER:
            gen_expr(g, t->a);
            gen_expr(g, n->b);
            op(g, OP_SET_PROP);
            emit32(g, atom_const(g, t->str));
            emit1(g, g->strict);
            return;
        case N_INDEX:
            gen_expr(g, t->a);
            gen_expr(g, t->b);
            gen_expr(g, n->b);
            op(g, OP_SET_ELEM);
            emit1(g, g->strict);
            return;
        case N_PRIVATE_MEMBER:
            gen_expr(g, t->a);
            get_binding_raw(g, t->binding);
            gen_expr(g, n->b);
            op(g, OP_SET_PRIVATE);
            return;
        case N_SUPER_MEMBER:
            gen_super_base(g, t);
            gen_expr(g, n->b);
            op(g, OP_SET_SUPER);
            emit32(g, atom_const(g, t->str));
            emit1(g, g->strict);
            return;
        case N_SUPER_INDEX:
            gen_super_base(g, t);
            gen_expr(g, t->b);
            gen_expr(g, n->b);
            op(g, OP_SET_SUPER_ELEM);
            emit1(g, g->strict);
            return;
        default:
            gen_expr(g, n->b);
            op(g, OP_DUP);
            assign_pattern(g, t, STORE_ASSIGN);
            return;
        }
    }
    if (n->op == P_LAND_ASSIGN || n->op == P_LOR_ASSIGN || n->op == P_NULLISH_ASSIGN) {
        gen_ref_get(g, t);
        int slots = ref_slots(t);
        int jo = n->op == P_LAND_ASSIGN ? OP_JMP_FALSE_KEEP : n->op == P_LOR_ASSIGN ? OP_JMP_TRUE_KEEP
                                                                                   : OP_JMP_NOT_NULLISH_KEEP;
        uint32_t keep = jump(g, jo);
        if (t->type == N_IDENT)
            gen_named(g, n->b, t->str);
        else
            gen_expr(g, n->b);
        gen_ref_set(g, t);
        uint32_t end = jump(g, OP_JMP);
        patch(g, keep);
        adjust(g, slots);
        /* the kept value over the reference's slots */
        for (int i = 0; i < slots; i++) {
            op(g, OP_SWAP);
            op(g, OP_POP);
        }
        patch(g, end);
        return;
    }
    gen_ref_get(g, t);
    gen_expr(g, n->b);
    op(g, binop(n->op));
    gen_ref_set(g, t);
}

static void gen_update(struct gen *g, struct node *n, bool want)
{
    struct node *t = n->a;
    bool prefix = (n->flags & NF_PREFIX) || !want;
    int o = n->op == P_INC ? OP_INC : OP_DEC;
    gen_ref_get(g, t);
    if (prefix) {
        op(g, o);
        gen_ref_set(g, t);
        return;
    }
    op(g, OP_TO_NUMERIC);
    switch (ref_slots(t)) {
    case 0:
        op(g, OP_DUP);
        op(g, o);
        gen_ref_set(g, t);
        op(g, OP_POP);
        return;
    case 1:                                     /* obj old */
        op(g, OP_DUP);                          /* obj old old */
        op(g, OP_ROT3R);                        /* old obj old */
        op(g, o);
        gen_ref_set(g, t);                      /* old new */
        op(g, OP_POP);
        return;
    default:                                    /* a b old */
        op(g, OP_DUP);                          /* a b old old */
        op(g, OP_ROT4R);                        /* old a b old */
        op(g, o);
        gen_ref_set(g, t);
        op(g, OP_POP);
        return;
    }
}

static void gen_delete(struct gen *g, struct node *x)
{
    switch (x->type) {
    case N_MEMBER:
        gen_expr(g, x->a);
        op(g, OP_DELETE_PROP);
        emit32(g, atom_const(g, x->str));
        emit1(g, g->strict);
        return;
    case N_INDEX:
        gen_expr(g, x->a);
        gen_expr(g, x->b);
        op(g, OP_DELETE_ELEM);
        emit1(g, g->strict);
        return;
    case N_IDENT:
        if (!x->binding) {
            op_atom(g, OP_DELETE_GLOBAL, x->str);
            return;
        }
        op(g, OP_FALSE);
        return;
    case N_SUPER_MEMBER: case N_SUPER_INDEX:
        op32(g, OP_THROW_ERROR, 0);
        g->code[g->len - 4] = 1;                /* (a ReferenceError) */
        g->len -= 4;
        emit32(g, atom_const(g, atom_cstr(g->J, "Unsupported reference to 'super'")));
        op(g, OP_TRUE);
        return;
    case N_CHAIN: {
        struct patches *saved = g->chain, mine = { 0 };
        g->chain = &mine;
        struct node *m = x->a;
        if (m->type == N_MEMBER || m->type == N_INDEX) {
            gen_expr(g, m->a);
            if (m->flags & NF_OPTIONAL)
                add_patch(g->chain, jump(g, OP_JMP_NULLISH_UNDEF));
            if (m->type == N_MEMBER) {
                op(g, OP_DELETE_PROP);
                emit32(g, atom_const(g, m->str));
                emit1(g, g->strict);
            } else {
                gen_expr(g, m->b);
                op(g, OP_DELETE_ELEM);
                emit1(g, g->strict);
            }
            uint32_t skip = jump(g, OP_JMP);
            for (int i = 0; i < mine.n; i++)
                patch(g, mine.at[i]);
            op(g, OP_POP);
            op(g, OP_TRUE);
            patch(g, skip);
        } else {
            gen_expr(g, x);
            op(g, OP_POP);
            op(g, OP_TRUE);
        }
        g->chain = saved;
        return;
    }
    }
    gen_effect(g, x);
    op(g, OP_TRUE);
}

static void gen_expr(struct gen *g, struct node *n)
{
    switch (n->type) {
    case N_NUM: {
        double d = n->num;
        if (d >= -2147483648.0 && d <= 2147483647.0 && d == (int32_t)d && !(d == 0 && signbit(d)))
            op32(g, OP_INT, (uint32_t)(int32_t)d);
        else
            op32(g, OP_CONST, add_const(g, js_number(d)));
        return;
    }
    case N_STR:
        op32(g, OP_CONST, atom_const(g, n->str));
        return;
    case N_TEMPLATE:
        gen_template(g, n);
        return;
    case N_TAGGED:
        gen_tagged(g, n);
        return;
    case N_REGEXP: {
        uint32_t k = new_const(g, str_val(n->str));
        new_const(g, str_val(n->raw));
        op32(g, OP_REGEXP, k);
        return;
    }
    case N_NULL:
        op(g, OP_NULL);
        return;
    case N_TRUE:
        op(g, OP_TRUE);
        return;
    case N_FALSE:
        op(g, OP_FALSE);
        return;
    case N_IDENT:
        get_binding(g, n->binding, n->str);
        return;
    case N_THIS:
        if (!n->binding)
            op(g, OP_GLOBAL_THIS);
        else
            get_binding(g, n->binding, g->J->atom[A_this]);
        return;
    case N_NEW_TARGET:
        if (!n->binding)
            op(g, OP_UNDEF);
        else
            get_binding_raw(g, n->binding);
        return;
    case N_ARRAY:
        gen_array(g, n);
        return;
    case N_OBJECT:
        gen_object(g, n);
        return;
    case N_FUNCTION:
        closure(g, n);
        return;
    case N_CLASS:
        gen_class(g, n, n->str);
        return;
    case N_MEMBER:
        gen_expr(g, n->a);
        if (n->flags & NF_OPTIONAL)
            add_patch(g->chain, jump(g, OP_JMP_NULLISH_UNDEF));
        line(g, n);
        op_atom(g, OP_GET_PROP, n->str);
        return;
    case N_INDEX:
        gen_expr(g, n->a);
        if (n->flags & NF_OPTIONAL)
            add_patch(g->chain, jump(g, OP_JMP_NULLISH_UNDEF));
        gen_expr(g, n->b);
        line(g, n);
        op(g, OP_GET_ELEM);
        return;
    case N_PRIVATE_MEMBER:
        gen_expr(g, n->a);
        if (n->flags & NF_OPTIONAL)
            add_patch(g->chain, jump(g, OP_JMP_NULLISH_UNDEF));
        get_binding_raw(g, n->binding);
        op(g, OP_GET_PRIVATE);
        return;
    case N_PRIVATE_IN:
        node_error(g->R, n, "Unexpected private name");
    case N_SUPER_MEMBER:
        gen_super_base(g, n);
        op_atom(g, OP_GET_SUPER, n->str);
        return;
    case N_SUPER_INDEX:
        gen_super_base(g, n);
        gen_expr(g, n->b);
        op(g, OP_GET_SUPER_ELEM);
        return;
    case N_SUPER_CALL: {
        /* func new.target args -> this, bound once; then the fields */
        if (!n->binding || !n->binding3)
            node_error(g->R, n, "super() is only valid in derived class constructors");
        get_binding_raw(g, n->binding);
        get_binding_raw(g, n->binding3);
        bool spread;
        gen_args(g, n->list, n->n, &spread);
        line(g, n);
        if (spread)
            op(g, OP_SUPER_CALL_SPREAD);
        else
            call_op(g, OP_SUPER_CALL, n->n);
        struct binding *tb = n->binding2;
        if (tb->in_heap) {
            op(g, OP_SET_THIS_ONCE_SCOPE);
            emit16(g, hops_to(g, tb->scope));
            emit16(g, (unsigned)tb->slot);
        } else
            op16(g, OP_SET_THIS_ONCE_REG, (unsigned)tb->slot);
        op(g, OP_DUP);
        get_binding_raw(g, n->binding);
        op(g, OP_INIT_FIELDS);
        return;
    }
    case N_CALL:
        gen_call(g, n);
        return;
    case N_NEW: {
        gen_expr(g, n->a);
        op(g, OP_DUP);
        bool spread;
        gen_args(g, n->list, n->n, &spread);
        line(g, n);
        if (spread)
            op(g, OP_NEW_SPREAD);
        else
            call_op(g, OP_NEW, n->n);
        return;
    }
    case N_CHAIN: {
        struct patches *saved = g->chain, mine = { 0 };
        g->chain = &mine;
        gen_expr(g, n->a);
        for (int i = 0; i < mine.n; i++)
            patch(g, mine.at[i]);
        g->chain = saved;
        return;
    }
    case N_UNARY: {
        int u = n->op;
        if (u == 1000 + KW_DELETE) {
            gen_delete(g, n->a);
            return;
        }
        if (u == 1000 + KW_TYPEOF && n->a->type == N_IDENT && !n->a->binding) {
            op_atom(g, OP_GET_GLOBAL_TYPEOF, n->a->str);
            op(g, OP_TYPEOF);
            return;
        }
        gen_expr(g, n->a);
        switch (u) {
        case P_MINUS: op(g, OP_NEG); break;
        case P_PLUS: op(g, OP_PLUS); break;
        case P_BANG: op(g, OP_NOT); break;
        case P_TILDE: op(g, OP_BNOT); break;
        case 1000 + KW_TYPEOF: op(g, OP_TYPEOF); break;
        case 1000 + KW_VOID: op(g, OP_POP); op(g, OP_UNDEF); break;
        }
        return;
    }
    case N_UPDATE:
        gen_update(g, n, true);
        return;
    case N_BINARY:
        if (n->op == 1000 + KW_IN && n->a->type == N_PRIVATE_IN) {
            get_binding_raw(g, n->a->binding);
            gen_expr(g, n->b);
            op(g, OP_PRIVATE_IN);
            return;
        }
        gen_expr(g, n->a);
        gen_expr(g, n->b);
        line(g, n);
        op(g, binop(n->op));
        return;
    case N_LOGICAL: {
        gen_expr(g, n->a);
        int jo = n->op == P_AND ? OP_JMP_FALSE_KEEP : n->op == P_OR ? OP_JMP_TRUE_KEEP : OP_JMP_NOT_NULLISH_KEEP;
        uint32_t j = jump(g, jo);
        gen_expr(g, n->b);
        patch(g, j);
        return;
    }
    case N_ASSIGN:
        gen_assign(g, n);
        return;
    case N_COND: {
        gen_expr(g, n->a);
        uint32_t f = jump(g, OP_JMP_FALSE);
        gen_expr(g, n->b);
        uint32_t end = jump(g, OP_JMP);
        adjust(g, -1);
        patch(g, f);
        gen_expr(g, n->c);
        patch(g, end);
        return;
    }
    case N_SEQ:
        for (int i = 0; i < n->n - 1; i++)
            gen_effect(g, n->list[i]);
        gen_expr(g, n->list[n->n - 1]);
        return;
    case N_YIELD: case N_AWAIT:
        gen_yield(g, n);
        return;
    case N_SPREAD:
        node_error(g->R, n, "Unexpected spread");
    }
    node_error(g->R, n, "Unexpected expression");
}

static void gen_effect(struct gen *g, struct node *n)
{
    switch (n->type) {
    case N_UPDATE:
        gen_update(g, n, false);
        op(g, OP_POP);
        return;
    case N_ASSIGN:
        if (n->op == P_ASSIGN && n->a->type == N_IDENT) {
            gen_named(g, n->b, n->a->str);
            set_binding(g, n->a->binding, n->a->str, STORE_ASSIGN, false);
            return;
        }
        if (n->op == P_ASSIGN && (n->a->type == N_ARRAY_PATTERN || n->a->type == N_OBJECT_PATTERN)) {
            gen_expr(g, n->b);
            assign_pattern(g, n->a, STORE_ASSIGN);
            return;
        }
        break;
    case N_SEQ:
        for (int i = 0; i < n->n; i++)
            gen_effect(g, n->list[i]);
        return;
    }
    gen_expr(g, n);
    op(g, OP_POP);
}

/* --- classes ------------------------------------------------------------ */

static void gen_default_ctor(struct gen *g, struct node *c, struct jsstring *name, bool derived)
{
    struct funcdef *d = gen_function(g, 0, c);
    d->name = name;
    (void)derived;
    op32(g, OP_CLOSURE, new_const(g, cell_val(d)));
}

static void gen_class(struct gen *g, struct node *c, struct jsstring *name)
{
    struct cscope *s = c->scope;
    line(g, c);
    enter_scope(g, s);
    /* private names: a symbol each */
    for (int i = 0; i < s->nb; i++) {
        struct binding *b = s->b[i];
        if (b->func && b->name && str_len(b->name) && str_char_at(g->J, b->name, 0) == '#') {
            op32(g, OP_PRIVATE_NAME, atom_const(g, b->name));
            set_binding(g, b, b->name, STORE_INIT, false);
        }
    }
    if (c->a)
        gen_expr(g, c->a);
    if (c->b) {
        struct funcdef *d = gen_function(g, c->b->fn, c);
        op32(g, OP_CLOSURE, new_const(g, cell_val(d)));
    } else
        gen_default_ctor(g, c, name ? name : g->J->atom[A_empty], c->a != 0);
    op(g, OP_CLASS);
    emit32(g, atom_const(g, name ? name : g->J->atom[A_empty]));
    emit1(g, c->a != 0);
    if (!c->a)
        adjust(g, 1);                           /* ctor -> ctor proto */
    /* stack: ctor proto */
    bool has_fields = false, has_statics = false;
    for (int i = 0; i < c->n; i++) {
        struct node *m = c->list[i];
        bool is_static = m->flags & NF_STATIC;
        if (m->type == N_STATIC_BLOCK) {
            has_statics = true;
            continue;
        }
        if (m->op == PK_FIELD) {
            if (m->flags & NF_COMPUTED) {
                gen_expr(g, m->a);
                op(g, OP_TO_PROPKEY);
                set_binding(g, m->binding, m->binding->name, STORE_INIT, false);
            }
            if (is_static)
                has_statics = true;
            else
                has_fields = true;
            continue;
        }
        int kind = m->op == PK_GET ? 1 : m->op == PK_SET ? 2 : 0;
        if (m->a->type == N_PRIVATE_MEMBER) {
            if (!is_static) {
                /* instance private methods: kept, installed by the fields' initializer */
                closure(g, m->b);
                set_binding(g, m->binding2, m->binding2->name, STORE_INIT, false);
                has_fields = true;
                continue;
            }
            op(g, OP_SWAP);                     /* proto ctor */
            get_binding_raw(g, m->a->binding);
            closure(g, m->b);
            op(g, OP_DEFINE_METHOD_ELEM);
            emit1(g, (unsigned)(kind | 4));     /* 4: a private name (not enumerable, not a field) */
            emit1(g, 0);
            op(g, OP_SWAP);
            continue;
        }
        if (is_static)
            op(g, OP_SWAP);                     /* proto ctor */
        if (m->flags & NF_COMPUTED) {
            gen_expr(g, m->a);
            op(g, OP_TO_PROPKEY);
            closure(g, m->b);
            op(g, OP_DEFINE_METHOD_ELEM);
            emit1(g, (unsigned)kind);
            emit1(g, 0);
        } else {
            closure(g, m->b);
            op(g, OP_DEFINE_METHOD);
            emit32(g, atom_const(g, m->a->str));
            emit1(g, (unsigned)kind);
            emit1(g, 0);
        }
        if (is_static)
            op(g, OP_SWAP);
    }
    if (has_fields) {
        struct funcdef *d = gen_function(g, c->fn, c);
        op32(g, OP_CLOSURE, new_const(g, cell_val(d)));
        op(g, OP_SET_FIELDS);
    }
    op(g, OP_POP);                              /* ctor */
    if (c->str) {
        op(g, OP_DUP);
        set_binding(g, find_local(s, c->str), c->str, STORE_INIT, false);
    }
    if (has_statics) {
        struct funcdef *d = gen_function(g, c->fn2, c);
        op32(g, OP_CLOSURE, new_const(g, cell_val(d)));
        op(g, OP_CALL_STATIC_INIT);
    }
    leave_scope(g, s);
}

/* The fields' initializer: this.key = value for each field of the class
 * (statics: of the constructor), and the private methods. */
static void gen_fields_body(struct gen *g, struct node *c, bool statics)
{
    struct binding *tb = 0;
    for (int i = 0; i < g->fn->scope->nb; i++)
        if (g->fn->scope->b[i]->kind == B_THIS)
            tb = g->fn->scope->b[i];
    for (int i = 0; i < c->n; i++) {
        struct node *m = c->list[i];
        bool is_static = (m->flags & NF_STATIC) != 0;
        if (is_static != statics)
            continue;
        if (m->type == N_STATIC_BLOCK) {
            enter_scope(g, m->scope);
            for (int k = 0; k < m->b->fn->nbody; k++)
                gen_stmt(g, m->b->fn->body[k]);
            leave_scope(g, m->scope);
            continue;
        }
        if (m->op != PK_FIELD) {
            if (!statics && m->a->type == N_PRIVATE_MEMBER && m->binding2) {
                op(g, OP_THIS_FRAME);
                get_binding_raw(g, m->a->binding);
                get_binding_raw(g, m->binding2);
                if (m->op == PK_METHOD) {
                    op(g, OP_DEFINE_PRIVATE);
                } else {
                    op(g, OP_DEFINE_METHOD_ELEM);
                    emit1(g, (unsigned)((m->op == PK_GET ? 1 : 2) | 4));
                    emit1(g, 0);
                }
                op(g, OP_POP);
            }
            continue;
        }
        line(g, m);
        op(g, OP_THIS_FRAME);
        struct node *init = m->b ? m->b->fn->expr : 0;
        if (m->a->type == N_PRIVATE_MEMBER) {
            get_binding_raw(g, m->a->binding);
            if (init)
                gen_named(g, init, private_atom(g->R, m->a->str));
            else
                op(g, OP_UNDEF);
            op(g, OP_DEFINE_PRIVATE);
        } else if (m->flags & NF_COMPUTED) {
            get_binding_raw(g, m->binding);
            if (init) {
                gen_expr(g, init);
                if (is_anonymous_fn(init)) {
                    op(g, OP_SET_NAME_ELEM);
                    emit1(g, 0);
                }
            } else
                op(g, OP_UNDEF);
            op(g, OP_DEFINE_ELEM);
        } else {
            if (init)
                gen_named(g, init, m->a->str);
            else
                op(g, OP_UNDEF);
            op_atom(g, OP_DEFINE_FIELD, m->a->str);
        }
        op(g, OP_POP);
    }
    (void)tb;
}

/* --- statements ------------------------------------------------------------ */

static struct target *push_target(struct gen *g, bool loop)
{
    struct target *t = arena_new(sizeof *t);
    t->prev = g->targets;
    t->loop = loop;
    t->plain_break = loop;
    t->actions = g->nactions;
    t->sp = g->sp;
    t->cont_sp = g->sp;
    for (int i = 0; i < g->npending && i < 8; i++)
        t->labels[t->nlabels++] = g->pending[i];
    g->npending = 0;
    g->targets = t;
    return t;
}

static void pop_target(struct gen *g, struct target *t, uint32_t break_at, uint32_t cont_at)
{
    for (int i = 0; i < t->breaks.n; i++)
        patch_to(g, t->breaks.at[i], break_at);
    for (int i = 0; i < t->conts.n; i++)
        patch_to(g, t->conts.at[i], cont_at);
    g->targets = t->prev;
}

static bool has_label(struct target *t, struct jsstring *l)
{
    for (int i = 0; i < t->nlabels; i++)
        if (t->labels[i] == l)
            return true;
    return false;
}

static void gen_jump_stmt(struct gen *g, struct node *n, bool is_break)
{
    for (struct target *t = g->targets; t; t = t->prev) {
        bool ok = n->str ? has_label(t, n->str) && (is_break || t->loop)
                         : (is_break ? t->plain_break : t->loop);
        if (!ok)
            continue;
        int saved_sp = g->sp;
        emit_actions(g, t->actions);
        uint32_t at = jump(g, OP_JMP);
        add_patch(is_break ? &t->breaks : &t->conts, at);
        g->sp = saved_sp;
        return;
    }
    if (n->str)
        node_error(g->R, n, "Undefined label");
    node_error(g->R, n, is_break ? "Illegal break statement" : "Illegal continue statement");
}

/* (27r) A generator resumed by return() at a yield: the value on the
 * stack is returned as a return statement there would (the expression's
 * values above the statement's start dropped, the finally blocks run). */
static void return_from_yield(struct gen *g)
{
    int r = new_reg(g);
    op16(g, OP_PUT_REG, (unsigned)r);
    while (g->sp > g->stmt_sp)
        op(g, OP_POP);
    emit_actions(g, 0);
    op16(g, OP_GET_REG, (unsigned)r);
    op(g, OP_RETURN);
}

/* after a yield or an await: how it was resumed (a throw is thrown there; a
 * return returns, with the finally blocks) */
static void resume_switch(struct gen *g, bool returns)
{
    if (!returns) {
        op32(g, OP_RESUME_SWITCH, 0);
        return;
    }
    uint32_t to_return = jump(g, OP_RESUME_SWITCH);
    uint32_t over = jump(g, OP_JMP);
    int sp = g->sp;
    patch(g, to_return);
    return_from_yield(g);
    g->sp = sp;
    patch(g, over);
}

static void gen_yield(struct gen *g, struct node *n)
{
    if (n->type == N_AWAIT) {
        gen_expr(g, n->a);
        op(g, OP_AWAIT);
        resume_switch(g, false);
        return;
    }
    if (!(n->flags & NF_PREFIX)) {
        if (n->a)
            gen_expr(g, n->a);
        else
            op(g, OP_UNDEF);
        op(g, OP_YIELD);
        resume_switch(g, true);
        return;
    }
    /* yield*: the inner iterator's results passed out as they are, what comes
     * passed in (next, throw, return) until it is done */
    gen_expr(g, n->a);
    op(g, OP_DELEGATE_START);                   /* iterator, next, received, kind */
    int loop_sp = g->sp;
    uint32_t loop = g->len;
    op(g, OP_DELEGATE_STEP);
    uint32_t done_at = g->len;
    emit32(g, 0);
    uint32_t return_at = g->len;
    emit32(g, 0);
    adjust(g, -1);                              /* (on: iterator, next, the inner result) */
    op(g, OP_YIELD_RAW);                        /* (resumed: iterator, next, received, kind) */
    jump_to(g, OP_JMP, loop);
    g->sp = loop_sp - 3;                        /* (both ends: the value alone) */
    patch(g, return_at);
    return_from_yield(g);
    g->sp = loop_sp - 3;
    patch(g, done_at);
}

static void gen_return(struct gen *g, struct node *n)
{
    bool derived = (g->fn->flags & FN_DERIVED) != 0;
    if (n->a)
        gen_expr(g, n->a);
    else
        op(g, OP_UNDEF);
    if (g->nactions) {
        int r = new_reg(g);
        op16(g, OP_PUT_REG, (unsigned)r);
        int saved_sp = g->sp;
        emit_actions(g, 0);
        g->sp = saved_sp;
        op16(g, OP_GET_REG, (unsigned)r);
    }
    if (derived) {
        struct binding *tb = find_local(g->fn->scope, g->J->atom[A_this]);
        get_binding_raw(g, tb);
        op(g, OP_CHECK_DERIVED_RETURN);
    }
    op(g, OP_RETURN);
}

static void gen_var(struct gen *g, struct node *n)
{
    int mode = n->op == VK_VAR ? STORE_ASSIGN : STORE_INIT;
    bool script_top = g->scope->kind == S_SCRIPT;
    if (script_top && n->op != VK_VAR)
        mode = STORE_INIT_LEX;
    for (int i = 0; i < n->n; i++) {
        struct node *d = n->list[i];
        line(g, d);
        if (!d->b) {
            if (n->op == VK_VAR)
                continue;
            op(g, OP_UNDEF);
        } else if (d->a->type == N_IDENT)
            gen_named(g, d->b, d->a->str);
        else
            gen_expr(g, d->b);
        if (n->op == VK_VAR && d->a->type == N_IDENT) {
            set_binding(g, d->a->binding, d->a->str, STORE_INIT, false);
            continue;
        }
        assign_pattern(g, d->a, mode == STORE_ASSIGN ? STORE_INIT : mode);
    }
}

static void gen_body(struct gen *g, struct node *n)
{
    gen_stmt(g, n);
}

static void gen_for(struct gen *g, struct node *n)
{
    struct cscope *s = n->scope;
    if (s)
        enter_scope(g, s);
    if (n->a) {
        if (n->a->type == N_VAR)
            gen_var(g, n->a);
        else
            gen_effect(g, n->a);
    }
    bool per_iteration = s && s->heap;
    if (per_iteration)
        op(g, OP_COPY_SCOPE);
    uint32_t top = g->len;
    uint32_t exit = 0;
    bool has_test = n->b != 0;
    if (has_test) {
        line(g, n);
        gen_expr(g, n->b);
        exit = jump(g, OP_JMP_FALSE);
    }
    struct target *t = push_target(g, true);
    gen_body(g, n->d);
    uint32_t cont = g->len;
    if (per_iteration)
        op(g, OP_COPY_SCOPE);
    if (n->c)
        gen_effect(g, n->c);
    jump_to(g, OP_JMP, top);
    uint32_t brk = g->len;
    if (has_test)
        patch(g, exit);
    pop_target(g, t, brk, cont);
    if (s)
        leave_scope(g, s);
}

static void gen_for_in_of(struct gen *g, struct node *n)
{
    bool of = n->type == N_FOR_OF;
    int k = of ? 3 : 1;
    struct cscope *s = n->scope;
    gen_expr(g, n->b);
    op(g, of ? OP_GET_ITER : OP_FOR_IN_START);
    push_action(g, of ? ACT_ITER : ACT_POP, 0);
    struct target *t = push_target(g, true);
    int base = g->sp;                           /* with the loop's slots */
    uint32_t top = g->len;
    uint32_t done = jump(g, of ? OP_ITER_NEXT : OP_FOR_IN_NEXT);
    uint32_t handler = 0;
    if (of) {
        handler = jump(g, OP_TRY);              /* the body's exceptions close the iterator */
        push_action(g, ACT_TRY, 0);
    }
    if (s)
        enter_scope(g, s);
    struct node *left = n->a;
    if (left->type == N_VAR) {
        int mode = STORE_INIT;
        if (left->op != VK_VAR && g->scope->kind == S_SCRIPT)
            mode = STORE_INIT_LEX;
        assign_pattern(g, left->list[0]->a, mode);
    } else
        assign_pattern(g, left, STORE_ASSIGN);
    gen_body(g, n->c);
    if (s)
        leave_scope(g, s);
    if (of) {
        g->nactions--;
        op(g, OP_END_TRY);
    }
    jump_to(g, OP_JMP, top);
    /* break: the iterator closed */
    uint32_t brk = g->len;
    g->sp = base;
    op(g, of ? OP_ITER_CLOSE : OP_POP);
    uint32_t end = jump(g, OP_JMP);
    /* done: the loop ran out */
    patch(g, done);
    g->sp = base;
    for (int i = 0; i < k; i++)
        op(g, OP_POP);
    uint32_t end2 = jump(g, OP_JMP);
    if (of) {
        patch(g, handler);                      /* it next at value exc */
        g->sp = base + 2;
        op(g, OP_SWAP);
        op(g, OP_POP);
        op(g, OP_ITER_CLOSE_EXC);
        op(g, OP_THROW);
    }
    g->nactions--;                              /* the iterator's action */
    pop_target(g, t, brk, top);
    patch(g, end);
    patch(g, end2);
    g->sp = base - k;
}

static void gen_switch(struct gen *g, struct node *n)
{
    gen_expr(g, n->a);
    push_action(g, ACT_POP, 0);
    enter_scope(g, n->scope);
    struct target *t = push_target(g, false);
    t->plain_break = true;
    uint32_t *jumps = arena_new((size_t)(n->n + 1) * sizeof *jumps);
    int def = -1;
    for (int i = 0; i < n->n; i++) {
        struct node *c = n->list[i];
        if (!c->a) {
            def = i;
            continue;
        }
        op(g, OP_DUP);
        gen_expr(g, c->a);
        op(g, OP_SEQ);
        jumps[i] = jump(g, OP_JMP_TRUE);
    }
    uint32_t to_default = jump(g, OP_JMP);
    for (int i = 0; i < n->n; i++) {
        struct node *c = n->list[i];
        if (i == def)
            patch(g, to_default);
        else
            patch(g, jumps[i]);
        for (int k = 0; k < c->n; k++)
            gen_stmt(g, c->list[k]);
    }
    if (def < 0)
        patch(g, to_default);
    uint32_t brk = g->len;
    pop_target(g, t, brk, brk);
    leave_scope(g, n->scope);
    g->nactions--;
    op(g, OP_POP);
}

static void gen_try(struct gen *g, struct node *n)
{
    bool has_catch = n->flags & NF_PREFIX;
    uint32_t fin_handler = 0;
    int base_sp = g->sp;
    if (n->d) {
        fin_handler = jump(g, OP_TRY);
        push_action(g, ACT_FINALLY, n->d);
    }
    if (has_catch) {
        uint32_t ch = jump(g, OP_TRY);
        push_action(g, ACT_TRY, 0);
        gen_stmt(g, n->a);
        g->nactions--;
        op(g, OP_END_TRY);
        uint32_t done = jump(g, OP_JMP);
        patch(g, ch);
        g->sp = base_sp + 1;                    /* the exception */
        if (n->scope) {
            enter_scope(g, n->scope);
            if (n->b)
                assign_pattern(g, n->b, STORE_INIT);
            else
                op(g, OP_POP);
            gen_stmt(g, n->c);
            leave_scope(g, n->scope);
        } else {
            op(g, OP_POP);
            gen_stmt(g, n->c);
        }
        patch(g, done);
        g->sp = base_sp;
    } else
        gen_stmt(g, n->a);
    if (n->d) {
        g->nactions--;
        op(g, OP_END_TRY);
        gen_stmt(g, n->d);
        uint32_t end = jump(g, OP_JMP);
        patch(g, fin_handler);
        g->sp = base_sp + 1;
        int r = new_reg(g);
        op16(g, OP_PUT_REG, (unsigned)r);
        gen_stmt(g, n->d);
        op16(g, OP_GET_REG, (unsigned)r);
        op(g, OP_THROW);
        patch(g, end);
        g->sp = base_sp;
    }
}

static bool is_loop(struct node *n)
{
    return n->type == N_FOR || n->type == N_FOR_IN || n->type == N_FOR_OF || n->type == N_WHILE ||
           n->type == N_DO_WHILE;
}

static void gen_stmt_at(struct gen *g, struct node *n);

/* (27r) a statement, its starting depth noted: a generator's return from a
 * yield inside it drops what the statement had pushed above that first */
static void gen_stmt(struct gen *g, struct node *n)
{
    int saved = g->stmt_sp;
    g->stmt_sp = g->sp;
    gen_stmt_at(g, n);
    g->stmt_sp = saved;
}

static void gen_stmt_at(struct gen *g, struct node *n)
{
    if (!n)
        return;
    line(g, n);
    if (n->type != N_LABELED && !is_loop(n))
        g->npending = 0;
    switch (n->type) {
    case N_EMPTY: case N_DEBUGGER:
        return;
    case N_EXPR_STMT:
        if (g->completion >= 0) {
            gen_expr(g, n->a);
            op16(g, OP_PUT_REG, (unsigned)g->completion);
        } else
            gen_effect(g, n->a);
        return;
    case N_VAR:
        gen_var(g, n);
        return;
    case N_FUNC_DECL:
        if (n->a->fn->flags & (FN_GENERATOR | FN_ASYNC)) {
            /* made at the scope's entry; nothing here */
        }
        if (n->binding2) {
            /* Annex B: the block's function into its function's var */
            get_binding(g, n->binding, n->str);
            set_binding(g, n->binding2, n->str, STORE_INIT, false);
        } else if ((n->flags & NF_STATIC) && n->binding) {
            get_binding(g, n->binding, n->str);
            set_binding(g, 0, n->str, STORE_ASSIGN, false);
        }
        return;
    case N_CLASS_DECL: {
        gen_class(g, n->a, n->str);
        if (n->binding || g->scope->kind != S_SCRIPT)
            set_binding(g, n->binding, n->str, STORE_INIT, false);
        else
            set_binding(g, 0, n->str, STORE_INIT_LEX, false);
        return;
    }
    case N_RETURN:
        gen_return(g, n);
        return;
    case N_IF: {
        gen_expr(g, n->a);
        uint32_t f = jump(g, OP_JMP_FALSE);
        gen_stmt(g, n->b);
        if (n->c) {
            uint32_t end = jump(g, OP_JMP);
            patch(g, f);
            gen_stmt(g, n->c);
            patch(g, end);
        } else
            patch(g, f);
        return;
    }
    case N_BLOCK:
        enter_scope(g, n->scope);
        for (int i = 0; i < n->n; i++)
            gen_stmt(g, n->list[i]);
        leave_scope(g, n->scope);
        return;
    case N_WHILE: {
        uint32_t top = g->len;
        gen_expr(g, n->a);
        uint32_t exit = jump(g, OP_JMP_FALSE);
        struct target *t = push_target(g, true);
        gen_body(g, n->b);
        jump_to(g, OP_JMP, top);
        patch(g, exit);
        pop_target(g, t, g->len, top);
        return;
    }
    case N_DO_WHILE: {
        uint32_t top = g->len;
        struct target *t = push_target(g, true);
        gen_body(g, n->b);
        uint32_t cont = g->len;
        gen_expr(g, n->a);
        jump_to(g, OP_JMP_TRUE, top);
        pop_target(g, t, g->len, cont);
        return;
    }
    case N_FOR:
        gen_for(g, n);
        return;
    case N_FOR_IN: case N_FOR_OF:
        gen_for_in_of(g, n);
        return;
    case N_BREAK:
        gen_jump_stmt(g, n, true);
        return;
    case N_CONTINUE:
        gen_jump_stmt(g, n, false);
        return;
    case N_THROW:
        gen_expr(g, n->a);
        line(g, n);
        op(g, OP_THROW);
        return;
    case N_TRY:
        gen_try(g, n);
        return;
    case N_SWITCH:
        gen_switch(g, n);
        return;
    case N_LABELED: {
        struct node *body = n;
        while (body->type == N_LABELED) {
            if (g->npending < 8)
                g->pending[g->npending++] = body->str;
            body = body->a;
        }
        if (is_loop(body)) {
            gen_stmt(g, body);
            return;
        }
        struct target *t = push_target(g, false);
        gen_stmt(g, body);
        pop_target(g, t, g->len, g->len);
        return;
    }
    }
    node_error(g->R, n, "Unexpected statement");
}

/* --- functions ------------------------------------------------------------- */

static struct funcdef *finish(struct gen *g, struct jsstring *name, uint32_t flags, int nparams)
{
    struct funcdef *d = gc_alloc(g->J, CELL_FUNCDEF, sizeof *d);
    if (!d)
        abort();
    d->code = g->code;
    d->code_len = g->len;
    d->consts = g->consts;
    d->nconsts = g->nconsts;
    d->lines = g->lines;
    d->nlines = g->nlines;
    d->nregs = (uint16_t)g->nregs;
    d->nparams = (uint16_t)nparams;
    d->flags = (uint16_t)flags;
    d->stack_max = (uint16_t)(g->max_sp + 4 > 65535 ? 65535 : g->max_sp + 4);
    d->name = name;
    d->file = g->file;
    d->source = g->source;
    if (g->max_sp > 60000)
        node_error(g->R, 0, "Expression too deep");
    return d;
}

static uint32_t def_flags(struct cfunc *fn)
{
    uint32_t f = 0;
    if (fn->flags & FN_STRICT) f |= FD_STRICT;
    if (fn->flags & FN_ARROW) f |= FD_ARROW | FD_NO_CTOR;
    if (fn->flags & (FN_METHOD | FN_GETTER | FN_SETTER)) f |= FD_METHOD | FD_NO_CTOR;
    if (fn->flags & FN_CLASS_CTOR) f = (f & ~(uint32_t)(FD_METHOD | FD_NO_CTOR)) | FD_CLASS_CTOR;
    if (fn->flags & FN_DERIVED) f |= FD_DERIVED;
    if (fn->flags & FN_GENERATOR) f |= FD_GENERATOR | FD_NO_CTOR;
    if (fn->flags & FN_ASYNC) f |= FD_ASYNC | FD_NO_CTOR;
    if (fn->flags & FN_FIELD_INIT) f |= FD_FIELD_INIT;
    return f;
}

static void gen_prologue_specials(struct gen *g, struct cscope *s)
{
    for (int i = 0; i < s->nb; i++) {
        struct binding *b = s->b[i];
        int o = -1;
        switch (b->kind) {
        case B_THIS: o = OP_THIS_FRAME; break;
        case B_NEW_TARGET: o = OP_NEW_TARGET_FRAME; break;
        case B_HOME: o = b->name == g->J->atom[A_func] ? OP_CALLEE : OP_HOME_FRAME; break;
        case B_ARGUMENTS: o = OP_ARGUMENTS; break;
        case B_FUNC_NAME: o = OP_CALLEE; break;
        }
        if (o < 0)
            continue;
        if (o == OP_ARGUMENTS) {
            /* sloppy simple parameters: the elements are the parameters (their slots) */
            struct cfunc *fn = g->fn;
            bool mapped = !(fn->flags & FN_STRICT) && (fn->flags & FN_SIMPLE_PARAMS);
            op16(g, OP_ARGUMENTS, mapped ? (unsigned)fn->nparams : 0);
            if (mapped)
                for (int k = 0; k < fn->nparams; k++)
                    emit16(g, (unsigned)fn->params[k]->binding->slot);
        } else
            op(g, o);
        set_binding(g, b, b->name, STORE_INIT, false);
    }
}

/* A function's code; fn 0 with class_node: the class's default constructor. */
static struct funcdef *gen_function(struct gen *parent, struct cfunc *fn, struct node *class_node)
{
    struct gen G = { 0 }, *g = &G;
    g->J = parent->J;
    g->R = parent->R;
    g->parent = parent;
    g->file = parent->file;
    g->source = parent->source;
    g->completion = -1;
    g->last_line = -1;
    if (!fn) {
        /* default constructor */
        bool derived = class_node->a != 0;
        g->fn = arena_new(sizeof *g->fn);
        g->fn->flags = FN_CLASS_CTOR | FN_STRICT | (derived ? FN_DERIVED : 0);
        g->strict = true;
        if (derived) {
            op(g, OP_CALLEE);
            op(g, OP_NEW_TARGET_FRAME);
            op16(g, OP_REST, 0);
            op(g, OP_SUPER_CALL_SPREAD);
            op(g, OP_DUP);
            op(g, OP_CALLEE);
            op(g, OP_INIT_FIELDS);
            op(g, OP_RETURN);
        } else {
            op(g, OP_THIS_FRAME);
            op(g, OP_CALLEE);
            op(g, OP_INIT_FIELDS);
            op(g, OP_RETURN_UNDEF);
        }
        struct funcdef *d = finish(g, 0, FD_CLASS_CTOR | FD_STRICT | (derived ? FD_DERIVED : 0), 0);
        d->src_start = class_node->pos;
        d->src_end = class_node->end;
        return d;
    }
    g->fn = fn;
    g->strict = (fn->flags & FN_STRICT) != 0;
    struct cscope *s = fn->scope;
    if (fn->uses_arguments && !(fn->flags & FN_STRICT) && (fn->flags & FN_SIMPLE_PARAMS))
        for (int i = 0; i < fn->nparams; i++)
            fn->params[i]->binding->captured = true;   /* mapped by the arguments object */
    alloc_scope(g, s);
    g->scope = s;
    if (s->heap)
        op16(g, OP_PUSH_SCOPE, (unsigned)s->heap_size);
    gen_prologue_specials(g, s);
    init_lexicals(g, s);
    /* hoisted function declarations */
    for (int i = 0; i < s->nb; i++) {
        struct binding *b = s->b[i];
        if (b->kind == B_FUNCTION && b->func) {
            closure(g, b->func->a);
            set_binding(g, b, b->name, STORE_INIT, false);
        }
    }
    if (fn->flags & FN_FIELD_INIT) {
        gen_fields_body(g, class_node, (fn->flags & FN_STATIC_BLOCK) != 0);
        op(g, OP_RETURN_UNDEF);
        return finish(g, 0, def_flags(fn), 0);
    }
    if ((fn->flags & FN_GENERATOR) && (fn->flags & FN_ASYNC)) {
        /* not yet: calling one throws */
        op32(g, OP_UNSUPPORTED, atom_const(g, atom_cstr(g->J, "async generators are not supported yet")));
        op(g, OP_RETURN_UNDEF);
        struct funcdef *d = finish(g, fn->name, def_flags(fn), fn->length);
        d->src_start = fn->start;
        d->src_end = fn->end;
        return d;
    }
    if (fn->flags & FN_ASYNC) {                 /* (27r) its parameters' errors reject its promise */
        op(g, OP_INITIAL_YIELD);
        emit1(g, 1);
    }
    for (int i = 0; i < fn->nparams; i++) {
        struct node *p = fn->params[i];
        if (p->type == N_REST) {
            op16(g, OP_REST, (unsigned)i);
            assign_pattern(g, p->a, STORE_INIT);
        } else if (p->type == N_ASSIGN_PATTERN) {
            op16(g, OP_ARG, (unsigned)i);
            assign_default(g, p->b, p->a);
            assign_pattern(g, p->a, STORE_INIT);
        } else {
            op16(g, OP_ARG, (unsigned)i);
            assign_pattern(g, p, STORE_INIT);
        }
    }
    if ((fn->flags & FN_CLASS_CTOR) && !(fn->flags & FN_DERIVED)) {
        get_binding_raw(g, find_local(s, g->J->atom[A_this]));
        get_binding_raw(g, find_local(s, g->J->atom[A_func]));
        op(g, OP_INIT_FIELDS);
    }
    if ((fn->flags & FN_GENERATOR) && !(fn->flags & FN_ASYNC)) {
        op(g, OP_INITIAL_YIELD);                /* (27r) after its parameters: the generator is returned */
        emit1(g, 0);
    }
    if (fn->expr) {
        gen_expr(g, fn->expr);
        op(g, OP_RETURN);
    } else {
        for (int i = 0; i < fn->nbody; i++)
            gen_stmt(g, fn->body[i]);
        if (fn->flags & FN_DERIVED) {
            op(g, OP_UNDEF);
            get_binding_raw(g, find_local(s, g->J->atom[A_this]));
            op(g, OP_CHECK_DERIVED_RETURN);
            op(g, OP_RETURN);
        } else
            op(g, OP_RETURN_UNDEF);
    }
    struct funcdef *d = finish(g, fn->name, def_flags(fn), fn->length);
    d->src_start = fn->start;
    d->src_end = fn->end;
    if (fn->flags & FN_CLASS_CTOR && class_node) {
        d->src_start = class_node->pos;
        d->src_end = class_node->end;
    }
    return d;
}

static struct funcdef *gen_script(struct gen *g, struct node *script, struct script_decls *decls)
{
    struct cfunc *fn = g->fn;
    g->strict = (fn->flags & FN_STRICT) != 0;
    struct cscope *s = fn->scope;
    g->scope = s;
    g->completion = new_reg(g);
    /* global declarations: lexical names first (they check for clashes), then vars and functions */
    for (int i = 0; i < decls->nlex; i++) {
        op(g, OP_DECLARE_GLOBAL_LEX);
        emit32(g, atom_const(g, decls->lex[i]));
        emit1(g, decls->lex_const[i]);
    }
    for (int i = 0; i < decls->nvars; i++)
        op_atom(g, OP_DEFINE_GLOBAL_VAR, decls->vars[i]);
    for (int i = 0; i < decls->nfuncs; i++) {
        struct node *f = decls->funcs[i];
        closure(g, f->a);
        op_atom(g, OP_DEFINE_GLOBAL_FUNC, f->str);
    }
    for (int i = 0; i < script->n; i++)
        gen_stmt(g, script->list[i]);
    op16(g, OP_GET_REG, (unsigned)g->completion);
    op(g, OP_RETURN);
    return finish(g, 0, FD_SCRIPT | (g->strict ? FD_STRICT : 0), 0);
}

/* --- entry points ------------------------------------------------------ */

static void syntax_error(struct js *J, struct lexer *L, struct jsstring *file)
{
    const char *msg = L->error ? L->error : "Syntax error";
    jsval e = error_create(J, J->proto[P_SYNTAX_ERROR], str_val(str_from_utf8(J, msg, strlen(msg))));
    if (js_is_object(e)) {
        struct jsobject *o = to_obj(e);
        o->u.err.file = file;
        o->u.err.line = L->error_line;
        o->h.flags |= OBJ_ERROR_DATA;
    }
    js_throw(J, e);
}

struct funcdef *js_compile(struct js *J, const char *text, size_t len, const char *file, bool eval_code)
{
    J->gc_inhibit++;
    struct jsstring *src = str_flat(J, str_from_utf8(J, text, len));
    struct jsstring *fname = str_from_ascii(J, file ? file : "");
    jmp_buf fail;
    struct lexer *L = calloc(1, sizeof *L);
    struct resolver *R = calloc(1, sizeof *R);
    struct gen *g = calloc(1, sizeof *g);
    struct funcdef *volatile result = 0;
    if (!L || !R || !g)
        abort();
    if (setjmp(fail)) {
        free(g->code);
        free(g->consts);
        free(g->lines);
        syntax_error(J, L, fname);
        goto done;
    }
    (void)eval_code;
    lex_init(L, J, src->chars, str_len(src), &fail);
    struct node *script = parse_script(L, false);
    struct script_decls *decls = arena_new(sizeof *decls);
    R->J = J;
    R->L = L;
    R->script = decls;
    struct cfunc *fn = arena_new(sizeof *fn);
    fn->flags = FN_SCRIPT | ((script->flags & 1) ? FN_STRICT : 0);
    fn->scope = new_scope(0, fn, S_SCRIPT);
    L->strict = (fn->flags & FN_STRICT) != 0;
    hoist_list(R, script->list, script->n, fn->scope, true);
    declare_lexical(R, script->list, script->n, fn->scope, true);
    resolve_list(R, script->list, script->n, fn->scope);
    g->J = J;
    g->R = R;
    g->fn = fn;
    g->file = fname;
    g->source = src;
    g->last_line = -1;
    result = gen_script(g, script, decls);
    result->src_start = 0;
    result->src_end = str_len(src);
done:
    arena_free_all();
    free(L->error);
    free(L);
    free(R);
    free(g);
    J->gc_inhibit--;
    return result;
}

struct funcdef *js_compile_function(struct js *J, struct jsstring *params, struct jsstring *body, int kind)
{
    J->gc_inhibit++;
    struct sbuf b = { 0 };
    sb_ascii(&b, "function anonymous(");
    sb_str(J, &b, params);
    sb_ascii(&b, "\n) {\n");
    sb_str(J, &b, body);
    sb_ascii(&b, "\n}");
    struct jsstring *src = sb_finish(J, &b);
    struct funcdef *volatile result = 0;
    if (!src) {
        J->gc_inhibit--;
        return 0;
    }
    jmp_buf fail;
    struct lexer *L = calloc(1, sizeof *L);
    struct resolver *R = calloc(1, sizeof *R);
    struct gen *g = calloc(1, sizeof *g);
    if (!L || !R || !g)
        abort();
    struct jsstring *fname = str_from_ascii(J, "");
    if (setjmp(fail)) {
        syntax_error(J, L, fname);
        goto done;
    }
    lex_init(L, J, src->chars, str_len(src), &fail);
    struct node *f = parse_function_text(L, kind);
    R->J = J;
    R->L = L;
    struct script_decls *decls = arena_new(sizeof *decls);
    R->script = decls;
    struct cfunc *top = arena_new(sizeof *top);
    top->flags = FN_SCRIPT;
    top->scope = new_scope(0, top, S_SCRIPT);
    resolve_function(R, f, top->scope);
    g->J = J;
    g->R = R;
    g->fn = top;
    g->file = fname;
    g->source = src;
    g->completion = -1;
    result = gen_function(g, f->fn, 0);
    result->src_start = 0;
    result->src_end = str_len(src);
done:
    arena_free_all();
    free(L->error);
    free(L);
    free(R);
    free(g->code);
    free(g->consts);
    free(g->lines);
    free(g);
    J->gc_inhibit--;
    return result;
}
