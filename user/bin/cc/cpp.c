/*
 * The preprocessor (60a): directives, conditional groups and macro
 * expansion on a list of tokens (translation phase 4). Expansion follows
 * Dave Prosser's algorithm, the one behind C's own rules (6.10.3.4): every
 * token carries a "hide set", the macros it came out of, and a name in its
 * own hide set is not expanded again. An object-like macro's result gets
 * the name token's set plus the macro; a function-like one's the
 * intersection of the name's and the closing parenthesis's sets plus the
 * macro. Arguments are expanded on their own first, except next to # and ##.
 *
 * Included files are read, tokenized and put in front of the rest of the
 * input when their #include is reached, so the list is walked once.
 */
#include <time.h>
#include "cc.h"
#include "type.h"

struct hideset {
    struct hideset *next;
    const char *name;
    int len;
};

struct macro_param {
    struct macro_param *next;
    struct token *name;
};

struct macro;
typedef struct token *builtin_macro(struct token *at);

struct macro {
    struct macro *next;                         /* (hash chain) */
    char *name;
    bool function_like;
    struct macro_param *params;
    bool variadic;
    char *va_name;                              /* "__VA_ARGS__", or GNU's NAME... */
    struct token *body;                         /* ends with an EOF token */
    builtin_macro *builtin;
    bool deleted;
};

struct macro_arg {
    struct macro_arg *next;
    char *name;
    struct token *tokens;                       /* as written, ending with an EOF token */
    struct token *expanded;                     /* (made when first needed) */
    bool is_va;
};

struct cond {
    struct cond *outer;
    enum { IN_THEN, IN_ELIF, IN_ELSE } ctx;
    struct token *where;
    bool taken;                                 /* a group of this #if was included */
};

struct include_dir {
    struct include_dir *next;
    char *path;
};

static struct macro *macros[1024];
static struct cond *conds;
static struct include_dir *user_dirs, *system_dirs;
static char **once_files;
static int once_count;
static int counter;
struct buffer cpp_dependencies;
bool cpp_dependencies_all;                      /* (-MD: the system's headers too; -MMD not) */
static bool found_in_system;                    /* (the last find_include's: a system folder) */

/* --- small helpers ------------------------------------------------------- */

static unsigned name_hash(const char *s, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h & 1023;
}

static struct macro *find_macro(struct token *t)
{
    if (t->kind != TK_IDENT)
        return 0;
    for (struct macro *m = macros[name_hash(t->text, t->len)]; m; m = m->next)
        if ((int)strlen(m->name) == t->len && memcmp(m->name, t->text, (size_t)t->len) == 0)
            return m->deleted ? 0 : m;
    return 0;
}

static struct macro *add_macro(const char *name, bool function_like, struct token *body)
{
    unsigned h = name_hash(name, (int)strlen(name));
    struct macro *m = arena_alloc(sizeof(*m));
    m->name = arena_strndup(name, strlen(name));
    m->function_like = function_like;
    m->body = body;
    m->next = macros[h];
    macros[h] = m;
    return m;
}

static struct token *new_eof(struct token *like)
{
    struct token *t = token_copy(like);
    t->kind = TK_EOF;
    t->len = 0;
    return t;
}

static bool in_hideset(struct hideset *hs, struct token *t)
{
    for (; hs; hs = hs->next)
        if (hs->len == t->len && memcmp(hs->name, t->text, (size_t)t->len) == 0)
            return true;
    return false;
}

static struct hideset *hideset_add(struct hideset *hs, const char *name)
{
    struct hideset *h = arena_alloc(sizeof(*h));
    h->name = name;
    h->len = (int)strlen(name);
    h->next = hs;
    return h;
}

static struct hideset *hideset_union(struct hideset *a, struct hideset *b)
{
    struct hideset head = { 0 }, *cur = &head;
    for (; a; a = a->next) {
        cur = cur->next = arena_alloc(sizeof(*cur));
        cur->name = a->name;
        cur->len = a->len;
    }
    cur->next = b;
    return head.next;
}

static struct hideset *hideset_intersection(struct hideset *a, struct hideset *b)
{
    struct hideset head = { 0 }, *cur = &head;
    for (; a; a = a->next) {
        bool found = false;
        for (struct hideset *x = b; x && !found; x = x->next)
            found = x->len == a->len && memcmp(x->name, a->name, (size_t)a->len) == 0;
        if (found) {
            cur = cur->next = arena_alloc(sizeof(*cur));
            cur->name = a->name;
            cur->len = a->len;
        }
    }
    return head.next;
}

/* Copies of a list's tokens (up to its EOF) with hs added, followed by tail. */
static struct token *add_hideset(struct token *t, struct hideset *hs)
{
    struct token head = { 0 }, *cur = &head;
    for (; t; t = t->next) {
        cur = cur->next = token_copy(t);
        cur->hideset = hideset_union(cur->hideset, hs);
    }
    return head.next;
}


static struct token *skip_line(struct token *t)
{
    if (t->at_line_start)
        return t;
    warn_at(t, "extra tokens at end of directive");
    while (!t->at_line_start)
        t = t->next;
    return t;
}

/* The rest of a directive's line, ending with an EOF token. */
static struct token *copy_line(struct token **rest, struct token *t)
{
    struct token head = { 0 }, *cur = &head;
    for (; !t->at_line_start; t = t->next)
        cur = cur->next = token_copy(t);
    cur->next = new_eof(t);
    *rest = t;
    return head.next;
}

/* --- stringizing and pasting --------------------------------------------- */

static char *spell(struct token *t, bool quote_literals)
{
    struct buffer b = { 0 };
    for (struct token *x = t; x && x->kind != TK_EOF; x = x->next) {
        if (x != t && x->space_before)
            buf_byte(&b, ' ');
        if (quote_literals && (x->kind == TK_STR || x->kind == TK_CHAR)) {
            for (int i = 0; i < x->len; i++) {
                if (x->text[i] == '\\' || x->text[i] == '"')
                    buf_byte(&b, '\\');
                buf_byte(&b, x->text[i]);
            }
        } else
            buf_add(&b, x->text, (size_t)x->len);
    }
    char *s = arena_strndup(b.data ? b.data : "", b.len);
    free(b.data);
    return s;
}

