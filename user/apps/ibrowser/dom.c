/*
 * The document's objects for scripts (27p): the DOM over html.c's tree, in
 * a JavaScript world of RitterOS's engine (user/lib/js/) made for each page.
 *
 * Each node gets its object when a script first reaches it (node->js),
 * kept alive by one rooted array (`registry`) until the page is left: a
 * node is never freed while its page lives (one taken out of the tree by
 * a script waits in `detached`), so objects and the picture loader may
 * hold nodes. Collections (childNodes, children, getElementsBy...) are
 * arrays made when asked for, not live lists. ::before and ::after, which
 * css.c makes as children tagged "::before"/"::after", are not seen.
 *
 * <script> elements run in document order after the page is parsed (the
 * tree is the whole page by then); one that a script adds runs after the
 * one adding it. document.write puts its HTML after the running script.
 * Events (27q) follow the DOM standard's dispatch (below); timers are kept
 * here and run by the window program's timer thread (dom_run_timers). After
 * the scripts the window program styles and lays the page out again; a
 * script asking for geometry (getBoundingClientRect, offsetWidth, ...)
 * gets it from a layout made at that moment (dom_layout_hook).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "browser.h"
#include "../../lib/js/js.h"

static struct js *J;
static struct node *root;
static char page_url[URL_MAX];
static int view_w, view_h;
static jsval registry = JS_UNDEFINED;   /* every node's object made for this page */
static uint32_t registry_n;
static jsval roots[48];                 /* the prototypes and the like, rooted */
static struct node **detached;
static int detached_n, detached_cap;
static struct node *current_script;
static struct node docnode;             /* the document: a node whose child is <html> (root keeps no parent) */
static bool running;
static _Thread_local void *stack_base;  /* (27q: each thread entering scripts has its own) */

void (*dom_layout_hook)(void);
bool dom_changed;
bool dom_print;
int (*dom_fetch_hook)(const char *url, struct response *r);
char dom_navigate_to[URL_MAX];
bool dom_navigate_link;
int dom_scroll_y, dom_scroll_request = -1;
struct node *dom_focused;
void (*dom_focus_hook)(struct node *e);
void (*dom_control_hook)(struct node *e);
void (*dom_timers_hook)(void);
bool dom_submit_pending;
struct node *dom_submit_form, *dom_submitter;

static void script_error(const char *where);
static int depth;                       /* (27q) scripts running on the stack (a checkpoint at 0) */
static bool layout_fresh;               /* (27q) the boxes are the tree's as it is (no change since) */
static int ready_state;                 /* 0 loading, 1 interactive, 2 complete */
static char **wanted;                   /* the event types something listens to (dom_wants) */
static int wanted_n, wanted_cap;
static void note_type(const char *type);
static void note_attributes(struct node *top);

enum {
    R_NODE, R_ELEMENT, R_TEXT, R_DOCUMENT, R_FRAGMENT, R_NODELIST, R_TOKENS, R_STYLE, R_TARGET,
    R_DOCUMENT_OBJ, R_LOCATION, R_STORAGE_L, R_STORAGE_S, R_COMMENT,
    R_LISTENERS, R_HANDLERS, R_TIMERS, R_FUNCTION, R_FORM,     /* (27q) */
    R_REQUESTS,                                                 /* (27r) */
    R_EVENT, R_COUNT = R_EVENT + 11             /* (an Event prototype for each kind, EK_COUNT) */
};

static const struct js_host_class node_class = { "Node", 0, 0 };
static const struct js_host_class tokens_class = { "DOMTokenList", 0, 0 };
static const struct js_host_class style_class = { "CSSStyleDeclaration", 0, 0 };

#define ARGS struct js *J, jsval this_val, int argc, jsval *argv, jsval nt
#define UNUSED (void)J, (void)this_val, (void)argc, (void)argv, (void)nt

void dom_set_stack_base(void *p)
{
    stack_base = p;
}

/* --- nodes ---------------------------------------------------------------- */

static bool seen(const struct node *n)
{
    return n && n->tag[0] != ':';
}

static bool is_element(const struct node *n)
{
    return n && n->kind == NODE_ELEMENT && n->tag[0] != '#' && n->tag[0] != ':';
}

static bool is_fragment(const struct node *n)
{
    return n && n->kind == NODE_ELEMENT && !strcmp(n->tag, "#fragment");
}

static bool is_comment(const struct node *n)
{
    return n && n->kind == NODE_ELEMENT && !strcmp(n->tag, "#comment");
}

static struct node *first_seen(struct node *n)
{
    struct node *c = n->first;
    while (c && !seen(c))
        c = c->next;
    return c;
}

static struct node *last_seen(struct node *n)
{
    struct node *c = n->last;
    while (c && !seen(c))
        c = c->prev;
    return c;
}

static struct node *next_seen(struct node *n)
{
    struct node *c = n->next;
    while (c && !seen(c))
        c = c->next;
    return c;
}

static struct node *prev_seen(struct node *n)
{
    struct node *c = n->prev;
    while (c && !seen(c))
        c = c->prev;
    return c;
}

/* the next node in document order inside `top`, skipping generated ones */
static struct node *walk_next(struct node *n, struct node *top)
{
    struct node *c = first_seen(n);
    if (c)
        return c;
    while (n && n != top) {
        struct node *s = next_seen(n);
        if (s)
            return s;
        n = n->parent;
    }
    return 0;
}

static struct node *new_node(enum node_kind kind, const char *tag)
{
    struct node *n = calloc(1, sizeof(*n));
    if (!n)
        return 0;
    n->kind = kind;
    for (int i = 0; tag && tag[i] && i < (int)sizeof(n->tag) - 1; i++)
        n->tag[i] = (char)lower((unsigned char)tag[i]);
    return n;
}

static void keep_detached(struct node *n)
{
    for (int i = 0; i < detached_n; i++)
        if (detached[i] == n)
            return;
    if (detached_n == detached_cap) {
        int cap = detached_cap ? detached_cap * 2 : 64;
        struct node **d = realloc(detached, (size_t)cap * sizeof(*d));
        if (!d)
            return;
        detached = d;
        detached_cap = cap;
    }
    detached[detached_n++] = n;
}

static void forget_detached(struct node *n)
{
    for (int i = 0; i < detached_n; i++)
        if (detached[i] == n) {
            detached[i] = detached[--detached_n];
            return;
        }
}

static void unlink_node(struct node *n)
{
    struct node *p = n->parent;
    if (!p) {
        return;
    }
    if (n->prev)
        n->prev->next = n->next;
    else
        p->first = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        p->last = n->prev;
    n->parent = n->prev = n->next = 0;
    keep_detached(n);
    dom_changed = true, layout_fresh = false;
}

/* n (without a parent) into parent, before ref (0: at the end) */
static void link_before(struct node *parent, struct node *n, struct node *ref)
{
    forget_detached(n);
    n->parent = parent;
    n->next = ref;
    n->prev = ref ? ref->prev : parent->last;
    if (n->prev)
        n->prev->next = n;
    else
        parent->first = n;
    if (ref)
        ref->prev = n;
    else
        parent->last = n;
    dom_changed = true, layout_fresh = false;
}

static bool connected(const struct node *n)
{
    while (n && n->parent)
        n = n->parent;
    return n == root;
}

static void set_attr(struct node *e, const char *name, const char *value)
{
    for (int i = 0; i < e->attr_count; i++)
        if (!strcmp(e->attrs[i].name, name)) {
            char *v = strdup(value);
            if (!v)
                return;
            free(e->attrs[i].value);
            e->attrs[i].value = v;
            dom_changed = true, layout_fresh = false;
            return;
        }
    struct attr *a = realloc(e->attrs, sizeof(*a) * (size_t)(e->attr_count + 1));
    if (!a)
        return;
    e->attrs = a;
    a[e->attr_count].name = strdup(name);
    a[e->attr_count].value = strdup(value);
    if (!a[e->attr_count].name || !a[e->attr_count].value) {
        free(a[e->attr_count].name);
        free(a[e->attr_count].value);
        return;
    }
    e->attr_count++;
    dom_changed = true, layout_fresh = false;
}

static bool remove_attr(struct node *e, const char *name)
{
    for (int i = 0; i < e->attr_count; i++)
        if (!strcmp(e->attrs[i].name, name)) {
            free(e->attrs[i].name);
            free(e->attrs[i].value);
            e->attrs[i] = e->attrs[--e->attr_count];
            dom_changed = true, layout_fresh = false;
            return true;
        }
    return false;
}

static struct node *clone(const struct node *n, bool deep)
{
    struct node *c = new_node(n->kind, n->tag);
    if (!c)
        return 0;
    if (n->text)
        c->text = strdup(n->text);
    for (int i = 0; i < n->attr_count; i++)
        set_attr(c, n->attrs[i].name, n->attrs[i].value);
    c->script_state = n->script_state;
    if (deep)
        for (struct node *k = first_seen((struct node *)n); k; k = next_seen(k)) {
            struct node *kc = clone(k, true);
            if (kc)
                link_before(c, kc, 0);
        }
    keep_detached(c);
    return c;
}

/* All the text inside (textContent). */
static void text_into(const struct node *n, char **out, size_t *len, size_t *cap)
{
    if (n->kind == NODE_TEXT) {
        size_t k = strlen(n->text ? n->text : "");
        if (*len + k + 1 > *cap) {
            size_t c = (*len + k + 1) * 2;
            char *g = realloc(*out, c);
            if (!g)
                return;
            *out = g;
            *cap = c;
        }
        memcpy(*out + *len, n->text ? n->text : "", k);
        *len += k;
        (*out)[*len] = 0;
        return;
    }
    if (is_comment(n))
        return;
    for (struct node *c = first_seen((struct node *)n); c; c = next_seen(c))
        text_into(c, out, len, cap);
}

static char *text_content(const struct node *n)
{
    char *out = malloc(1);
    size_t len = 0, cap = 1;
    if (!out)
        return 0;
    out[0] = 0;
    text_into(n, &out, &len, &cap);
    return out;
}

/* --- HTML text in and out ------------------------------------------------ */

static const char *const void_tags[] = { "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
                                         "meta", "source", "track", "wbr", 0 };

static bool is_void(const char *tag)
{
    for (int i = 0; void_tags[i]; i++)
        if (!strcmp(void_tags[i], tag))
            return true;
    return false;
}

struct out {
    char *s;
    size_t n, cap;
};

static void put(struct out *o, const char *s, size_t k)
{
    if (o->n + k + 1 > o->cap) {
        size_t c = (o->n + k + 1) * 2;
        char *g = realloc(o->s, c);
        if (!g)
            return;
        o->s = g;
        o->cap = c;
    }
    memcpy(o->s + o->n, s, k);
    o->n += k;
    o->s[o->n] = 0;
}

static void put_str(struct out *o, const char *s)
{
    put(o, s, strlen(s));
}