/* #arg: a string literal of the argument's spelling (6.10.3.2). */
static struct token *stringize(struct token *hash, struct token *arg)
{
    char *s = spell(arg, true);
    char *quoted = format("\"%s\"", s);
    struct token *t = tokenize_text(hash->src ? hash->src->name : "", quoted);
    t->src = hash->src;
    t->line = hash->line;
    t->col = hash->col;
    t->space_before = hash->space_before;
    t->next = 0;
    return t;
}

static struct token *paste(struct token *a, struct token *b)
{
    char *text = format("%.*s%.*s", a->len, a->text, b->len, b->text);
    struct token *t = tokenize_text(a->src ? a->src->name : "", text);
    if (t->next->kind != TK_EOF)
        error_at(a, "pasting \"%.*s\" and \"%.*s\" does not give a valid preprocessing token",
                 a->len, a->text, b->len, b->text);
    t->src = a->src;
    t->line = a->line;
    t->col = a->col;
    t->space_before = a->space_before;
    t->at_line_start = false;
    t->hideset = a->hideset;
    t->origin = a->origin;
    t->next = 0;
    return t;
}

/* --- macro definitions --------------------------------------------------- */

static struct token *read_params(struct token **rest, struct token *t, struct macro *m)
{
    struct macro_param head = { 0 }, *cur = &head;
    while (!token_is(t, ")")) {
        if (cur != &head) {
            if (!token_is(t, ","))
                error_at(t, "expected ',' or ')' in macro parameter list");
            t = t->next;
        }
        if (token_is(t, "...")) {
            m->variadic = true;
            m->va_name = "__VA_ARGS__";
            t = t->next;
            if (!token_is(t, ")"))
                error_at(t, "expected ')' after '...'");
            break;
        }
        if (t->kind != TK_IDENT)
            error_at(t, "expected a parameter name");
        if (token_is(t->next, "...")) {
            /* GNU: NAME... names the variable arguments */
            m->variadic = true;
            m->va_name = token_text(t);
            t = t->next->next;
            if (!token_is(t, ")"))
                error_at(t, "expected ')' after '...'");
            break;
        }
        cur = cur->next = arena_alloc(sizeof(*cur));
        cur->name = t;
        t = t->next;
    }
    *rest = t->next;
    return (struct token *)head.next;
}

static bool same_body(struct token *a, struct token *b)
{
    for (bool first = true; a->kind != TK_EOF && b->kind != TK_EOF; a = a->next, b = b->next, first = false)
        if (a->len != b->len || memcmp(a->text, b->text, (size_t)a->len) != 0 ||
            (!first && a->space_before != b->space_before))
            return false;
    return a->kind == TK_EOF && b->kind == TK_EOF;
}

static void define_macro(struct token **rest, struct token *t)
{
    if (t->kind != TK_IDENT)
        error_at(t, "macro names must be identifiers");
    char *name = token_text(t);
    if (strcmp(name, "defined") == 0)
        error_at(t, "'defined' cannot be used as a macro name");
    struct token *name_token = t;
    t = t->next;
    struct macro tmp = { 0 };
    bool function_like = false;
    if (token_is(t, "(") && !t->space_before && !t->at_line_start) {
        function_like = true;
        tmp.params = (struct macro_param *)read_params(&t, t->next, &tmp);
    }
    struct token *body = copy_line(rest, t);
    if (body->kind != TK_EOF)
        body->space_before = false;
    struct macro *old = find_macro(name_token);
    if (old && !old->builtin && !same_body(old->body, body))
        warn_at(name_token, "'%s' macro redefined", name);
    struct macro *m = add_macro(name, function_like, body);
    m->params = tmp.params;
    m->variadic = tmp.variadic;
    m->va_name = tmp.va_name;
}

static void undefine(const char *name)
{
    struct token t = { .kind = TK_IDENT, .text = name, .len = (int)strlen(name) };
    struct macro *m = find_macro(&t);
    if (m)
        m->deleted = true;
}

void cpp_define(const char *def)
{
    const char *eq = strchr(def, '=');
    char *text = eq ? format("%.*s %s", (int)(eq - def), def, eq + 1) : format("%s 1", def);
    struct token *t = tokenize_text("<command line>", text);
    struct token *rest;
    define_macro(&rest, t);
}

void cpp_undefine(const char *name)
{
    undefine(name);
}

/* --- macro expansion ----------------------------------------------------- */

static struct token *expand_list(struct token *t);

static struct macro_arg *read_arg(struct token **rest, struct token *t, bool read_rest)
{
    struct token head = { 0 }, *cur = &head;
    int depth = 0;
    for (;;) {
        if (depth == 0 && token_is(t, ")"))
            break;
        if (depth == 0 && !read_rest && token_is(t, ","))
            break;
        if (t->kind == TK_EOF)
            error_at(t, "unterminated argument list invoking macro");
        if (token_is(t, "("))
            depth++;
        else if (token_is(t, ")"))
            depth--;
        cur = cur->next = token_copy(t);
        t = t->next;
    }
    cur->next = new_eof(t);
    struct macro_arg *arg = arena_alloc(sizeof(*arg));
    arg->tokens = head.next;
    *rest = t;
    return arg;
}

static struct macro_arg *read_args(struct token **rest, struct token *t, struct macro *m)
{
    struct token *start = t;
    t = t->next->next;                          /* (after NAME and '(') */
    struct macro_arg head = { 0 }, *cur = &head;
    struct macro_param *p = m->params;
    for (; p; p = p->next) {
        if (cur != &head) {
            if (!token_is(t, ","))
                error_at(start, "macro '%s' requires more arguments", m->name);
            t = t->next;
        }
        cur = cur->next = read_arg(&t, t, false);
        cur->name = token_text(p->name);
    }
    if (m->variadic) {
        struct macro_arg *va;
        if (token_is(t, ")")) {
            va = arena_alloc(sizeof(*va));
            va->tokens = new_eof(t);
        } else {
            if (m->params) {
                if (!token_is(t, ","))
                    error_at(start, "macro '%s' requires more arguments", m->name);
                t = t->next;
            }
            va = read_arg(&t, t, true);
        }
        va->name = m->va_name;
        va->is_va = true;
        cur = cur->next = va;
    } else if (!m->params && !token_is(t, ")")) {
        /* () may hold one empty argument only */
        error_at(start, "macro '%s' takes no arguments", m->name);
    }
    if (!token_is(t, ")"))
        error_at(start, "too many arguments for macro '%s'", m->name);
    *rest = t;                                  /* the closing parenthesis */
    return head.next;
}

static struct macro_arg *find_arg(struct macro_arg *args, struct token *t)
{
    if (t->kind != TK_IDENT)
        return 0;
    for (struct macro_arg *a = args; a; a = a->next)
        if ((int)strlen(a->name) == t->len && memcmp(a->name, t->text, (size_t)t->len) == 0)
            return a;
    return 0;
}

static struct token *expanded_arg(struct macro_arg *a)
{
    if (!a->expanded)
        a->expanded = expand_list(a->tokens);
    return a->expanded;
}

static bool va_args_empty(struct macro_arg *args)
{
    for (struct macro_arg *a = args; a; a = a->next)
        if (a->is_va)
            return a->tokens->kind == TK_EOF;
    return true;
}

/* __VA_OPT__(content): the content if the variable arguments are not empty
 * (C23, also taken by GCC and clang in C11). */
static struct token *va_opt_content(struct token **rest, struct token *t)
{
    t = t->next;
    if (!token_is(t, "("))
        error_at(t, "__VA_OPT__ must be followed by '('");
    struct token head = { 0 }, *cur = &head;
    int depth = 0;
    for (t = t->next; !(depth == 0 && token_is(t, ")")); t = t->next) {
        if (t->kind == TK_EOF)
            error_at(t, "unterminated __VA_OPT__");
        if (token_is(t, "("))
            depth++;
        else if (token_is(t, ")"))
            depth--;
        cur = cur->next = token_copy(t);
    }
    cur->next = new_eof(t);
    *rest = t->next;
    return head.next;
}

/* Copies of a list's tokens (up to its EOF); the first takes the spacing
 * of the parameter it stands for. */
static struct token *copy_operand(struct token *x, struct token *param, struct token **last)
{
    struct token head = { 0 }, *cur = &head;
    for (; x->kind != TK_EOF; x = x->next) {
        cur = cur->next = token_copy(x);
        if (cur == head.next)
            cur->space_before = param->space_before;
    }
    *last = cur == &head ? 0 : cur;
    return head.next;
}

/* The macro's body with the arguments put in (6.10.3.1-6.10.3.3). Each
 * operand (a token, a parameter, or # and a parameter) gives a list,
 * maybe empty; "A ## B" pastes the last token of A's to the first of B's,
 * and an empty one is C's placemarker: the other side stays as it is. */
static struct token *substitute(struct macro *m, struct macro_arg *args, struct token *use)
{
    struct token head = { 0 }, *cur = &head;
    struct token *t = m->body;
    bool pending = false, left_has = false;
    if (token_is(t, "##"))
        error_at(t, "'##' cannot appear at either end of a macro expansion");
    while (t->kind != TK_EOF) {
        struct token *list = 0, *last = 0;
        if (token_is(t, "#") && m->function_like) {
            struct macro_arg *a = find_arg(args, t->next);
            if (!a)
                error_at(t->next, "'#' is not followed by a macro parameter");
            list = last = stringize(t, a->tokens);
            t = t->next->next;
        } else if (token_is(t, ",") && token_is(t->next, "##") && m->variadic &&
                   find_arg(args, t->next->next) && find_arg(args, t->next->next)->is_va) {
            /* GNU's  , ## __VA_ARGS__  : the comma goes when there are none,
             * and nothing is pasted */
            struct macro_arg *a = find_arg(args, t->next->next);
            if (a->tokens->kind != TK_EOF) {
                cur = cur->next = token_copy(t);
                for (list = copy_operand(a->tokens, t->next->next, &last); list; list = list->next)
                    cur = cur->next = list;
            }
            t = t->next->next->next;
            left_has = a->tokens->kind != TK_EOF;
            pending = false;
            if (token_is(t, "##")) {
                pending = true;
                t = t->next;
            }
            continue;
        } else if (token_is(t, "__VA_OPT__") && m->variadic) {
            struct token *content = va_opt_content(&t, t);
            if (!va_args_empty(args)) {
                /* (the content is substituted like a body of its own) */
                struct macro sub = *m;
                sub.body = content;
                list = substitute(&sub, args, use);
                for (last = list; last && last->next; last = last->next)
                    ;
            }
        } else {
            struct macro_arg *a = m->function_like ? find_arg(args, t) : 0;
            if (a) {
                bool raw = pending || token_is(t->next, "##");
                list = copy_operand(raw ? a->tokens : expanded_arg(a), t, &last);
            } else
                list = last = token_copy(t);
            t = t->next;
        }
        if (pending && left_has && list) {
            struct token *rest = list->next;
            *cur = *paste(cur, list);
            cur->next = 0;
            if (rest) {
                cur->next = rest;
                cur = last;
            }
        } else if (list) {
            cur->next = list;
            cur = last;
        }
        left_has = pending ? (left_has || list) : list != 0;
        pending = false;
        if (token_is(t, "##")) {
            if (t->next->kind == TK_EOF)
                error_at(t, "'##' cannot appear at either end of a macro expansion");
            pending = true;
            t = t->next;
        }
    }
    cur->next = 0;
    return head.next;
}

/* Expands the macro use at t, if it is one: *rest is then the expansion
 * followed by what came after the use. */