static void put_escaped(struct out *o, const char *s, bool attr)
{
    for (; *s; s++) {
        if (*s == '&')
            put_str(o, "&amp;");
        else if (*s == '<' && !attr)
            put_str(o, "&lt;");
        else if (*s == '>' && !attr)
            put_str(o, "&gt;");
        else if (*s == '"' && attr)
            put_str(o, "&quot;");
        else if ((unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xA0) {
            put_str(o, "&nbsp;");
            s++;
        } else
            put(o, s, 1);
    }
}

static void serialize(struct out *o, const struct node *n, bool raw_text);

static void serialize_children(struct out *o, const struct node *n)
{
    bool raw = n->kind == NODE_ELEMENT && (!strcmp(n->tag, "script") || !strcmp(n->tag, "style"));
    for (struct node *c = first_seen((struct node *)n); c; c = next_seen(c))
        serialize(o, c, raw);
}

static void serialize(struct out *o, const struct node *n, bool raw_text)
{
    if (n->kind == NODE_TEXT) {
        if (raw_text)
            put_str(o, n->text ? n->text : "");
        else
            put_escaped(o, n->text ? n->text : "", false);
        return;
    }
    if (is_comment(n)) {
        put_str(o, "<!--");
        put_str(o, n->text ? n->text : "");
        put_str(o, "-->");
        return;
    }
    if (is_fragment(n)) {
        serialize_children(o, n);
        return;
    }
    put_str(o, "<");
    put_str(o, n->tag);
    for (int i = 0; i < n->attr_count; i++) {
        put_str(o, " ");
        put_str(o, n->attrs[i].name);
        put_str(o, "=\"");
        put_escaped(o, n->attrs[i].value, true);
        put_str(o, "\"");
    }
    put_str(o, ">");
    if (is_void(n->tag))
        return;
    serialize_children(o, n);
    put_str(o, "</");
    put_str(o, n->tag);
    put_str(o, ">");
}

/* HTML text as a fragment (its nodes under a "#fragment" node); scripts in
 * it never run (as the standard says of innerHTML), unless `runnable`. */
static struct node *parse_fragment(const char *text, bool runnable)
{
    size_t n = strlen(text);
    char *doc = malloc(n + 7);
    if (!doc)
        return 0;
    memcpy(doc, "<body>", 6);
    memcpy(doc + 6, text, n + 1);
    struct node *tree = html_parse(doc, n + 6);
    free(doc);
    struct node *frag = new_node(NODE_ELEMENT, "#fragment");
    if (!tree || !frag) {
        html_free(tree);
        free(frag);
        return 0;
    }
    struct node *body = node_find(tree, "body");
    struct node *head = node_find(tree, "head");
    struct node *from[2] = { head, body };
    for (int k = 0; k < 2; k++) {
        if (!from[k])
            continue;
        while (from[k]->first) {
            struct node *c = from[k]->first;
            from[k]->first = c->next;
            if (c->next)
                c->next->prev = 0;
            else
                from[k]->last = 0;
            c->parent = c->prev = c->next = 0;
            link_before(frag, c, 0);
        }
    }
    html_free(tree);
    if (!runnable)
        for (struct node *s = frag; s; s = node_next(s, frag))
            if (s->kind == NODE_ELEMENT && !strcmp(s->tag, "script"))
                s->script_state = 1;
    keep_detached(frag);
    note_attributes(frag);                      /* (27q) */
    return frag;
}

/* --- objects for nodes ------------------------------------------------------ */

static jsval wrap(struct node *n)
{
    if (!n)
        return JS_NULL;
    if (n->js)
        return (jsval)n->js;
    jsval proto = n->kind == NODE_TEXT ? roots[R_TEXT] : is_fragment(n) ? roots[R_FRAGMENT]
                  : is_comment(n) ? roots[R_COMMENT] : !strcmp(n->tag, "form") ? roots[R_FORM] : roots[R_ELEMENT];
    jsval o = js_new_host(J, proto, n, &node_class);
    js_set_index(J, registry, registry_n++, o);
    n->js = (uint64_t)o;
    return o;
}

static struct node *node_of(jsval v)
{
    return js_host_data(v, &node_class);
}

static struct node *this_node(struct js *J, jsval this_val)
{
    struct node *n = node_of(this_val);
    if (!n)
        js_throw_type(J, "Illegal invocation");
    return n;
}

#define THIS_NODE                                \
    struct node *self = this_node(J, this_val);  \
    if (!self)                                   \
        return JS_EXCEPTION

static jsval str(struct js *J, const char *s)
{
    return js_cstring(J, s ? s : "");
}

static char *arg_text(struct js *J, jsval v)
{
    return js_to_utf8(J, v, 0);
}

static jsval list_of(struct node **nodes, int n)
{
    jsval a = js_new_array(J, 0);
    for (int i = 0; i < n; i++)
        js_set_index(J, a, (uint32_t)i, wrap(nodes[i]));
    /* an array whose prototype also has item() */
    js_set(J, a, "__proto__", roots[R_NODELIST]);
    return a;
}

struct gather {
    struct node **v;
    int n, cap;
};

static void gather_add(struct gather *g, struct node *n)
{
    if (g->n == g->cap) {
        int cap = g->cap ? g->cap * 2 : 16;
        struct node **v = realloc(g->v, (size_t)cap * sizeof(*v));
        if (!v)
            return;
        g->v = v;
        g->cap = cap;
    }
    g->v[g->n++] = n;
}

static jsval gathered(struct gather *g)
{
    jsval a = list_of(g->v, g->n);
    free(g->v);
    return a;
}

/* A script asks for geometry: the page laid out now, unless nothing changed
 * since the last time (27q: a script asking for sizes in a loop restyled a
 * large page each time) */
static void flush_layout(void)
{
    if (dom_layout_hook && !layout_fresh) {
        dom_layout_hook();
        layout_fresh = true;
    }
}

/* --- Node ---------------------------------------------------------------- */

static jsval n_node_type(ARGS)
{
    UNUSED;
    THIS_NODE;
    if (self->kind == NODE_TEXT)
        return js_number(3);
    if (is_fragment(self))
        return js_number(11);
    if (is_comment(self))
        return js_number(8);
    return js_number(1);
}

static jsval n_node_name(ARGS)
{
    UNUSED;
    THIS_NODE;
    if (self->kind == NODE_TEXT)
        return str(J, "#text");
    if (is_fragment(self))
        return str(J, "#document-fragment");
    if (is_comment(self))
        return str(J, "#comment");
    char up[16];
    size_t i = 0;
    for (; self->tag[i] && i < sizeof(up) - 1; i++)
        up[i] = self->tag[i] >= 'a' && self->tag[i] <= 'z' ? (char)(self->tag[i] - 32) : self->tag[i];
    up[i] = 0;
    return str(J, up);
}

static jsval n_parent(ARGS)
{
    UNUSED;
    THIS_NODE;
    if (self == root)
        return roots[R_DOCUMENT_OBJ];
    return wrap(self->parent);
}

static jsval n_parent_element(ARGS)
{
    UNUSED;
    THIS_NODE;
    return self->parent && is_element(self->parent) ? wrap(self->parent) : JS_NULL;
}

static jsval n_first_child(ARGS) { UNUSED; THIS_NODE; return wrap(first_seen(self)); }
static jsval n_last_child(ARGS) { UNUSED; THIS_NODE; return wrap(last_seen(self)); }
static jsval n_next_sibling(ARGS) { UNUSED; THIS_NODE; return self->parent ? wrap(next_seen(self)) : JS_NULL; }
static jsval n_previous_sibling(ARGS) { UNUSED; THIS_NODE; return self->parent ? wrap(prev_seen(self)) : JS_NULL; }

static jsval n_child_nodes(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct gather g = { 0 };
    for (struct node *c = first_seen(self); c; c = next_seen(c))
        gather_add(&g, c);
    return gathered(&g);
}

static jsval n_has_child_nodes(ARGS)
{
    UNUSED;
    THIS_NODE;
    return js_bool(first_seen(self) != 0);
}

static jsval n_text_content(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *t = text_content(self);
    jsval r = str(J, t);
    free(t);
    return r;
}

static void replace_children_with_text(struct node *e, const char *text)
{
    for (struct node *c = e->first; c;) {
        struct node *after = c->next;
        if (seen(c))
            unlink_node(c);
        c = after;
    }
    if (text[0]) {
        struct node *t = new_node(NODE_TEXT, "");
        if (t) {
            t->text = strdup(text);
            link_before(e, t, 0);
        }
    }
    dom_changed = true, layout_fresh = false;
}

static jsval n_set_text_content(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *t = js_is_nullish(argv[0]) ? strdup("") : arg_text(J, argv[0]);
    if (!t)
        return JS_EXCEPTION;
    if (self->kind == NODE_TEXT || is_comment(self)) {
        free(self->text);
        self->text = t;
        dom_changed = true, layout_fresh = false;
        return JS_UNDEFINED;
    }
    replace_children_with_text(self, t);
    free(t);
    return JS_UNDEFINED;
}

static jsval n_node_value(ARGS)
{
    UNUSED;
    THIS_NODE;
    if (self->kind == NODE_TEXT || is_comment(self))
        return str(J, self->text);
    return JS_NULL;
}

static bool contains(const struct node *a, const struct node *b)
{
    if (a == &docnode)
        return connected(b);
    for (; b; b = b->parent)
        if (a == b)
            return true;
    return false;
}

/* inserting `child` (a node or a fragment) into parent before ref */
static jsval insert(struct js *J, struct node *parent, jsval childv, struct node *ref)
{
    struct node *child = node_of(childv);
    if (!child)
        return js_throw_type(J, "Argument 1 is not a Node");
    if (child == root || child == &docnode)     /* (the document and its element stay where they are) */
        return js_throw_named(J, "HierarchyRequestError", "The document element cannot be moved.");
    if (contains(child, parent))
        return js_throw_named(J, "HierarchyRequestError", "The new child element contains the parent.");
    if (parent->kind == NODE_TEXT)
        return js_throw_named(J, "HierarchyRequestError", "This node type does not support this method.");
    if (ref && ref->parent != parent)
        return js_throw_named(J, "NotFoundError", "The object can not be found here.");
    if (ref == child)
        return childv;
    if (is_fragment(child)) {
        while (child->first) {
            struct node *c = child->first;
            if (!seen(c)) {
                unlink_node(c);
                continue;
            }
            unlink_node(c);
            link_before(parent, c, ref);
        }
        return childv;
    }
    if (child->parent)
        unlink_node(child);
    link_before(parent, child, ref);
    return childv;
}

static void run_one(struct node *s);

/* (inserted into the document by a script) its inline scripts run now, the
 * ones with src when the current ones are done (dom_run_scripts' rounds) */
static void run_inserted(struct node *n)
{
    if (!connected(n))
        return;
    for (struct node *s = n; s; s = node_next(s, n))
        if (s->kind == NODE_ELEMENT && !strcmp(s->tag, "script") && !s->script_state && !node_attr(s, "src")) {
            struct node *saved = current_script;
            run_one(s);
            current_script = saved;
        }
}

static jsval inserted(jsval v)
{
    struct node *n = node_of(v);
    if (n && v != JS_EXCEPTION)
        run_inserted(n);
    return v;
}

static jsval n_append_child(ARGS)
{
    UNUSED;
    THIS_NODE;
    return inserted(insert(J, self, argv[0], 0));
}

static jsval n_insert_before(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct node *ref = js_is_nullish(argv[1]) ? 0 : node_of(argv[1]);
    if (!js_is_nullish(argv[1]) && !ref)
        return js_throw_type(J, "Argument 2 is not a Node");
    return inserted(insert(J, self, argv[0], ref));
}

static jsval n_remove_child(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct node *c = node_of(argv[0]);
    if (!c)
        return js_throw_type(J, "Argument 1 is not a Node");
    if (c->parent != self)
        return js_throw_named(J, "NotFoundError", "The node to be removed is not a child of this node.");
    unlink_node(c);
    return argv[0];
}

static jsval n_replace_child(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct node *old = node_of(argv[1]);
    if (!old || old->parent != self)
        return js_throw_named(J, "NotFoundError", "The node to be replaced is not a child of this node.");
    struct node *after = old->next;
    unlink_node(old);
    jsval r = insert(J, self, argv[0], after);
    if (r == JS_EXCEPTION)
        return r;
    return argv[1];
}

static jsval n_clone_node(ARGS)
{
    UNUSED;
    THIS_NODE;
    return wrap(clone(self, argc > 0 && js_to_boolean(argv[0])));
}

static jsval n_contains(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct node *other = node_of(argv[0]);
    return js_bool(other && contains(self, other));
}

static jsval n_is_connected(ARGS)
{
    UNUSED;
    THIS_NODE;
    return js_bool(connected(self));
}

static jsval n_owner_document(ARGS)
{
    UNUSED;
    THIS_NODE;
    return self == &docnode ? JS_NULL : roots[R_DOCUMENT_OBJ];
}

static jsval n_is_equal_node(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct node *o = node_of(argv[0]);
    if (!o)
        return JS_FALSE;
    struct out a = { 0 }, b = { 0 };
    serialize(&a, self, false);
    serialize(&b, o, false);
    bool same = a.s && b.s && !strcmp(a.s, b.s);
    free(a.s);
    free(b.s);
    return js_bool(same);
}

/* ChildNode / ParentNode: remove, before, after, replaceWith, append, prepend */
static struct node *nodes_arg(struct js *J, int argc, jsval *argv)
{
    struct node *frag = new_node(NODE_ELEMENT, "#fragment");
    if (!frag)
        return 0;
    keep_detached(frag);
    for (int i = 0; i < argc; i++) {
        struct node *n = node_of(argv[i]);
        if (!n) {
            char *t = arg_text(J, argv[i]);
            if (!t)
                return 0;
            n = new_node(NODE_TEXT, "");
            if (!n) {
                free(t);
                return 0;
            }
            n->text = t;
        } else if (n == root || n == &docnode) {
            js_throw_named(J, "HierarchyRequestError", "The document element cannot be moved.");
            return 0;
        } else if (n->parent)
            unlink_node(n);
        if (is_fragment(n)) {
            while (n->first) {
                struct node *c = n->first;
                unlink_node(c);
                link_before(frag, c, 0);
            }
        } else
            link_before(frag, n, 0);
    }
    return frag;
}

static jsval n_remove(ARGS)
{
    UNUSED;
    THIS_NODE;
    if (self->parent)
        unlink_node(self);
    return JS_UNDEFINED;
}

static bool among(jsval *argv, int argc, const struct node *n)
{
    for (int i = 0; i < argc; i++)
        if (node_of(argv[i]) == n)
            return true;
    return false;
}

/* the standard's pre-insert check: no argument may hold the parent */
static bool insertable(struct js *J, struct node *parent, int argc, jsval *argv)
{
    for (int i = 0; i < argc; i++) {
        struct node *n = node_of(argv[i]);
        if (n && contains(n, parent)) {
            js_throw_named(J, "HierarchyRequestError", "The new child element contains the parent.");
            return false;
        }
    }
    return true;
}

static void move_in(struct node *frag, struct node *parent, struct node *ref)
{
    struct node *added[64];
    int na = 0;
    while (frag->first) {
        struct node *c = frag->first;
        unlink_node(c);
        link_before(parent, c, ref);
        if (na < 64)
            added[na++] = c;
    }
    for (int i = 0; i < na; i++)
        run_inserted(added[i]);
}

static jsval n_before_after(ARGS)
{
    UNUSED;
    int after = js_magic(J);
    THIS_NODE;
    struct node *p = self->parent;
    if (!p)
        return JS_UNDEFINED;
    if (!insertable(J, p, argc, argv))
        return JS_EXCEPTION;
    /* the viable sibling: the nearest one that is not being moved */
    struct node *prev = self->prev, *next = self->next;
    while (prev && among(argv, argc, prev))
        prev = prev->prev;
    while (next && among(argv, argc, next))
        next = next->next;
    bool self_moved = among(argv, argc, self);
    struct node *frag = nodes_arg(J, argc, argv);
    if (!frag)
        return JS_EXCEPTION;
    struct node *ref;
    if (after)
        ref = next;
    else if (self_moved)
        ref = prev ? prev->next : p->first;
    else
        ref = self;
    move_in(frag, p, ref);
    return JS_UNDEFINED;
}

static jsval n_replace_with(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct node *p = self->parent;
    if (!p)
        return JS_UNDEFINED;
    if (!insertable(J, p, argc, argv))
        return JS_EXCEPTION;
    struct node *next = self->next;
    while (next && among(argv, argc, next))
        next = next->next;
    bool self_moved = among(argv, argc, self);
    struct node *frag = nodes_arg(J, argc, argv);
    if (!frag)
        return JS_EXCEPTION;
    if (!self_moved && self->parent == p)
        unlink_node(self);
    move_in(frag, p, next && next->parent == p ? next : 0);
    return JS_UNDEFINED;
}

static jsval n_append_prepend(ARGS)
{
    UNUSED;
    int prepend = js_magic(J);
    THIS_NODE;
    if (!insertable(J, self, argc, argv))
        return JS_EXCEPTION;
    struct node *ref = prepend ? self->first : 0;
    while (ref && among(argv, argc, ref))
        ref = ref->next;
    struct node *frag = nodes_arg(J, argc, argv);
    if (!frag)
        return JS_EXCEPTION;
    move_in(frag, self, ref);
    return JS_UNDEFINED;
}

static jsval n_replace_children(ARGS)
{
    UNUSED;
    THIS_NODE;
    if (!insertable(J, self, argc, argv))
        return JS_EXCEPTION;
    struct node *frag = nodes_arg(J, argc, argv);
    if (!frag)
        return JS_EXCEPTION;
    replace_children_with_text(self, "");
    while (frag->first) {
        struct node *c = frag->first;
        unlink_node(c);
        link_before(self, c, 0);
    }
    return JS_UNDEFINED;
}

/* Text */
static jsval t_data(ARGS)
{
    UNUSED;
    THIS_NODE;
    return str(J, self->text);
}

static jsval t_length(ARGS)
{
    UNUSED;
    THIS_NODE;
    jsval s = str(J, self->text);
    return js_get(J, s, "length");
}

/* --- Element ------------------------------------------------------------ */

static jsval e_tag_name(ARGS)
{
    return n_node_name(J, this_val, argc, argv, nt);
}

static jsval e_local_name(ARGS)
{
    UNUSED;
    THIS_NODE;
    return str(J, self->tag);
}

static jsval e_get_attribute(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *name = arg_text(J, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    for (char *p = name; *p; p++)
        *p = (char)lower((unsigned char)*p);
    const char *v = self->kind == NODE_ELEMENT ? node_attr(self, name) : 0;
    free(name);
    return v ? str(J, v) : JS_NULL;
}

static jsval e_set_attribute(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *name = arg_text(J, argv[0]);
    char *value = name ? arg_text(J, argv[1]) : 0;
    if (!name || !value) {
        free(name);
        return JS_EXCEPTION;
    }
    for (char *p = name; *p; p++)
        *p = (char)lower((unsigned char)*p);
    if (!name[0] || strpbrk(name, " \t\n\"'<>/=")) {
        free(name);
        free(value);
        return js_throw_named(J, "InvalidCharacterError", "Invalid attribute name.");
    }
    if (self->kind == NODE_ELEMENT)
        set_attr(self, name, value);
    if (name[0] == 'o' && name[1] == 'n' && name[2])
        note_type(name + 2);                    /* (27q: an onclick="..." made by a script) */
    if (!strcmp(name, "value") && self->control)
        control_set_text(self->control, value);
    free(name);
    free(value);
    return JS_UNDEFINED;
}

static jsval e_remove_attribute(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *name = arg_text(J, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    for (char *p = name; *p; p++)
        *p = (char)lower((unsigned char)*p);
    if (self->kind == NODE_ELEMENT)
        remove_attr(self, name);
    free(name);
    return JS_UNDEFINED;
}

static jsval e_has_attribute(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *name = arg_text(J, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    for (char *p = name; *p; p++)
        *p = (char)lower((unsigned char)*p);
    bool has = self->kind == NODE_ELEMENT && node_attr(self, name);
    free(name);
    return js_bool(has);
}

static jsval e_toggle_attribute(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *name = arg_text(J, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    for (char *p = name; *p; p++)
        *p = (char)lower((unsigned char)*p);
    bool has = node_attr(self, name) != 0;
    bool want = argc > 1 ? js_to_boolean(argv[1]) : !has;
    if (want && !has)
        set_attr(self, name, "");
    else if (!want && has)
        remove_attr(self, name);
    free(name);
    return js_bool(want);
}

static jsval e_get_attribute_names(ARGS)
{
    UNUSED;
    THIS_NODE;
    jsval a = js_new_array(J, 0);
    for (int i = 0; i < self->attr_count; i++)
        js_set_index(J, a, (uint32_t)i, str(J, self->attrs[i].name));
    return a;
}

static jsval e_attributes(ARGS)
{
    UNUSED;
    THIS_NODE;
    jsval a = js_new_array(J, 0);
    for (int i = 0; i < self->attr_count; i++) {
        jsval at = js_new_object(J);
        js_set(J, at, "name", str(J, self->attrs[i].name));
        js_set(J, at, "value", str(J, self->attrs[i].value));
        js_set(J, at, "nodeName", str(J, self->attrs[i].name));
        js_set(J, at, "nodeValue", str(J, self->attrs[i].value));
        js_set_index(J, a, (uint32_t)i, at);
        js_set(J, a, self->attrs[i].name, at);
    }
    return a;
}

/* reflected attributes: magic indexes this table */
enum { RA_STRING, RA_URL, RA_BOOL, RA_NUMBER };
static const struct { const char *prop, *attr; int kind; } reflected[] = {
    { "id", "id", RA_STRING }, { "className", "class", RA_STRING }, { "title", "title", RA_STRING },
    { "lang", "lang", RA_STRING }, { "dir", "dir", RA_STRING }, { "href", "href", RA_URL },
    { "src", "src", RA_URL }, { "alt", "alt", RA_STRING }, { "name", "name", RA_STRING },
    { "rel", "rel", RA_STRING }, { "target", "target", RA_STRING }, { "placeholder", "placeholder", RA_STRING },
    { "htmlFor", "for", RA_STRING }, { "action", "action", RA_URL }, { "method", "method", RA_STRING },
    { "hidden", "hidden", RA_BOOL }, { "disabled", "disabled", RA_BOOL }, { "required", "required", RA_BOOL },
    { "readOnly", "readonly", RA_BOOL }, { "multiple", "multiple", RA_BOOL }, { "defer", "defer", RA_BOOL },
    { "async", "async", RA_BOOL }, { "content", "content", RA_STRING }, { "role", "role", RA_STRING },
    { "slot", "slot", RA_STRING }, { "autofocus", "autofocus", RA_BOOL }, { "noModule", "nomodule", RA_BOOL },
    { "tabIndex", "tabindex", RA_NUMBER }, { "colSpan", "colspan", RA_NUMBER }, { "rowSpan", "rowspan", RA_NUMBER },
    { "maxLength", "maxlength", RA_NUMBER }, { "download", "download", RA_STRING }, { "charset", "charset", RA_STRING },
    { "crossOrigin", "crossorigin", RA_STRING }, { "integrity", "integrity", RA_STRING }, { "media", "media", RA_STRING },
    { "srcset", "srcset", RA_STRING }, { "sizes", "sizes", RA_STRING }, { "loading", "loading", RA_STRING },
    { "draggable", "draggable", RA_STRING }, { "contentEditable", "contenteditable", RA_STRING },
    { "accessKey", "accesskey", RA_STRING }, { "enctype", "enctype", RA_STRING }, { "pattern", "pattern", RA_STRING },
    { "min", "min", RA_STRING }, { "max", "max", RA_STRING }, { "step", "step", RA_STRING },
    { "autocomplete", "autocomplete", RA_STRING }, { "label", "label", RA_STRING }, { "open", "open", RA_BOOL },
};

static jsval e_reflect_get(ARGS)
{
    UNUSED;
    int k = js_magic(J);
    THIS_NODE;
    const char *v = self->kind == NODE_ELEMENT ? node_attr(self, reflected[k].attr) : 0;
    switch (reflected[k].kind) {
    case RA_BOOL:
        return js_bool(v != 0);
    case RA_NUMBER:
        if (!v)
            return js_number(!strcmp(reflected[k].attr, "tabindex") ? -1 : !strcmp(reflected[k].attr, "maxlength") ? -1 : 1);
        return js_number(atoi(v));
    case RA_URL:
        if (!v)
            return str(J, "");
        {
            char abs[URL_MAX];
            if (url_resolve(page_url, v, abs, sizeof(abs)))
                return str(J, abs);
        }
        return str(J, v);
    }
    if (!strcmp(reflected[k].attr, "type") && !v)
        return str(J, "");
    return str(J, v);
}

static jsval e_reflect_set(ARGS)
{
    UNUSED;
    int k = js_magic(J);
    THIS_NODE;
    if (self->kind != NODE_ELEMENT)
        return JS_UNDEFINED;
    if (reflected[k].kind == RA_BOOL) {
        if (js_to_boolean(argv[0]))
            set_attr(self, reflected[k].attr, "");
        else
            remove_attr(self, reflected[k].attr);
        return JS_UNDEFINED;
    }
    char *v = arg_text(J, argv[0]);
    if (!v)
        return JS_EXCEPTION;
    set_attr(self, reflected[k].attr, v);
    free(v);
    return JS_UNDEFINED;
}

static jsval e_type_get(ARGS)
{
    UNUSED;
    THIS_NODE;
    const char *v = node_attr(self, "type");
    if (!strcmp(self->tag, "input"))
        return str(J, v && v[0] ? v : "text");
    if (!strcmp(self->tag, "button"))
        return str(J, v && v[0] ? v : "submit");
    if (!strcmp(self->tag, "select"))
        return str(J, node_attr(self, "multiple") ? "select-multiple" : "select-one");
    if (!strcmp(self->tag, "textarea"))
        return str(J, "textarea");
    return str(J, v);
}

static jsval e_type_set(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *v = arg_text(J, argv[0]);
    if (!v)
        return JS_EXCEPTION;
    set_attr(self, "type", v);
    free(v);
    return JS_UNDEFINED;
}

/* form controls: value, checked, selectedIndex */
static jsval e_value_get(ARGS)
{
    UNUSED;
    THIS_NODE;
    if (is_control(self)) {
        struct control *c = control_of(self);
        if (c && (c->kind == CONTROL_TEXT || c->kind == CONTROL_PASSWORD || c->kind == CONTROL_TEXTAREA ||
                  c->kind == CONTROL_HIDDEN))
            return str(J, c->value);
        if (c && c->kind == CONTROL_SELECT) {
            struct node *opts[256];
            int n = select_options(self, opts, 256);
            if (c->selected >= 0 && c->selected < n) {
                char *made = 0;
                const char *v = option_value(opts[c->selected], &made);
                jsval r = str(J, v);
                free(made);
                return r;
            }
            return str(J, "");
        }
        const char *v = node_attr(self, "value");
        if (!v && (c && (c->kind == CONTROL_CHECKBOX || c->kind == CONTROL_RADIO)))
            v = "on";
        return str(J, v);
    }
    if (!strcmp(self->tag, "option")) {
        char *made = 0;
        const char *v = option_value(self, &made);
        jsval r = str(J, v);
        free(made);
        return r;
    }
    return str(J, node_attr(self, "value"));
}

static jsval e_value_set(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *v = js_is_nullish(argv[0]) ? strdup("") : arg_text(J, argv[0]);
    if (!v)
        return JS_EXCEPTION;
    struct control *c = is_control(self) ? control_of(self) : 0;
    if (c && (c->kind == CONTROL_TEXT || c->kind == CONTROL_PASSWORD || c->kind == CONTROL_TEXTAREA ||
              c->kind == CONTROL_HIDDEN))
        control_set_text(c, v);
    else if (c && c->kind == CONTROL_SELECT) {
        struct node *opts[256];
        int n = select_options(self, opts, 256);
        for (int i = 0; i < n; i++) {
            char *made = 0;
            const char *ov = option_value(opts[i], &made);
            bool same = !strcmp(ov, v);
            free(made);
            if (same) {
                c->selected = i;
                break;
            }
        }
    } else
        set_attr(self, "value", v);
    dom_changed = true, layout_fresh = false;
    free(v);
    return JS_UNDEFINED;
}

static jsval e_checked_get(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct control *c = is_control(self) ? control_of(self) : 0;
    return js_bool(c ? c->checked : node_attr(self, "checked") != 0);
}

static jsval e_checked_set(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct control *c = is_control(self) ? control_of(self) : 0;
    bool on = js_to_boolean(argv[0]);
    if (c) {
        if (on && c->kind == CONTROL_RADIO)
            radio_check(self, root);
        c->checked = on;
    }
    dom_changed = true, layout_fresh = false;
    return JS_UNDEFINED;
}

static jsval e_selected_index_get(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct control *c = is_control(self) ? control_of(self) : 0;
    return js_number(c && c->kind == CONTROL_SELECT ? c->selected : -1);
}

static jsval e_selected_index_set(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct control *c = is_control(self) ? control_of(self) : 0;
    bool threw = false;
    double d = js_to_number(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (c && c->kind == CONTROL_SELECT)
        c->selected = (int)d;
    dom_changed = true, layout_fresh = false;
    return JS_UNDEFINED;
}

static jsval e_options(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct node *opts[256];
    int n = select_options(self, opts, 256);
    return list_of(opts, n);
}

static jsval e_inner_html(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct out o = { 0 };
    put(&o, "", 0);
    serialize_children(&o, self);
    jsval r = str(J, o.s);
    free(o.s);
    return r;
}

static jsval e_set_inner_html(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *t = js_is_nullish(argv[0]) ? strdup("") : arg_text(J, argv[0]);
    if (!t)
        return JS_EXCEPTION;
    replace_children_with_text(self, "");
    if (self->kind == NODE_ELEMENT && (!strcmp(self->tag, "script") || !strcmp(self->tag, "style"))) {
        replace_children_with_text(self, t);
        free(t);
        return JS_UNDEFINED;
    }
    struct node *frag = parse_fragment(t, false);
    free(t);
    if (!frag)
        return JS_EXCEPTION;
    while (frag->first) {
        struct node *c = frag->first;
        unlink_node(c);
        link_before(self, c, 0);
    }
    return JS_UNDEFINED;
}

static jsval e_outer_html(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct out o = { 0 };
    put(&o, "", 0);
    serialize(&o, self, false);
    jsval r = str(J, o.s);
    free(o.s);
    return r;
}

static jsval e_set_outer_html(ARGS)
{
    UNUSED;
    THIS_NODE;
    if (!self->parent)
        return JS_UNDEFINED;
    char *t = arg_text(J, argv[0]);
    if (!t)
        return JS_EXCEPTION;
    struct node *frag = parse_fragment(t, false);
    free(t);
    if (!frag)
        return JS_EXCEPTION;
    struct node *p = self->parent, *ref = self->next;
    unlink_node(self);
    while (frag->first) {
        struct node *c = frag->first;
        unlink_node(c);
        link_before(p, c, ref);
    }
    return JS_UNDEFINED;
}

static jsval e_insert_adjacent_html(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *where = arg_text(J, argv[0]);
    char *t = where ? arg_text(J, argv[1]) : 0;
    if (!t) {
        free(where);
        return JS_EXCEPTION;
    }
    struct node *frag = parse_fragment(t, false);
    free(t);
    struct node *parent = 0, *ref = 0;
    if (same_word(where, "beforebegin"))
        parent = self->parent, ref = self;
    else if (same_word(where, "afterbegin"))
        parent = self, ref = self->first;
    else if (same_word(where, "beforeend"))
        parent = self, ref = 0;
    else if (same_word(where, "afterend"))
        parent = self->parent, ref = self->next;
    else {
        free(where);
        return js_throw_named(J, "SyntaxError", "The value provided is not one of 'beforeBegin', 'afterBegin', 'beforeEnd', or 'afterEnd'.");
    }
    free(where);
    if (frag && parent)
        while (frag->first) {
            struct node *c = frag->first;
            unlink_node(c);
            link_before(parent, c, ref);
        }
    return JS_UNDEFINED;
}

static jsval e_children(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct gather g = { 0 };
    for (struct node *c = first_seen(self); c; c = next_seen(c))
        if (is_element(c))
            gather_add(&g, c);
    return gathered(&g);
}

static jsval e_child_element_count(ARGS)
{
    UNUSED;
    THIS_NODE;
    int n = 0;
    for (struct node *c = first_seen(self); c; c = next_seen(c))
        n += is_element(c);
    return js_number(n);
}

static jsval e_first_element_child(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct node *c = first_seen(self);
    while (c && !is_element(c))
        c = next_seen(c);
    return wrap(c);
}

static jsval e_last_element_child(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct node *c = last_seen(self);
    while (c && !is_element(c))
        c = prev_seen(c);
    return wrap(c);
}

static jsval e_next_element_sibling(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct node *c = self->parent ? next_seen(self) : 0;
    while (c && !is_element(c))
        c = next_seen(c);
    return wrap(c);
}

static jsval e_previous_element_sibling(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct node *c = self->parent ? prev_seen(self) : 0;
    while (c && !is_element(c))
        c = prev_seen(c);
    return wrap(c);
}

/* --- queries ---------------------------------------------------------------- */

static jsval query(struct js *J, struct node *top, jsval sel, bool all)
{
    char *text = arg_text(J, sel);
    if (!text)
        return JS_EXCEPTION;
    struct selector_list *l = css_selectors(text);
    if (!l) {
        char msg[300];
        snprintf(msg, sizeof msg, "'%.200s' is not a valid selector.", text);
        free(text);
        return js_throw_named(J, "SyntaxError", msg);
    }
    free(text);
    struct gather g = { 0 };
    for (struct node *n = walk_next(top, top); n; n = walk_next(n, top))
        if (css_selectors_match(l, n)) {
            if (!all) {
                css_selectors_free(l);
                return wrap(n);
            }
            gather_add(&g, n);
        }
    css_selectors_free(l);
    if (!all)
        return JS_NULL;
    return gathered(&g);
}

static struct node *query_top(struct js *J, jsval this_val)
{
    return this_node(J, this_val);
}

static jsval q_query_selector(ARGS)
{
    UNUSED;
    struct node *top = query_top(J, this_val);
    if (!top)
        return JS_EXCEPTION;
    if (top == root) {
        jsval r = query(J, top, argv[0], false);
        /* the root itself may match */
        return r;
    }
    return query(J, top, argv[0], false);
}

static jsval q_query_selector_all(ARGS)
{
    UNUSED;
    struct node *top = query_top(J, this_val);
    if (!top)
        return JS_EXCEPTION;
    return query(J, top, argv[0], true);
}

static jsval document_query(struct js *J, jsval sel, bool all)
{
    /* the document's own: the root element counts too */
    char *text = arg_text(J, sel);
    if (!text)
        return JS_EXCEPTION;
    struct selector_list *l = css_selectors(text);
    if (!l) {
        char msg[300];
        snprintf(msg, sizeof msg, "'%.200s' is not a valid selector.", text);
        free(text);
        return js_throw_named(J, "SyntaxError", msg);
    }
    free(text);
    struct gather g = { 0 };
    for (struct node *n = root; n; n = walk_next(n, root))
        if (css_selectors_match(l, n)) {
            if (!all) {
                css_selectors_free(l);
                return wrap(n);
            }
            gather_add(&g, n);
        }
    css_selectors_free(l);
    return all ? gathered(&g) : JS_NULL;
}

static jsval d_query_selector(ARGS) { UNUSED; return document_query(J, argv[0], false); }
static jsval d_query_selector_all(ARGS) { UNUSED; return document_query(J, argv[0], true); }

static jsval e_matches(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *text = arg_text(J, argv[0]);
    if (!text)
        return JS_EXCEPTION;
    struct selector_list *l = css_selectors(text);
    free(text);
    if (!l)
        return js_throw_named(J, "SyntaxError", "Not a valid selector.");
    bool m = css_selectors_match(l, self);
    css_selectors_free(l);
    return js_bool(m);
}

static jsval e_closest(ARGS)
{
    UNUSED;
    THIS_NODE;
    char *text = arg_text(J, argv[0]);
    if (!text)
        return JS_EXCEPTION;
    struct selector_list *l = css_selectors(text);
    free(text);
    if (!l)
        return js_throw_named(J, "SyntaxError", "Not a valid selector.");
    for (struct node *n = self; n; n = n->parent)
        if (css_selectors_match(l, n)) {
            css_selectors_free(l);
            return wrap(n);
        }
    css_selectors_free(l);
    return JS_NULL;
}

static bool has_class(const struct node *e, const char *cls)
{
    const char *c = node_attr(e, "class");
    size_t k = strlen(cls);
    while (c && *c) {
        while (*c && is_space((unsigned char)*c))
            c++;
        const char *s = c;
        while (*c && !is_space((unsigned char)*c))
            c++;
        if ((size_t)(c - s) == k && !memcmp(s, cls, k))
            return true;
    }
    return false;
}

/* getElementsByTagName (0), getElementsByClassName (1), getElementsByName (2) */
static jsval get_elements(struct js *J, struct node *top, jsval arg, int kind, bool include_top)
{
    char *text = arg_text(J, arg);
    if (!text)
        return JS_EXCEPTION;
    if (kind == 0)
        for (char *p = text; *p; p++)
            *p = (char)lower((unsigned char)*p);
    char *classes[16];
    int nc = 0;
    if (kind == 1) {
        char *save = text;
        while (*save && nc < 16) {
            while (*save && is_space((unsigned char)*save))
                *save++ = 0;
            if (!*save)
                break;
            classes[nc++] = save;
            while (*save && !is_space((unsigned char)*save))
                save++;
        }
    }
    struct gather g = { 0 };
    for (struct node *n = include_top ? top : walk_next(top, top); n; n = walk_next(n, top)) {
        if (!is_element(n))
            continue;
        bool ok;
        if (kind == 0)
            ok = !strcmp(text, "*") || !strcmp(n->tag, text);
        else if (kind == 1) {
            ok = nc > 0;
            for (int i = 0; i < nc && ok; i++)
                ok = has_class(n, classes[i]);
        } else {
            const char *nm = node_attr(n, "name");
            ok = nm && !strcmp(nm, text);
        }
        if (ok)
            gather_add(&g, n);
    }
    free(text);
    return gathered(&g);
}

static jsval e_get_by_tag(ARGS) { UNUSED; THIS_NODE; return get_elements(J, self, argv[0], 0, false); }
static jsval e_get_by_class(ARGS) { UNUSED; THIS_NODE; return get_elements(J, self, argv[0], 1, false); }
static jsval d_get_by_tag(ARGS) { UNUSED; return get_elements(J, root, argv[0], 0, true); }
static jsval d_get_by_class(ARGS) { UNUSED; return get_elements(J, root, argv[0], 1, true); }
static jsval d_get_by_name(ARGS) { UNUSED; return get_elements(J, root, argv[0], 2, true); }

static jsval d_get_by_id(ARGS)
{
    UNUSED;
    char *id = arg_text(J, argv[0]);
    if (!id)
        return JS_EXCEPTION;
    for (struct node *n = root; n; n = walk_next(n, root))
        if (is_element(n)) {
            const char *v = node_attr(n, "id");
            if (v && !strcmp(v, id)) {
                free(id);
                return wrap(n);
            }
        }
    free(id);
    return JS_NULL;
}

/* --- geometry --------------------------------------------------------------- */

static jsval e_rect(ARGS)
{
    UNUSED;
    THIS_NODE;
    flush_layout();
    jsval r = js_new_object(J);
    double x = self->box[0], y = self->box[1] - dom_scroll_y, w = self->box[2], h = self->box[3];
    js_set(J, r, "x", js_number(x));
    js_set(J, r, "y", js_number(y));
    js_set(J, r, "width", js_number(w));
    js_set(J, r, "height", js_number(h));
    js_set(J, r, "left", js_number(x));
    js_set(J, r, "top", js_number(y));
    js_set(J, r, "right", js_number(x + w));
    js_set(J, r, "bottom", js_number(y + h));
    return r;
}

/* magic: 0 offsetWidth/clientWidth/scrollWidth, 1 heights, 2 offsetLeft, 3 offsetTop */
static jsval e_metric(ARGS)
{
    UNUSED;
    int which = js_magic(J);
    THIS_NODE;
    flush_layout();
    switch (which) {
    case 0: return js_number(self->box[2]);
    case 1: return js_number(self->box[3]);
    case 2: return js_number(self->box[0]);
    case 3: return js_number(self->box[1]);
    }
    return js_number(0);
}

static jsval e_zero(ARGS)
{
    UNUSED;
    return js_number(0);
}

static jsval e_offset_parent(ARGS)
{
    UNUSED;
    THIS_NODE;
    return self == root ? JS_NULL : wrap(node_find(root, "body"));
}

static jsval e_noop(ARGS)
{
    UNUSED;
    return JS_UNDEFINED;
}

/* --- classList ------------------------------------------------------------- */

static struct node *tokens_node(struct js *J, jsval this_val)
{
    struct node *n = js_host_data(this_val, &tokens_class);
    if (!n)
        js_throw_type(J, "Illegal invocation");
    return n;
}

static jsval e_class_list(ARGS)
{
    UNUSED;
    THIS_NODE;
    jsval o = js_get(J, this_val, "%classList");
    if (js_is_object(o))
        return o;
    o = js_new_host(J, roots[R_TOKENS], self, &tokens_class);
    js_define_value(J, this_val, "%classList", o);
    return o;
}

/* the class attribute's words (malloc'd copies), n of them */
static int class_words(const struct node *e, char **words, int max)
{
    const char *c = node_attr(e, "class");
    int n = 0;
    while (c && *c && n < max) {
        while (*c && is_space((unsigned char)*c))
            c++;
        const char *s = c;
        while (*c && !is_space((unsigned char)*c))
            c++;
        if (c > s) {
            bool dup = false;
            for (int i = 0; i < n; i++)
                if (strlen(words[i]) == (size_t)(c - s) && !memcmp(words[i], s, (size_t)(c - s)))
                    dup = true;
            if (!dup)
                words[n++] = copy_text(s, (size_t)(c - s));
        }
    }
    return n;
}

static void set_class_words(struct node *e, char **words, int n)
{
    struct out o = { 0 };
    put(&o, "", 0);
    for (int i = 0; i < n; i++) {
        if (i)
            put_str(&o, " ");
        put_str(&o, words[i]);
    }
    set_attr(e, "class", o.s ? o.s : "");
    free(o.s);
}

/* magic: 0 add, 1 remove, 2 toggle, 3 contains, 4 replace */
static jsval tl_op(ARGS)
{
    UNUSED;
    int op = js_magic(J);
    struct node *e = tokens_node(J, this_val);
    if (!e)
        return JS_EXCEPTION;
    char *words[256];
    int n = class_words(e, words, 256);
    jsval result = JS_UNDEFINED;
    int nargs = op == 2 || op == 3 ? 1 : op == 4 ? 2 : argc;
    char *toks[16];
    int nt2 = 0;
    for (int i = 0; i < nargs && i < 16; i++) {
        toks[nt2] = arg_text(J, argv[i]);
        if (!toks[nt2])
            goto out;
        if (!toks[nt2][0] || strpbrk(toks[nt2], " \t\n\f\r")) {
            nt2++;
            result = toks[nt2 - 1][0] ? js_throw_named(J, "InvalidCharacterError", "The token contains HTML space characters.")
                                      : js_throw_named(J, "SyntaxError", "The token must not be empty.");
            goto out;
        }
        nt2++;
    }
    switch (op) {
    case 0:
        for (int i = 0; i < nt2; i++) {
            bool have = false;
            for (int k = 0; k < n; k++)
                have |= !strcmp(words[k], toks[i]);
            if (!have && n < 256)
                words[n++] = strdup(toks[i]);
        }
        set_class_words(e, words, n);
        break;
    case 1:
        for (int i = 0; i < nt2; i++)
            for (int k = 0; k < n; k++)
                if (!strcmp(words[k], toks[i])) {
                    free(words[k]);
                    memmove(words + k, words + k + 1, (size_t)(n - k - 1) * sizeof(*words));
                    n--;
                    k--;
                }
        set_class_words(e, words, n);
        break;
    case 2: {
        int at = -1;
        for (int k = 0; k < n; k++)
            if (!strcmp(words[k], toks[0]))
                at = k;
        bool want = argc > 1 && argv[1] != JS_UNDEFINED ? js_to_boolean(argv[1]) : at < 0;
        if (want && at < 0 && n < 256)
            words[n++] = strdup(toks[0]);
        else if (!want && at >= 0) {
            free(words[at]);
            memmove(words + at, words + at + 1, (size_t)(n - at - 1) * sizeof(*words));
            n--;
        }
        set_class_words(e, words, n);
        result = js_bool(want);
        break;
    }
    case 3: {
        bool have = false;
        for (int k = 0; k < n; k++)
            have |= !strcmp(words[k], toks[0]);
        result = js_bool(have);
        break;
    }
    case 4: {
        bool done = false;
        for (int k = 0; k < n; k++)
            if (!strcmp(words[k], toks[0])) {
                free(words[k]);
                words[k] = strdup(toks[1]);
                done = true;
                break;
            }
        if (done)
            set_class_words(e, words, n);
        result = js_bool(done);
        break;
    }
    }
out:
    for (int i = 0; i < nt2; i++)
        free(toks[i]);
    for (int k = 0; k < n; k++)
        free(words[k]);
    return result;
}

static jsval tl_length(ARGS)
{
    UNUSED;
    struct node *e = tokens_node(J, this_val);
    if (!e)
        return JS_EXCEPTION;
    char *words[256];
    int n = class_words(e, words, 256);
    for (int k = 0; k < n; k++)
        free(words[k]);
    return js_number(n);
}

static jsval tl_item(ARGS)
{
    UNUSED;
    struct node *e = tokens_node(J, this_val);
    if (!e)
        return JS_EXCEPTION;
    char *words[256];
    int n = class_words(e, words, 256);
    bool threw = false;
    double i = js_to_number(J, argv[0], &threw);
    jsval r = i >= 0 && i < n ? str(J, words[(int)i]) : JS_NULL;
    for (int k = 0; k < n; k++)
        free(words[k]);
    return r;
}

static jsval tl_value(ARGS)
{
    UNUSED;
    struct node *e = tokens_node(J, this_val);
    if (!e)
        return JS_EXCEPTION;
    return str(J, node_attr(e, "class"));
}

static jsval tl_set_value(ARGS)
{
    UNUSED;
    struct node *e = tokens_node(J, this_val);
    if (!e)
        return JS_EXCEPTION;
    char *v = arg_text(J, argv[0]);
    if (!v)
        return JS_EXCEPTION;
    set_attr(e, "class", v);
    free(v);
    return JS_UNDEFINED;
}

/* --- style ----------------------------------------------------------------- */

static const char *const css_props[] = {
    "align-content", "align-items", "align-self", "animation", "background", "background-color",
    "background-image", "background-position", "background-repeat", "background-size", "border",
    "border-bottom", "border-color", "border-left", "border-radius", "border-right", "border-style",
    "border-top", "border-width", "bottom", "box-shadow", "box-sizing", "clear", "color", "column-gap",
    "content", "cursor", "display", "fill", "filter", "flex", "flex-basis", "flex-direction", "flex-flow",
    "flex-grow", "flex-shrink", "flex-wrap", "float", "font", "font-family", "font-size", "font-style",
    "font-weight", "gap", "grid-area", "grid-column", "grid-row", "grid-template-areas",
    "grid-template-columns", "grid-template-rows", "height", "justify-content", "justify-items",
    "justify-self", "left", "letter-spacing", "line-height", "list-style", "list-style-type", "margin",
    "margin-bottom", "margin-left", "margin-right", "margin-top", "max-height", "max-width", "min-height",
    "min-width", "object-fit", "opacity", "order", "outline", "overflow", "overflow-x", "overflow-y",
    "padding", "padding-bottom", "padding-left", "padding-right", "padding-top", "pointer-events",
    "position", "right", "row-gap", "stroke", "text-align", "text-decoration", "text-indent",
    "text-overflow", "text-shadow", "text-transform", "top", "transform", "transform-origin", "transition",
    "vertical-align", "visibility", "white-space", "width", "word-break", "word-spacing", "z-index",
    "inset", "border-collapse", "border-spacing", "table-layout", "user-select", "will-change",
};

static struct node *style_node(struct js *J, jsval this_val)
{
    struct node *n = js_host_data(this_val, &style_class);
    if (!n)
        js_throw_type(J, "Illegal invocation");
    return n;
}

/* the value of property `name` in a style="" text: malloc'd, or 0 */
static char *style_value(const char *text, const char *name)
{
    size_t k = strlen(name);
    const char *p = text;
    while (p && *p) {
        while (*p && (is_space((unsigned char)*p) || *p == ';'))
            p++;
        const char *ns = p;
        while (*p && *p != ':' && *p != ';')
            p++;
        const char *ne = p;
        while (ne > ns && is_space((unsigned char)ne[-1]))
            ne--;
        if (*p != ':') {
            continue;
        }
        p++;
        const char *vs = p;
        int paren = 0;
        while (*p && (*p != ';' || paren)) {
            paren += *p == '(' ? 1 : *p == ')' ? -1 : 0;
            p++;
        }
        const char *ve = p;
        while (vs < ve && is_space((unsigned char)*vs))
            vs++;
        while (ve > vs && is_space((unsigned char)ve[-1]))
            ve--;
        if ((size_t)(ne - ns) == k && !strncmp(ns, name, k))
            return copy_text(vs, (size_t)(ve - vs));
    }
    return 0;
}

/* style="" with `name` set to value (0 or "": removed) */
static void style_put(struct node *e, const char *name, const char *value)
{
    const char *text = node_attr(e, "style");
    struct out o = { 0 };
    put(&o, "", 0);
    bool done = false;
    size_t k = strlen(name);
    const char *p = text;
    while (p && *p) {
        while (*p && (is_space((unsigned char)*p) || *p == ';'))
            p++;
        if (!*p)
            break;
        const char *ds = p;
        int paren = 0;
        while (*p && (*p != ';' || paren)) {
            paren += *p == '(' ? 1 : *p == ')' ? -1 : 0;
            p++;
        }
        const char *de = p;
        const char *colon = memchr(ds, ':', (size_t)(de - ds));
        const char *ne = colon ? colon : de;
        while (ne > ds && is_space((unsigned char)ne[-1]))
            ne--;
        bool same = (size_t)(ne - ds) == k && !strncmp(ds, name, k);
        if (same) {
            if (value && value[0] && !done) {
                if (o.n)
                    put_str(&o, " ");
                put_str(&o, name);
                put_str(&o, ": ");
                put_str(&o, value);
                put_str(&o, ";");
            }
            done = true;
            continue;
        }
        if (o.n)
            put_str(&o, " ");
        put(&o, ds, (size_t)(de - ds));
        put_str(&o, ";");
    }
    if (!done && value && value[0]) {
        if (o.n)
            put_str(&o, " ");
        put_str(&o, name);
        put_str(&o, ": ");
        put_str(&o, value);
        put_str(&o, ";");
    }
    if (o.n)
        set_attr(e, "style", o.s);
    else
        remove_attr(e, "style");
    free(o.s);
}

static jsval st_get(ARGS)
{
    UNUSED;
    int k = js_magic(J);
    struct node *e = style_node(J, this_val);
    if (!e)
        return JS_EXCEPTION;
    char *v = style_value(node_attr(e, "style"), css_props[k]);
    jsval r = str(J, v);
    free(v);
    return r;
}

static jsval st_set(ARGS)
{
    UNUSED;
    int k = js_magic(J);
    struct node *e = style_node(J, this_val);
    if (!e)
        return JS_EXCEPTION;
    char *v = js_is_nullish(argv[0]) ? strdup("") : arg_text(J, argv[0]);
    if (!v)
        return JS_EXCEPTION;
    /* a number alone for a length: as browsers, ignored (only 0 and unitless properties) */
    style_put(e, css_props[k], v);
    free(v);
    return JS_UNDEFINED;
}

static jsval st_css_text(ARGS)
{
    UNUSED;
    struct node *e = style_node(J, this_val);
    if (!e)
        return JS_EXCEPTION;
    return str(J, node_attr(e, "style"));
}

static jsval st_set_css_text(ARGS)
{
    UNUSED;
    struct node *e = style_node(J, this_val);
    if (!e)
        return JS_EXCEPTION;
    char *v = arg_text(J, argv[0]);
    if (!v)
        return JS_EXCEPTION;
    if (v[0])
        set_attr(e, "style", v);
    else
        remove_attr(e, "style");
    free(v);
    return JS_UNDEFINED;
}

/* setProperty (0), getPropertyValue (1), removeProperty (2) */
static jsval st_method(ARGS)
{
    UNUSED;
    int op = js_magic(J);
    struct node *e = style_node(J, this_val);
    if (!e)
        return JS_EXCEPTION;
    char *name = arg_text(J, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    jsval r = JS_UNDEFINED;
    if (op == 0) {
        char *v = js_is_nullish(argv[1]) ? strdup("") : arg_text(J, argv[1]);
        if (v) {
            style_put(e, name, v);
            free(v);
        } else
            r = JS_EXCEPTION;
    } else {
        char *v = style_value(node_attr(e, "style"), name);
        r = str(J, v);
        free(v);
        if (op == 2)
            style_put(e, name, 0);
    }
    free(name);
    return r;
}

static jsval e_style(ARGS)
{
    UNUSED;
    THIS_NODE;
    jsval o = js_get(J, this_val, "%style");
    if (js_is_object(o))
        return o;
    o = js_new_host(J, roots[R_STYLE], self, &style_class);
    js_define_value(J, this_val, "%style", o);
    return o;
}

static jsval e_dataset(ARGS)
{
    UNUSED;
    THIS_NODE;
    jsval o = js_new_object(J);
    for (int i = 0; i < self->attr_count; i++) {
        const char *a = self->attrs[i].name;
        if (strncmp(a, "data-", 5))
            continue;
        char camel[128];
        size_t k = 0;
        for (const char *p = a + 5; *p && k < sizeof(camel) - 1; p++) {
            if (*p == '-' && p[1] >= 'a' && p[1] <= 'z') {
                camel[k++] = (char)(p[1] - 32);
                p++;
            } else
                camel[k++] = *p;
        }
        camel[k] = 0;
        js_set(J, o, camel, str(J, self->attrs[i].value));
    }
    return o;
}

static jsval e_inner_text(ARGS)
{
    return n_text_content(J, this_val, argc, argv, nt);
}

/* --- document ----------------------------------------------------------- */

static jsval d_document_element(ARGS) { UNUSED; return wrap(root); }
static jsval d_head(ARGS) { UNUSED; return wrap(node_find(root, "head")); }
static jsval d_body(ARGS) { UNUSED; return wrap(node_find(root, "body")); }

static jsval d_title(ARGS)
{
    UNUSED;
    struct node *t = node_find(root, "title");
    if (!t)
        return str(J, "");
    char *s = text_content(t);
    /* collapsed white space, as the standard says */
    char *o = s;
    bool space = true;
    for (char *p = s; p && *p; p++) {
        if (is_space((unsigned char)*p)) {
            if (!space)
                *o++ = ' ';
            space = true;
        } else {
            *o++ = *p;
            space = false;
        }
    }
    if (o > s && o[-1] == ' ')
        o--;
    if (s)
        *o = 0;
    jsval r = str(J, s);
    free(s);
    return r;
}

static jsval d_set_title(ARGS)
{
    UNUSED;
    char *v = arg_text(J, argv[0]);
    if (!v)
        return JS_EXCEPTION;
    struct node *t = node_find(root, "title");
    if (!t) {
        struct node *head = node_find(root, "head");
        t = new_node(NODE_ELEMENT, "title");
        if (t && head)
            link_before(head, t, 0);
    }
    if (t)
        replace_children_with_text(t, v);
    free(v);
    return JS_UNDEFINED;
}

static jsval d_create_element(ARGS)
{
    UNUSED;
    char *tag = arg_text(J, argv[js_magic(J) ? 1 : 0]);
    if (!tag)
        return JS_EXCEPTION;
    if (!tag[0] || strpbrk(tag, " <>\"'/=")) {
        free(tag);
        return js_throw_named(J, "InvalidCharacterError", "Invalid tag name.");
    }
    struct node *e = new_node(NODE_ELEMENT, tag);
    free(tag);
    if (!e)
        return JS_EXCEPTION;
    keep_detached(e);
    return wrap(e);
}

static jsval d_create_text_node(ARGS)
{
    UNUSED;
    char *t = arg_text(J, argv[0]);
    if (!t)
        return JS_EXCEPTION;
    struct node *n = new_node(NODE_TEXT, "");
    if (!n) {
        free(t);
        return JS_EXCEPTION;
    }
    n->text = t;
    keep_detached(n);
    return wrap(n);
}

static jsval d_create_comment(ARGS)
{
    UNUSED;
    char *t = arg_text(J, argv[0]);
    if (!t)
        return JS_EXCEPTION;
    struct node *n = new_node(NODE_ELEMENT, "#comment");
    if (!n) {
        free(t);
        return JS_EXCEPTION;
    }
    n->text = t;
    keep_detached(n);
    return wrap(n);
}

static jsval d_create_fragment(ARGS)
{
    UNUSED;
    struct node *n = new_node(NODE_ELEMENT, "#fragment");
    if (!n)
        return JS_EXCEPTION;
    keep_detached(n);
    return wrap(n);
}

static jsval d_write(ARGS)
{
    UNUSED;
    int line = js_magic(J);
    struct out o = { 0 };
    put(&o, "", 0);
    for (int i = 0; i < argc; i++) {
        char *t = arg_text(J, argv[i]);
        if (!t) {
            free(o.s);
            return JS_EXCEPTION;
        }
        put_str(&o, t);
        free(t);
    }
    if (line)
        put_str(&o, "\n");
    struct node *frag = parse_fragment(o.s, true);
    free(o.s);
    if (!frag)
        return JS_EXCEPTION;
    /* parsed at once, where the parser stands: the end of the running
     * script's parent (what follows is held out meanwhile) */
    struct node *parent = current_script && current_script->parent ? current_script->parent : node_find(root, "body");
    struct node *ref = 0;
    if (!parent)
        return JS_UNDEFINED;
    while (frag->first) {
        struct node *c = frag->first;
        unlink_node(c);
        link_before(parent, c, ref);
    }
    return JS_UNDEFINED;
}

static jsval d_ready_state(ARGS)
{
    UNUSED;
    return str(J, ready_state == 0 ? "loading" : ready_state == 1 ? "interactive" : "complete");
}

static jsval d_current_script(ARGS)
{
    UNUSED;
    return current_script ? wrap(current_script) : JS_NULL;
}

static jsval d_url(ARGS)
{
    UNUSED;
    return str(J, page_url);
}

static jsval d_cookie(ARGS)
{
    UNUSED;
    char *c = cookie_get(page_url, true);       /* (27q: the browser's cookie jar, http.c) */
    jsval v = str(J, c);
    free(c);
    return v;
}

static jsval d_set_cookie(ARGS)
{
    UNUSED;
    char *v = arg_text(J, argv[0]);
    if (!v)
        return JS_EXCEPTION;
    cookie_set(page_url, v, true);
    free(v);
    return JS_UNDEFINED;
}

/* document.forms (0), images (1), links (2), scripts (3) */
static jsval d_collection(ARGS)
{
    UNUSED;
    int which = js_magic(J);
    struct gather g = { 0 };
    for (struct node *n = root; n; n = walk_next(n, root)) {
        if (!is_element(n))
            continue;
        bool ok = which == 0 ? !strcmp(n->tag, "form") : which == 1 ? !strcmp(n->tag, "img")
                  : which == 2 ? (!strcmp(n->tag, "a") || !strcmp(n->tag, "area")) && node_attr(n, "href")
                               : !strcmp(n->tag, "script");
        if (ok)
            gather_add(&g, n);
    }
    return gathered(&g);
}

static jsval d_default_view(ARGS)
{
    UNUSED;
    return js_global(J);
}

static jsval d_active_element(ARGS)
{
    UNUSED;
    return wrap(dom_focused ? dom_focused : node_find(root, "body"));
}

static jsval d_child_nodes(ARGS)
{
    UNUSED;
    struct node *a[1] = { root };
    return list_of(a, 1);
}

static jsval d_true(ARGS)
{
    UNUSED;
    return JS_TRUE;
}

static jsval d_node_type(ARGS)
{
    UNUSED;
    return js_number(9);
}

static jsval d_node_name(ARGS)
{
    UNUSED;
    return str(J, "#document");
}

static jsval d_null(ARGS)
{
    UNUSED;
    return JS_NULL;
}

static jsval d_visibility(ARGS)
{
    UNUSED;
    return str(J, "visible");
}

static jsval d_false(ARGS)
{
    UNUSED;
    return JS_FALSE;
}

/* --- events (27q) ------------------------------------------------------------
 *
 * Event objects carry a `struct event` (their fields, read by the
 * prototypes' accessors). A target's listeners are kept under a private
 * key (scripts cannot see them): an object of type -> array of records
 * [callback, flags]; a record's flags are changed in place (removed, a
 * once listener used), so a dispatch working on a copy of the list sees
 * it. Event handlers (onclick, and onclick="..." attributes) are one
 * record each with LISTEN_HANDLER: the value is looked up when called, in
 * a second private map, type -> [value, the attribute text it came from].
 * An attribute's handler is compiled when it is first needed and again
 * when the attribute's text changed; it is registered first in the list
 * (as the parser would have done, before any script ran). */

enum { EVF_BUBBLES = 1, EVF_CANCELABLE = 2, EVF_CANCELED = 4, EVF_STOP = 8, EVF_STOP_NOW = 16, EVF_TRUSTED = 32,
       EVF_DISPATCH = 64, EVF_INIT = 128, EVF_PASSIVE = 256, EVF_COMPOSED = 512, EVF_REPEAT = 1024 };
enum { EK_EVENT, EK_CUSTOM, EK_UI, EK_MOUSE, EK_POINTER, EK_WHEEL, EK_KEYBOARD, EK_FOCUS, EK_INPUT, EK_SUBMIT,
       EK_HASH, EK_COUNT };
enum { LISTEN_CAPTURE = 1, LISTEN_ONCE = 2, LISTEN_PASSIVE = 4, LISTEN_REMOVED = 8, LISTEN_HANDLER = 16 };

struct event {
    jsval type, target, current, related, detail, key, code, data, input_type, path, submitter, old_url, new_url;
    char *ctype;                                /* type as C text */
    double stamp, client_x, client_y, screen_x, screen_y, page_x, page_y, offset_x, offset_y;
    double delta_x, delta_y, delta_z;
    int phase, kind, button, buttons, mods, key_code, char_code, location, delta_mode;
    unsigned flags;
};
#define EVENT_VALUES 13                         /* (the jsvals first, in order) */

static void event_finalize(void *data)
{
    struct event *e = data;
    free(e->ctype);
    free(e);
}

static void event_trace(struct js *J, void *data)
{
    jsval *v = data;
    for (int i = 0; i < EVENT_VALUES; i++)
        js_mark(J, v[i]);
}

static const struct js_host_class event_class = { "Event", event_finalize, event_trace };


static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void note_type(const char *type)
{
    for (int i = 0; i < wanted_n; i++)
        if (!strcmp(wanted[i], type))
            return;
    if (wanted_n == wanted_cap) {
        int cap = wanted_cap ? wanted_cap * 2 : 16;
        char **w = realloc(wanted, (size_t)cap * sizeof *w);
        if (!w)
            return;
        wanted = w, wanted_cap = cap;
    }
    char *t = strdup(type);
    if (t)
        wanted[wanted_n++] = t;
}

/* on* attributes in the tree under top: their types are listened to */
static void note_attributes(struct node *top)
{
    for (struct node *n = top; n; n = node_next(n, top))
        if (n->kind == NODE_ELEMENT)
            for (int i = 0; i < n->attr_count; i++)
                if (n->attrs[i].name[0] == 'o' && n->attrs[i].name[1] == 'n' && n->attrs[i].name[2])
                    note_type(n->attrs[i].name + 2);
}

bool dom_wants(const char *type)
{
    if (!J)
        return false;
    for (int i = 0; i < wanted_n; i++)
        if (!strcmp(wanted[i], type))
            return true;
    return false;
}

/* (a microtask checkpoint: what queueMicrotask, and later Promises, queued) */
static void checkpoint(void)
{
    while (J && js_run_jobs(J) == JS_EXCEPTION)
        script_error("a microtask");
}

static void enter(void)
{
    if (!depth)
        js_set_stack_base(J, stack_base);
}

static jsval call_script(jsval f, jsval this_val, int argc, jsval *argv)
{
    depth++;
    jsval r = js_call(J, f, this_val, argc, argv);
    depth--;
    return r;
}

/* the events the browser fires: their kind, bubbles, cancelable */
static const struct { const char *type; int kind; bool bubbles, cancelable; } event_types[] = {
    { "click", EK_MOUSE, 1, 1 }, { "dblclick", EK_MOUSE, 1, 1 }, { "mousedown", EK_MOUSE, 1, 1 },
    { "mouseup", EK_MOUSE, 1, 1 }, { "mousemove", EK_MOUSE, 1, 1 }, { "mouseover", EK_MOUSE, 1, 1 },
    { "mouseout", EK_MOUSE, 1, 1 }, { "mouseenter", EK_MOUSE, 0, 0 }, { "mouseleave", EK_MOUSE, 0, 0 },
    { "contextmenu", EK_MOUSE, 1, 1 }, { "pointerdown", EK_POINTER, 1, 1 }, { "pointerup", EK_POINTER, 1, 1 },
    { "pointermove", EK_POINTER, 1, 1 }, { "pointerover", EK_POINTER, 1, 1 }, { "pointerout", EK_POINTER, 1, 1 },
    { "pointerenter", EK_POINTER, 0, 0 }, { "pointerleave", EK_POINTER, 0, 0 }, { "wheel", EK_WHEEL, 1, 1 },
    { "keydown", EK_KEYBOARD, 1, 1 }, { "keypress", EK_KEYBOARD, 1, 1 }, { "keyup", EK_KEYBOARD, 1, 1 },
    { "focus", EK_FOCUS, 0, 0 }, { "blur", EK_FOCUS, 0, 0 }, { "focusin", EK_FOCUS, 1, 0 },
    { "focusout", EK_FOCUS, 1, 0 }, { "input", EK_INPUT, 1, 0 }, { "change", EK_EVENT, 1, 0 },
    { "submit", EK_SUBMIT, 1, 1 }, { "reset", EK_EVENT, 1, 1 }, { "scroll", EK_EVENT, 1, 0 },
    { "resize", EK_EVENT, 0, 0 }, { "load", EK_EVENT, 0, 0 }, { "error", EK_EVENT, 0, 0 },
    { "DOMContentLoaded", EK_EVENT, 1, 0 }, { "readystatechange", EK_EVENT, 0, 0 },
    { "hashchange", EK_HASH, 0, 0 },
};

static const char *const kind_names[EK_COUNT] = {
    "Event", "CustomEvent", "UIEvent", "MouseEvent", "PointerEvent", "WheelEvent", "KeyboardEvent", "FocusEvent",
    "InputEvent", "SubmitEvent", "HashChangeEvent",
};
static const int kind_parent[EK_COUNT] = { -1, EK_EVENT, EK_EVENT, EK_UI, EK_MOUSE, EK_MOUSE, EK_UI, EK_UI, EK_UI,
                                           EK_EVENT, EK_EVENT };
static bool kind_is(int kind, int of)
{
    for (; kind >= 0; kind = kind_parent[kind])
        if (kind == of)
            return true;
    return false;
}

/* a new event object of kind (its fields undefined or 0), proto its prototype */
static jsval event_object(int kind, jsval proto, struct event **out)
{
    struct event *e = calloc(1, sizeof *e);
    if (!e)
        return JS_EXCEPTION;
    jsval *v = (jsval *)e;
    for (int i = 0; i < EVENT_VALUES; i++)
        v[i] = JS_NULL;
    e->detail = e->path = JS_UNDEFINED;
    e->kind = kind;
    e->stamp = now_ms();
    jsval o = js_new_host(J, js_is_object(proto) ? proto : roots[R_EVENT + kind], e, &event_class);
    *out = e;
    return o;
}

static void set_type(struct event *e, jsval type)
{
    e->type = type;
    free(e->ctype);
    e->ctype = js_to_utf8(J, type, 0);
    if (!e->ctype) {
        js_take_exception(J);
        e->ctype = strdup("");
    }
}

/* an init dictionary's member: false on a throw */
static bool member(jsval dict, const char *name, jsval *out)
{
    *out = js_is_object(dict) ? js_get(J, dict, name) : JS_UNDEFINED;
    return *out != JS_EXCEPTION;
}

static bool member_flag(jsval dict, const char *name, unsigned bit, unsigned *flags)
{
    jsval v;
    if (!member(dict, name, &v))
        return false;
    if (js_to_boolean(v))
        *flags |= bit;
    return true;
}

static bool member_number(jsval dict, const char *name, double *out)
{
    jsval v;
    if (!member(dict, name, &v))
        return false;
    if (js_is_undefined(v))
        return true;
    bool threw = false;
    double d = js_to_number(J, v, &threw);
    if (threw)
        return false;
    *out = d != d ? 0 : d;
    return true;
}

static bool member_int(jsval dict, const char *name, int *out)
{
    double d = *out;
    if (!member_number(dict, name, &d))
        return false;
    *out = d > 2e9 ? 2000000000 : d < -2e9 ? -2000000000 : (int)d;
    return true;
}

static bool member_string(jsval dict, const char *name, jsval *out)
{
    jsval v;
    if (!member(dict, name, &v))
        return false;
    if (!js_is_undefined(v)) {
        v = js_to_string(J, v);
        if (v == JS_EXCEPTION)
            return false;
        *out = v;
    }
    return true;
}

static bool member_node(jsval dict, const char *name, jsval *out)
{
    jsval v;
    if (!member(dict, name, &v))
        return false;
    *out = js_is_object(v) ? v : JS_NULL;
    return true;
}

static jsval event_construct(struct js *J, int argc, jsval *argv, jsval nt, int kind)
{
    if (js_is_undefined(nt))
        return js_throw_type(J, "Failed to construct '%s': Please use the 'new' operator", kind_names[kind]);
    if (argc < 1)
        return js_throw_type(J, "Failed to construct '%s': 1 argument required", kind_names[kind]);
    jsval proto = js_get(J, nt, "prototype");
    if (proto == JS_EXCEPTION)
        return JS_EXCEPTION;
    struct event *e;
    jsval o = event_object(kind, proto, &e);
    if (o == JS_EXCEPTION)
        return o;
    jsval type = js_to_string(J, argv[0]);
    if (type == JS_EXCEPTION)
        return JS_EXCEPTION;
    set_type(e, type);
    jsval d = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!js_is_nullish(d) && !js_is_object(d))
        return js_throw_type(J, "Failed to construct '%s': The provided value is not of type '%sInit'",
                             kind_names[kind], kind_names[kind]);
    unsigned f = 0;
    if (!member_flag(d, "bubbles", EVF_BUBBLES, &f) || !member_flag(d, "cancelable", EVF_CANCELABLE, &f) ||
        !member_flag(d, "composed", EVF_COMPOSED, &f))
        return JS_EXCEPTION;
    if (kind == EK_CUSTOM || kind_is(kind, EK_UI)) {
        jsval detail;
        if (!member(d, "detail", &detail))
            return JS_EXCEPTION;
        e->detail = kind == EK_CUSTOM ? (js_is_undefined(detail) ? JS_NULL : detail)
                                      : js_number(js_is_undefined(detail) ? 0 : js_to_number(J, detail, 0));
    }
    if (kind_is(kind, EK_MOUSE) || kind == EK_KEYBOARD) {
        static const char *const mod_names[4] = { "ctrlKey", "shiftKey", "altKey", "metaKey" };
        for (int i = 0; i < 4; i++) {
            unsigned bit = 0;
            if (!member_flag(d, mod_names[i], 1, &bit))
                return JS_EXCEPTION;
            if (bit)
                e->mods |= 1 << i;
        }
    }
    if (kind_is(kind, EK_MOUSE)) {
        if (!member_number(d, "screenX", &e->screen_x) || !member_number(d, "screenY", &e->screen_y) ||
            !member_number(d, "clientX", &e->client_x) || !member_number(d, "clientY", &e->client_y) ||
            !member_int(d, "button", &e->button) || !member_int(d, "buttons", &e->buttons) ||
            !member_node(d, "relatedTarget", &e->related))
            return JS_EXCEPTION;
        e->page_x = e->offset_x = e->client_x;
        e->page_y = e->offset_y = e->client_y;
    }
    if (kind == EK_WHEEL && (!member_number(d, "deltaX", &e->delta_x) || !member_number(d, "deltaY", &e->delta_y) ||
                             !member_number(d, "deltaZ", &e->delta_z) || !member_int(d, "deltaMode", &e->delta_mode)))
        return JS_EXCEPTION;
    if (kind == EK_KEYBOARD) {
        e->key = e->code = str(J, "");
        if (!member_string(d, "key", &e->key) || !member_string(d, "code", &e->code) ||
            !member_int(d, "location", &e->location) || !member_flag(d, "repeat", EVF_REPEAT, &f) ||
            !member_int(d, "keyCode", &e->key_code) || !member_int(d, "charCode", &e->char_code))
            return JS_EXCEPTION;
    }
    if (kind == EK_FOCUS && !member_node(d, "relatedTarget", &e->related))
        return JS_EXCEPTION;
    if (kind == EK_INPUT) {
        e->input_type = str(J, "");
        jsval data;
        if (!member(d, "data", &data) || !member_string(d, "inputType", &e->input_type))
            return JS_EXCEPTION;
        e->data = js_is_nullish(data) ? JS_NULL : js_to_string(J, data);
    }
    if (kind == EK_SUBMIT && !member_node(d, "submitter", &e->submitter))
        return JS_EXCEPTION;
    if (kind == EK_HASH) {
        e->old_url = e->new_url = str(J, "");
        if (!member_string(d, "oldURL", &e->old_url) || !member_string(d, "newURL", &e->new_url))
            return JS_EXCEPTION;
    }
    e->flags = f | EVF_INIT;
    return o;
}

#define EVENT_CTOR(fn, kind) \
    static jsval fn(ARGS) { (void)this_val; return event_construct(J, argc, argv, nt, kind); }
EVENT_CTOR(ctor_event, EK_EVENT)
EVENT_CTOR(ctor_custom, EK_CUSTOM)
EVENT_CTOR(ctor_ui, EK_UI)
EVENT_CTOR(ctor_mouse, EK_MOUSE)
EVENT_CTOR(ctor_pointer, EK_POINTER)
EVENT_CTOR(ctor_wheel, EK_WHEEL)
EVENT_CTOR(ctor_keyboard, EK_KEYBOARD)
EVENT_CTOR(ctor_focus, EK_FOCUS)
EVENT_CTOR(ctor_input, EK_INPUT)
EVENT_CTOR(ctor_submit, EK_SUBMIT)
EVENT_CTOR(ctor_hash, EK_HASH)
static const js_native event_ctors[EK_COUNT] = {
    ctor_event, ctor_custom, ctor_ui, ctor_mouse, ctor_pointer, ctor_wheel, ctor_keyboard, ctor_focus, ctor_input,
    ctor_submit, ctor_hash,
};

static struct event *this_event(struct js *J, jsval this_val)
{
    struct event *e = js_host_data(this_val, &event_class);
    if (!e)
        js_throw_type(J, "Illegal invocation");
    return e;
}

#define THIS_EVENT                                \
    struct event *ev = this_event(J, this_val);   \
    if (!ev)                                      \
        return JS_EXCEPTION

enum { EG_TYPE, EG_TARGET, EG_CURRENT, EG_PHASE, EG_BUBBLES, EG_CANCELABLE, EG_PREVENTED, EG_COMPOSED, EG_TRUSTED,
       EG_STAMP, EG_RETURN, EG_CANCEL_BUBBLE, EG_DETAIL, EG_VIEW, EG_WHICH, EG_SCREEN_X, EG_SCREEN_Y, EG_CLIENT_X,
       EG_CLIENT_Y, EG_PAGE_X, EG_PAGE_Y, EG_OFFSET_X, EG_OFFSET_Y, EG_CTRL, EG_SHIFT, EG_ALT, EG_META, EG_BUTTON,
       EG_BUTTONS, EG_RELATED, EG_MOVEMENT, EG_KEY, EG_CODE, EG_LOCATION, EG_REPEAT, EG_COMPOSING, EG_KEY_CODE,
       EG_CHAR_CODE, EG_DATA, EG_INPUT_TYPE, EG_DELTA_X, EG_DELTA_Y, EG_DELTA_Z, EG_DELTA_MODE, EG_SUBMITTER,
       EG_POINTER_ID, EG_POINTER_TYPE, EG_PRIMARY, EG_WIDTH, EG_OLD_URL, EG_NEW_URL };

static jsval ev_get(ARGS)
{
    UNUSED;
    int which = js_magic(J);
    THIS_EVENT;
    switch (which) {
    case EG_TYPE: return ev->type;
    case EG_TARGET: return ev->target;
    case EG_CURRENT: return ev->current;
    case EG_PHASE: return js_number(ev->phase);
    case EG_BUBBLES: return js_bool(ev->flags & EVF_BUBBLES);
    case EG_CANCELABLE: return js_bool(ev->flags & EVF_CANCELABLE);
    case EG_PREVENTED: return js_bool(ev->flags & EVF_CANCELED);
    case EG_COMPOSED: return js_bool(ev->flags & EVF_COMPOSED);
    case EG_TRUSTED: return js_bool(ev->flags & EVF_TRUSTED);
    case EG_STAMP: return js_number(ev->stamp);
    case EG_RETURN: return js_bool(!(ev->flags & EVF_CANCELED));
    case EG_CANCEL_BUBBLE: return js_bool(ev->flags & EVF_STOP);
    case EG_DETAIL: return ev->detail;
    case EG_VIEW: return kind_is(ev->kind, EK_UI) && (ev->flags & EVF_TRUSTED) ? js_global(J) : JS_NULL;
    case EG_WHICH: return js_number(ev->kind == EK_KEYBOARD ? (ev->key_code ? ev->key_code : ev->char_code)
                                    : kind_is(ev->kind, EK_MOUSE) ? ev->button + 1 : 0);
    case EG_SCREEN_X: return js_number(ev->screen_x);
    case EG_SCREEN_Y: return js_number(ev->screen_y);
    case EG_CLIENT_X: return js_number(ev->client_x);
    case EG_CLIENT_Y: return js_number(ev->client_y);
    case EG_PAGE_X: return js_number(ev->page_x);
    case EG_PAGE_Y: return js_number(ev->page_y);
    case EG_OFFSET_X: return js_number(ev->offset_x);
    case EG_OFFSET_Y: return js_number(ev->offset_y);
    case EG_CTRL: return js_bool(ev->mods & 1);
    case EG_SHIFT: return js_bool(ev->mods & 2);
    case EG_ALT: return js_bool(ev->mods & 4);
    case EG_META: return js_bool(ev->mods & 8);
    case EG_BUTTON: return js_number(ev->button);
    case EG_BUTTONS: return js_number(ev->buttons);
    case EG_RELATED: return ev->related;
    case EG_MOVEMENT: return js_number(0);
    case EG_KEY: return ev->key;
    case EG_CODE: return ev->code;
    case EG_LOCATION: return js_number(ev->location);
    case EG_REPEAT: return js_bool(ev->flags & EVF_REPEAT);
    case EG_COMPOSING: return JS_FALSE;
    case EG_KEY_CODE: return js_number(ev->key_code);
    case EG_CHAR_CODE: return js_number(ev->char_code);
    case EG_DATA: return ev->data;
    case EG_INPUT_TYPE: return ev->input_type;
    case EG_DELTA_X: return js_number(ev->delta_x);
    case EG_DELTA_Y: return js_number(ev->delta_y);
    case EG_DELTA_Z: return js_number(ev->delta_z);
    case EG_DELTA_MODE: return js_number(ev->delta_mode);
    case EG_SUBMITTER: return ev->submitter;
    case EG_POINTER_ID: return js_number(1);
    case EG_POINTER_TYPE: return str(J, "mouse");
    case EG_PRIMARY: return JS_TRUE;
    case EG_WIDTH: return js_number(1);
    case EG_OLD_URL: return ev->old_url;
    case EG_NEW_URL: return ev->new_url;
    }
    return JS_UNDEFINED;
}

static void cancel(struct event *ev)
{
    if ((ev->flags & EVF_CANCELABLE) && !(ev->flags & EVF_PASSIVE))
        ev->flags |= EVF_CANCELED;
}

/* returnValue, cancelBubble */
static jsval ev_set(ARGS)
{
    UNUSED;
    int which = js_magic(J);
    THIS_EVENT;
    bool on = js_to_boolean(argv[0]);
    if (which == EG_RETURN && !on)
        cancel(ev);
    if (which == EG_CANCEL_BUBBLE && on)
        ev->flags |= EVF_STOP;
    return JS_UNDEFINED;
}

/* magic 0 stopPropagation, 1 stopImmediatePropagation, 2 preventDefault, 3 composedPath */
static jsval ev_method(ARGS)
{
    UNUSED;
    int which = js_magic(J);
    THIS_EVENT;
    switch (which) {
    case 0:
        ev->flags |= EVF_STOP;
        break;
    case 1:
        ev->flags |= EVF_STOP | EVF_STOP_NOW;
        break;
    case 2:
        cancel(ev);
        break;
    case 3: {
        jsval a = js_new_array(J, 0);
        uint32_t n = js_is_array(ev->path) ? js_array_length(ev->path) : 0;
        for (uint32_t i = 0; i < n; i++)
            js_set_index(J, a, i, js_get_index(J, ev->path, i));
        return a;
    }
    }
    return JS_UNDEFINED;
}

/* initEvent(type, bubbles, cancelable), initCustomEvent(..., detail) (magic 1) */
static jsval ev_init(ARGS)
{
    UNUSED;
    int custom = js_magic(J);
    THIS_EVENT;
    if (ev->flags & EVF_DISPATCH)
        return JS_UNDEFINED;
    jsval type = js_to_string(J, argv[0]);
    if (type == JS_EXCEPTION)
        return JS_EXCEPTION;
    set_type(ev, type);
    ev->flags = (ev->flags & EVF_TRUSTED) | EVF_INIT | (js_to_boolean(argv[1]) ? EVF_BUBBLES : 0) |
                (js_to_boolean(argv[2]) ? EVF_CANCELABLE : 0);
    if (custom)
        ev->detail = argc > 3 ? argv[3] : JS_NULL;
    return JS_UNDEFINED;
}

static jsval ev_modifier_state(ARGS)
{
    UNUSED;
    THIS_EVENT;
    char *k = arg_text(J, argv[0]);
    if (!k)
        return JS_EXCEPTION;
    bool on = (!strcmp(k, "Control") && (ev->mods & 1)) || (!strcmp(k, "Shift") && (ev->mods & 2)) ||
              (!strcmp(k, "Alt") && (ev->mods & 4)) || (!strcmp(k, "Meta") && (ev->mods & 8));
    free(k);
    return js_bool(on);
}

/* document.createEvent(name): not initialized until initEvent */
static jsval d_create_event(ARGS)
{
    UNUSED;
    char *name = arg_text(J, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    static const struct { const char *name; int kind; } names[] = {
        { "event", EK_EVENT }, { "events", EK_EVENT }, { "htmlevents", EK_EVENT }, { "customevent", EK_CUSTOM },
        { "uievent", EK_UI }, { "uievents", EK_UI }, { "mouseevent", EK_MOUSE }, { "mouseevents", EK_MOUSE },
        { "keyboardevent", EK_KEYBOARD }, { "focusevent", EK_FOCUS }, { "wheelevent", EK_WHEEL },
        { "hashchangeevent", EK_HASH },
    };
    int kind = -1;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (same_word(name, names[i].name))
            kind = names[i].kind;
    free(name);
    if (kind < 0)
        return js_throw_named(J, "NotSupportedError", "The provided event type is invalid.");
    struct event *e;
    jsval o = event_object(kind, JS_UNDEFINED, &e);
    if (o != JS_EXCEPTION)
        set_type(e, str(J, ""));
    return o;
}

/* --- listeners --- */

static jsval listener_list(jsval target, const char *type, bool make)
{
    jsval all = js_get_key(J, target, roots[R_LISTENERS]);
    if (!js_is_object(all)) {
        if (!make)
            return JS_UNDEFINED;
        all = js_new_object_proto(J, JS_NULL);
        js_set_key(J, target, roots[R_LISTENERS], all);
    }
    jsval list = js_get(J, all, type);
    if (!js_is_array(list)) {
        if (!make)
            return JS_UNDEFINED;
        list = js_new_array(J, 0);
        js_set(J, all, type, list);
    }
    return list;
}

static unsigned record_flags(jsval rec)
{
    jsval f = js_get_index(J, rec, 1);
    return js_is_number(f) ? (unsigned)js_number_value(f) : 0;
}

/* the list without rec (marked removed: a dispatch going on skips it) */
static void drop_record(jsval target, const char *type, jsval rec)
{
    js_set_index(J, rec, 1, js_number(record_flags(rec) | LISTEN_REMOVED));
    jsval list = listener_list(target, type, false);
    if (!js_is_array(list))
        return;
    jsval kept = js_new_array(J, 0);
    uint32_t n = js_array_length(list), k = 0;
    for (uint32_t i = 0; i < n; i++) {
        jsval r = js_get_index(J, list, i);
        if (r != rec)
            js_set_index(J, kept, k++, r);
    }
    js_set(J, js_get_key(J, target, roots[R_LISTENERS]), type, kept);
}

static jsval event_target(struct js *J, jsval this_val)
{
    jsval t = js_is_nullish(this_val) ? js_global(J) : this_val;
    if (!js_is_object(t))
        js_throw_type(J, "Illegal invocation");
    return js_is_object(t) ? t : JS_EXCEPTION;
}

/* options: a boolean (capture) or { capture, once, passive } */
static bool listen_options(jsval opt, bool adding, unsigned *flags)
{
    *flags = 0;
    if (!js_is_object(opt)) {
        if (js_to_boolean(opt))
            *flags |= LISTEN_CAPTURE;
        return true;
    }
    if (!member_flag(opt, "capture", LISTEN_CAPTURE, flags))
        return false;
    if (adding && (!member_flag(opt, "once", LISTEN_ONCE, flags) || !member_flag(opt, "passive", LISTEN_PASSIVE, flags)))
        return false;
    return true;
}

static jsval et_add(ARGS)
{
    UNUSED;
    jsval target = event_target(J, this_val);
    if (target == JS_EXCEPTION)
        return target;
    if (js_is_nullish(argv[1]))
        return JS_UNDEFINED;
    if (!js_is_object(argv[1]))
        return js_throw_type(J, "Failed to execute 'addEventListener' on 'EventTarget': parameter 2 is not of type 'Object'.");
    unsigned flags;
    if (!listen_options(argv[2], true, &flags))
        return JS_EXCEPTION;
    char *type = arg_text(J, argv[0]);
    if (!type)
        return JS_EXCEPTION;
    jsval list = listener_list(target, type, true);
    uint32_t n = js_array_length(list);
    for (uint32_t i = 0; i < n; i++) {
        jsval rec = js_get_index(J, list, i);
        unsigned f = record_flags(rec);
        if (js_get_index(J, rec, 0) == argv[1] && !(f & LISTEN_HANDLER) &&
            (f & LISTEN_CAPTURE) == (flags & LISTEN_CAPTURE)) {
            free(type);
            return JS_UNDEFINED;                /* (already there) */
        }
    }
    jsval rec = js_new_array(J, 0);
    js_set_index(J, rec, 0, argv[1]);
    js_set_index(J, rec, 1, js_number(flags));
    js_set_index(J, list, n, rec);
    note_type(type);
    free(type);
    return JS_UNDEFINED;
}

static jsval et_remove(ARGS)
{
    UNUSED;
    jsval target = event_target(J, this_val);
    if (target == JS_EXCEPTION)
        return target;
    unsigned flags;
    if (!listen_options(argv[2], false, &flags))
        return JS_EXCEPTION;
    char *type = arg_text(J, argv[0]);
    if (!type)
        return JS_EXCEPTION;
    jsval list = listener_list(target, type, false);
    uint32_t n = js_is_array(list) ? js_array_length(list) : 0;
    for (uint32_t i = 0; i < n; i++) {
        jsval rec = js_get_index(J, list, i);
        unsigned f = record_flags(rec);
        if (js_get_index(J, rec, 0) == argv[1] && !(f & LISTEN_HANDLER) &&
            (f & LISTEN_CAPTURE) == (flags & LISTEN_CAPTURE)) {
            drop_record(target, type, rec);
            break;
        }
    }
    free(type);
    return JS_UNDEFINED;
}

/* --- event handlers (onclick and onclick="...") --- */

static const char *const handler_names[] = {
    "abort", "blur", "change", "click", "close", "contextmenu", "dblclick", "error", "focus", "focusin",
    "focusout", "input", "invalid", "keydown", "keypress", "keyup", "load", "mousedown", "mouseenter",
    "mouseleave", "mousemove", "mouseout", "mouseover", "mouseup", "pointerdown", "pointermove", "pointerup",
    "pointerover", "pointerout", "pointerenter", "pointerleave", "reset", "resize", "scroll", "select",
    "submit", "wheel", "toggle", "animationend", "transitionend", "loadend", "loadstart", "progress",
    "hashchange", "popstate", "beforeunload", "unload", "message", "storage", "online", "offline", "pageshow",
    "pagehide", "readystatechange",
};
#define FIRST_WINDOW_ONLY 43                    /* (hashchange on: the window's alone) */

/* the body's handlers of these are the window's (onload="..." on <body>) */
static bool window_reflected(const char *type)
{
    static const char *const names[] = { "blur", "error", "focus", "load", "resize", "scroll", "hashchange",
                                         "popstate", "beforeunload", "unload", "message", "storage", "online",
                                         "offline", "pageshow", "pagehide" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(names[i], type))
            return true;
    return false;
}

/* the element whose on<type> attribute is target's handler, or 0 */
static struct node *attribute_holder(jsval target, const char *type)
{
    if (target == js_global(J))
        return window_reflected(type) ? node_find(root, "body") : 0;
    struct node *n = node_of(target);
    if (!is_element(n) || (!strcmp(n->tag, "body") && window_reflected(type)))
        return 0;
    return n;
}

/* the handler is in target's list (first: an attribute's, as if parsed) */
static void handler_listed(jsval target, const char *type, bool first)
{
    jsval list = listener_list(target, type, true);
    uint32_t n = js_array_length(list);
    for (uint32_t i = 0; i < n; i++)
        if (record_flags(js_get_index(J, list, i)) & LISTEN_HANDLER)
            return;
    jsval rec = js_new_array(J, 0);
    js_set_index(J, rec, 0, JS_NULL);
    js_set_index(J, rec, 1, js_number(LISTEN_HANDLER));
    if (!first) {
        js_set_index(J, list, n, rec);
        return;
    }
    jsval moved = js_new_array(J, 0);
    js_set_index(J, moved, 0, rec);
    for (uint32_t i = 0; i < n; i++)
        js_set_index(J, moved, i + 1, js_get_index(J, list, i));
    js_set(J, js_get_key(J, target, roots[R_LISTENERS]), type, moved);
}

static jsval compile_handler(const char *text)
{
    jsval args[2] = { str(J, "event"), str(J, text) };
    jsval f = js_call(J, roots[R_FUNCTION], JS_UNDEFINED, 2, args);
    if (f == JS_EXCEPTION) {
        script_error("an event handler");
        return JS_NULL;
    }
    return f;
}

/* target's handler record for type ([value, the attribute text it was
 * made from or null]), brought up to date with the attribute; make: one
 * made if there is none */
static jsval handler_record(jsval target, const char *type, bool make)
{
    jsval all = js_get_key(J, target, roots[R_HANDLERS]);
    jsval rec = js_is_object(all) ? js_get(J, all, type) : JS_UNDEFINED;
    struct node *holder = attribute_holder(target, type);
    char name[80];
    snprintf(name, sizeof name, "on%s", type);
    const char *text = holder ? node_attr(holder, name) : 0;
    jsval last = js_is_array(rec) ? js_get_index(J, rec, 1) : JS_NULL;
    char *was = js_is_string(last) ? js_to_utf8(J, last, 0) : 0;
    bool stale = text ? !was || strcmp(was, text) : was != 0;
    free(was);
    if (!js_is_array(rec) && !make && !stale)
        return JS_UNDEFINED;
    if (!js_is_array(rec)) {
        if (!js_is_object(all)) {
            all = js_new_object_proto(J, JS_NULL);
            js_set_key(J, target, roots[R_HANDLERS], all);
        }
        rec = js_new_array(J, 0);
        js_set_index(J, rec, 0, JS_NULL);
        js_set_index(J, rec, 1, JS_NULL);
        js_set(J, all, type, rec);
    }
    if (stale) {
        js_set_index(J, rec, 1, text ? str(J, text) : JS_NULL);
        js_set_index(J, rec, 0, text ? compile_handler(text) : JS_NULL);
        if (text)                               /* (the body's for the window: where <body> was parsed) */
            handler_listed(target, type, target != js_global(J));
    }
    return rec;
}

/* <body onload="..."> and the like are the window's handlers, listed
 * when the body is parsed: after the head's scripts, before the body's */
static bool body_handlers_listed;

static void list_body_handlers(void)
{
    struct node *body = node_find(root, "body");
    if (body_handlers_listed || !body)
        return;
    body_handlers_listed = true;
    for (int i = 0; i < body->attr_count; i++) {
        const char *name = body->attrs[i].name;
        if (name[0] == 'o' && name[1] == 'n' && window_reflected(name + 2))
            handler_record(js_global(J), name + 2, false);
    }
}

static bool in_body(const struct node *n)
{
    for (; n; n = n->parent)
        if (!strcmp(n->tag, "body"))
            return true;
    return false;
}

static jsval handler_object(struct js *J, jsval this_val, const char *type)
{
    jsval t = event_target(J, this_val);
    if (t == JS_EXCEPTION)
        return t;
    struct node *n = node_of(t);
    if (n && !strcmp(n->tag, "body") && window_reflected(type))
        return js_global(J);                    /* (body.onload is the window's) */
    return t;
}

static jsval handler_get(ARGS)
{
    UNUSED;
    const char *type = handler_names[js_magic(J)];
    jsval t = handler_object(J, this_val, type);
    if (t == JS_EXCEPTION)
        return t;
    jsval rec = handler_record(t, type, false);
    return js_is_array(rec) ? js_get_index(J, rec, 0) : JS_NULL;
}

static jsval handler_set(ARGS)
{
    UNUSED;
    const char *type = handler_names[js_magic(J)];
    jsval t = handler_object(J, this_val, type);
    if (t == JS_EXCEPTION)
        return t;
    jsval rec = handler_record(t, type, true);
    js_set_index(J, rec, 0, js_is_object(argv[0]) ? argv[0] : JS_NULL);
    handler_listed(t, type, false);
    note_type(type);
    return JS_UNDEFINED;
}

/* --- dispatching --- */

static void call_listener(jsval obj, jsval rec, unsigned flags, jsval evv, struct event *ev)
{
    jsval r;
    if (flags & LISTEN_HANDLER) {
        jsval h = handler_record(obj, ev->ctype, false);
        jsval f = js_is_array(h) ? js_get_index(J, h, 0) : JS_NULL;
        if (!js_is_callable(f))
            return;
        r = call_script(f, obj, 1, &evv);
        if (r == JS_FALSE)
            cancel(ev);                         /* (return false: the default does not happen) */
    } else {
        jsval cb = js_get_index(J, rec, 0);
        if (js_is_callable(cb))
            r = call_script(cb, obj, 1, &evv);
        else {
            jsval he = js_get(J, cb, "handleEvent");
            if (he != JS_EXCEPTION && !js_is_callable(he))
                he = js_throw_type(J, "handleEvent is not a function");
            r = he == JS_EXCEPTION ? he : call_script(he, cb, 1, &evv);
        }
    }
    if (r == JS_EXCEPTION)
        script_error("an event listener");
    if (!depth)
        checkpoint();
}

/* obj's listeners for ev: capture ones (phase 1) or the others (3) */
static void invoke(jsval obj, jsval evv, struct event *ev, int which)
{
    if (attribute_holder(obj, ev->ctype))
        handler_record(obj, ev->ctype, false);  /* (an onclick="..." becomes a listener) */
    jsval list = listener_list(obj, ev->ctype, false);
    if (!js_is_array(list))
        return;
    uint32_t n = js_array_length(list);
    jsval copy = js_new_array(J, 0);
    for (uint32_t i = 0; i < n; i++)
        js_set_index(J, copy, i, js_get_index(J, list, i));
    ev->current = obj;
    for (uint32_t i = 0; i < n; i++) {
        jsval rec = js_get_index(J, copy, i);
        unsigned flags = record_flags(rec);
        if ((flags & LISTEN_REMOVED) || (which == 1) != ((flags & LISTEN_CAPTURE) != 0))
            continue;
        if (flags & LISTEN_ONCE)
            drop_record(obj, ev->ctype, rec);
        if (flags & LISTEN_PASSIVE)
            ev->flags |= EVF_PASSIVE;
        call_listener(obj, rec, flags, evv, ev);
        ev->flags &= ~EVF_PASSIVE;
        if (ev->flags & EVF_STOP_NOW)
            break;
    }
}

/* The standard's dispatch: the path from the target up (its ancestors,
 * the document, the window), the capturing listeners from the top down,
 * the target's, then the bubbling ones back up. True if not canceled. */
static bool dispatch(jsval target, jsval evv, struct event *ev)
{
    ev->flags |= EVF_DISPATCH;
    ev->target = target;
    jsval path = js_new_array(J, 0);
    uint32_t n = 0;
    js_set_index(J, path, n++, target);
    struct node *t = node_of(target), *p = t;
    if (t && t != &docnode) {
        while (p->parent) {
            p = p->parent;
            js_set_index(J, path, n++, wrap(p));
        }
    }
    if (t && (p == root || t == &docnode)) {
        if (t != &docnode)
            js_set_index(J, path, n++, wrap(&docnode));
        if (strcmp(ev->ctype, "load"))          /* (an element's load stays below the window) */
            js_set_index(J, path, n++, js_global(J));
    }
    ev->path = path;
    for (uint32_t i = n; i-- > 1 && !(ev->flags & EVF_STOP);) {
        ev->phase = 1;
        invoke(js_get_index(J, path, i), evv, ev, 1);
    }
    ev->phase = 2;
    if (!(ev->flags & EVF_STOP))
        invoke(target, evv, ev, 1);
    if (!(ev->flags & EVF_STOP))
        invoke(target, evv, ev, 3);
    for (uint32_t i = 1; i < n && (ev->flags & EVF_BUBBLES) && !(ev->flags & EVF_STOP); i++) {
        ev->phase = 3;
        invoke(js_get_index(J, path, i), evv, ev, 3);
    }
    ev->phase = 0;
    ev->current = JS_NULL;
    ev->path = JS_UNDEFINED;
    ev->flags &= ~(EVF_DISPATCH | EVF_STOP | EVF_STOP_NOW);
    return !(ev->flags & EVF_CANCELED);
}

static jsval et_dispatch(ARGS)
{
    UNUSED;
    jsval target = event_target(J, this_val);
    if (target == JS_EXCEPTION)
        return target;
    struct event *ev = js_host_data(argv[0], &event_class);
    if (!ev)
        return js_throw_type(J, "Failed to execute 'dispatchEvent' on 'EventTarget': parameter 1 is not of type 'Event'.");
    if ((ev->flags & EVF_DISPATCH) || !(ev->flags & EVF_INIT))
        return js_throw_named(J, "InvalidStateError", "The event is already being dispatched.");
    ev->flags &= ~(EVF_TRUSTED | EVF_CANCELED);
    jsval keep = argv[0];
    bool ok = dispatch(target, keep, ev);
    return js_bool(ok);
}

static jsval et_ctor(ARGS)
{
    UNUSED;
    if (js_is_undefined(nt))
        return js_throw_type(J, "Failed to construct 'EventTarget': Please use the 'new' operator");
    jsval proto = js_get(J, nt, "prototype");
    return proto == JS_EXCEPTION ? proto : js_new_object_proto(J, js_is_object(proto) ? proto : roots[R_TARGET]);
}

/* --- the browser's events --- */

struct node *dom_document(void)
{
    return &docnode;
}

/* a trusted event of type for target with the user's input d (or 0) */
static jsval user_event(const char *type, struct node *target, const struct dom_event *d, struct event **out)
{
    int kind = EK_EVENT;
    unsigned flags = EVF_TRUSTED | EVF_INIT;
    for (size_t i = 0; i < sizeof event_types / sizeof event_types[0]; i++)
        if (!strcmp(event_types[i].type, type)) {
            kind = event_types[i].kind;
            flags |= (event_types[i].bubbles ? EVF_BUBBLES : 0) | (event_types[i].cancelable ? EVF_CANCELABLE : 0);
        }
    struct event *e;
    jsval o = event_object(kind, JS_UNDEFINED, &e);
    if (o == JS_EXCEPTION)
        return o;
    set_type(e, str(J, type));
    e->flags = flags;
    if (kind_is(kind, EK_UI))
        e->detail = js_number(d ? d->detail : 0);
    if (d) {
        e->client_x = e->screen_x = e->page_x = d->x;
        e->client_y = e->screen_y = d->y;
        e->page_y = d->y + dom_scroll_y;
        e->offset_x = e->page_x - (target ? target->box[0] : 0);
        e->offset_y = e->page_y - (target ? target->box[1] : 0);
        e->button = d->button;
        e->buttons = d->buttons;
        e->mods = d->mods;
        e->related = d->related ? wrap(d->related) : JS_NULL;
        e->key = str(J, d->key ? d->key : "");
        e->code = str(J, d->code ? d->code : "");
        e->key_code = d->key_code;
        e->char_code = d->char_code;
        if (d->repeat)
            e->flags |= EVF_REPEAT;
        e->delta_y = d->delta_y;
        e->data = d->data ? str(J, d->data) : JS_NULL;
        e->input_type = str(J, d->input_type ? d->input_type : "");
        e->submitter = d->submitter ? wrap(d->submitter) : JS_NULL;
    }
    *out = e;
    return o;
}

bool dom_fire(struct node *target, const char *type, const struct dom_event *d)
{
    if (!J || !dom_wants(type))
        return true;
    enter();
    struct event *e;
    jsval ev = user_event(type, target, d, &e);
    if (ev == JS_EXCEPTION) {
        js_take_exception(J);
        return true;
    }
    bool ok = dispatch(target ? wrap(target) : js_global(J), ev, e);
    if (!depth)
        checkpoint();
    return ok;
}

/* what a click on n activates: n or the nearest ancestor that does
 * something (a link, a button, a check box, a label) */
static struct node *activation_target(struct node *n)
{
    for (; n && n != &docnode; n = n->parent) {
        if (!is_element(n))
            continue;
        if ((!strcmp(n->tag, "a") || !strcmp(n->tag, "area")) && node_attr(n, "href"))
            return n;
        if (!strcmp(n->tag, "label") || !strcmp(n->tag, "button"))
            return n;
        if (!strcmp(n->tag, "input")) {
            struct control *k = control_of(n);
            if (k && k->kind != CONTROL_TEXT && k->kind != CONTROL_PASSWORD && k->kind != CONTROL_HIDDEN)
                return n;
        }
    }
    return 0;
}

static bool disabled_control(const struct node *n)
{
    return n && is_control(n) && node_attr(n, "disabled");
}

static struct node *label_control(struct node *label)
{
    const char *id = node_attr(label, "for");
    if (id) {
        for (struct node *n = root; n; n = node_next(n, root))
            if (is_element(n) && is_control(n) && node_attr(n, "id") && !strcmp(node_attr(n, "id"), id))
                return n;
        return 0;
    }
    for (struct node *n = label->first; n; n = node_next(n, label))
        if (is_element(n) && is_control(n))
            return n;
    return 0;
}

/* the click's work after its event (unless canceled): a link sets where
 * to go, a submit button sends its form (after the submit event), a
 * reset button resets it, a label clicks its control */
static void request_submit(struct node *form, struct node *submitter);

static void activate(struct node *act, const struct dom_event *d)
{
    if (!act)
        return;
    if (!strcmp(act->tag, "a") || !strcmp(act->tag, "area")) {
        const char *href = node_attr(act, "href");
        if (starts_word(href, "javascript:")) {
            if (J) {
                char *code = strdup(href + 11), *o = code;
                for (char *p = code; p && *p; p++)      /* (the address's %XX) */
                    if (*p == '%' && hex_digit(p[1]) >= 0 && hex_digit(p[2]) >= 0) {
                        *o++ = (char)(hex_digit(p[1]) * 16 + hex_digit(p[2]));
                        p += 2;
                    } else
                        *o++ = *p;
                if (o)
                    *o = 0;
                if (code) {
                    depth++;
                    jsval r = js_eval(J, code, strlen(code), page_url);
                    depth--;
                    if (r == JS_EXCEPTION)
                        script_error(page_url);
                    free(code);
                }
            }
        } else if (!dom_navigate_to[0] &&
                   url_resolve(page_url, href, dom_navigate_to, sizeof dom_navigate_to))
            dom_navigate_link = true;
        return;
    }
    if (!strcmp(act->tag, "label")) {
        struct node *c = label_control(act);
        struct control *k = c ? control_of(c) : 0;
        if (k && (k->kind == CONTROL_TEXT || k->kind == CONTROL_PASSWORD || k->kind == CONTROL_TEXTAREA ||
                  k->kind == CONTROL_SELECT)) {
            if (dom_focus_hook)
                dom_focus_hook(c);              /* (a field's label gives it the keyboard) */
        } else if (c)
            dom_click(c, d);                    /* (its click activates the control, not the label again) */
        return;
    }
    struct control *k = control_of(act);
    if (!k)
        return;
    if (k->kind == CONTROL_CHECKBOX || k->kind == CONTROL_RADIO) {
        dom_fire(act, "input", 0);
        dom_fire(act, "change", 0);
    }
    if (k->kind == CONTROL_SUBMIT && form_of(act, root))
        request_submit(form_of(act, root), act);       /* (outside a form it does nothing) */
    else if (k->kind == CONTROL_RESET) {
        struct node *form = form_of(act, root);
        if (form && dom_fire(form, "reset", 0))
            form_reset(form, root);
    }
    if (k->kind != CONTROL_SUBMIT && dom_control_hook)
        dom_control_hook(act);
}

/* the radio button of e's group that is on, or 0 */
static struct node *checked_radio(struct node *e)
{
    const char *name = node_attr(e, "name");
    if (!name)
        return 0;
    struct node *form = form_of(e, root);
    for (struct node *n = root; n; n = node_next(n, root)) {
        struct control *k = n->kind == NODE_ELEMENT && !strcmp(n->tag, "input") ? control_of(n) : 0;
        if (k && k->kind == CONTROL_RADIO && k->checked && node_attr(n, "name") &&
            !strcmp(node_attr(n, "name"), name) && form_of(n, root) == form)
            return n;
    }
    return 0;
}

static struct node *clicking[8];                /* (the clicks going on: one never clicks itself again) */
static int clicking_n;

bool dom_click(struct node *e, const struct dom_event *d)
{
    if (!e || disabled_control(e) || clicking_n == 8)
        return false;
    for (int i = 0; i < clicking_n; i++)
        if (clicking[i] == e)
            return false;
    struct node *act = activation_target(e);
    struct control *k = act && is_control(act) ? control_of(act) : 0;
    bool was = k && k->checked;
    struct node *radio_was = 0;
    if (k && k->kind == CONTROL_CHECKBOX)
        k->checked = !k->checked;               /* (before the event: listeners see the new state) */
    else if (k && k->kind == CONTROL_RADIO) {
        radio_was = checked_radio(act);
        radio_check(act, root);
    }
    bool ok = true;
    clicking[clicking_n++] = e;
    if (J && dom_wants("click")) {
        enter();
        static const struct dom_event none;
        struct event *ev;
        jsval v = user_event("click", e, d ? d : &none, &ev);
        if (v != JS_EXCEPTION) {
            if (!d)
                ev->flags &= ~EVF_TRUSTED;      /* (a script's el.click()) */
            ok = dispatch(wrap(e), v, ev);
        } else
            js_take_exception(J);
        if (!depth)
            checkpoint();
    }
    clicking_n--;
    if (!ok) {                                  /* canceled: as it was */
        if (k && (k->kind == CONTROL_CHECKBOX || k->kind == CONTROL_RADIO))
            k->checked = was;
        if (radio_was && control_of(radio_was))
            control_of(radio_was)->checked = true;
        return false;
    }
    activate(act, d);
    return true;
}

static void request_submit(struct node *form, struct node *submitter)
{
    struct dom_event d;
    memset(&d, 0, sizeof d);
    d.submitter = submitter;
    if (form && !dom_fire(form, "submit", &d))
        return;                                 /* (a listener said no) */
    if (!dom_submit_pending) {
        dom_submit_pending = true;
        dom_submit_form = form;
        dom_submitter = submitter;
    }
}

void dom_submit(struct node *form, struct node *submitter)
{
    request_submit(form, submitter);
}

static jsval e_click(ARGS)
{
    UNUSED;
    THIS_NODE;
    dom_click(self, 0);
    return JS_UNDEFINED;
}

/* magic 0 focus(), 1 blur() */
static jsval e_focus(ARGS)
{
    UNUSED;
    int blur = js_magic(J);
    THIS_NODE;
    if (!dom_focus_hook || !is_control(self) || disabled_control(self))
        return JS_UNDEFINED;
    if (!blur && dom_focused != self)
        dom_focus_hook(self);
    else if (blur && dom_focused == self)
        dom_focus_hook(0);
    return JS_UNDEFINED;
}

/* a form's submit() (0: no submit event), requestSubmit() (1), reset() (2) */
static jsval f_submit(ARGS)
{
    UNUSED;
    int which = js_magic(J);
    THIS_NODE;
    if (strcmp(self->tag, "form"))
        return JS_UNDEFINED;
    if (which == 0 && !dom_submit_pending) {
        dom_submit_pending = true;
        dom_submit_form = self;
        dom_submitter = 0;
    } else if (which == 1) {
        struct node *s = node_of(argv[0]);
        request_submit(self, s && is_control(s) ? s : 0);
    } else if (which == 2 && dom_fire(self, "reset", 0)) {
        form_reset(self, root);
        if (dom_control_hook)
            dom_control_hook(self);
    }
    return JS_UNDEFINED;
}

static jsval f_elements(ARGS)
{
    UNUSED;
    THIS_NODE;
    struct gather g = { 0 };
    for (struct node *n = self->first ? self : 0; n; n = node_next(n, self))
        if (n != self && is_element(n) && is_control(n))
            gather_add(&g, n);
    return gathered(&g);
}

/* --- timers --- */

struct timer {
    int id;
    double due;
    double every;                               /* setInterval's period, or -1 */
    int kind;                                   /* 0 a timeout or interval, 1 an animation frame, 2 idle */
    int nesting;
};
static struct timer *timers;
static int timer_n, timer_cap, timer_ids, timer_nesting;

static void forget_timer(int id)
{
    for (int i = 0; i < timer_n; i++)
        if (timers[i].id == id) {
            timers[i] = timers[--timer_n];
            break;
        }
    if (js_is_object(roots[R_TIMERS]))
        js_set_index(J, roots[R_TIMERS], (uint32_t)id, JS_UNDEFINED);
}

static int add_timer(jsval fn, int argc, jsval *argv, double delay, bool repeat, int kind)
{
    if (timer_n == timer_cap) {
        int cap = timer_cap ? timer_cap * 2 : 32;
        struct timer *t = realloc(timers, (size_t)cap * sizeof *t);
        if (!t)
            return 0;
        timers = t, timer_cap = cap;
    }
    if (delay != delay || delay < 0)
        delay = 0;
    if (delay > 2147483647.0)
        delay = 0;                              /* (as browsers do with a delay past 32 bits) */
    if (timer_nesting >= 5 && delay < 4)
        delay = 4;                              /* (the standard's clamp for nested timers) */
    int id = ++timer_ids;
    jsval rec = js_new_array(J, 0);
    js_set_index(J, rec, 0, fn);
    for (int i = 0; i < argc; i++)
        js_set_index(J, rec, (uint32_t)i + 1, argv[i]);
    js_set_index(J, roots[R_TIMERS], (uint32_t)id, rec);
    struct timer *t = &timers[timer_n++];
    t->id = id;
    t->due = now_ms() + delay;
    t->every = repeat ? (delay < 4 ? 4 : delay) : -1;
    t->kind = kind;
    t->nesting = timer_nesting + 1;
    if (dom_timers_hook)
        dom_timers_hook();
    return id;
}

/* setTimeout (magic 0), setInterval (1) */
static jsval w_set_timer(ARGS)
{
    UNUSED;
    bool repeat = js_magic(J);
    jsval fn = argv[0];
    if (!js_is_callable(fn)) {
        fn = js_to_string(J, fn);               /* (a string: run as a script then) */
        if (fn == JS_EXCEPTION)
            return fn;
    }
    bool threw = false;
    double delay = argc > 1 ? js_to_number(J, argv[1], &threw) : 0;
    if (threw)
        return JS_EXCEPTION;
    int extra = argc > 2 ? argc - 2 : 0;
    return js_number(add_timer(fn, extra > 15 ? 15 : extra, argv + 2, delay, repeat, 0));
}

static jsval w_clear_timer(ARGS)
{
    UNUSED;
    bool threw = false;
    double id = js_to_number(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (id >= 1 && id <= timer_ids && id == (int)id)
        forget_timer((int)id);
    return JS_UNDEFINED;
}

/* requestAnimationFrame (magic 0: at the next frame, every 16 ms),
 * requestIdleCallback (1) */
static jsval w_request_frame(ARGS)
{
    UNUSED;
    bool idle = js_magic(J);
    if (!js_is_callable(argv[0]))
        return js_throw_type(J, "Failed to execute: The callback provided as parameter 1 is not a function.");
    double now = now_ms(), next = (floor(now / 16) + 1) * 16;
    return js_number(add_timer(argv[0], 0, 0, idle ? 1 : next - now, false, idle ? 2 : 1));
}

static jsval w_queue_microtask(ARGS)
{
    UNUSED;
    if (!js_is_callable(argv[0]))
        return js_throw_type(J, "Failed to execute 'queueMicrotask' on 'Window': parameter 1 is not of type 'Function'.");
    js_queue_job(J, argv[0], 0, 0);
    return JS_UNDEFINED;
}

static jsval idle_time_remaining(ARGS)
{
    UNUSED;
    return js_number(50);
}

long dom_next_timer(void)
{
    if (!J || !timer_n)
        return -1;
    double first = timers[0].due;
    for (int i = 1; i < timer_n; i++)
        if (timers[i].due < first)
            first = timers[i].due;
    double wait = first - now_ms();
    return wait <= 0 ? 0 : (long)ceil(wait);
}

/* the timers due now, in order (those set meanwhile wait for the next look) */
void dom_run_timers(void)
{
    if (!J || !timer_n)
        return;
    enter();
    double now = now_ms();
    struct { int id; double due; } due[256];
    int n = 0;
    for (int i = 0; i < timer_n && n < 256; i++)
        if (timers[i].due <= now) {
            int k = n++;                        /* (in order of time, then of id) */
            while (k > 0 && (due[k - 1].due > timers[i].due ||
                             (due[k - 1].due == timers[i].due && due[k - 1].id > timers[i].id))) {
                due[k] = due[k - 1];
                k--;
            }
            due[k].id = timers[i].id, due[k].due = timers[i].due;
        }
    for (int k = 0; k < n && J; k++) {
        struct timer *t = 0;
        for (int i = 0; i < timer_n; i++)
            if (timers[i].id == due[k].id)
                t = &timers[i];
        if (!t)
            continue;                           /* (cleared by an earlier one) */
        jsval rec = js_get_index(J, roots[R_TIMERS], (uint32_t)t->id);
        int id = t->id, kind = t->kind, nesting = t->nesting;
        if (t->every >= 0)
            t->due = now_ms() + t->every;
        else
            forget_timer(id);
        if (!js_is_array(rec))
            continue;
        jsval fn = js_get_index(J, rec, 0);
        jsval args[16];
        int argc = 0;
        uint32_t len = js_array_length(rec);
        if (kind == 1)
            args[argc++] = js_number(now);      /* (the frame's time) */
        else if (kind == 2) {
            jsval deadline = js_new_object(J);
            js_set(J, deadline, "didTimeout", JS_FALSE);
            js_define_method(J, deadline, "timeRemaining", idle_time_remaining, 0);
            args[argc++] = deadline;
        } else
            for (uint32_t i = 1; i < len && argc < 16; i++)
                args[argc++] = js_get_index(J, rec, i);
        timer_nesting = nesting;
        jsval r;
        if (js_is_string(fn)) {
            char *code = js_to_utf8(J, fn, 0);
            depth++;
            r = code ? js_eval(J, code, strlen(code), page_url) : JS_EXCEPTION;
            depth--;
            free(code);
        } else
            r = call_script(fn, js_global(J), argc, args);
        timer_nesting = 0;
        if (r == JS_EXCEPTION)
            script_error("a timer");
        checkpoint();
    }
}

/* --- the page's stages, the window --- */

void dom_loaded(int stage)
{
    if (!J)
        return;
    ready_state = stage;
    dom_fire(&docnode, "readystatechange", 0);
    if (stage == 1)
        dom_fire(&docnode, "DOMContentLoaded", 0);
    else if (dom_wants("load")) {
        enter();
        struct event *e;
        jsval ev = user_event("load", 0, 0, &e);
        if (ev != JS_EXCEPTION) {
            jsval path_top = js_global(J);
            dispatch(path_top, ev, e);          /* (the window's load: its target is the document) */
        }
        checkpoint();
    }
}

void dom_resize(int width, int height)
{
    view_w = width;
    view_h = height;
    layout_fresh = false;
    dom_fire(0, "resize", 0);
}

void dom_set_url(const char *url)
{
    snprintf(page_url, sizeof page_url, "%s", url);
}

/* --- requests (27r): fetch and XMLHttpRequest ------------------------------
 *
 * A script's request is checked here first (the address, mixed content,
 * the mode), then made on a thread of the window program's
 * (dom_request_hook) by dom_perform: a CORS preflight when the request is
 * more than a simple one, then the request itself one redirection at a
 * time (cookies only for the page's own origin, or with credentials
 * "include"), then the CORS check of the answer. dom_request_done calls the
 * script's callback under the page's lock, unless the page was left or the
 * request aborted. The objects scripts see (fetch, Response, Headers,
 * XMLHttpRequest, ...) are the prelude's below, written in JavaScript over
 * the natives start and abort. */

struct dom_request {
    unsigned id, world;
    char url[URL_MAX];
    char method[16];
    char *headers;                      /* the script's header lines (forbidden ones dropped) */
    char *body;
    size_t body_n;
    bool has_body;
    bool credentials;                   /* "include" */
    bool same_origin_only;              /* mode "same-origin" */
    bool no_cors;                       /* mode "no-cors": an opaque answer */
    char origin[URL_MAX];               /* the page's ("null" for a file) */
    struct response r;
    bool got;
    bool tainted;                       /* a redirection left the page's origin */
    char error[200];
};

int (*dom_request_hook)(struct dom_request *q);
static unsigned world_id, request_ids;

static void origin_of(const char *address, char *out, size_t size)
{
    struct url u;
    if (!url_parse(address, &u) || (strcmp(u.scheme, "http") && strcmp(u.scheme, "https"))) {
        snprintf(out, size, "null");
        return;
    }
    bool usual = (!strcmp(u.scheme, "http") && u.port == 80) || (!strcmp(u.scheme, "https") && u.port == 443);
    if (usual)
        snprintf(out, size, "%s://%s", u.scheme, u.host);
    else
        snprintf(out, size, "%s://%s:%u", u.scheme, u.host, u.port);
}

/* headers a script may not set (the Fetch standard's forbidden names) */
static bool forbidden_header(const char *name, size_t n)
{
    static const char *const names[] = {
        "accept-charset", "accept-encoding", "access-control-request-headers", "access-control-request-method",
        "connection", "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive",
        "origin", "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via", 0
    };
    for (int i = 0; names[i]; i++)
        if (strlen(names[i]) == n && !strncmp(names[i], name, n))
            return true;
    return (n >= 6 && !strncmp(name, "proxy-", 6)) || (n >= 4 && !strncmp(name, "sec-", 4));
}

/* a header CORS lets through without a preflight */
static bool safelisted_header(const char *name, size_t n, const char *value, size_t vn)
{
    if ((n == 6 && !strncmp(name, "accept", 6)) || (n == 15 && !strncmp(name, "accept-language", 15)) ||
        (n == 16 && !strncmp(name, "content-language", 16)))
        return true;
    if (n == 12 && !strncmp(name, "content-type", 12)) {
        static const char *const types[] = { "application/x-www-form-urlencoded", "multipart/form-data",
                                             "text/plain", 0 };
        for (int i = 0; types[i]; i++) {
            size_t t = strlen(types[i]);
            if (vn >= t && !strncmp(value, types[i], t) && (vn == t || value[t] == ';'))
                return true;
        }
    }
    return false;
}

/* the value of header name in a response's lines ("name: value\n"), malloc'ed, or 0 */
static char *header_value(const char *headers, const char *name)
{
    size_t n = strlen(name);
    for (const char *h = headers; h && *h;) {
        const char *e = strchr(h, '\n');
        size_t len = e ? (size_t)(e - h) : strlen(h);
        size_t k = 0;
        while (k < n && k < len && lower((unsigned char)h[k]) == name[k])
            k++;
        if (k == n && k < len && h[k] == ':') {
            const char *v = h + k + 1;
            while (v < h + len && (*v == ' ' || *v == '\t'))
                v++;
            return copy_text(v, (size_t)(h + len - v));
        }
        h = e ? e + 1 : 0;
    }
    return 0;
}

/* does a comma list (an Access-Control-Allow-* value) hold word (or "*")? */
static bool list_allows(const char *list, const char *word, size_t n, bool star)
{
    for (const char *p = list; p && *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        const char *e = p;
        while (*e && *e != ',')
            e++;
        const char *t = e;
        while (t > p && (t[-1] == ' ' || t[-1] == '\t'))
            t--;
        size_t len = (size_t)(t - p);
        if ((star && len == 1 && *p == '*') || (len == n && !strncmp(p, word, n)))
            return true;
        if (len == n) {
            size_t k = 0;
            while (k < n && lower((unsigned char)p[k]) == lower((unsigned char)word[k]))
                k++;
            if (k == n)
                return true;
        }
        p = e;
    }
    return false;
}

/* CORS's check of an answer to origin: Access-Control-Allow-Origin (and
 * -Credentials with credentials) */
static bool cors_allows(const struct response *r, const char *origin, bool credentials)
{
    char *allow = header_value(r->headers, "access-control-allow-origin");
    bool ok = allow && ((!credentials && !strcmp(allow, "*")) || !strcmp(allow, origin));
    free(allow);
    if (ok && credentials) {
        char *c = header_value(r->headers, "access-control-allow-credentials");
        ok = c && !strcmp(c, "true");
        free(c);
    }
    return ok;
}

static bool simple_method(const char *m)
{
    return !strcmp(m, "GET") || !strcmp(m, "HEAD") || !strcmp(m, "POST");
}

/* the request's headers that are not safelisted, as a lower-case comma list (malloc'ed) */
static char *unsafe_headers(const char *headers)
{
    size_t size = strlen(headers ? headers : "") + 1;
    char *out = calloc(1, size);
    if (!out)
        return 0;
    for (const char *h = headers; h && *h;) {
        const char *e = strstr(h, "\r\n");
        size_t len = e ? (size_t)(e - h) : strlen(h);
        const char *colon = memchr(h, ':', len);
        if (colon) {
            const char *v = colon + 1;
            while (v < h + len && *v == ' ')
                v++;
            if (!safelisted_header(h, (size_t)(colon - h), v, (size_t)(h + len - v))) {
                if (out[0])
                    strcat(out, ",");
                strncat(out, h, (size_t)(colon - h));
            }
        }
        h = e ? e + 2 : 0;
    }
    return out;
}

/* On the request's thread (no script world here): the preflight, the
 * request a redirection at a time, the CORS checks. */
void dom_perform(struct dom_request *q)
{
    char url[URL_MAX], here[URL_MAX];
    snprintf(url, sizeof url, "%s", q->url);
    const char *method = q->method;
    bool body = q->has_body;
    for (int hops = 0; hops <= 20; hops++) {
        origin_of(url, here, sizeof here);
        bool cross = q->tainted || strcmp(here, q->origin);
        if (cross && q->same_origin_only) {
            snprintf(q->error, sizeof q->error, "the request leaves the page's origin (mode same-origin)");
            return;
        }
        char *unsafe = unsafe_headers(q->headers);
        bool preflight = cross && !q->no_cors && (!simple_method(method) || (unsafe && unsafe[0]));
        if (preflight) {
            size_t size = strlen(q->origin) + strlen(unsafe ? unsafe : "") + 160;
            char *pre = malloc(size);
            if (pre) {
                snprintf(pre, size, "Origin: %s\r\nAccess-Control-Request-Method: %s\r\n%s%s%s", q->origin, method,
                         unsafe && unsafe[0] ? "Access-Control-Request-Headers: " : "", unsafe ? unsafe : "",
                         unsafe && unsafe[0] ? "\r\n" : "");
                struct http_request p = { .url = url, .method = "OPTIONS", .headers = pre, .no_cookies = true,
                                          .no_files = true, .one_hop = true };
                struct response r;
                int got = http_request(&p, &r);
                free(pre);
                bool ok = got && r.status >= 200 && r.status < 300 && cors_allows(&r, q->origin, q->credentials);
                if (ok && !simple_method(method)) {
                    char *m = header_value(r.headers, "access-control-allow-methods");
                    ok = m && list_allows(m, method, strlen(method), !q->credentials);
                    free(m);
                }
                for (const char *h = unsafe; ok && h && *h;) {
                    const char *e = strchr(h, ',');
                    size_t len = e ? (size_t)(e - h) : strlen(h);
                    char *a = header_value(r.headers, "access-control-allow-headers");
                    ok = a && list_allows(a, h, len, !q->credentials);
                    free(a);
                    h = e ? e + 1 : 0;
                }
                response_free(&r);
                if (!ok) {
                    free(unsafe);
                    snprintf(q->error, sizeof q->error, "CORS: the server did not allow the request (preflight)");
                    return;
                }
            }
        }
        free(unsafe);
        size_t size = strlen(q->headers ? q->headers : "") + strlen(q->origin) + 40;
        char *lines = malloc(size);
        if (!lines) {
            snprintf(q->error, sizeof q->error, "out of memory");
            return;
        }
        snprintf(lines, size, "%s%s%s%s", q->headers ? q->headers : "", cross || strcmp(method, "GET") ? "Origin: " : "",
                 cross || strcmp(method, "GET") ? q->origin : "", cross || strcmp(method, "GET") ? "\r\n" : "");
        struct http_request h = { .url = url, .method = method, .headers = lines, .body = body ? q->body : 0,
                                  .body_n = body ? q->body_n : 0, .no_cookies = cross && !q->credentials,
                                  .no_files = true, .one_hop = true };
        response_free(&q->r);
        q->got = http_request(&h, &q->r);
        free(lines);
        if (!q->got) {
            snprintf(q->error, sizeof q->error, "%s", q->r.error);
            return;
        }
        if (cross && !q->no_cors && !cors_allows(&q->r, q->origin, q->credentials)) {
            q->got = false;
            snprintf(q->error, sizeof q->error, "CORS: the server did not allow the page to read the answer");
            return;
        }
        if (!q->r.redirected)
            break;
        if (hops == 20) {
            q->got = false;
            snprintf(q->error, sizeof q->error, "too many redirections");
            return;
        }
        q->tainted = q->tainted || cross;
        snprintf(url, sizeof url, "%s", q->r.url);     /* (one_hop: the Location, made absolute) */
        if (q->r.status != 307 && q->r.status != 308 && strcmp(method, "HEAD"))
            method = "GET", body = false;
        q->r.redirected = false;
    }
    if (q->no_cors && (q->tainted || strcmp(here, q->origin))) {
        free(q->r.body);                        /* (an opaque answer: nothing of it for the script) */
        q->r.body = calloc(1, 1);
        q->r.length = 0;
        free(q->r.headers);
        q->r.headers = 0;
        q->r.status = 0;
        q->r.status_text[0] = 0;
    }
    q->r.redirected = strcmp(q->r.url, q->url) != 0;
}

static void request_free(struct dom_request *q)
{
    response_free(&q->r);
    free(q->headers);
    free(q->body);
    free(q);
}

/* Under the page's lock: the script's callback(status (-1: failed), status
 * text, url, header lines, body, redirected, error). */
void dom_request_done(struct dom_request *q)
{
    jsval cb = J && q->world == world_id ? js_get_index(J, roots[R_REQUESTS], q->id) : JS_UNDEFINED;
    if (js_is_callable(cb)) {
        enter();
        js_set_index(J, roots[R_REQUESTS], q->id, JS_UNDEFINED);
        jsval args[7];
        args[0] = js_number(q->got ? q->r.status : -1);
        args[1] = str(J, q->got ? q->r.status_text : "");
        args[2] = str(J, q->got ? q->r.url : q->url);
        args[3] = str(J, q->got && q->r.headers ? q->r.headers : "");
        args[4] = q->got ? js_string(J, q->r.body ? q->r.body : "", q->r.body ? (size_t)q->r.length : 0) : str(J, "");
        args[5] = js_bool(q->got && q->r.redirected);
        args[6] = str(J, q->got ? "" : q->error);
        jsval r = call_script(cb, JS_UNDEFINED, 7, args);
        if (r == JS_EXCEPTION)
            script_error("a request's callback");
        if (!depth)
            checkpoint();
    }
    request_free(q);
}

/* start(method, url, header lines, body or null, credentials, mode, callback):
 * the request's number, or a text saying why it is refused */
static jsval r_start(ARGS)
{
    UNUSED;
    char *method = arg_text(J, argv[0]), *target = method ? arg_text(J, argv[1]) : 0;
    char *headers = target ? arg_text(J, argv[2]) : 0, *credentials = headers ? arg_text(J, argv[4]) : 0;
    char *mode = credentials ? arg_text(J, argv[5]) : 0;
    char *body = 0;
    size_t body_n = 0;
    if (mode && !js_is_nullish(argv[3]))
        body = js_to_utf8(J, argv[3], &body_n);
    jsval result = JS_EXCEPTION;
    struct dom_request *q = 0;
    if (!mode || (!js_is_nullish(argv[3]) && !body))
        goto out;
    for (char *p = method; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 32);
    char abs[URL_MAX];
    struct url u;
    if (!url_resolve(page_url, target, abs, sizeof abs) || !url_parse(abs, &u)) {
        result = str(J, "Failed to fetch: not an address");
        goto out;
    }
    if (strcmp(u.scheme, "http") && strcmp(u.scheme, "https")) {
        result = str(J, "Failed to fetch: only http and https addresses");
        goto out;
    }
    if (starts_word(page_url, "https:") && !strcmp(u.scheme, "http")) {
        result = str(J, "Failed to fetch: an https page cannot fetch http (mixed content)");
        goto out;
    }
    if (!strcmp(method, "CONNECT") || !strcmp(method, "TRACE") || !strcmp(method, "TRACK") || strlen(method) > 15) {
        result = str(J, "Failed to fetch: a method scripts may not use");
        goto out;
    }
    bool no_cors = !strcmp(mode, "no-cors");
    if (no_cors && !simple_method(method)) {
        result = str(J, "Failed to fetch: no-cors allows only GET, HEAD and POST");
        goto out;
    }
    q = calloc(1, sizeof *q);
    if (!q)
        goto out;
    q->headers = calloc(1, strlen(headers) + 1);
    if (!q->headers)
        goto out;
    for (const char *h = headers; *h;) {                /* (forbidden names dropped, names in lower case) */
        const char *e = strstr(h, "\r\n");
        size_t len = e ? (size_t)(e - h) : strlen(h);
        const char *colon = memchr(h, ':', len);
        if (colon && colon > h) {
            char name[128];
            size_t n = (size_t)(colon - h) < sizeof name - 1 ? (size_t)(colon - h) : sizeof name - 1;
            for (size_t i = 0; i < n; i++)
                name[i] = (char)lower((unsigned char)h[i]);
            name[n] = 0;
            if (!forbidden_header(name, n) && !memchr(h, '\n', len) && !memchr(h, '\r', len)) {
                strncat(q->headers, name, n);
                strncat(q->headers, colon, (size_t)(h + len - colon));
                strcat(q->headers, "\r\n");
            }
        }
        h = e ? e + 2 : h + len;
    }
    q->id = ++request_ids;
    q->world = world_id;
    snprintf(q->url, sizeof q->url, "%s", abs);
    snprintf(q->method, sizeof q->method, "%s", method);
    q->body = body, q->body_n = body_n, q->has_body = body != 0;
    body = 0;
    q->credentials = !strcmp(credentials, "include");
    q->same_origin_only = !strcmp(mode, "same-origin");
    q->no_cors = no_cors;
    origin_of(page_url, q->origin, sizeof q->origin);
    js_set_index(J, roots[R_REQUESTS], q->id, argv[6]);
    result = js_number(q->id);
    if (!dom_request_hook || !dom_request_hook(q)) {
        js_set_index(J, roots[R_REQUESTS], q->id, JS_UNDEFINED);
        result = str(J, "Failed to fetch: no thread for the request");
        request_free(q);
    }
    q = 0;
out:
    if (q)
        request_free(q);
    free(method), free(target), free(headers), free(credentials), free(mode), free(body);
    return result;
}

/* abort(number): its answer is not given to the script */
static jsval r_abort(ARGS)
{
    UNUSED;
    bool threw = false;
    double id = js_to_number(J, argv[0], &threw);
    if (!threw && id >= 1 && id <= request_ids && id == (uint32_t)id)
        js_set_index(J, roots[R_REQUESTS], (uint32_t)id, JS_UNDEFINED);
    return JS_UNDEFINED;
}

/* --- window ---------------------------------------------------------------- */

static jsval w_console(ARGS)
{
    UNUSED;
    struct out o = { 0 };
    put(&o, "", 0);
    for (int i = 0; i < argc; i++) {
        char *t = js_to_utf8(J, argv[i], 0);
        if (!t) {
            js_take_exception(J);
            t = strdup(js_is_symbol(argv[i]) ? "Symbol()" : "[object]");
        }
        if (i)
            put_str(&o, " ");
        if (t)
            put_str(&o, t);
        free(t);
    }
    int level = js_magic(J);
    printf("ibrowser: console: %s%s\n", level == 1 ? "alert: " : "", o.s ? o.s : "");
    fflush(stdout);
    free(o.s);
    return JS_UNDEFINED;
}

static jsval w_confirm(ARGS)
{
    UNUSED;
    int prompt = js_magic(J);
    w_console(J, this_val, argc, argv, nt);
    return prompt ? JS_NULL : JS_FALSE;         /* (no dialogs: confirm false, prompt null) */
}

static jsval w_inner(ARGS)
{
    UNUSED;
    return js_number(js_magic(J) ? view_h : view_w);
}

static jsval w_one(ARGS)
{
    UNUSED;
    return js_number(1);
}

static jsval w_scroll_pos(ARGS)
{
    UNUSED;
    return js_number(js_magic(J) ? dom_scroll_y : 0);
}

/* scrollTo(x, y) or ({ top }) (magic 0), scrollBy (1); the window
 * program scrolls once the script is done */
static jsval w_scroll_to(ARGS)
{
    UNUSED;
    bool by = js_magic(J);
    double y;
    bool threw = false;
    if (js_is_object(argv[0])) {
        jsval top = js_get(J, argv[0], "top");
        if (top == JS_EXCEPTION)
            return top;
        if (js_is_undefined(top))
            return JS_UNDEFINED;
        y = js_to_number(J, top, &threw);
    } else
        y = js_to_number(J, argv[1], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (y != y)
        y = 0;
    double from = dom_scroll_request >= 0 ? dom_scroll_request : dom_scroll_y;
    y = by ? from + y : y;
    dom_scroll_request = y < 0 ? 0 : y > 1e8 ? 100000000 : (int)y;
    return JS_UNDEFINED;
}

static jsval e_scroll_into_view(ARGS)
{
    UNUSED;
    THIS_NODE;
    flush_layout();
    if (self->box[2] || self->box[3])
        dom_scroll_request = self->box[1];
    return JS_UNDEFINED;
}

static jsval w_match_media(ARGS)
{
    UNUSED;
    char *q = arg_text(J, argv[0]);
    if (!q)
        return JS_EXCEPTION;
    jsval r = js_new_object(J);
    js_set(J, r, "matches", js_bool(css_media_matches(q, view_w, view_h)));
    js_set(J, r, "media", str(J, q));
    js_define_method(J, r, "addListener", e_noop, 1);
    js_define_method(J, r, "removeListener", e_noop, 1);
    js_define_method(J, r, "addEventListener", et_add, 2);
    js_define_method(J, r, "removeEventListener", et_remove, 2);
    free(q);
    return r;
}

static jsval w_get_computed_style(ARGS)
{
    UNUSED;
    struct node *e = node_of(argv[0]);
    if (!e)
        return js_throw_type(J, "Argument 1 is not an Element");
    flush_layout();
    jsval r = js_new_object(J);
    const struct style *s = e->style;
    static const char *const displays[] = { "inline", "block", "list-item", "none", "table-row", "table-cell",
                                            "inline-block", "contents" };
    const char *disp = s && (unsigned)s->display < 8 ? displays[s->display] : "block";
    if (s && s->display == DISPLAY_BLOCK && s->table)
        disp = "table";
    js_set(J, r, "display", str(J, disp));
    char buf[32];
    snprintf(buf, sizeof buf, "%dpx", e->box[2]);
    js_set(J, r, "width", str(J, buf));
    snprintf(buf, sizeof buf, "%dpx", e->box[3]);
    js_set(J, r, "height", str(J, buf));
    static const char *const positions[] = { "static", "relative", "absolute", "fixed" };
    js_set(J, r, "position", str(J, s ? positions[s->position & 3] : "static"));
    jsval self = r;
    js_define_method(J, self, "getPropertyValue", e_noop, 1);
    return r;
}

/* localStorage and sessionStorage: kept for the page only (nothing on disk) */
static jsval s_get_item(ARGS)
{
    UNUSED;
    char *k = arg_text(J, argv[0]);
    if (!k)
        return JS_EXCEPTION;
    jsval data = js_get(J, this_val, "%data");
    jsval v = js_is_object(data) ? js_get(J, data, k) : JS_UNDEFINED;
    free(k);
    return js_is_string(v) ? v : JS_NULL;
}

static jsval s_set_item(ARGS)
{
    UNUSED;
    char *k = arg_text(J, argv[0]);
    if (!k)
        return JS_EXCEPTION;
    jsval v = js_to_string(J, argv[1]);
    if (v == JS_EXCEPTION) {
        free(k);
        return v;
    }
    jsval data = js_get(J, this_val, "%data");
    if (js_is_object(data))
        js_set(J, data, k, v);
    free(k);
    return JS_UNDEFINED;
}

static jsval s_remove_item(ARGS)
{
    UNUSED;
    char *k = arg_text(J, argv[0]);
    if (!k)
        return JS_EXCEPTION;
    jsval data = js_get(J, this_val, "%data");
    if (js_is_object(data))
        js_delete(J, data, k);
    free(k);
    return JS_UNDEFINED;
}

static jsval s_clear(ARGS)
{
    UNUSED;
    js_define_value(J, this_val, "%data", js_new_object_proto(J, JS_NULL));
    return JS_UNDEFINED;
}

static jsval s_keys(struct js *J, jsval this_val)
{
    jsval data = js_get(J, this_val, "%data");
    jsval keys = js_get(J, js_get(J, js_global(J), "Object"), "keys");
    return js_call(J, keys, JS_UNDEFINED, 1, &data);
}

static jsval s_length(ARGS)
{
    UNUSED;
    jsval k = s_keys(J, this_val);
    return k == JS_EXCEPTION ? k : js_number(js_array_length(k));
}

static jsval s_key(ARGS)
{
    UNUSED;
    jsval k = s_keys(J, this_val);
    if (k == JS_EXCEPTION)
        return k;
    bool threw = false;
    double i = js_to_number(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (i < 0 || i >= js_array_length(k))
        return JS_NULL;
    return js_get_index(J, k, (uint32_t)i);
}

static jsval make_storage(void)
{
    jsval s = js_new_object(J);
    js_define_value(J, s, "%data", js_new_object_proto(J, JS_NULL));
    js_define_method(J, s, "getItem", s_get_item, 1);
    js_define_method(J, s, "setItem", s_set_item, 2);
    js_define_method(J, s, "removeItem", s_remove_item, 1);
    js_define_method(J, s, "clear", s_clear, 0);
    js_define_method(J, s, "key", s_key, 1);
    js_define_accessor(J, s, "length", s_length, 0);
    return s;
}

/* location: its parts from the page's address */
static void location_parts(char *out, size_t size, int which)
{
    struct url u;
    out[0] = 0;
    if (!url_parse(page_url, &u)) {
        if (which == 0)
            snprintf(out, size, "%s", page_url);
        return;
    }
    const char *q = strchr(u.path, '?');
    char port[16] = "";
    if (u.port && !((!strcmp(u.scheme, "http") && u.port == 80) || (!strcmp(u.scheme, "https") && u.port == 443)))
        snprintf(port, sizeof port, "%u", u.port);
    switch (which) {
    case 0: snprintf(out, size, "%s", page_url); break;
    case 1: snprintf(out, size, "%s:", u.scheme); break;
    case 2: snprintf(out, size, "%s%s%s", u.host, port[0] ? ":" : "", port); break;
    case 3: snprintf(out, size, "%s", u.host); break;
    case 4: snprintf(out, size, "%s", port); break;
    case 5: snprintf(out, size, "%.*s", q ? (int)(q - u.path) : (int)strlen(u.path), u.path); break;
    case 6: snprintf(out, size, "%s", q && q[1] ? q : ""); break;
    case 7: {
        const char *h = strchr(page_url, '#');
        snprintf(out, size, "%s", h && h[1] ? h : "");
        break;
    }
    case 8: snprintf(out, size, "%s://%s%s%s", u.scheme, u.host, port[0] ? ":" : "", port); break;
    }
}

static jsval l_part(ARGS)
{
    UNUSED;
    char buf[URL_MAX + 64];
    location_parts(buf, sizeof buf, js_magic(J));
    return str(J, buf);
}

static jsval l_navigate(ARGS)
{
    UNUSED;
    char *u = arg_text(J, argv[0]);
    if (!u)
        return JS_EXCEPTION;
    if (!url_resolve(page_url, u, dom_navigate_to, sizeof dom_navigate_to))
        snprintf(dom_navigate_to, sizeof dom_navigate_to, "%s", u);
    free(u);
    return JS_UNDEFINED;
}

static jsval l_reload(ARGS)
{
    UNUSED;
    snprintf(dom_navigate_to, sizeof dom_navigate_to, "%s", page_url);
    return JS_UNDEFINED;
}

static jsval l_set_hash(ARGS)
{
    UNUSED;
    char *h = arg_text(J, argv[0]);
    if (!h)
        return JS_EXCEPTION;
    char base[URL_MAX];
    snprintf(base, sizeof base, "%.*s", (int)strcspn(page_url, "#"), page_url);
    snprintf(dom_navigate_to, sizeof dom_navigate_to, "%s%s%s", base, h[0] == '#' ? "" : "#", h);
    free(h);
    return JS_UNDEFINED;
}

static jsval w_atob(ARGS)
{
    UNUSED;
    char *in = arg_text(J, argv[0]);
    if (!in)
        return JS_EXCEPTION;
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in);
    /* bytes 0-255 as code points (a "binary string") */
    jsval chars = js_new_array(J, 0);
    uint32_t bits = 0, nb = 0, k = 0;
    size_t valid = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (is_space((unsigned char)c))
            continue;
        if (c == '=')
            break;
        const char *p = strchr(b64, c);
        if (!p || !c) {
            free(in);
            return js_throw_named(J, "InvalidCharacterError", "The string contains invalid characters.");
        }
        valid++;
        bits = bits << 6 | (uint32_t)(p - b64);
        nb += 6;
        if (nb >= 8) {
            nb -= 8;
            js_set_index(J, chars, k++, js_number((bits >> nb) & 0xFF));
        }
    }
    free(in);
    if (valid % 4 == 1)
        return js_throw_named(J, "InvalidCharacterError", "The string is not correctly encoded.");
    jsval fcc = js_get(J, js_get(J, js_global(J), "String"), "fromCharCode");
    jsval apply = js_get(J, fcc, "apply");
    jsval args[2] = { JS_NULL, chars };
    return js_call(J, apply, fcc, 2, args);
}

static jsval w_btoa(ARGS)
{
    UNUSED;
    jsval s = js_to_string(J, argv[0]);
    if (s == JS_EXCEPTION)
        return s;
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t n = (uint32_t)js_number_value(js_get(J, s, "length"));
    struct out o = { 0 };
    put(&o, "", 0);
    jsval cca = js_get(J, s, "charCodeAt");
    uint32_t buf = 0;
    int nb = 0;
    for (uint32_t i = 0; i < n; i++) {
        jsval idx = js_number(i);
        double c = js_number_value(js_call(J, cca, s, 1, &idx));
        if (c > 255) {
            free(o.s);
            return js_throw_named(J, "InvalidCharacterError", "The string contains characters outside of the Latin1 range.");
        }
        buf = buf << 8 | (uint32_t)c;
        nb += 8;
        while (nb >= 6) {
            nb -= 6;
            char ch = b64[(buf >> nb) & 63];
            put(&o, &ch, 1);
        }
    }
    if (nb) {
        char ch = b64[(buf << (6 - nb)) & 63];
        put(&o, &ch, 1);
    }
    while (o.n % 4)
        put_str(&o, "=");
    jsval r = str(J, o.s);
    free(o.s);
    return r;
}

static jsval w_performance_now(ARGS)
{
    UNUSED;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return js_number(ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6);
}

/* new Image(width, height), new Option(text, value) */
static jsval image_or_option(struct js *J, int argc, jsval *argv, bool option)
{
    struct node *e = new_node(NODE_ELEMENT, option ? "option" : "img");
    if (!e)
        return JS_EXCEPTION;
    keep_detached(e);
    jsval v = wrap(e);
    if (option) {
        if (argc > 0) {
            char *t = arg_text(J, argv[0]);
            if (t) {
                replace_children_with_text(e, t);
                free(t);
            }
        }
        if (argc > 1) {
            char *t = arg_text(J, argv[1]);
            if (t) {
                set_attr(e, "value", t);
                free(t);
            }
        }
    } else {
        char buf[16];
        if (argc > 0 && js_is_number(argv[0])) {
            snprintf(buf, sizeof buf, "%d", (int)js_number_value(argv[0]));
            set_attr(e, "width", buf);
        }
        if (argc > 1 && js_is_number(argv[1])) {
            snprintf(buf, sizeof buf, "%d", (int)js_number_value(argv[1]));
            set_attr(e, "height", buf);
        }
    }
    return v;
}

static jsval w_image(ARGS)
{
    UNUSED;
    return image_or_option(J, argc, argv, false);
}

static jsval w_option(ARGS)
{
    UNUSED;
    return image_or_option(J, argc, argv, true);
}

/* document.implementation.createHTMLDocument: a tree of its own (html,
 * head, body), detached, with the document's methods */
static jsval impl_create_html_document(ARGS)
{
    UNUSED;
    struct node *html = new_node(NODE_ELEMENT, "html"), *head = new_node(NODE_ELEMENT, "head"),
                *body = new_node(NODE_ELEMENT, "body");
    if (!html || !head || !body) {
        free(html);
        free(head);
        free(body);
        return JS_EXCEPTION;
    }
    keep_detached(html);
    link_before(html, head, 0);
    link_before(html, body, 0);
    keep_detached(html);
    jsval d = js_new_object_proto(J, roots[R_DOCUMENT_OBJ]);
    js_define_value(J, d, "documentElement", wrap(html));
    js_define_value(J, d, "head", wrap(head));
    js_define_value(J, d, "body", wrap(body));
    if (argc > 0 && !js_is_undefined(argv[0])) {
        char *t = arg_text(J, argv[0]);
        if (t) {
            struct node *title = new_node(NODE_ELEMENT, "title");
            if (title) {
                link_before(head, title, 0);
                replace_children_with_text(title, t);
            }
            free(t);
        }
    }
    return d;
}

static jsval illegal_ctor(ARGS)
{
    UNUSED;
    return js_throw_type(J, "Illegal constructor");
}

static jsval nl_item(ARGS)
{
    UNUSED;
    bool threw = false;
    double i = js_to_number(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (i < 0 || i >= js_array_length(this_val))
        return JS_NULL;
    return js_get_index(J, this_val, (uint32_t)i);
}

static jsval w_history_noop(ARGS)
{
    UNUSED;
    return JS_UNDEFINED;
}

/* --- setting up ------------------------------------------------------------- */

static void accessor(jsval obj, const char *name, js_native get, js_native set)
{
    js_define_accessor(J, obj, name, get, set);
}

static void method(jsval obj, const char *name, js_native fn, int len)
{
    js_define_method(J, obj, name, fn, len);
}

static void tag_of(jsval obj, const char *name)
{
    jsval sym = js_get(J, js_get(J, js_global(J), "Symbol"), "toStringTag");
    jsval key = sym;
    jsval set_args[3] = { obj, key, JS_UNDEFINED };
    (void)set_args;
    jsval desc = js_new_object(J);
    js_set(J, desc, "value", str(J, name));
    js_set(J, desc, "configurable", JS_TRUE);
    jsval dp = js_get(J, js_get(J, js_global(J), "Object"), "defineProperty");
    jsval args[3] = { obj, key, desc };
    js_call(J, dp, JS_UNDEFINED, 3, args);
}

static jsval interface(const char *name, jsval parent_proto, jsval parent_ctor)
{
    jsval proto = js_new_object_proto(J, parent_proto);
    jsval ctor = js_new_constructor(J, name, illegal_ctor, 0, proto);
    if (js_is_object(parent_ctor))
        js_set(J, ctor, "__proto__", parent_ctor);
    js_define_value(J, js_global(J), name, ctor);
    tag_of(proto, name);
    return proto;
}

static void setup(void)
{
    jsval g = js_global(J);
    js_delete(J, g, "print");
    js_delete(J, g, "gc");
    registry = js_new_array(J, 0);
    registry_n = 0;

    jsval target = js_new_object_proto(J, js_get_proto(J, js_new_object(J)));
    roots[R_TARGET] = target;
    js_define_value(J, g, "EventTarget", js_new_constructor(J, "EventTarget", et_ctor, 0, target));
    tag_of(target, "EventTarget");
    roots[R_LISTENERS] = js_new_private_key(J, "listeners");
    roots[R_HANDLERS] = js_new_private_key(J, "handlers");
    roots[R_TIMERS] = js_new_object_proto(J, JS_NULL);
    roots[R_FUNCTION] = js_get(J, g, "Function");
    method(target, "addEventListener", et_add, 2);
    method(target, "removeEventListener", et_remove, 2);
    method(target, "dispatchEvent", et_dispatch, 1);
    jsval target_ctor = js_get(J, g, "EventTarget");

    jsval nodep = interface("Node", target, target_ctor);
    roots[R_NODE] = nodep;
    jsval node_ctor = js_get(J, g, "Node");
    static const struct { const char *name; int v; } node_consts[] = {
        { "ELEMENT_NODE", 1 }, { "TEXT_NODE", 3 }, { "COMMENT_NODE", 8 }, { "DOCUMENT_NODE", 9 },
        { "DOCUMENT_FRAGMENT_NODE", 11 },
    };
    for (size_t i = 0; i < sizeof node_consts / sizeof node_consts[0]; i++) {
        js_define_value(J, node_ctor, node_consts[i].name, js_number(node_consts[i].v));
        js_define_value(J, nodep, node_consts[i].name, js_number(node_consts[i].v));
    }
    accessor(nodep, "nodeType", n_node_type, 0);
    accessor(nodep, "nodeName", n_node_name, 0);
    accessor(nodep, "nodeValue", n_node_value, n_set_text_content);
    accessor(nodep, "parentNode", n_parent, 0);
    accessor(nodep, "parentElement", n_parent_element, 0);
    accessor(nodep, "firstChild", n_first_child, 0);
    accessor(nodep, "lastChild", n_last_child, 0);
    accessor(nodep, "nextSibling", n_next_sibling, 0);
    accessor(nodep, "previousSibling", n_previous_sibling, 0);
    accessor(nodep, "childNodes", n_child_nodes, 0);
    accessor(nodep, "textContent", n_text_content, n_set_text_content);
    accessor(nodep, "isConnected", n_is_connected, 0);
    accessor(nodep, "ownerDocument", n_owner_document, 0);
    method(nodep, "appendChild", n_append_child, 1);
    method(nodep, "insertBefore", n_insert_before, 2);
    method(nodep, "removeChild", n_remove_child, 1);
    method(nodep, "replaceChild", n_replace_child, 2);
    method(nodep, "cloneNode", n_clone_node, 0);
    method(nodep, "contains", n_contains, 1);
    method(nodep, "hasChildNodes", n_has_child_nodes, 0);
    method(nodep, "isEqualNode", n_is_equal_node, 1);

    jsval chard = interface("CharacterData", nodep, node_ctor);
    accessor(chard, "data", t_data, n_set_text_content);
    accessor(chard, "length", t_length, 0);
    method(chard, "remove", n_remove, 0);
    js_define_method_magic(J, chard, "before", n_before_after, 0, 0);
    js_define_method_magic(J, chard, "after", n_before_after, 0, 1);
    method(chard, "replaceWith", n_replace_with, 0);
    jsval chard_ctor = js_get(J, g, "CharacterData");
    roots[R_TEXT] = interface("Text", chard, chard_ctor);
    roots[R_COMMENT] = interface("Comment", chard, chard_ctor);

    jsval elem = interface("Element", nodep, node_ctor);
    jsval elem_ctor = js_get(J, g, "Element");
    accessor(elem, "tagName", e_tag_name, 0);
    accessor(elem, "localName", e_local_name, 0);
    method(elem, "getAttribute", e_get_attribute, 1);
    method(elem, "setAttribute", e_set_attribute, 2);
    method(elem, "removeAttribute", e_remove_attribute, 1);
    method(elem, "hasAttribute", e_has_attribute, 1);
    method(elem, "toggleAttribute", e_toggle_attribute, 1);
    method(elem, "getAttributeNames", e_get_attribute_names, 0);
    accessor(elem, "attributes", e_attributes, 0);
    accessor(elem, "innerHTML", e_inner_html, e_set_inner_html);
    accessor(elem, "outerHTML", e_outer_html, e_set_outer_html);
    method(elem, "insertAdjacentHTML", e_insert_adjacent_html, 2);
    accessor(elem, "children", e_children, 0);
    accessor(elem, "childElementCount", e_child_element_count, 0);
    accessor(elem, "firstElementChild", e_first_element_child, 0);
    accessor(elem, "lastElementChild", e_last_element_child, 0);
    accessor(elem, "nextElementSibling", e_next_element_sibling, 0);
    accessor(elem, "previousElementSibling", e_previous_element_sibling, 0);
    method(elem, "querySelector", q_query_selector, 1);
    method(elem, "querySelectorAll", q_query_selector_all, 1);
    method(elem, "matches", e_matches, 1);
    method(elem, "closest", e_closest, 1);
    method(elem, "getElementsByTagName", e_get_by_tag, 1);
    method(elem, "getElementsByClassName", e_get_by_class, 1);
    method(elem, "getBoundingClientRect", e_rect, 0);
    accessor(elem, "classList", e_class_list, 0);
    method(elem, "remove", n_remove, 0);
    js_define_method_magic(J, elem, "before", n_before_after, 0, 0);
    js_define_method_magic(J, elem, "after", n_before_after, 0, 1);
    method(elem, "replaceWith", n_replace_with, 0);
    js_define_method_magic(J, elem, "append", n_append_prepend, 0, 0);
    js_define_method_magic(J, elem, "prepend", n_append_prepend, 0, 1);
    method(elem, "replaceChildren", n_replace_children, 0);
    js_define_accessor_magic(J, elem, "clientWidth", e_metric, 0, 0);
    js_define_accessor_magic(J, elem, "clientHeight", e_metric, 0, 1);
    js_define_accessor_magic(J, elem, "scrollWidth", e_metric, 0, 0);
    js_define_accessor_magic(J, elem, "scrollHeight", e_metric, 0, 1);
    accessor(elem, "scrollTop", e_zero, e_noop);
    accessor(elem, "scrollLeft", e_zero, e_noop);
    method(elem, "scrollIntoView", e_scroll_into_view, 0);
    for (size_t i = 0; i < sizeof reflected / sizeof reflected[0]; i++)
        js_define_accessor_magic(J, elem, reflected[i].prop, e_reflect_get, e_reflect_set, (int)i);
    accessor(elem, "type", e_type_get, e_type_set);

    jsval html = interface("HTMLElement", elem, elem_ctor);
    roots[R_ELEMENT] = html;
    accessor(html, "style", e_style, 0);
    accessor(html, "dataset", e_dataset, 0);
    accessor(html, "innerText", e_inner_text, n_set_text_content);
    js_define_accessor_magic(J, html, "offsetWidth", e_metric, 0, 0);
    js_define_accessor_magic(J, html, "offsetHeight", e_metric, 0, 1);
    js_define_accessor_magic(J, html, "offsetLeft", e_metric, 0, 2);
    js_define_accessor_magic(J, html, "offsetTop", e_metric, 0, 3);
    accessor(html, "offsetParent", e_offset_parent, 0);
    accessor(html, "value", e_value_get, e_value_set);
    accessor(html, "checked", e_checked_get, e_checked_set);
    accessor(html, "selectedIndex", e_selected_index_get, e_selected_index_set);
    accessor(html, "options", e_options, 0);
    jsval html_ctor = js_get(J, g, "HTMLElement");
    static const char *const kinds[] = { "HTMLDivElement", "HTMLSpanElement", "HTMLAnchorElement",
                                         "HTMLImageElement", "HTMLInputElement", "HTMLButtonElement",
                                         "HTMLFormElement", "HTMLScriptElement", "HTMLCanvasElement",
                                         "HTMLSelectElement", "HTMLTextAreaElement", "HTMLIFrameElement",
                                         "HTMLVideoElement", "HTMLTemplateElement", "HTMLBodyElement",
                                         "HTMLHeadElement", "HTMLHtmlElement", "HTMLParagraphElement" };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        jsval p = interface(kinds[i], html, html_ctor);   /* (for instanceof tests that only need them to exist) */
        if (!strcmp(kinds[i], "HTMLFormElement"))
            roots[R_FORM] = p;
    }
    method(html, "click", e_click, 0);           /* (27q) */
    js_define_method_magic(J, html, "focus", e_focus, 0, 0);
    js_define_method_magic(J, html, "blur", e_focus, 0, 1);
    js_define_method_magic(J, roots[R_FORM], "submit", f_submit, 0, 0);
    js_define_method_magic(J, roots[R_FORM], "requestSubmit", f_submit, 0, 1);
    js_define_method_magic(J, roots[R_FORM], "reset", f_submit, 0, 2);
    accessor(roots[R_FORM], "elements", f_elements, 0);
    for (int i = 0; i < FIRST_WINDOW_ONLY; i++) {     /* (body's window ones: handler_object) */
        char name[64];
        snprintf(name, sizeof name, "on%s", handler_names[i]);
        js_define_accessor_magic(J, html, name, handler_get, handler_set, i);
    }

    /* (27q) events */
    jsval event_ctor_of[EK_COUNT];
    for (int k = 0; k < EK_COUNT; k++) {
        jsval parent = kind_parent[k] < 0 ? js_get_proto(J, js_new_object(J)) : roots[R_EVENT + kind_parent[k]];
        jsval p = js_new_object_proto(J, parent);
        roots[R_EVENT + k] = p;
        jsval c = js_new_constructor(J, kind_names[k], event_ctors[k], 1, p);
        if (kind_parent[k] >= 0)
            js_set(J, c, "__proto__", event_ctor_of[kind_parent[k]]);
        event_ctor_of[k] = c;
        js_define_value(J, g, kind_names[k], c);
        tag_of(p, kind_names[k]);
    }
    static const struct { int kind; const char *name; int which; } event_props[] = {
        { EK_EVENT, "type", EG_TYPE }, { EK_EVENT, "target", EG_TARGET }, { EK_EVENT, "srcElement", EG_TARGET },
        { EK_EVENT, "currentTarget", EG_CURRENT }, { EK_EVENT, "eventPhase", EG_PHASE },
        { EK_EVENT, "bubbles", EG_BUBBLES }, { EK_EVENT, "cancelable", EG_CANCELABLE },
        { EK_EVENT, "defaultPrevented", EG_PREVENTED }, { EK_EVENT, "composed", EG_COMPOSED },
        { EK_EVENT, "isTrusted", EG_TRUSTED }, { EK_EVENT, "timeStamp", EG_STAMP },
        { EK_CUSTOM, "detail", EG_DETAIL }, { EK_UI, "detail", EG_DETAIL }, { EK_UI, "view", EG_VIEW },
        { EK_UI, "which", EG_WHICH }, { EK_MOUSE, "screenX", EG_SCREEN_X }, { EK_MOUSE, "screenY", EG_SCREEN_Y },
        { EK_MOUSE, "clientX", EG_CLIENT_X }, { EK_MOUSE, "clientY", EG_CLIENT_Y }, { EK_MOUSE, "x", EG_CLIENT_X },
        { EK_MOUSE, "y", EG_CLIENT_Y }, { EK_MOUSE, "pageX", EG_PAGE_X }, { EK_MOUSE, "pageY", EG_PAGE_Y },
        { EK_MOUSE, "offsetX", EG_OFFSET_X }, { EK_MOUSE, "offsetY", EG_OFFSET_Y },
        { EK_MOUSE, "ctrlKey", EG_CTRL }, { EK_MOUSE, "shiftKey", EG_SHIFT }, { EK_MOUSE, "altKey", EG_ALT },
        { EK_MOUSE, "metaKey", EG_META }, { EK_MOUSE, "button", EG_BUTTON }, { EK_MOUSE, "buttons", EG_BUTTONS },
        { EK_MOUSE, "relatedTarget", EG_RELATED }, { EK_MOUSE, "movementX", EG_MOVEMENT },
        { EK_MOUSE, "movementY", EG_MOVEMENT }, { EK_POINTER, "pointerId", EG_POINTER_ID },
        { EK_POINTER, "pointerType", EG_POINTER_TYPE }, { EK_POINTER, "isPrimary", EG_PRIMARY },
        { EK_POINTER, "width", EG_WIDTH }, { EK_POINTER, "height", EG_WIDTH },
        { EK_WHEEL, "deltaX", EG_DELTA_X }, { EK_WHEEL, "deltaY", EG_DELTA_Y }, { EK_WHEEL, "deltaZ", EG_DELTA_Z },
        { EK_WHEEL, "deltaMode", EG_DELTA_MODE }, { EK_KEYBOARD, "key", EG_KEY }, { EK_KEYBOARD, "code", EG_CODE },
        { EK_KEYBOARD, "location", EG_LOCATION }, { EK_KEYBOARD, "repeat", EG_REPEAT },
        { EK_KEYBOARD, "isComposing", EG_COMPOSING }, { EK_KEYBOARD, "keyCode", EG_KEY_CODE },
        { EK_KEYBOARD, "charCode", EG_CHAR_CODE }, { EK_KEYBOARD, "ctrlKey", EG_CTRL },
        { EK_KEYBOARD, "shiftKey", EG_SHIFT }, { EK_KEYBOARD, "altKey", EG_ALT }, { EK_KEYBOARD, "metaKey", EG_META },
        { EK_FOCUS, "relatedTarget", EG_RELATED }, { EK_INPUT, "data", EG_DATA },
        { EK_INPUT, "inputType", EG_INPUT_TYPE }, { EK_INPUT, "isComposing", EG_COMPOSING },
        { EK_SUBMIT, "submitter", EG_SUBMITTER }, { EK_HASH, "oldURL", EG_OLD_URL }, { EK_HASH, "newURL", EG_NEW_URL },
    };
    for (size_t i = 0; i < sizeof event_props / sizeof event_props[0]; i++)
        js_define_accessor_magic(J, roots[R_EVENT + event_props[i].kind], event_props[i].name, ev_get, 0,
                                 event_props[i].which);
    jsval evp = roots[R_EVENT];
    js_define_accessor_magic(J, evp, "returnValue", ev_get, ev_set, EG_RETURN);
    js_define_accessor_magic(J, evp, "cancelBubble", ev_get, ev_set, EG_CANCEL_BUBBLE);
    js_define_method_magic(J, evp, "stopPropagation", ev_method, 0, 0);
    js_define_method_magic(J, evp, "stopImmediatePropagation", ev_method, 0, 1);
    js_define_method_magic(J, evp, "preventDefault", ev_method, 0, 2);
    js_define_method_magic(J, evp, "composedPath", ev_method, 0, 3);
    js_define_method_magic(J, evp, "initEvent", ev_init, 3, 0);
    js_define_method_magic(J, roots[R_EVENT + EK_CUSTOM], "initCustomEvent", ev_init, 4, 1);
    method(roots[R_EVENT + EK_MOUSE], "getModifierState", ev_modifier_state, 1);
    method(roots[R_EVENT + EK_KEYBOARD], "getModifierState", ev_modifier_state, 1);
    static const char *const phases[] = { "NONE", "CAPTURING_PHASE", "AT_TARGET", "BUBBLING_PHASE" };
    for (int i = 0; i < 4; i++) {
        js_define_value(J, evp, phases[i], js_number(i));
        js_define_value(J, event_ctor_of[EK_EVENT], phases[i], js_number(i));
    }
    static const char *const locations[] = { "DOM_KEY_LOCATION_STANDARD", "DOM_KEY_LOCATION_LEFT",
                                             "DOM_KEY_LOCATION_RIGHT", "DOM_KEY_LOCATION_NUMPAD" };
    for (int i = 0; i < 4; i++) {
        js_define_value(J, roots[R_EVENT + EK_KEYBOARD], locations[i], js_number(i));
        js_define_value(J, event_ctor_of[EK_KEYBOARD], locations[i], js_number(i));
    }

    jsval frag = interface("DocumentFragment", nodep, node_ctor);
    roots[R_FRAGMENT] = frag;
    method(frag, "querySelector", q_query_selector, 1);
    method(frag, "querySelectorAll", q_query_selector_all, 1);
    js_define_method_magic(J, frag, "append", n_append_prepend, 0, 0);
    js_define_method_magic(J, frag, "prepend", n_append_prepend, 0, 1);
    accessor(frag, "children", e_children, 0);
    accessor(frag, "firstElementChild", e_first_element_child, 0);

    jsval nl = interface("NodeList", js_get_proto(J, js_new_array(J, 0)), JS_UNDEFINED);
    roots[R_NODELIST] = nl;
    method(nl, "item", nl_item, 1);

    jsval tl = interface("DOMTokenList", js_get_proto(J, js_new_object(J)), JS_UNDEFINED);
    roots[R_TOKENS] = tl;
    static const char *const tl_names[] = { "add", "remove", "toggle", "contains", "replace" };
    for (int i = 0; i < 5; i++)
        js_define_method_magic(J, tl, tl_names[i], tl_op, i == 4 ? 2 : 1, i);
    accessor(tl, "length", tl_length, 0);
    accessor(tl, "value", tl_value, tl_set_value);
    method(tl, "item", tl_item, 1);
    method(tl, "toString", tl_value, 0);

    jsval st = interface("CSSStyleDeclaration", js_get_proto(J, js_new_object(J)), JS_UNDEFINED);
    roots[R_STYLE] = st;
    for (size_t i = 0; i < sizeof css_props / sizeof css_props[0]; i++) {
        char camel[64];
        size_t k = 0;
        for (const char *p = css_props[i]; *p && k < sizeof camel - 1; p++) {
            if (*p == '-' && p[1]) {
                camel[k++] = (char)(p[1] - 32);
                p++;
            } else
                camel[k++] = *p;
        }
        camel[k] = 0;
        js_define_accessor_magic(J, st, camel, st_get, st_set, (int)i);
        if (strchr(css_props[i], '-'))
            js_define_accessor_magic(J, st, css_props[i], st_get, st_set, (int)i);
    }
    accessor(st, "cssText", st_css_text, st_set_css_text);
    js_define_method_magic(J, st, "setProperty", st_method, 2, 0);
    js_define_method_magic(J, st, "getPropertyValue", st_method, 1, 1);
    js_define_method_magic(J, st, "removeProperty", st_method, 1, 2);

    /* the document */
    jsval docp = interface("Document", nodep, node_ctor);
    roots[R_DOCUMENT] = docp;
    memset(&docnode, 0, sizeof docnode);
    docnode.kind = NODE_ELEMENT;
    strcpy(docnode.tag, "#document");
    docnode.first = docnode.last = root;
    jsval doc = js_new_host(J, docp, &docnode, &node_class);
    docnode.js = (uint64_t)doc;
    roots[R_DOCUMENT_OBJ] = doc;
    js_define_value(J, g, "document", doc);
    accessor(docp, "documentElement", d_document_element, 0);
    accessor(docp, "head", d_head, 0);
    accessor(docp, "body", d_body, 0);
    accessor(docp, "title", d_title, d_set_title);
    accessor(docp, "URL", d_url, 0);
    accessor(docp, "documentURI", d_url, 0);
    accessor(docp, "readyState", d_ready_state, 0);
    accessor(docp, "currentScript", d_current_script, 0);
    accessor(docp, "cookie", d_cookie, d_set_cookie);
    accessor(docp, "defaultView", d_default_view, 0);
    accessor(docp, "activeElement", d_active_element, 0);
    accessor(docp, "childNodes", d_child_nodes, 0);
    accessor(docp, "firstChild", d_document_element, 0);
    accessor(docp, "nodeType", d_node_type, 0);
    accessor(docp, "nodeName", d_node_name, 0);
    accessor(docp, "parentNode", d_null, 0);
    accessor(docp, "visibilityState", d_visibility, 0);
    accessor(docp, "hidden", d_false, 0);
    js_define_accessor_magic(J, docp, "forms", d_collection, 0, 0);
    js_define_accessor_magic(J, docp, "images", d_collection, 0, 1);
    js_define_accessor_magic(J, docp, "links", d_collection, 0, 2);
    js_define_accessor_magic(J, docp, "scripts", d_collection, 0, 3);
    method(docp, "getElementById", d_get_by_id, 1);
    method(docp, "getElementsByTagName", d_get_by_tag, 1);
    method(docp, "getElementsByClassName", d_get_by_class, 1);
    method(docp, "getElementsByName", d_get_by_name, 1);
    method(docp, "querySelector", d_query_selector, 1);
    method(docp, "querySelectorAll", d_query_selector_all, 1);
    method(docp, "createElement", d_create_element, 1);
    js_define_method_magic(J, docp, "createElementNS", d_create_element, 2, 1);
    method(docp, "createTextNode", d_create_text_node, 1);
    method(docp, "createComment", d_create_comment, 1);
    method(docp, "createDocumentFragment", d_create_fragment, 0);
    method(docp, "createEvent", d_create_event, 1);
    for (int i = 0; i < (int)(sizeof handler_names / sizeof handler_names[0]); i++)
        if (i < FIRST_WINDOW_ONLY || !strcmp(handler_names[i], "readystatechange")) {
            char name[64];
            snprintf(name, sizeof name, "on%s", handler_names[i]);
            js_define_accessor_magic(J, docp, name, handler_get, handler_set, i);
        }
    js_define_method_magic(J, docp, "write", d_write, 1, 0);
    js_define_method_magic(J, docp, "writeln", d_write, 1, 1);
    method(docp, "hasFocus", d_true, 0);
    method(docp, "open", e_noop, 0);
    method(docp, "close", e_noop, 0);
    jsval impl = js_new_object(J);
    method(impl, "createHTMLDocument", impl_create_html_document, 1);
    method(impl, "hasFeature", d_true, 0);
    js_define_value(J, docp, "implementation", impl);
    js_define_value(J, docp, "referrer", str(J, ""));
    js_define_value(J, docp, "characterSet", str(J, "UTF-8"));
    js_define_value(J, docp, "compatMode", str(J, "CSS1Compat"));
    js_define_value(J, docp, "contentType", str(J, "text/html"));

    /* the window: the global object */
    js_define_value(J, g, "window", g);
    js_define_value(J, g, "self", g);
    js_define_value(J, g, "top", g);
    js_define_value(J, g, "parent", g);
    js_define_value(J, g, "frames", g);
    js_set(J, g, "__proto__", js_new_object_proto(J, target));
    tag_of(g, "Window");
    jsval console = js_new_object(J);
    static const char *const levels[] = { "log", "info", "warn", "error", "debug", "trace" };
    for (int i = 0; i < 6; i++)
        js_define_method_magic(J, console, levels[i], w_console, 0, 0);
    method(console, "group", e_noop, 0);
    method(console, "groupEnd", e_noop, 0);
    method(console, "time", e_noop, 0);
    method(console, "timeEnd", e_noop, 0);
    method(console, "table", w_console, 1);
    method(console, "dir", w_console, 1);
    js_define_value(J, g, "console", console);
    js_define_method_magic(J, g, "alert", w_console, 0, 1);
    js_define_method_magic(J, g, "confirm", w_confirm, 0, 0);
    js_define_method_magic(J, g, "prompt", w_confirm, 0, 2);
    js_define_accessor_magic(J, g, "innerWidth", w_inner, 0, 0);
    js_define_accessor_magic(J, g, "innerHeight", w_inner, 0, 1);
    js_define_accessor_magic(J, g, "outerWidth", w_inner, 0, 0);
    js_define_accessor_magic(J, g, "outerHeight", w_inner, 0, 1);
    accessor(g, "devicePixelRatio", w_one, 0);
    js_define_accessor_magic(J, g, "scrollX", w_scroll_pos, 0, 0);
    js_define_accessor_magic(J, g, "scrollY", w_scroll_pos, 0, 1);
    js_define_accessor_magic(J, g, "pageXOffset", w_scroll_pos, 0, 0);
    js_define_accessor_magic(J, g, "pageYOffset", w_scroll_pos, 0, 1);
    js_define_method_magic(J, g, "scrollTo", w_scroll_to, 2, 0);
    js_define_method_magic(J, g, "scroll", w_scroll_to, 2, 0);
    js_define_method_magic(J, g, "scrollBy", w_scroll_to, 2, 1);
    js_define_method_magic(J, g, "setTimeout", w_set_timer, 2, 0);     /* (27q) */
    js_define_method_magic(J, g, "setInterval", w_set_timer, 2, 1);
    method(g, "clearTimeout", w_clear_timer, 1);
    method(g, "clearInterval", w_clear_timer, 1);
    js_define_method_magic(J, g, "requestAnimationFrame", w_request_frame, 1, 0);
    method(g, "cancelAnimationFrame", w_clear_timer, 1);
    js_define_method_magic(J, g, "requestIdleCallback", w_request_frame, 1, 1);
    method(g, "cancelIdleCallback", w_clear_timer, 1);
    method(g, "queueMicrotask", w_queue_microtask, 1);
    for (int i = 0; i < (int)(sizeof handler_names / sizeof handler_names[0]); i++)
        if (strcmp(handler_names[i], "readystatechange")) {
            char name[64];
            snprintf(name, sizeof name, "on%s", handler_names[i]);
            js_define_accessor_magic(J, g, name, handler_get, handler_set, i);
        }
    method(g, "focus", e_noop, 0);
    method(g, "matchMedia", w_match_media, 1);
    method(g, "getComputedStyle", w_get_computed_style, 1);
    method(g, "atob", w_atob, 1);
    method(g, "btoa", w_btoa, 1);
    js_define_value(J, g, "localStorage", make_storage());
    js_define_value(J, g, "sessionStorage", make_storage());
    jsval image_ctor = js_new_constructor(J, "Image", w_image, 0, JS_UNDEFINED);     /* (27q: constructors) */
    js_define_value(J, image_ctor, "prototype", roots[R_ELEMENT]);
    js_define_value(J, g, "Image", image_ctor);
    jsval option_ctor = js_new_constructor(J, "Option", w_option, 0, JS_UNDEFINED);
    js_define_value(J, option_ctor, "prototype", roots[R_ELEMENT]);
    js_define_value(J, g, "Option", option_ctor);
    jsval perf = js_new_object(J);
    method(perf, "now", w_performance_now, 0);
    js_define_value(J, g, "performance", perf);

    jsval loc = js_new_object(J);
    roots[R_LOCATION] = loc;
    static const char *const parts[] = { "href", "protocol", "host", "hostname", "port", "pathname", "search",
                                         "hash", "origin" };
    for (int i = 0; i < 9; i++)
        js_define_accessor_magic(J, loc, parts[i], l_part, i == 0 ? l_navigate : i == 7 ? l_set_hash : 0, i);
    method(loc, "assign", l_navigate, 1);
    method(loc, "replace", l_navigate, 1);
    method(loc, "reload", l_reload, 0);
    js_define_method_magic(J, loc, "toString", l_part, 0, 0);
    js_define_value(J, g, "location", loc);
    js_define_value(J, doc, "location", loc);

    jsval nav = js_new_object(J);
    char ua[128];
#if defined(__x86_64__)
    snprintf(ua, sizeof ua, "Mozilla/5.0 (RitterOS; x86_64) iBrowser/27");
#else
    snprintf(ua, sizeof ua, "Mozilla/5.0 (RitterOS; aarch64) iBrowser/27");
#endif
    js_define_value(J, nav, "userAgent", str(J, ua));
    js_define_value(J, nav, "appName", str(J, "Netscape"));
    js_define_value(J, nav, "appVersion", str(J, ua + 8));
    js_define_value(J, nav, "platform", str(J, "RitterOS"));
    js_define_value(J, nav, "vendor", str(J, ""));
    js_define_value(J, nav, "language", str(J, "en-US"));
    jsval langs = js_new_array(J, 0);
    js_set_index(J, langs, 0, str(J, "en-US"));
    js_set_index(J, langs, 1, str(J, "en"));
    js_define_value(J, nav, "languages", langs);
    js_define_value(J, nav, "onLine", JS_TRUE);
    js_define_value(J, nav, "cookieEnabled", JS_TRUE);
    js_define_value(J, nav, "hardwareConcurrency", js_number(1));
    js_define_value(J, nav, "maxTouchPoints", js_number(0));
    js_define_value(J, g, "navigator", nav);

    jsval scr = js_new_object(J);
    js_define_value(J, scr, "width", js_number(view_w));
    js_define_value(J, scr, "height", js_number(view_h));
    js_define_value(J, scr, "availWidth", js_number(view_w));
    js_define_value(J, scr, "availHeight", js_number(view_h));
    js_define_value(J, scr, "colorDepth", js_number(24));
    js_define_value(J, scr, "pixelDepth", js_number(24));
    js_define_value(J, g, "screen", scr);

    jsval hist = js_new_object(J);
    js_define_value(J, hist, "length", js_number(1));
    js_define_value(J, hist, "state", JS_NULL);
    method(hist, "pushState", w_history_noop, 3);
    method(hist, "replaceState", w_history_noop, 3);
    method(hist, "back", w_history_noop, 0);
    method(hist, "forward", w_history_noop, 0);
    method(hist, "go", w_history_noop, 1);
    js_define_value(J, g, "history", hist);

    /* (27r) fetch, XMLHttpRequest and the rest: the prelude over start and abort */
    roots[R_REQUESTS] = js_new_object_proto(J, JS_NULL);
    jsval prelude = js_eval(J, dom_prelude, strlen(dom_prelude), "ibrowser-prelude");
    jsval natives[2] = { js_new_function(J, "start", r_start, 7), js_new_function(J, "abort", r_abort, 1) };
    if (prelude == JS_EXCEPTION || js_call(J, prelude, JS_UNDEFINED, 2, natives) == JS_EXCEPTION)
        script_error("ibrowser-prelude");
}

void dom_start(struct node *tree, const char *url, int width, int height, bool scripting)
{
    dom_end();
    world_id++;                                 /* (27r) answers for the page left are dropped */
    root = tree;
    snprintf(page_url, sizeof page_url, "%s", url);
    view_w = width;
    view_h = height;
    dom_navigate_to[0] = 0;
    dom_navigate_link = false;
    dom_changed = false;
    dom_scroll_y = 0;
    dom_scroll_request = -1;
    dom_focused = 0;
    dom_submit_pending = false;
    dom_submit_form = dom_submitter = 0;
    ready_state = 0;
    depth = 0;
    layout_fresh = false;
    body_handlers_listed = false;
    if (!tree)
        return;
    docnode.kind = NODE_ELEMENT;                /* (the document, for clicks without scripts too) */
    strcpy(docnode.tag, "#document");
    docnode.first = docnode.last = root;
    if (!scripting)
        return;
    J = js_new();
    if (!J)
        return;
    js_set_stack_base(J, stack_base);
    for (int i = 0; i < R_COUNT; i++)
        roots[i] = JS_UNDEFINED;
    js_root_push(J, roots, R_COUNT);
    registry = JS_UNDEFINED;
    js_root_push(J, &registry, 1);
    setup();
    note_attributes(root);                      /* (27q: the types onclick="..." and the like want) */
}

static void script_error(const char *where)
{
    jsval e = js_take_exception(J);
    char *t = js_exception_text(J, e);
    if (dom_print) {                            /* (only with -t: other browsers show them in no window) */
        printf("ibrowser: script error: %s\n", t ? t : where);
        fflush(stdout);
    }
    free(t);
}

static bool runnable_type(const struct node *s)
{
    const char *type = node_attr(s, "type");
    if (!type || !type[0])
        return true;
    static const char *const ok[] = { "text/javascript", "application/javascript", "application/x-javascript",
                                      "text/ecmascript", "application/ecmascript", "text/jscript", 0 };
    for (int i = 0; ok[i]; i++)
        if (same_word(type, ok[i]))
            return true;
    return false;
}

static void run_one(struct node *s)
{
    s->script_state = 1;
    if (!runnable_type(s))
        return;                                 /* (modules, JSON, templates: not run) */
    const char *src = node_attr(s, "src");
    char *code = 0;
    long len = 0;
    char where[URL_MAX];
    snprintf(where, sizeof where, "%s", page_url);
    if (src) {
        char abs[URL_MAX];
        if (!url_resolve(page_url, src, abs, sizeof abs))
            return;
        struct response r;
        memset(&r, 0, sizeof r);
        int got = dom_fetch_hook ? dom_fetch_hook(abs, &r) : http_get(abs, &r);
        if (got < 0) {
            s->script_state = 2;                /* (27q: its file is on its way: it runs when it is here) */
            return;
        }
        if (!got) {
            if (dom_print)
                printf("ibrowser: script error: %s: %s\n", abs, r.error);
            response_free(&r);
            return;
        }
        if (r.status >= 400) {
            if (dom_print)
                printf("ibrowser: script error: %s: status %d\n", abs, r.status);
            response_free(&r);
            return;
        }
        code = r.body;
        len = r.length;
        r.body = 0;
        response_free(&r);
        snprintf(where, sizeof where, "%s", abs);
    } else {
        code = text_content(s);
        len = code ? (long)strlen(code) : 0;
    }
    if (!code)
        return;
    current_script = s;
    depth++;
    jsval r = js_eval(J, code, (size_t)len, where);
    depth--;
    current_script = 0;
    free(code);
    if (r == JS_EXCEPTION)
        script_error(where);
    if (!depth)
        checkpoint();                           /* (27q: its microtasks, before the next script) */
}

/* While a script runs, what follows it in the page is held out of the
 * tree, as a browser has not parsed it yet: then document.body is null
 * in the head, appendChild and document.write put their nodes right after
 * the script, and later elements are not found. Each ancestor's siblings
 * after the path stay linked among themselves (`first` of each held run). */
struct held {
    struct node *parent, *first;
};

static int hold_rest(struct node *s, struct held *h, int max)
{
    int n = 0;
    for (struct node *c = s; c && c->parent && n < max; c = c->parent) {
        struct node *p = c->parent, *after = c->next;
        if (!after)
            continue;
        c->next = 0;
        p->last = c;
        after->prev = 0;
        h[n].parent = p;
        h[n].first = after;
        n++;
    }
    return n;
}

static void restore_rest(struct held *h, int n)
{
    for (int i = 0; i < n; i++) {
        struct node *p = h[i].parent, *first = h[i].first, *end = first;
        while (end->next)
            end = end->next;
        for (struct node *k = first; k; k = k->next)
            k->parent = p;
        if (p->last) {
            p->last->next = first;
            first->prev = p->last;
        } else
            p->first = first;
        p->last = end;
    }
}

void dom_run_scripts(void)
{
    if (!J || !root)
        return;
    enter();
    running = true;
    /* in document order; scripts added meanwhile (later in the tree, or
     * anywhere: then by the next round) run too */
    for (int round = 0; round < 8; round++) {
        bool ran = false;
        for (struct node *n = root; n; n = walk_next(n, root)) {
            if (n->kind != NODE_ELEMENT || strcmp(n->tag, "script") || n->script_state)
                continue;
            ran = true;
            if (in_body(n))
                list_body_handlers();           /* (27q) */
            struct held held[64];
            int nh = hold_rest(n, held, 64);
            run_one(n);
            restore_rest(held, nh);
            if (dom_navigate_to[0])
                break;
            if (!connected(n))
                n = root;                       /* (it took itself out: from the top again) */
        }
        if (!ran || dom_navigate_to[0])
            break;
    }
    running = false;
    list_body_handlers();
}

/* (27q) The scripts whose files were not here yet: those that came run now
 * (in document order), the others wait on. */
void dom_run_waiting(void)
{
    if (!J || !root)
        return;
    enter();
    for (struct node *n = root; n && !dom_navigate_to[0]; n = walk_next(n, root))
        if (n->kind == NODE_ELEMENT && n->script_state == 2 && !strcmp(n->tag, "script"))
            run_one(n);
}

bool dom_script_runnable(const struct node *s)
{
    return runnable_type(s);
}

/* The script world and the nodes scripts took out of the tree go. */
void dom_end(void)
{
    if (J) {
        js_free(J);
        J = 0;
    }
    timer_n = 0;                                /* (27q) */
    timer_ids = 0;
    for (int i = 0; i < wanted_n; i++)
        free(wanted[i]);
    wanted_n = 0;
    dom_focused = 0;
    dom_submit_pending = false;
    dom_submit_form = dom_submitter = 0;
    for (int i = 0; i < detached_n; i++) {
        struct node *d = detached[i];
        if (!d->parent)                         /* (all of them: they lost their parent) */
            html_free(d);
    }
    detached_n = 0;
    if (root)
        for (struct node *n = root; n; n = node_next(n, root))
            n->js = 0;
    root = 0;
    memset(&docnode, 0, sizeof docnode);
}