static bool expand_macro(struct token **rest, struct token *t)
{
    if (in_hideset(t->hideset, t))
        return false;
    struct macro *m = find_macro(t);
    if (!m)
        return false;
    if (m->builtin) {
        struct token *r = m->builtin(t);
        r->next = t->next;
        *rest = r;
        return true;
    }
    struct token *root = t;
    while (root->origin)
        root = root->origin;
    if (!m->function_like) {
        struct hideset *hs = hideset_add(t->hideset, m->name);
        struct token *body = add_hideset(substitute(m, 0, t), hs);
        if (!body) {
            /* (an empty expansion leaves its spacing to the next token) */
            *rest = t->next;
            if (t->next->kind != TK_EOF && !t->next->at_line_start)
                t->next->space_before |= t->space_before;
            return true;
        }
        struct token *last = body;
        for (struct token *x = body; x; x = x->next) {
            x->origin = root;
            last = x;
        }
        body->at_line_start = t->at_line_start;
        body->space_before = t->space_before;
        last->next = t->next;
        *rest = body;
        return true;
    }
    /* A function-like macro's name without '(' is just a name. */
    if (!token_is(t->next, "("))
        return false;
    struct token *rparen;
    struct macro_arg *args = read_args(&rparen, t, m);
    struct hideset *hs = hideset_add(hideset_intersection(t->hideset, rparen->hideset), m->name);
    struct token *body = add_hideset(substitute(m, args, t), hs);
    if (!body) {
        *rest = rparen->next;
        if (rparen->next->kind != TK_EOF && !rparen->next->at_line_start)
            rparen->next->space_before |= t->space_before;
        return true;
    }
    struct token *last = body;
    for (struct token *x = body; x; x = x->next) {
        x->origin = root;
        x->at_line_start = false;
        last = x;
    }
    body->at_line_start = t->at_line_start;
    body->space_before = t->space_before;
    last->next = rparen->next;
    *rest = body;
    return true;
}

/* A list (ending with EOF) with every macro expanded: for arguments and
 * #if lines. Directives cannot occur in them. */
static struct token *expand_list(struct token *t)
{
    struct token head = { 0 }, *cur = &head;
    while (t->kind != TK_EOF) {
        if (expand_macro(&t, t))
            continue;
        cur = cur->next = token_copy(t);
        t = t->next;
    }
    cur->next = t;
    return head.next;
}

/* --- #include ------------------------------------------------------------ */

void cpp_add_include(const char *dir, bool system)
{
    struct include_dir *d = arena_alloc(sizeof(*d)), **list = system ? &system_dirs : &user_dirs;
    d->path = arena_strndup(dir, strlen(dir));
    while (*list)
        list = &(*list)->next;
    *list = d;
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != 0;
}

static char *directory_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? arena_strndup(path, (size_t)(slash - path)) : ".";
}

static char *find_include(const char *name, bool quoted, struct token *where)
{
    found_in_system = false;
    if (name[0] == '/')
        return file_exists(name) ? (char *)name : 0;
    if (quoted) {
        char *p = format("%s/%s", directory_of(where->src->name), name);
        if (strncmp(p, "./", 2) == 0)
            p += 2;
        if (file_exists(p))
            return p;
    }
    for (int pass = 0; pass < 2; pass++)
        for (struct include_dir *d = pass ? system_dirs : user_dirs; d; d = d->next) {
            char *p = format("%s/%s", d->path, name);
            if (file_exists(p)) {
                found_in_system = pass == 1;
                return p;
            }
        }
    return 0;
}

static bool included_once(const char *path)
{
    for (int i = 0; i < once_count; i++)
        if (strcmp(once_files[i], path) == 0)
            return true;
    return false;
}

static int include_depth;

/* The file's tokens followed by tail. */
static struct token *include_file(struct token *tail, char *path, struct token *where)
{
    if (included_once(path))
        return tail;
    if (++include_depth > 200)
        error_at(where, "#include nested too deeply");
    struct source *s = source_read(path);
    if (!s)
        error_at(where, "cannot read '%s'", path);
    if (cpp_dependencies_all || !found_in_system)        /* (63g: -MD, -MMD) */
        buf_printf(&cpp_dependencies, " %s", path);
    struct token *t = tokenize(s);
    if (t->kind == TK_EOF)
        return tail;
    struct token *last = t;
    while (last->next->kind != TK_EOF)
        last = last->next;
    /* The file's EOF stays, marked, so -E's line markers see the return
     * and the include depth goes down; preprocess skips it. */
    struct token *end = last->next;
    end->kind = TK_OTHER;
    end->len = 0;
    end->value = -1;                            /* (the end of an included file) */
    end->next = tail;
    end->at_line_start = true;
    return t;
}

static char *read_include_name(struct token **rest, struct token *t, bool *quoted)
{
    if (t->kind == TK_STR && !t->at_line_start && t->text[0] == '"') {
        *quoted = true;
        *rest = skip_line(t->next);
        return arena_strndup(t->text + 1, (size_t)t->len - 2);
    }
    if (token_is(t, "<") && !t->at_line_start) {
        struct token *start = t;
        for (t = t->next; !token_is(t, ">"); t = t->next)
            if (t->at_line_start || t->kind == TK_EOF)
                error_at(start, "expected '>'");
        *quoted = false;
        struct token *end = t;
        struct buffer b = { 0 };
        for (struct token *x = start->next; x != end; x = x->next) {
            if (x != start->next && x->space_before)
                buf_byte(&b, ' ');
            buf_add(&b, x->text, (size_t)x->len);
        }
        *rest = skip_line(t->next);
        char *s = arena_strndup(b.data ? b.data : "", b.len);
        free(b.data);
        return s;
    }
    /* #include MACRO: the line expanded, then one of the forms above */
    if (t->kind == TK_IDENT && !t->at_line_start) {
        struct token *line = copy_line(rest, t);
        line = expand_list(line);
        line->at_line_start = false;
        struct token *end = line;
        while (end->kind != TK_EOF)
            end = end->next;
        end->at_line_start = true;
        struct token *ignored;
        return read_include_name(&ignored, line, quoted);
    }
    error_at(t, "expected \"FILENAME\" or <FILENAME>");
}

/* --- #if ----------------------------------------------------------------- */

struct pp_value {
    uint64_t v;
    bool is_unsigned;
};

static struct token *expr_at;
static int not_taken;                           /* (inside a side that is not evaluated) */

static struct pp_value pp_conditional(void);

static struct token *expect_punct(struct token *t, const char *s)
{
    if (!token_is(t, s))
        error_at(t, "expected '%s' in preprocessor expression", s);
    return t->next;
}

static struct pp_value pp_number(struct token *t)
{
    struct token c = *t;
    c.next = 0;
    convert_pp_tokens(&c);
    if (c.type && (c.type->kind == TY_FLOAT || c.type->kind == TY_DOUBLE || c.type->kind == TY_LDOUBLE))
        error_at(t, "floating constant in preprocessor expression");
    return (struct pp_value){ (uint64_t)c.value, t->kind == TK_PPNUM && c.type && c.type->is_unsigned };
}

static struct pp_value pp_primary(void)
{
    struct token *t = expr_at;
    if (token_is(t, "(")) {
        expr_at = t->next;
        struct pp_value v = pp_conditional();
        expr_at = expect_punct(expr_at, ")");
        return v;
    }
    if (t->kind == TK_PPNUM || t->kind == TK_CHAR) {
        expr_at = t->next;
        return pp_number(t);
    }
    if (t->kind == TK_IDENT) {
        /* (names left after expansion are 0) */
        expr_at = t->next;
        return (struct pp_value){ 0, false };
    }
    error_at(t, "invalid token at start of a preprocessor expression");
}

static struct pp_value pp_unary(void)
{
    struct token *t = expr_at;
    if (token_is(t, "+") || token_is(t, "-") || token_is(t, "~") || token_is(t, "!")) {
        expr_at = t->next;
        struct pp_value v = pp_unary();
        switch (t->text[0]) {
        case '-': v.v = -v.v; break;
        case '~': v.v = ~v.v; break;
        case '!': v.v = !v.v; v.is_unsigned = false; break;
        }
        return v;
    }
    return pp_primary();
}

static int precedence(struct token *t)
{
    static const struct {
        const char *op;
        int prec;
    } ops[] = {
        { "*", 10 }, { "/", 10 }, { "%", 10 }, { "+", 9 }, { "-", 9 }, { "<<", 8 }, { ">>", 8 },
        { "<", 7 }, { ">", 7 }, { "<=", 7 }, { ">=", 7 }, { "==", 6 }, { "!=", 6 }, { "&", 5 },
        { "^", 4 }, { "|", 3 }, { "&&", 2 }, { "||", 1 },
    };
    if (t->kind != TK_PUNCT)
        return 0;
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (token_is(t, ops[i].op))
            return ops[i].prec;
    return 0;
}

/* Binary operators by precedence climbing, on intmax_t or uintmax_t
 * (6.10.1p4: both 64 bits here). && and || still evaluate both sides, but
 * a division by zero on the side not taken is no error. */
static struct pp_value pp_binary(int min_prec)
{
    struct pp_value a = pp_unary();
    for (;;) {
        struct token *op = expr_at;
        int prec = precedence(op);
        if (!prec || prec < min_prec)
            return a;
        expr_at = op->next;
        bool skip = (token_is(op, "&&") && !a.v) || (token_is(op, "||") && a.v);
        not_taken += skip;
        struct pp_value b = pp_binary(prec + 1);
        not_taken -= skip;
        bool u = a.is_unsigned || b.is_unsigned;
        struct pp_value r = { 0, u };
        int64_t sa = (int64_t)a.v, sb = (int64_t)b.v;
        if (token_is(op, "*"))
            r.v = a.v * b.v;
        else if (token_is(op, "/") || token_is(op, "%")) {
            if (b.v == 0) {
                if (!not_taken)
                    error_at(op, "division by zero in preprocessor expression");
                r.v = 0;
            } else if (u)
                r.v = op->text[0] == '/' ? a.v / b.v : a.v % b.v;
            else if (sb == -1)
                r.v = op->text[0] == '/' ? (uint64_t)0 - a.v : 0;
            else
                r.v = (uint64_t)(op->text[0] == '/' ? sa / sb : sa % sb);
        } else if (token_is(op, "+"))
            r.v = a.v + b.v;
        else if (token_is(op, "-"))
            r.v = a.v - b.v;
        else if (token_is(op, "<<")) {
            r.v = b.v >= 64 ? 0 : a.v << b.v;
            r.is_unsigned = a.is_unsigned;
        } else if (token_is(op, ">>")) {
            r.is_unsigned = a.is_unsigned;
            if (b.v >= 64)
                r.v = a.is_unsigned || sa >= 0 ? 0 : (uint64_t)-1;
            else
                r.v = a.is_unsigned ? a.v >> b.v : (uint64_t)(sa >> b.v);
        } else if (token_is(op, "<") || token_is(op, ">") || token_is(op, "<=") || token_is(op, ">=")) {
            bool lt = u ? a.v < b.v : sa < sb, gt = u ? a.v > b.v : sa > sb;
            if (token_is(op, "<"))
                r.v = lt;
            else if (token_is(op, ">"))
                r.v = gt;
            else if (token_is(op, "<="))
                r.v = !gt;
            else
                r.v = !lt;
            r.is_unsigned = false;
        } else if (token_is(op, "==")) {
            r.v = a.v == b.v;
            r.is_unsigned = false;
        } else if (token_is(op, "!=")) {
            r.v = a.v != b.v;
            r.is_unsigned = false;
        } else if (token_is(op, "&"))
            r.v = a.v & b.v;
        else if (token_is(op, "^"))
            r.v = a.v ^ b.v;
        else if (token_is(op, "|"))
            r.v = a.v | b.v;
        else if (token_is(op, "&&")) {
            r.v = a.v && b.v;
            r.is_unsigned = false;
        } else if (token_is(op, "||")) {
            r.v = a.v || b.v;
            r.is_unsigned = false;
        }
        a = r;
    }
}

static struct pp_value pp_conditional(void)
{
    struct pp_value c = pp_binary(1);
    if (!token_is(expr_at, "?"))
        return c;
    expr_at = expr_at->next;
    not_taken += !c.v;
    struct pp_value a = pp_conditional();
    not_taken -= !c.v;
    expr_at = expect_punct(expr_at, ":");
    not_taken += c.v != 0;
    struct pp_value b = pp_conditional();
    not_taken -= c.v != 0;
    struct pp_value r = c.v ? a : b;
    r.is_unsigned = a.is_unsigned || b.is_unsigned;
    return r;
}

static struct token *new_number(struct token *like, int value)
{
    struct token *t = token_copy(like);
    t->kind = TK_PPNUM;
    t->text = value ? "1" : "0";
    t->len = 1;
    return t;
}

/* The line after #if or #elif: defined and __has_include first, then
 * macros, then the expression. */
static bool eval_condition(struct token **rest, struct token *t)
{
    struct token *start = t;
    struct token *line = copy_line(rest, t);
    struct token head = { 0 }, *cur = &head;
    for (struct token *x = line; x->kind != TK_EOF; x = x->next) {
        if (token_is(x, "defined")) {
            struct token *d = x;
            x = x->next;
            bool paren = token_is(x, "(");
            if (paren)
                x = x->next;
            if (x->kind != TK_IDENT)
                error_at(d, "macro names must be identifiers");
            struct token *name = x;
            if (paren) {
                x = x->next;
                if (!token_is(x, ")"))
                    error_at(x, "missing ')' after 'defined'");
            }
            cur = cur->next = new_number(d, find_macro(name) != 0);
            continue;
        }
        if (token_is(x, "__has_include") || token_is(x, "__has_include_next")) {
            struct token *h = x;
            x = x->next;
            if (!token_is(x, "("))
                error_at(h, "missing '(' after '__has_include'");
            x = x->next;
            bool quoted;
            char *name;
            if (x->kind == TK_STR) {
                quoted = true;
                name = arena_strndup(x->text + 1, (size_t)x->len - 2);
            } else if (token_is(x, "<")) {
                struct buffer b = { 0 };
                for (x = x->next; !token_is(x, ">"); x = x->next) {
                    if (x->kind == TK_EOF)
                        error_at(h, "expected '>'");
                    buf_add(&b, x->text, (size_t)x->len);
                }
                quoted = false;
                name = arena_strndup(b.data ? b.data : "", b.len);
                free(b.data);
            } else
                error_at(x, "expected \"FILENAME\" or <FILENAME>");
            x = x->next;
            if (!token_is(x, ")"))
                error_at(x, "missing ')' after '__has_include'");
            cur = cur->next = new_number(h, find_include(name, quoted, h) != 0);
            continue;
        }
        cur = cur->next = token_copy(x);
    }
    cur->next = new_eof(start);
    struct token *expanded = expand_list(head.next);
    if (expanded->kind == TK_EOF)
        error_at(start, "#if with no expression");
    expr_at = expanded;
    struct pp_value v = pp_conditional();
    if (expr_at->kind != TK_EOF)
        error_at(expr_at, "extra tokens in preprocessor expression");
    return v.v != 0;
}

static void push_cond(struct token *where, bool taken)
{
    struct cond *c = arena_alloc(sizeof(*c));
    c->outer = conds;
    c->ctx = IN_THEN;
    c->where = where;
    c->taken = taken;
    conds = c;
}

/* Skips a group that is not included: to the #elif, #else or #endif that
 * belongs to it, over nested ones. */
static struct token *skip_group(struct token *t)
{
    int depth = 0;
    while (t->kind != TK_EOF) {
        if (t->at_line_start && token_is(t, "#")) {
            struct token *d = t->next;
            if (!d->at_line_start) {
                if (token_is(d, "if") || token_is(d, "ifdef") || token_is(d, "ifndef")) {
                    depth++;
                    t = d->next;
                    continue;
                }
                if (token_is(d, "endif")) {
                    if (depth == 0)
                        return t;
                    depth--;
                    t = d->next;
                    continue;
                }
                if (depth == 0 && (token_is(d, "elif") || token_is(d, "else")))
                    return t;
            }
        }
        if (t->kind == TK_OTHER && t->len == 0 && t->value == -1)
            error_at(conds->where, "unterminated conditional directive");
        t = t->next;
    }
    return t;
}

/* --- built-in macros ----------------------------------------------------- */

static struct token *string_token(struct token *at, const char *s)
{
    struct buffer b = { 0 };
    buf_byte(&b, '"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            buf_byte(&b, '\\');
        buf_byte(&b, *s);
    }
    buf_byte(&b, '"');
    struct token *t = token_copy(at);
    t->kind = TK_STR;
    t->text = arena_strndup(b.data, b.len);
    t->len = (int)b.len;
    free(b.data);
    return t;
}

static struct token *number_token(struct token *at, long n)
{
    struct token *t = token_copy(at);
    t->kind = TK_PPNUM;
    t->text = format("%ld", n);
    t->len = (int)strlen(t->text);
    return t;
}

static struct token *root_of(struct token *t)
{
    while (t->origin)
        t = t->origin;
    return t;
}

static struct token *macro_file(struct token *at)
{
    return string_token(at, root_of(at)->src->display_name);
}

static struct token *macro_line(struct token *at)
{
    struct token *r = root_of(at);
    return number_token(at, r->line + r->line_delta);
}

static struct token *macro_counter(struct token *at)
{
    return number_token(at, counter++);
}

static char date_text[16], time_text[16];

static struct token *macro_date(struct token *at)
{
    return string_token(at, date_text);
}

static struct token *macro_time(struct token *at)
{
    return string_token(at, time_text);
}

static void add_builtin(const char *name, builtin_macro *fn)
{
    struct macro *m = add_macro(name, false, 0);
    m->builtin = fn;
}

void cpp_predefine(void)
{
    static const char *common =
        "#define __STDC__ 1\n"
        "#define __STDC_VERSION__ 201112L\n"
        "#define __STDC_HOSTED__ 1\n"
        "#define __STDC_UTF_16__ 1\n"
        "#define __STDC_UTF_32__ 1\n"
        "#define __STDC_NO_COMPLEX__ 1\n"
        "#define __STDC_IEC_559__ 1\n"
        "#define __ELF__ 1\n"
        "#define __LP64__ 1\n"
        "#define _LP64 1\n"
        "#define __CHAR_BIT__ 8\n"
        "#define __SIZEOF_SHORT__ 2\n"
        "#define __SIZEOF_INT__ 4\n"
        "#define __SIZEOF_LONG__ 8\n"
        "#define __SIZEOF_LONG_LONG__ 8\n"
        "#define __SIZEOF_POINTER__ 8\n"
        "#define __SIZEOF_FLOAT__ 4\n"
        "#define __SIZEOF_DOUBLE__ 8\n"
        "#define __SIZEOF_LONG_DOUBLE__ 16\n"
        "#define __SIZEOF_SIZE_T__ 8\n"
        "#define __SIZEOF_WCHAR_T__ 4\n"
        "#define __SIZEOF_INT128__ 16\n"
        "#define __SIZE_TYPE__ unsigned long\n"
        "#define __PTRDIFF_TYPE__ long\n"
        "#define __INTMAX_TYPE__ long\n"
        "#define __UINTMAX_TYPE__ unsigned long\n"
        "#define __INTPTR_TYPE__ long\n"
        "#define __UINTPTR_TYPE__ unsigned long\n"
        "#define __CHAR16_TYPE__ unsigned short\n"
        "#define __CHAR32_TYPE__ unsigned int\n"
        "#define __WINT_TYPE__ int\n"
        "#define __ORDER_LITTLE_ENDIAN__ 1234\n"
        "#define __ORDER_BIG_ENDIAN__ 4321\n"
        "#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__\n"
        "#define __LITTLE_ENDIAN__ 1\n"
        "#define __ATOMIC_RELAXED 0\n"
        "#define __ATOMIC_CONSUME 1\n"
        "#define __ATOMIC_ACQUIRE 2\n"
        "#define __ATOMIC_RELEASE 3\n"
        "#define __ATOMIC_ACQ_REL 4\n"
        "#define __ATOMIC_SEQ_CST 5\n";
    static const char *x86 =
        "#define __x86_64__ 1\n"
        "#define __x86_64 1\n"
        "#define __amd64__ 1\n"
        "#define __amd64 1\n"
        "#define __WCHAR_TYPE__ int\n"
        "#define __LDBL_MANT_DIG__ 64\n";
    static const char *arm =
        "#define __aarch64__ 1\n"
        "#define __CHAR_UNSIGNED__ 1\n"
        "#define __WCHAR_TYPE__ unsigned int\n"
        "#define __WCHAR_UNSIGNED__ 1\n"
        "#define __LDBL_MANT_DIG__ 113\n";
    char *text = format("%s%s", common, target.arch == ARCH_X86_64 ? x86 : arm);
    struct token *t = tokenize_text("<built-in>", text);
    while (t->kind != TK_EOF) {
        /* (each line is "# define NAME BODY") */
        t = t->next->next;
        define_macro(&t, t);
    }
    add_builtin("__FILE__", macro_file);
    add_builtin("__LINE__", macro_line);
    add_builtin("__COUNTER__", macro_counter);
    add_builtin("__DATE__", macro_date);
    add_builtin("__TIME__", macro_time);
    time_t now = time(0);
    struct tm *tm = localtime(&now);
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    snprintf(date_text, sizeof(date_text), "%s %2d %d", months[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
    snprintf(time_text, sizeof(time_text), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* --- the directives ------------------------------------------------------ */

static void line_directive(struct token **rest, struct token *t)
{
    struct token *line = expand_list(copy_line(rest, t));
    if (line->kind != TK_PPNUM)
        error_at(t, "#line directive requires a positive integer argument");
    struct token c = *line;
    c.next = 0;
    convert_pp_tokens(&c);
    struct token *next_line = *rest;
    struct source *src = t->src;
    struct source *renamed = src;
    if (line->next->kind == TK_STR) {
        renamed = arena_alloc(sizeof(*renamed));
        *renamed = *src;
        renamed->display_name = arena_strndup(line->next->text + 1, (size_t)line->next->len - 2);
    }
    /* The following line has the number given: every later token of this
     * file gets the difference. */
    int delta = (int)c.value - next_line->line;
    for (struct token *x = next_line; x->src == src && x->kind != TK_EOF; x = x->next) {
        x->line_delta = delta;
        x->src = renamed;
    }
}

static void pragma(struct token **rest, struct token *t)
{
    if (token_is(t, "once") && !t->at_line_start) {
        char *path = t->src->name;
        once_files = realloc(once_files, sizeof(char *) * (size_t)(once_count + 1));
        once_files[once_count++] = path;
        *rest = skip_line(t->next);
        return;
    }
    /* Others (GCC diagnostics, pack) are ignored for now. */
    while (!t->at_line_start)
        t = t->next;
    *rest = t;
}

struct token *preprocess(struct token *t)
{
    struct token head = { 0 }, *cur = &head;
    while (t->kind != TK_EOF) {
        if (t->kind == TK_OTHER && t->len == 0 && t->value == -1) {
            /* the end of an included file */
            include_depth--;
            cur = cur->next = t;
            t = t->next;
            continue;
        }
        if (expand_macro(&t, t))
            continue;
        if (!(t->at_line_start && token_is(t, "#") && !t->origin)) {
            if (token_is(t, "_Pragma") && token_is(t->next, "(")) {
                /* _Pragma("...") (6.10.9): dropped, like most pragmas */
                struct token *p = t->next->next;
                if (p->kind != TK_STR || !token_is(p->next, ")"))
                    error_at(t, "_Pragma takes a parenthesized string literal");
                t = p->next->next;
                continue;
            }
            cur = cur->next = t;
            t = t->next;
            continue;
        }
        struct token *hash = t;
        t = t->next;
        if (t->at_line_start)
            continue;                           /* the null directive */
        if (token_is(t, "include") || token_is(t, "include_next")) {
            bool quoted;
            char *name = read_include_name(&t, t->next, &quoted);
            char *path = find_include(name, quoted, hash);
            if (!path)
                error_at(hash->next, "'%s' file not found", name);
            t = include_file(t, path, hash->next->next);
            continue;
        }
        if (token_is(t, "define")) {
            define_macro(&t, t->next);
            continue;
        }
        if (token_is(t, "undef")) {
            t = t->next;
            if (t->kind != TK_IDENT)
                error_at(t, "macro names must be identifiers");
            undefine(token_text(t));
            t = skip_line(t->next);
            continue;
        }
        if (token_is(t, "if")) {
            bool v = eval_condition(&t, t->next);
            push_cond(hash, v);
            if (!v)
                t = skip_group(t);
            continue;
        }
        if (token_is(t, "ifdef") || token_is(t, "ifndef")) {
            bool want = token_is(t, "ifdef");
            t = t->next;
            if (t->kind != TK_IDENT || t->at_line_start)
                error_at(t, "macro names must be identifiers");
            bool v = (find_macro(t) != 0) == want;
            push_cond(hash, v);
            t = skip_line(t->next);
            if (!v)
                t = skip_group(t);
            continue;
        }
        if (token_is(t, "elif")) {
            if (!conds || conds->ctx == IN_ELSE)
                error_at(hash, "#elif without #if");
            conds->ctx = IN_ELIF;
            if (conds->taken) {
                /* an earlier group was included: skip without evaluating */
                while (!t->at_line_start)
                    t = t->next;
                t = skip_group(t);
                continue;
            }
            bool v = eval_condition(&t, t->next);
            if (v)
                conds->taken = true;
            else
                t = skip_group(t);
            continue;
        }
        if (token_is(t, "else")) {
            if (!conds || conds->ctx == IN_ELSE)
                error_at(hash, "#else without #if");
            conds->ctx = IN_ELSE;
            t = skip_line(t->next);
            if (conds->taken)
                t = skip_group(t);
            else
                conds->taken = true;
            continue;
        }
        if (token_is(t, "endif")) {
            if (!conds)
                error_at(hash, "#endif without #if");
            conds = conds->outer;
            t = skip_line(t->next);
            continue;
        }
        if (token_is(t, "line")) {
            line_directive(&t, t->next);
            continue;
        }
        if (t->kind == TK_PPNUM) {
            /* GNU's line markers: # 12 "file" */
            line_directive(&t, t);
            continue;
        }
        if (token_is(t, "pragma")) {
            pragma(&t, t->next);
            continue;
        }
        if (token_is(t, "error")) {
            struct token *msg = copy_line(&t, t->next);
            error_at(hash, "#error %s", spell(msg, false));
        }
        if (token_is(t, "warning")) {
            struct token *msg = copy_line(&t, t->next);
            warn_at(hash, "#warning %s", spell(msg, false));
            continue;
        }
        if (token_is(t, "ident") || token_is(t, "sccs")) {
            while (!t->at_line_start)
                t = t->next;
            continue;
        }
        error_at(t, "invalid preprocessing directive #%.*s", t->len, t->text);
    }
    if (conds)
        error_at(conds->where, "unterminated conditional directive");
    cur->next = t;
    /* (the markers for included files' ends stay for -E; the parser's
     * conversion drops them) */
    return head.next;
}

/* -E: the tokens as text, a line of output for each source line, with GNU's
 * line markers where the file changes or lines were left out. */
/* (62e) assembly: the source's own spacing (".text", "0(%rdi)" stay as
 * written) and no line markers, as GNU's preprocessor for .S files */
void print_preprocessed(FILE *out, struct token *t, bool assembly)
{
    struct source *src = 0;
    int line = 0;
    bool line_empty = true;
    struct token *prev = 0;
    for (; t->kind != TK_EOF; t = t->next) {
        if (t->kind == TK_OTHER && t->len == 0 && t->value == -1)
            continue;
        struct token *r = root_of(t);
        int tl = r->line + r->line_delta;
        if (t->at_line_start || !src) {
            if (assembly && (r->src != src || tl < line)) {
                if (!line_empty)
                    fputc('\n', out);
                src = r->src;
                line = tl;
                line_empty = true;
            } else if (r->src != src || tl < line || (tl > line + 8 && !assembly)) {
                if (!line_empty)
                    fputc('\n', out);
                fprintf(out, "# %d \"%s\"\n", tl, r->src->display_name);
                src = r->src;
                line = tl;
                line_empty = true;
            } else if (tl > line || !line_empty) {
                if (!line_empty)
                    fputc('\n', out);
                for (; line + 1 < tl; line++)
                    fputc('\n', out);
                line = tl;
                line_empty = true;
            }
        }
        if (!line_empty && (t->space_before || t->at_line_start))
            fputc(' ', out);
        else if (!line_empty && prev && !assembly) {
            /* two tokens that would read as one need a space */
            char a = prev->text[prev->len - 1], b = t->text[0];
            bool word_a = a == '_' || (a >= '0' && a <= '9') || (a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z') || a == '.' || (unsigned char)a >= 0x80;
            bool word_b = b == '_' || (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || b == '.' || (unsigned char)b >= 0x80 || b == '"' || b == '\'';
            if ((word_a && word_b) || (prev->kind == TK_PUNCT && t->kind == TK_PUNCT) ||
                (prev->kind == TK_PPNUM && (b == '+' || b == '-')))
                fputc(' ', out);
        }
        fwrite(t->text, 1, (size_t)t->len, out);
        line_empty = false;
        prev = t;
    }
    if (!line_empty)
        fputc('\n', out);
}
