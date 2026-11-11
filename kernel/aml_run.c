/*
 * The AML interpreter (37b): reading and running the code (ACPI 6.5
 * chapter 20's grammar, chapter 19's meaning). A table loads by running its
 * term list at the root; a method runs its body in a frame of its own,
 * with the method's node as the scope its names are made in (gone at its
 * end) and looked up from.
 *
 * Terms are read and run in one pass, recursively: every nesting (a block,
 * an operand, a call) counts against limits that keep the kernel's stack
 * (32 KiB) safe whatever the firmware's code does; aml_stats has the
 * deepest use. An error ends the method; while loading, the term it
 * happened in (read past without running it: skim_term) and the table
 * goes on.
 */
#include "aml_impl.h"

enum { FLOW_NORMAL, FLOW_RETURN, FLOW_BREAK, FLOW_CONTINUE, FLOW_ERROR };

#define CALLS_DEEPEST 24                /* methods calling methods */
#define NESTING_DEEPEST 80              /* terms inside terms, all calls together */
#define LOOP_ITERATIONS 1000000
#define LOOP_NS 5000000000ull           /* a While polling hardware gives up */
#define TABLE_LARGEST (1u << 20)

struct run {
    const uint8_t *p, *end, *base;
    struct aml_node *scope;
    struct aml_frame *frame;
    struct aml_value result;
    int flow;
};

/* Operators and statements are never inlined into the dispatchers: inlined,
 * every level of the recursion carried all their locals together (620
 * bytes a level, measured with clang -O2; now NESTING_DEEPEST's levels fit
 * well inside a kernel stack). */
#define NOINLINE __attribute__((noinline))

static int calls, nesting;
static uintptr_t stack_top;

/* The mutexes acquired and not yet released: all of them given back when
 * the outermost evaluation ends (ACPICA's way: a method that ends holding
 * one does not keep it). */
#define HELD_LARGEST 32
static struct aml_node *held[HELD_LARGEST];
static int held_count;

static void release_all(void)
{
    while (held_count)
        held[--held_count]->mutex.depth = 0;
}

static NOINLINE void fail(struct run *r, const char *what)
{
    if (r->flow != FLOW_ERROR) {
        char path[96] = "loading";
        if (r->frame->method)
            aml_path(r->frame->method, path, sizeof(path));
        aml_fail("%s: %s (at +0x%X)", path, what, (unsigned)(r->p - r->base));
    }
    r->flow = FLOW_ERROR;
}

/* A failure already logged further in (a call, a field). */
static bool failed(struct run *r)
{
    r->flow = FLOW_ERROR;
    return false;
}

static bool enter(struct run *r)
{
    uintptr_t here = (uintptr_t)&here;
    if (stack_top > here && stack_top - here > aml_counters.stack_deepest)
        aml_counters.stack_deepest = (uint32_t)(stack_top - here);
    if (++nesting > NESTING_DEEPEST) {
        nesting--;
        fail(r, "terms nested too deep");
        return false;
    }
    return true;
}

static int next_byte(struct run *r)
{
    if (r->p >= r->end) {
        fail(r, "the code ends inside a term");
        return -1;
    }
    return *r->p++;
}

static bool take(struct run *r, uint32_t n, const uint8_t **at)
{
    if ((size_t)(r->end - r->p) < n) {
        fail(r, "the code ends inside a term");
        return false;
    }
    *at = r->p;
    r->p += n;
    return true;
}

static bool pkg_value(struct run *r, uint32_t *v)
{
    int lead = next_byte(r);
    if (lead < 0)
        return false;
    int more = lead >> 6;
    if (!more) {
        *v = (uint32_t)lead & 0x3F;
        return true;
    }
    uint32_t n = (uint32_t)lead & 0xF;
    for (int i = 0; i < more; i++) {
        int b = next_byte(r);
        if (b < 0)
            return false;
        n |= (uint32_t)b << (4 + 8 * i);
    }
    *v = n;
    return true;
}

/* A PkgLength: where the package that starts here ends. */
static bool pkg_length(struct run *r, const uint8_t **end)
{
    const uint8_t *start = r->p;
    uint32_t length;
    if (!pkg_value(r, &length))
        return false;
    if (length < (uint32_t)(r->p - start) || length > (size_t)(r->end - start)) {
        fail(r, "a package length past its block");
        return false;
    }
    *end = start + length;
    return true;
}

/* ---- Names ---- */

struct name {
    bool root;
    uint8_t parents, count;
    const uint8_t *segs;
};

static bool lead_char(int c)
{
    return c == '_' || (c >= 'A' && c <= 'Z');
}

static bool name_start(int c)
{
    return lead_char(c) || c == '\\' || c == '^' || c == 0x2E || c == 0x2F;
}

static bool name_string(struct run *r, struct name *n)
{
    memset(n, 0, sizeof(*n));
    int c = next_byte(r);
    if (c == '\\') {
        n->root = true;
        c = next_byte(r);
    } else {
        while (c == '^' && n->parents < 255) {
            n->parents++;
            c = next_byte(r);
        }
    }
    uint32_t count;
    if (c == 0x00) {
        count = 0;
    } else if (c == 0x2E) {
        count = 2;
    } else if (c == 0x2F) {
        int k = next_byte(r);
        if (k < 0)
            return false;
        count = (uint32_t)k;
    } else if (lead_char(c)) {
        r->p--;
        count = 1;
    } else {
        if (c >= 0)
            fail(r, "a name expected");
        return false;
    }
    n->count = (uint8_t)count;
    return take(r, 4 * count, &n->segs);
}

static void name_text(const struct name *n, char *text, size_t size)
{
    size_t at = 0;
    if (n->root && at + 1 < size)
        text[at++] = '\\';
    for (int i = 0; i < n->parents && at + 1 < size; i++)
        text[at++] = '^';
    for (int i = 0; i < n->count; i++) {
        if (i && at + 1 < size)
            text[at++] = '.';
        for (int k = 0; k < 4 && at + 1 < size; k++)
            text[at++] = (char)n->segs[4 * i + k];
    }
    text[at] = 0;
}

/* A name to its node: a single segment by the search rules (this scope,
 * then each one above), anything else exactly. */
static struct aml_node *resolve(struct run *r, const struct name *n)
{
    struct aml_node *s = n->root ? aml_root : r->scope;
    if (!n->root && !n->parents && n->count == 1) {
        for (; s; s = s->parent) {
            struct aml_node *c = aml_child(s, (const char *)n->segs);
            if (c)
                return c;
        }
        return 0;
    }
    for (int i = 0; i < n->parents && s; i++)
        s = s->parent;
    for (int i = 0; i < n->count && s; i++)
        s = aml_child(aml_alias_target(s), (const char *)n->segs + 4 * i);
    return s;
}

/* A name's text ("\_SB.PCI0", "^FOO", "LNKA") to its node, as DerefOf of a
 * string does. */
static NOINLINE struct aml_node *resolve_text(struct run *r, const uint8_t *s, uint32_t length)
{
    return aml_lookup(r->scope, s, length);
}

static NOINLINE void not_found(struct run *r, const struct name *n)
{
    char text[80], what[100];
    name_text(n, text, sizeof(text));
    ksnprintf(what, sizeof(what), "%s not found", text);
    fail(r, what);
}

/* Remember a method's own object, freed at its end. */
static void made(struct run *r, struct aml_node *node)
{
    struct aml_frame *f = r->frame;
    node->temporary = true;
    if (f->made_count == f->made_size) {
        uint32_t size = f->made_size ? 2 * f->made_size : 8;
        struct aml_node **grown = kmalloc(size * sizeof(*grown));
        if (!grown) {
            aml_node_free(node);        /* never left behind unlisted */
            return;
        }
        if (f->made_count)
            memcpy(grown, f->made, f->made_count * sizeof(*grown));
        kfree(f->made);
        f->made = grown;
        f->made_size = size;
    }
    f->made[f->made_count++] = node;
}

/* The node a definition makes (its scope must be there), or the one that
 * was there (*existed). */
static NOINLINE struct aml_node *define(struct run *r, const struct name *n, uint8_t type, bool *existed)
{
    *existed = false;
    if (n->count == 0) {
        fail(r, "a definition without a name");
        return 0;
    }
    struct name prefix = *n;
    prefix.count--;
    struct aml_node *scope = r->scope;
    if (prefix.root || prefix.parents || prefix.count) {
        /* exactly there: a one-segment prefix is not searched for */
        scope = prefix.root ? aml_root : r->scope;
        for (int i = 0; i < prefix.parents && scope; i++)
            scope = scope->parent;
        for (int i = 0; i < prefix.count && scope; i++)
            scope = aml_child(aml_alias_target(scope), (const char *)prefix.segs + 4 * i);
        scope = aml_alias_target(scope);
    }
    if (!scope) {
        char text[80];
        name_text(n, text, sizeof(text));
        aml_fail("%s: its scope is not there", text);
        return 0;
    }
    const char *last = (const char *)n->segs + 4 * (n->count - 1);
    struct aml_node *node = aml_child(scope, last);
    if (node) {
        *existed = true;
        return node;
    }
    node = aml_node_new(scope, last, type);
    if (!node) {
        fail(r, "out of memory");
        return 0;
    }
    if (r->frame->method)
        made(r, node);
    return node;
}

static void already(const struct aml_node *node)
{
    char path[96];
    aml_path(node, path, sizeof(path));
    aml_fail("%s: defined again", path);
}

/* ---- Values from operands ---- */

static void run_term(struct run *r);
static bool eval(struct run *r, struct aml_value *out);

/* A reference's value in place of the reference (operators that compute). */
static NOINLINE bool plain(struct run *r, struct aml_value *v)
{
    if (v->type != AML_REFERENCE || v->ref == REF_DEBUG || (v->ref == REF_NODE &&
        aml_alias_target(v->node) && aml_alias_target(v->node)->type >= AML_DEVICE &&
        aml_alias_target(v->node)->type != AML_FIELD && aml_alias_target(v->node)->type != AML_BUFFER_FIELD))
        return true;
    struct aml_value target = {0};
    if (!aml_deref(v, &target))
        return failed(r);
    aml_release(v);
    *v = target;
    return true;
}

static bool integer_operand(struct run *r, uint64_t *n)
{
    struct aml_value v = {0};
    if (!eval(r, &v))
        return false;
    bool ok = aml_to_integer(&v, n, false);
    aml_release(&v);
    if (!ok)
        fail(r, "an integer expected");
    return ok;
}

static bool value_operand(struct run *r, struct aml_value *v)
{
    return eval(r, v) && plain(r, v);
}

/* ---- Targets: SuperName as a reference (not run: a method named is the
 * method) ---- */

static NOINLINE bool super_ref(struct run *r, struct aml_value *ref, bool may_be_missing)
{
    aml_release(ref);
    if (r->p >= r->end) {
        fail(r, "the code ends inside a term");
        return false;
    }
    int op = *r->p;
    if (op >= 0x60 && op <= 0x6E) {
        r->p++;
        ref->type = AML_REFERENCE;
        ref->ref = REF_SLOT;
        ref->slot = op <= 0x67 ? &r->frame->locals[op - 0x60] : &r->frame->args[op - 0x68];
        return true;
    }
    if (op == 0x5B && r->end - r->p >= 2 && r->p[1] == 0x31) {
        r->p += 2;
        ref->type = AML_REFERENCE;
        ref->ref = REF_DEBUG;
        return true;
    }
    if (name_start(op)) {
        struct name n;
        if (!name_string(r, &n))
            return false;
        struct aml_node *node = resolve(r, &n);
        if (!node) {
            if (may_be_missing)
                return true;            /* left AML_UNINIT */
            not_found(r, &n);
            return false;
        }
        ref->type = AML_REFERENCE;
        ref->ref = REF_NODE;
        ref->node = node;
        return true;
    }
    if (op == 0x83) {                   /* DerefOf(x) as a target: what x refers to */
        r->p++;
        if (!eval(r, ref))
            return false;
        if (ref->type == AML_STRING) {
            struct aml_node *node = resolve_text(r, ref->heap->bytes, ref->heap->length);
            aml_release(ref);
            if (!node) {
                fail(r, "DerefOf: no such name");
                return false;
            }
            ref->type = AML_REFERENCE;
            ref->ref = REF_NODE;
            ref->node = node;
        }
    } else if (!eval(r, ref)) {         /* Index, RefOf, a method returning a reference */
        return false;
    }
    if (ref->type != AML_REFERENCE) {
        fail(r, "a reference expected");
        return false;
    }
    return true;
}

/* A Target: a SuperName or nothing (NullName). */
static bool target(struct run *r, struct aml_value *ref)
{
    aml_release(ref);
    if (r->p < r->end && *r->p == 0x00) {
        r->p++;
        return true;
    }
    return super_ref(r, ref, false);
}

static bool is_arg(struct run *r, const struct aml_value *slot)
{
    return slot >= r->frame->args && slot < r->frame->args + 7;
}

static NOINLINE bool store_node(struct run *r, struct aml_node *n, const struct aml_value *v)
{
    n = aml_alias_target(n);
    if (!n)
        return failed(r);
    struct aml_value c = {0};
    bool ok;
    switch (n->type) {
    case AML_INTEGER: {
        uint64_t x;
        ok = aml_to_integer(v, &x, false);
        if (ok)
            aml_set_integer(&n->value, x);
        break;
    }
    case AML_STRING:
        ok = aml_to_string(v, &c, STRING_IMPLICIT) && aml_duplicate(&n->value, &c);
        break;
    case AML_BUFFER:
        /* into the buffer there (index references to it stay good):
         * cut or padded with zeros to its length */
        ok = aml_to_buffer(v, &c);
        if (ok) {
            struct aml_heap *h = n->value.heap;
            uint32_t k = c.heap->length < h->length ? c.heap->length : h->length;
            memmove(h->bytes, c.heap->bytes, k);
            memset(h->bytes + k, 0, h->length - k);
        }
        break;
    case AML_FIELD:
        ok = aml_field_write(n, v);
        break;
    case AML_BUFFER_FIELD:
        ok = aml_buffer_field_write(n, v);
        break;
    case AML_UNINIT:
    case AML_PACKAGE:
    case AML_REFERENCE:
        ok = aml_duplicate(&n->value, v);
        if (ok)
            n->type = n->value.type;
        break;
    default:
        fail(r, "a store into an object that holds no value");
        return false;
    }
    aml_release(&c);
    if (!ok)
        fail(r, "the store failed");
    return ok;
}

static NOINLINE bool store_ref(struct run *r, const struct aml_value *ref, const struct aml_value *v)
{
    switch (ref->ref) {
    case REF_NODE:
        return store_node(r, ref->node, v);
    case REF_SLOT:
        /* through an Arg that holds a reference (RefOf passed in) */
        if (is_arg(r, ref->slot) && ref->slot->type == AML_REFERENCE)
            return store_ref(r, ref->slot, v);
        if (!aml_duplicate(ref->slot, v))
            return failed(r);
        return true;
    case REF_INDEX: {
        struct aml_heap *h = ref->heap;
        if (ref->index >= h->length) {
            fail(r, "an index past the end");
            return false;
        }
        if (h->type == AML_PACKAGE) {
            if (!aml_duplicate(&h->elements[ref->index], v))
                return failed(r);
            return true;
        }
        uint64_t x;
        if (!aml_to_integer(v, &x, false)) {
            fail(r, "an integer expected");
            return false;
        }
        h->bytes[ref->index] = (uint8_t)x;
        return true;
    }
    case REF_DEBUG:
        return true;
    }
    return true;
}

static bool store(struct run *r, const struct aml_value *ref, const struct aml_value *v)
{
    if (ref->type == AML_UNINIT)        /* no target */
        return true;
    return store_ref(r, ref, v);
}

/* A SuperName's value now (Increment, SizeOf...). */
static NOINLINE bool ref_value(struct run *r, const struct aml_value *ref, struct aml_value *out)
{
    if (ref->ref == REF_SLOT && is_arg(r, ref->slot) && ref->slot->type == AML_REFERENCE)
        return aml_deref(ref->slot, out) || failed(r);
    if (ref->ref == REF_DEBUG) {
        fail(r, "Debug read");
        return false;
    }
    if (!aml_deref(ref, out))
        return failed(r);
    return plain(r, out);
}

/* ---- Data ---- */

static NOINLINE bool buffer_op(struct run *r, struct aml_value *out)
{
    const uint8_t *end, *outer = r->end;
    if (!pkg_length(r, &end))
        return false;
    r->end = end;
    uint64_t size;
    bool ok = integer_operand(r, &size);
    r->end = outer;
    if (!ok)
        return false;
    uint32_t given = (uint32_t)(end - r->p);
    if (size > TABLE_LARGEST) {
        fail(r, "a buffer too large");
        return false;
    }
    uint32_t length = size > given ? (uint32_t)size : given;
    if (!aml_set_buffer(out, 0, length))
        return failed(r);
    memcpy(out->heap->bytes, r->p, given);
    r->p = end;
    return true;
}

/* A name in a package not there yet: kept with its scope, resolved when
 * the table has loaded (aml_resolve_names). */
static NOINLINE bool unresolved(struct run *r, const struct name *n, struct aml_value *e)
{
    char text[200];
    aml_path(r->scope, text, 120);
    size_t at = strlen(text);
    text[at++] = ' ';
    name_text(n, text + at, sizeof(text) - at);
    if (!aml_set_string(e, text, (uint32_t)strlen(text)))
        return failed(r);
    e->type = AML_REFERENCE;            /* the heap stays: REF_NAME owns it */
    e->ref = REF_NAME;
    return true;
}

static NOINLINE bool package_op(struct run *r, struct aml_value *out, bool variable)
{
    const uint8_t *end, *outer = r->end;
    if (!pkg_length(r, &end))
        return false;
    r->end = end;
    uint64_t count;
    bool ok;
    if (variable) {
        ok = integer_operand(r, &count);
    } else {
        int b = next_byte(r);
        ok = b >= 0;
        count = (uint64_t)b;
    }
    if (ok && count > 65536) {
        fail(r, "a package too large");
        ok = false;
    }
    struct aml_value pkg = {0};
    if (ok && !aml_set_package(&pkg, (uint32_t)count))
        ok = failed(r);
    for (uint32_t i = 0; ok && r->p < end; i++) {
        struct aml_value e = {0};
        if (name_start(*r->p)) {
            /* a name in a package is the object named, not run */
            struct name n;
            ok = name_string(r, &n);
            /* while loading, every name waits until all tables are
             * loaded (aml_resolve_names: ACPICA's way; the old laptop's
             * _PRT names LNKD, which a later SSDT's field shadows) */
            struct aml_node *node = ok && r->frame->method ? resolve(r, &n) : 0;
            if (node)
                ok = aml_package_element(node, &e) || failed(r);
            else if (ok)
                ok = unresolved(r, &n, &e);
        } else {
            ok = eval(r, &e);
        }
        if (ok && i < count)
            pkg.heap->elements[i] = e;  /* the package takes it */
        else
            aml_release(&e);
    }
    r->end = outer;
    if (!ok) {
        aml_release(&pkg);
        return false;
    }
    r->p = end;
    aml_release(out);
    *out = pkg;
    return true;
}

/* ---- Computing ---- */

static uint64_t truth(bool b)
{
    return b ? aml_ones : 0;
}

/* a against b in a's type: integers by value, strings and buffers byte by
 * byte, then by length. */
static NOINLINE bool compare(struct run *r, const struct aml_value *a, const struct aml_value *b, int *result)
{
    if (a->type == AML_STRING || a->type == AML_BUFFER) {
        struct aml_value c = {0};
        bool ok = a->type == AML_STRING ? aml_to_string(b, &c, STRING_IMPLICIT)
                                        : aml_to_buffer(b, &c);
        if (!ok) {
            fail(r, "values that do not compare");
            return false;
        }
        uint32_t la = a->heap->length, lb = c.heap->length;
        int d = memcmp(a->heap->bytes, c.heap->bytes, la < lb ? la : lb);
        *result = d < 0 ? -1 : d > 0 ? 1 : la < lb ? -1 : la > lb ? 1 : 0;
        aml_release(&c);
        return true;
    }
    uint64_t x, y;
    if (!aml_to_integer(a, &x, false) || !aml_to_integer(b, &y, false)) {
        fail(r, "values that do not compare");
        return false;
    }
    *result = x < y ? -1 : x > y ? 1 : 0;
    return true;
}

/* Operand, Operand, Target: the integer operators. */
static NOINLINE bool binary(struct run *r, int op, struct aml_value *out)
{
    uint64_t a, b;
    struct aml_value t = {0};
    if (!integer_operand(r, &a) || !integer_operand(r, &b) || !target(r, &t))
        return false;
    uint64_t x = 0;
    switch (op) {
    case 0x72: x = a + b; break;
    case 0x74: x = a - b; break;
    case 0x77: x = a * b; break;
    case 0x79: x = b >= 64 ? 0 : a << b; break;
    case 0x7A: x = b >= 64 ? 0 : a >> b; break;
    case 0x7B: x = a & b; break;
    case 0x7C: x = ~(a & b); break;
    case 0x7D: x = a | b; break;
    case 0x7E: x = ~(a | b); break;
    case 0x7F: x = a ^ b; break;
    case 0x85:
        if (!b) {
            aml_release(&t);
            fail(r, "Mod by zero");
            return false;
        }
        x = a % b;
        break;
    }
    aml_set_integer(out, x);
    bool ok = store(r, &t, out);
    aml_release(&t);
    return ok;
}

/* Operand, Target: one integer in, one out. */
static NOINLINE bool unary(struct run *r, int op, struct aml_value *out)
{
    uint64_t a;
    struct aml_value t = {0};
    if (!integer_operand(r, &a) || !target(r, &t))
        return false;
    uint64_t x = 0;
    switch (op) {
    case 0x80: x = ~a; break;
    case 0x81:                          /* FindSetLeftBit: 1 + the highest one */
        for (int i = 63; i >= 0; i--)
            if (a >> i & 1) {
                x = (uint64_t)i + 1;
                break;
            }
        break;
    case 0x82:                          /* FindSetRightBit */
        for (int i = 0; i < 64; i++)
            if (a >> i & 1) {
                x = (uint64_t)i + 1;
                break;
            }
        break;
    case 0x28: {                        /* FromBCD */
        uint64_t m = 1;
        for (; a; a >>= 4, m *= 10)
            x += (a & 0xF) * m;
        break;
    }
    case 0x29: {                        /* ToBCD */
        for (int shift = 0; a && shift < 64; a /= 10, shift += 4)
            x |= (a % 10) << shift;
        break;
    }
    }
    aml_set_integer(out, x);
    bool ok = store(r, &t, out);
    aml_release(&t);
    return ok;
}

static NOINLINE bool divide(struct run *r, struct aml_value *out)
{
    uint64_t a, b;
    struct aml_value rem = {0}, quo = {0};
    if (!integer_operand(r, &a) || !integer_operand(r, &b) || !target(r, &rem))
        return false;
    if (!target(r, &quo)) {
        aml_release(&rem);
        return false;
    }
    bool ok = b != 0;
    if (!ok) {
        fail(r, "Divide by zero");
    } else {
        struct aml_value v = {0};
        aml_set_integer(&v, a % b);
        aml_set_integer(out, a / b);
        ok = store(r, &rem, &v) && store(r, &quo, out);
    }
    aml_release(&rem);
    aml_release(&quo);
    return ok;
}

static NOINLINE bool step(struct run *r, int delta, struct aml_value *out)
{
    struct aml_value ref = {0}, v = {0};
    if (!super_ref(r, &ref, false))
        return false;
    uint64_t x = 0;
    bool ok = ref_value(r, &ref, &v) && (aml_to_integer(&v, &x, false) || failed(r));
    if (ok) {
        aml_set_integer(out, x + (uint64_t)(int64_t)delta);
        ok = store(r, &ref, out);
    }
    aml_release(&v);
    aml_release(&ref);
    return ok;
}

static NOINLINE bool join(struct run *r, struct aml_value *a, struct aml_value *b, struct aml_value *out)
{
    struct aml_value c = {0};
    bool ok = true;
    uint32_t width = aml_ones == 0xFFFFFFFF ? 4 : 8;
    switch (a->type) {
    case AML_UNINIT:
    case AML_INTEGER: {
        uint64_t x = a->integer, y;
        ok = aml_to_integer(b, &y, false);
        if (ok && (ok = aml_set_buffer(&c, 0, 2 * width)))
            for (uint32_t i = 0; i < width; i++) {
                c.heap->bytes[i] = (uint8_t)(x >> (8 * i));
                c.heap->bytes[width + i] = (uint8_t)(y >> (8 * i));
            }
        break;
    }
    case AML_STRING:
    case AML_BUFFER: {
        struct aml_value d = {0};
        ok = a->type == AML_STRING ? aml_to_string(b, &d, STRING_IMPLICIT) : aml_to_buffer(b, &d);
        uint32_t la = a->heap->length;
        if (ok && (ok = (a->type == AML_STRING ? aml_set_string(&c, 0, la + d.heap->length)
                                                : aml_set_buffer(&c, 0, la + d.heap->length)))) {
            memcpy(c.heap->bytes, a->heap->bytes, la);
            memcpy(c.heap->bytes + la, d.heap->bytes, d.heap->length);
        }
        aml_release(&d);
        break;
    }
    default:
        ok = false;
    }
    if (!ok) {
        fail(r, "Concatenate of values that do not join");
        return false;
    }
    aml_release(out);
    *out = c;
    return true;
}

/* A resource template's length without its end tag (0x79, checksum). */
static uint32_t template_body(const struct aml_heap *h)
{
    uint32_t n = h->length;
    if (n >= 2 && h->bytes[n - 2] == 0x79)
        return n - 2;
    return n;
}

static NOINLINE bool concat_res(struct run *r, struct aml_value *a, struct aml_value *b, struct aml_value *out)
{
    struct aml_value x = {0}, y = {0};
    bool ok = aml_to_buffer(a, &x) && aml_to_buffer(b, &y);
    if (ok) {
        uint32_t la = template_body(x.heap), lb = template_body(y.heap);
        ok = aml_set_buffer(out, 0, la + lb + 2);
        if (ok) {
            memcpy(out->heap->bytes, x.heap->bytes, la);
            memcpy(out->heap->bytes + la, y.heap->bytes, lb);
            out->heap->bytes[la + lb] = 0x79;
            out->heap->bytes[la + lb + 1] = 0;
        }
    }
    aml_release(&x);
    aml_release(&y);
    if (!ok)
        fail(r, "ConcatenateResTemplate of values that are not buffers");
    return ok;
}

/* ToBuffer, ToDecimalString, ToHexString, ToInteger: Operand, Target. */
static NOINLINE bool convert(struct run *r, int op, struct aml_value *out)
{
    struct aml_value v = {0}, t = {0};
    if (!value_operand(r, &v))
        return false;
    if (!target(r, &t)) {
        aml_release(&v);
        return false;
    }
    bool ok;
    uint64_t n;
    switch (op) {
    case 0x96: ok = aml_to_buffer(&v, out); break;
    case 0x97: ok = aml_to_string(&v, out, STRING_DECIMAL); break;
    case 0x98: ok = aml_to_string(&v, out, STRING_HEX); break;
    default:
        ok = aml_to_integer(&v, &n, true);
        if (ok)
            aml_set_integer(out, n);
    }
    if (!ok)
        fail(r, "a conversion of a value that does not convert");
    ok = ok && store(r, &t, out);
    aml_release(&v);
    aml_release(&t);
    return ok;
}

static NOINLINE bool to_string(struct run *r, struct aml_value *out)
{
    struct aml_value v = {0}, b = {0}, t = {0};
    uint64_t limit;
    bool ok = value_operand(r, &v) && integer_operand(r, &limit) && target(r, &t);
    if (ok && !aml_to_buffer(&v, &b)) {
        fail(r, "ToString of a value that is not a buffer");
        ok = false;
    }
    if (ok) {
        uint32_t n = 0;
        while (n < b.heap->length && n < limit && b.heap->bytes[n])
            n++;
        ok = (aml_set_string(out, (const char *)b.heap->bytes, n) || failed(r)) &&
             store(r, &t, out);
    }
    aml_release(&v);
    aml_release(&b);
    aml_release(&t);
    return ok;
}

static NOINLINE bool mid(struct run *r, struct aml_value *out)
{
    struct aml_value v = {0}, t = {0};
    uint64_t at, length;
    bool ok = value_operand(r, &v) && integer_operand(r, &at) && integer_operand(r, &length) &&
              target(r, &t);
    if (ok && v.type != AML_STRING && v.type != AML_BUFFER) {
        fail(r, "Mid of a value that is not a string or buffer");
        ok = false;
    }
    if (ok) {
        uint32_t size = v.heap->length;
        uint32_t from = at < size ? (uint32_t)at : size;
        uint32_t n = length < size - from ? (uint32_t)length : size - from;
        ok = (v.type == AML_STRING ? aml_set_string(out, (const char *)v.heap->bytes + from, n)
                                   : aml_set_buffer(out, v.heap->bytes + from, n)) || failed(r);
        ok = ok && store(r, &t, out);
    }
    aml_release(&v);
    aml_release(&t);
    return ok;
}

static NOINLINE bool index_op(struct run *r, struct aml_value *out)
{
    struct aml_value v = {0}, t = {0};
    uint64_t i;
    bool ok = value_operand(r, &v) && integer_operand(r, &i) && target(r, &t);
    if (ok && v.type != AML_STRING && v.type != AML_BUFFER && v.type != AML_PACKAGE) {
        fail(r, "Index of a value that is not a string, buffer or package");
        ok = false;
    }
    if (ok && i >= v.heap->length) {
        fail(r, "Index past the end");
        ok = false;
    }
    if (ok) {
        aml_release(out);
        out->type = AML_REFERENCE;
        out->ref = REF_INDEX;
        out->container = v.type;
        out->index = (uint32_t)i;
        out->heap = v.heap;
        aml_hold(out);
        ok = store(r, &t, out);
    }
    aml_release(&v);
    aml_release(&t);
    return ok;
}

static bool match_one(struct run *r, int op, const struct aml_value *e, const struct aml_value *v,
                      bool *yes)
{
    if (op == 0) {
        *yes = true;
        return true;
    }
    int c;
    if (!compare(r, e, v, &c))
        return false;
    switch (op) {
    case 1: *yes = c == 0; break;
    case 2: *yes = c <= 0; break;
    case 3: *yes = c < 0; break;
    case 4: *yes = c >= 0; break;
    case 5: *yes = c > 0; break;
    default: *yes = false;
    }
    return true;
}

static NOINLINE bool match(struct run *r, struct aml_value *out)
{
    struct aml_value pkg = {0}, a = {0}, b = {0};
    int op1 = -1, op2 = -1;
    uint64_t start = 0;
    bool ok = value_operand(r, &pkg) && (op1 = next_byte(r)) >= 0 && value_operand(r, &a) &&
              (op2 = next_byte(r)) >= 0 && value_operand(r, &b) && integer_operand(r, &start);
    if (ok && pkg.type != AML_PACKAGE) {
        fail(r, "Match in a value that is not a package");
        ok = false;
    }
    uint64_t found = aml_ones;
    for (uint64_t i = start; ok && i < (ok ? pkg.heap->length : 0); i++) {
        const struct aml_value *e = &pkg.heap->elements[i];
        if (e->type != AML_INTEGER && e->type != AML_STRING && e->type != AML_BUFFER)
            continue;
        bool y1, y2;
        ok = match_one(r, op1, e, &a, &y1) && match_one(r, op2, e, &b, &y2);
        if (ok && y1 && y2) {
            found = i;
            break;
        }
    }
    if (ok)
        aml_set_integer(out, found);
    aml_release(&pkg);
    aml_release(&a);
    aml_release(&b);
    return ok;
}

static uint8_t type_of_node(struct aml_node *n)
{
    n = aml_alias_target(n);
    if (!n)
        return 0;
    if (n->type == AML_REFERENCE)
        return 20;                      /* ACPICA's local reference */
    return n->type <= AML_DEBUG ? n->type : 0;
}

static NOINLINE bool object_type(struct run *r, struct aml_value *out)
{
    struct aml_value ref = {0};
    if (!super_ref(r, &ref, false))
        return false;
    uint64_t type = 0;
    const struct aml_value *v = &ref;
    if (v->ref == REF_SLOT) {
        v = v->slot;
        type = v->type == AML_REFERENCE ? 0 : v->type;
    }
    if (v->type == AML_REFERENCE) {
        if (v->ref == REF_NODE)
            type = type_of_node(v->node);
        else if (v->ref == REF_INDEX)
            type = v->heap->type == AML_PACKAGE ? v->heap->elements[v->index].type
                                                : AML_BUFFER_FIELD;
        else if (v->ref == REF_DEBUG)
            type = AML_DEBUG;
    }
    aml_set_integer(out, type);
    aml_release(&ref);
    return true;
}

static NOINLINE bool size_of(struct run *r, struct aml_value *out)
{
    struct aml_value ref = {0}, v = {0};
    if (!super_ref(r, &ref, false))
        return false;
    bool ok = ref_value(r, &ref, &v);
    if (ok && v.type != AML_STRING && v.type != AML_BUFFER && v.type != AML_PACKAGE) {
        fail(r, "SizeOf a value without a size");
        ok = false;
    }
    if (ok)
        aml_set_integer(out, v.heap->length);
    aml_release(&v);
    aml_release(&ref);
    return ok;
}

static NOINLINE bool deref_of(struct run *r, struct aml_value *out)
{
    struct aml_value v = {0};
    if (!eval(r, &v))
        return false;
    bool ok;
    if (v.type == AML_STRING) {
        struct aml_node *node = resolve_text(r, v.heap->bytes, v.heap->length);
        ok = node && (aml_value_of(node, out) || failed(r));
        if (!node)
            fail(r, "DerefOf: no such name");
    } else if (v.type == AML_REFERENCE) {
        ok = aml_deref(&v, out) || failed(r);
    } else {
        fail(r, "DerefOf of a value that is not a reference");
        ok = false;
    }
    aml_release(&v);
    return ok;
}

static struct aml_node *ref_node(struct run *r, const struct aml_value *ref, uint8_t type)
{
    struct aml_node *n = ref->type == AML_REFERENCE && ref->ref == REF_NODE
                         ? aml_alias_target(ref->node) : 0;
    if (!n || n->type != type) {
        fail(r, type == AML_MUTEX ? "a mutex expected" : "an event expected");
        return 0;
    }
    return n;
}

/* Load(NameString, Target): a table from a region or a field. */
static NOINLINE bool load_op(struct run *r, struct aml_value *out)
{
    struct aml_value ref = {0}, t = {0}, data = {0};
    if (!super_ref(r, &ref, false) || !target(r, &t)) {
        aml_release(&ref);
        return false;
    }
    struct aml_node *n = aml_alias_target(ref.node);
    uint8_t *table = 0;
    uint32_t length = 0;
    bool ok = false;
    if (n && n->type == AML_REGION) {
        uint8_t header[8];
        if (aml_region_bytes(n, 0, header, 8)) {
            length = (uint32_t)header[4] | (uint32_t)header[5] << 8 |
                     (uint32_t)header[6] << 16 | (uint32_t)header[7] << 24;
            if (length >= 36 && length <= TABLE_LARGEST && (table = kmalloc(length)))
                ok = aml_region_bytes(n, 0, table, length);
        }
    } else if (n && (n->type == AML_FIELD || n->type == AML_BUFFER_FIELD || n->type == AML_BUFFER)) {
        if (aml_value_of(n, &data) && data.type == AML_BUFFER && data.heap->length >= 36 &&
            (table = kmalloc(data.heap->length))) {
            length = data.heap->length;
            memcpy(table, data.heap->bytes, length);
            ok = true;
        }
    }
    /* a loaded table's methods point into it: it stays */
    if (ok && (ok = aml_load(table, length) >= 0))
        aml_resolve_names();
    else
        kfree(table);
    if (!ok) {                          /* (ACPICA ends the method there too) */
        aml_release(&ref);
        aml_release(&t);
        aml_release(&data);
        fail(r, "Load: no table there");
        return false;
    }
    aml_set_integer(out, truth(ok));
    struct aml_value handle = {0};
    aml_set_integer(&handle, ok ? aml_counters.loads : 0);
    bool stored = store(r, &t, &handle);
    aml_release(&ref);
    aml_release(&t);
    aml_release(&data);
    return stored;
}

/* A NameString as an operand: a method is called (its arguments follow),
 * anything else gives its value. */
static NOINLINE bool name_term(struct run *r, struct aml_value *out, bool *method)
{
    struct name n;
    if (!name_string(r, &n))
        return false;
    struct aml_node *node = aml_alias_target(resolve(r, &n));
    if (!node) {
        not_found(r, &n);
        return false;
    }
    if (node->type != AML_METHOD)
        return aml_value_of(node, out) || failed(r);
    *method = true;
    int count = node->method.args;
    struct aml_value *args = kzalloc(7 * sizeof(*args));
    if (!args)
        return failed(r);
    bool ok = true;
    for (int i = 0; ok && i < count; i++)
        ok = eval(r, &args[i]);
    if (ok)
        ok = aml_run_method(node, args, count, out, r->frame) || failed(r);
    for (int i = 0; i < 7; i++)
        aml_release(&args[i]);
    kfree(args);
    return ok;
}

static NOINLINE bool ext_eval(struct run *r, struct aml_value *out)
{
    int op = next_byte(r);
    struct aml_value ref = {0}, t = {0};
    uint64_t n;
    bool ok = true;
    switch (op) {
    case 0x12: {                        /* CondRefOf(SuperName, Target) */
        ok = super_ref(r, &ref, true) && target(r, &t);
        if (ok) {
            aml_set_integer(out, truth(ref.type == AML_REFERENCE));
            if (ref.type == AML_REFERENCE)
                ok = store(r, &t, &ref);
        }
        break;
    }
    case 0x23: {                        /* Acquire(Mutex, Timeout): 0 = acquired */
        int lo, hi;
        ok = super_ref(r, &ref, false) && (lo = next_byte(r)) >= 0 && (hi = next_byte(r)) >= 0;
        struct aml_node *m = ok ? ref_node(r, &ref, AML_MUTEX) : 0;
        ok = m != 0;
        if (ok) {
            m->mutex.depth++;
            if (held_count < HELD_LARGEST)
                held[held_count++] = m;
            aml_set_integer(out, 0);
        }
        break;
    }
    case 0x25: {                        /* Wait(Event, Timeout): Ones = timed out */
        ok = super_ref(r, &ref, false) && integer_operand(r, &n);
        struct aml_node *e = ok ? ref_node(r, &ref, AML_EVENT) : 0;
        ok = e != 0;
        if (ok) {
            bool signalled = e->event.signals > 0;
            if (signalled)
                e->event.signals--;
            aml_set_integer(out, truth(!signalled));
        }
        break;
    }
    case 0x28:
    case 0x29:
        return unary(r, op, out);
    case 0x30:                          /* Revision: the interpreter's */
        aml_set_integer(out, 1);
        break;
    case 0x31:
        aml_release(out);
        out->type = AML_REFERENCE;
        out->ref = REF_DEBUG;
        break;
    case 0x33:                          /* Timer: 100 ns units */
        aml_set_integer(out, aml_now_ns() / 100);
        break;
    case 0x20:
        return load_op(r, out);
    case 0x1F: {                        /* LoadTable: by signature, not done */
        for (int i = 0; ok && i < 6; i++) {
            struct aml_value v = {0};
            ok = eval(r, &v);
            aml_release(&v);
        }
        if (ok) {
            aml_note("LoadTable: not done");
            aml_set_integer(out, 0);
        }
        break;
    }
    default:
        if (op >= 0) {
            char what[48];
            ksnprintf(what, sizeof(what), "opcode 0x5B 0x%02X unknown here", (unsigned)op);
            r->p--;
            fail(r, what);
        }
        ok = false;
    }
    aml_release(&ref);
    aml_release(&t);
    return ok && r->flow == FLOW_NORMAL;
}

static NOINLINE bool store_op(struct run *r, struct aml_value *out)
{
    struct aml_value v = {0}, t = {0};
    bool ok = eval(r, &v) && super_ref(r, &t, false) && store(r, &t, &v);
    if (ok)
        aml_copy(out, &v);
    aml_release(&v);
    aml_release(&t);
    return ok;
}

static NOINLINE bool ref_of(struct run *r, struct aml_value *out)
{
    struct aml_value t = {0};
    bool ok = super_ref(r, &t, false);
    if (ok)
        aml_copy(out, &t);
    aml_release(&t);
    return ok;
}

/* Concatenate, ConcatenateResTemplate: Operand, Operand, Target. */
static NOINLINE bool concat_op(struct run *r, int op, struct aml_value *out)
{
    struct aml_value a = {0}, b = {0}, t = {0};
    bool ok = value_operand(r, &a) && value_operand(r, &b) && target(r, &t) &&
              (op == 0x73 ? join(r, &a, &b, out) : concat_res(r, &a, &b, out)) &&
              store(r, &t, out);
    aml_release(&a);
    aml_release(&b);
    aml_release(&t);
    return ok;
}

/* LAnd, LOr (both operands always read), LNot (so LNotEqual, LLessEqual
 * and LGreaterEqual too: LNot of the comparison). */
static NOINLINE bool logic_op(struct run *r, int op, struct aml_value *out)
{
    uint64_t x, y = 0;
    bool ok = integer_operand(r, &x) && (op == 0x92 || integer_operand(r, &y));
    if (ok)
        aml_set_integer(out, truth(op == 0x90 ? x && y : op == 0x91 ? x || y : !x));
    return ok;
}

/* LEqual, LGreater, LLess. */
static NOINLINE bool compare_op(struct run *r, int op, struct aml_value *out)
{
    struct aml_value a = {0}, b = {0};
    int c = 0;
    bool ok = value_operand(r, &a) && value_operand(r, &b) && compare(r, &a, &b, &c);
    if (ok)
        aml_set_integer(out, truth(op == 0x93 ? c == 0 : op == 0x94 ? c > 0 : c < 0));
    aml_release(&a);
    aml_release(&b);
    return ok;
}

/* CopyObject(TermArg, SimpleName): the target takes the value's type. */
static NOINLINE bool copy_object(struct run *r, struct aml_value *out)
{
    struct aml_value a = {0}, t = {0};
    bool ok = eval(r, &a) && super_ref(r, &t, false);
    if (ok && t.ref == REF_NODE) {
        struct aml_node *n = aml_alias_target(t.node);
        if (n && (n->type == AML_FIELD || n->type == AML_BUFFER_FIELD)) {
            ok = store_node(r, n, &a);
        } else if (n && (n->type <= AML_PACKAGE || n->type == AML_REFERENCE)) {
            ok = aml_duplicate(&n->value, &a) || failed(r);
            n->type = n->value.type;
        } else {
            fail(r, "CopyObject over an object that holds no value");
            ok = false;
        }
    } else if (ok && t.ref == REF_SLOT) {
        ok = aml_duplicate(t.slot, &a) || failed(r);
    }
    if (ok)
        aml_copy(out, &a);
    aml_release(&a);
    aml_release(&t);
    return ok;
}

static NOINLINE bool unknown(struct run *r, int op)
{
    char what[40];
    r->p--;
    ksnprintf(what, sizeof(what), "opcode 0x%02X unknown here", (unsigned)op);
    fail(r, what);
    return false;
}

static NOINLINE bool constant(struct run *r, int op, struct aml_value *out)
{
    const uint8_t *at;
    uint32_t size = op == 0x0A ? 1 : op == 0x0B ? 2 : op == 0x0C ? 4 : 8;
    if (!take(r, size, &at))
        return false;
    uint64_t x = 0;
    for (uint32_t i = 0; i < size; i++)
        x |= (uint64_t)at[i] << (8 * i);
    aml_set_integer(out, x);
    return true;
}

static NOINLINE bool string_op(struct run *r, struct aml_value *out)
{
    const uint8_t *s = r->p;
    while (r->p < r->end && *r->p)
        r->p++;
    if (r->p >= r->end) {
        fail(r, "a string without its end");
        return false;
    }
    uint32_t length = (uint32_t)(r->p - s);
    r->p++;
    return aml_set_string(out, (const char *)s, length) || failed(r);
}

/* An operator's result, the method's value if it ends without a Return
 * ("implicit return": ACPICA's in slack mode, as Linux runs it). */
static void remember(struct run *r, const struct aml_value *v)
{
    if (r->frame->method)
        aml_copy(&r->frame->implicit, v);
}

static bool operator_op(struct run *r, int op, struct aml_value *out);

static bool eval_op(struct run *r, struct aml_value *out)
{
    int op = next_byte(r);
    if (op < 0)
        return false;
    switch (op) {
    case 0x00: aml_set_integer(out, 0); return true;
    case 0x01: aml_set_integer(out, 1); return true;
    case 0xFF: aml_set_integer(out, aml_ones); return true;
    case 0x0A: case 0x0B: case 0x0C: case 0x0E:
        return constant(r, op, out);
    case 0x0D: return string_op(r, out);
    case 0x11: return buffer_op(r, out);
    case 0x12: return package_op(r, out, false);
    case 0x13: return package_op(r, out, true);
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67:
        aml_copy(out, &r->frame->locals[op - 0x60]);
        return true;
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E:
        aml_copy(out, &r->frame->args[op - 0x68]);
        return true;
    }
    bool called = name_start(op);
    if (called) {
        r->p--;
        bool method = false;
        bool ok = name_term(r, out, &method);
        if (ok && method)
            remember(r, out);
        return ok;
    }
    bool ok = operator_op(r, op, out);
    if (ok)
        remember(r, out);
    return ok;
}

static bool operator_op(struct run *r, int op, struct aml_value *out)
{
    switch (op) {
    case 0x70: return store_op(r, out);
    case 0x71: return ref_of(r, out);
    case 0x72: case 0x74: case 0x77: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0x85:
        return binary(r, op, out);
    case 0x73: case 0x84: return concat_op(r, op, out);
    case 0x75: return step(r, 1, out);
    case 0x76: return step(r, -1, out);
    case 0x78: return divide(r, out);
    case 0x80: case 0x81: case 0x82:
        return unary(r, op, out);
    case 0x83: return deref_of(r, out);
    case 0x87: return size_of(r, out);
    case 0x88: return index_op(r, out);
    case 0x89: return match(r, out);
    case 0x8E: return object_type(r, out);
    case 0x90: case 0x91: case 0x92:
        return logic_op(r, op, out);
    case 0x93: case 0x94: case 0x95:
        return compare_op(r, op, out);
    case 0x96: case 0x97: case 0x98: case 0x99:
        return convert(r, op, out);
    case 0x9C: return to_string(r, out);
    case 0x9D: return copy_object(r, out);
    case 0x9E: return mid(r, out);
    case 0x5B: return ext_eval(r, out);
    }
    return unknown(r, op);
}

static bool eval(struct run *r, struct aml_value *out)
{
    if (r->flow != FLOW_NORMAL || !enter(r))
        return false;
    bool ok = eval_op(r, out) && r->flow == FLOW_NORMAL;
    nesting--;
    if (!ok && r->flow == FLOW_NORMAL)
        r->flow = FLOW_ERROR;
    return ok;
}

/* ---- Definitions and statements ---- */

static void run_block(struct run *r, const uint8_t *end, struct aml_node *scope);

static NOINLINE void name_op(struct run *r)
{
    struct name n;
    struct aml_value v = {0};
    if (!name_string(r, &n) || !eval(r, &v))
        return;
    bool existed;
    struct aml_node *node = define(r, &n, v.type, &existed);
    if (node && existed)
        already(node);
    if (node && !existed) {
        node->value = v;                /* the node takes it */
        node->type = v.type;
        return;
    }
    aml_release(&v);
}

static NOINLINE void method_op(struct run *r)
{
    const uint8_t *end;
    struct name n;
    int flags;
    if (!pkg_length(r, &end) || !name_string(r, &n) || (flags = next_byte(r)) < 0)
        return;
    if (r->p > end) {
        fail(r, "a method's header past its length");
        return;
    }
    bool existed;
    struct aml_node *node = define(r, &n, AML_METHOD, &existed);
    if (node && existed) {
        already(node);
    } else if (node) {
        node->method.code = r->p;
        node->method.length = (uint32_t)(end - r->p);
        node->method.args = (uint8_t)(flags & 7);
        node->method.flags = (uint8_t)flags;
    }
    r->p = end;
}

static NOINLINE void alias_op(struct run *r)
{
    struct name from, to;
    if (!name_string(r, &from) || !name_string(r, &to))
        return;
    struct aml_node *source = resolve(r, &from);
    if (!source) {
        char text[80];
        name_text(&from, text, sizeof(text));
        aml_fail("Alias of %s: not found", text);
        return;
    }
    bool existed;
    struct aml_node *node = define(r, &to, AML_ALIAS, &existed);
    if (node && existed)
        already(node);
    else if (node)
        node->alias = source;
}

/* Scope, Device, Processor, PowerResource, ThermalZone: a named scope and
 * the terms in it. */
static NOINLINE void scope_op(struct run *r, int kind)
{
    const uint8_t *end;
    struct name n;
    if (!pkg_length(r, &end) || !name_string(r, &n))
        return;
    struct aml_node *node;
    if (kind == AML_SCOPE) {
        node = aml_alias_target(resolve(r, &n));
        if (!node) {
            char text[80];
            name_text(&n, text, sizeof(text));
            aml_fail("Scope(%s): not found", text);
            r->p = end;
            return;
        }
    } else {
        bool existed;
        node = define(r, &n, (uint8_t)kind, &existed);
        if (!node) {
            r->p = end;
            return;
        }
        if (existed && node->type != kind) {
            already(node);
            r->p = end;
            return;
        }
        const uint8_t *at;
        if (kind == AML_PROCESSOR) {
            if (!take(r, 6, &at))
                return;
            node->processor.id = at[0];
            node->processor.block = (uint32_t)at[1] | (uint32_t)at[2] << 8 |
                                    (uint32_t)at[3] << 16 | (uint32_t)at[4] << 24;
            node->processor.block_length = at[5];
        } else if (kind == AML_POWER) {
            if (!take(r, 3, &at))
                return;
            node->power.level = at[0];
            node->power.order = (uint16_t)(at[1] | at[2] << 8);
        }
    }
    if (r->p > end) {
        fail(r, "a scope's header past its length");
        return;
    }
    run_block(r, end, node);
}

static NOINLINE void simple_op(struct run *r, uint8_t type)
{
    struct name n;
    int flags = 0;
    if (!name_string(r, &n) || (type == AML_MUTEX && (flags = next_byte(r)) < 0))
        return;
    bool existed;
    struct aml_node *node = define(r, &n, type, &existed);
    if (node && existed)
        already(node);
    else if (node && type == AML_MUTEX)
        node->mutex.sync = (uint8_t)(flags & 0xF);
}

static NOINLINE void region_op(struct run *r)
{
    struct name n;
    int space;
    uint64_t offset, length;
    if (!name_string(r, &n) || (space = next_byte(r)) < 0 || !integer_operand(r, &offset) ||
        !integer_operand(r, &length))
        return;
    bool existed;
    struct aml_node *node = define(r, &n, AML_REGION, &existed);
    if (node && existed) {
        already(node);
    } else if (node) {
        node->region.space = (uint8_t)space;
        node->region.offset = offset;
        node->region.length = length;
    }
}

/* DataTableRegion: a region over a table found by its signature (not done:
 * a region of no length). */
static NOINLINE void data_region_op(struct run *r)
{
    struct name n;
    if (!name_string(r, &n))
        return;
    for (int i = 0; i < 3; i++) {
        struct aml_value v = {0};
        if (!eval(r, &v))
            return;
        aml_release(&v);
    }
    bool existed;
    struct aml_node *node = define(r, &n, AML_REGION, &existed);
    if (node && existed) {
        already(node);
    } else if (node) {
        node->region.space = 0x7E;      /* ACPICA's data table space */
        aml_note("DataTableRegion: not done");
    }
}

/* A FieldList: named fields one after the other, from bit 0. */
static NOINLINE void field_list(struct run *r, const uint8_t *end, const struct aml_node *model)
{
    const uint8_t *outer = r->end;
    r->end = end;
    uint32_t bit = 0, length;
    uint8_t flags = model->field.flags;
    while (r->p < end && r->flow == FLOW_NORMAL) {
        int c = *r->p;
        const uint8_t *at;
        if (c == 0x00) {                /* ReservedField */
            r->p++;
            if (!pkg_value(r, &length))
                break;
            bit += length;
        } else if (c == 0x01) {         /* AccessField: AccessType AccessAttrib */
            r->p++;
            if (!take(r, 2, &at))
                break;
            flags = (uint8_t)((flags & ~0xF) | (at[0] & 0xF));
        } else if (c == 0x03) {         /* ExtendedAccessField */
            r->p++;
            if (!take(r, 3, &at))
                break;
            flags = (uint8_t)((flags & ~0xF) | (at[0] & 0xF));
        } else if (c == 0x02) {         /* ConnectField: a name or a buffer */
            r->p++;
            if (r->p < end && *r->p == 0x11) {
                struct aml_value v = {0};
                eval(r, &v);
                aml_release(&v);
            } else {
                struct name n;
                name_string(r, &n);
            }
        } else if (lead_char(c)) {
            struct name n = {.count = 1};
            if (!take(r, 4, &n.segs) || !pkg_value(r, &length))
                break;
            bool existed;
            struct aml_node *f = define(r, &n, AML_FIELD, &existed);
            if (f && existed) {
                already(f);
            } else if (f) {
                f->field = model->field;
                f->field.flags = flags;
                f->field.bit_offset = bit;
                f->field.bit_length = length;
            }
            bit += length;
        } else {
            fail(r, "a field list entry unknown");
        }
    }
    r->end = outer;
    if (r->flow == FLOW_NORMAL)
        r->p = end;
}

static struct aml_node *named_object(struct run *r, const char *what)
{
    struct name n;
    if (!name_string(r, &n))
        return 0;
    struct aml_node *node = aml_alias_target(resolve(r, &n));
    if (!node) {
        char text[80];
        name_text(&n, text, sizeof(text));
        aml_fail("%s %s: not found", what, text);
    }
    return node;
}

static NOINLINE void field_op(struct run *r, int kind)
{
    const uint8_t *end;
    if (!pkg_length(r, &end))
        return;
    const uint8_t *outer = r->end;
    r->end = end;
    struct aml_node model;
    memset(&model, 0, sizeof(model));
    model.field.kind = (uint8_t)kind;
    struct aml_node *first = named_object(r, kind == AML_INDEX_FIELD ? "IndexField" : "Field");
    struct aml_node *second = 0;
    bool ok = first != 0;
    if (ok && kind != AML_REGION_FIELD)
        ok = (second = named_object(r, kind == AML_INDEX_FIELD ? "IndexField" : "BankField")) != 0;
    if (ok && kind == AML_BANK_FIELD)
        ok = integer_operand(r, &model.field.bank_value);
    int flags = ok ? next_byte(r) : -1;
    r->end = outer;
    if (flags < 0) {
        if (r->flow == FLOW_NORMAL)
            r->p = end;                 /* its region missing: its fields are not made */
        return;
    }
    model.field.flags = (uint8_t)flags;
    if (kind == AML_INDEX_FIELD) {
        model.field.index = first;
        model.field.data = second;
    } else {
        model.field.region = first;
        model.field.bank = second;
    }
    field_list(r, end, &model);
}

/* CreateDWordField and its kin: SourceBuffer, an index, NameString. */
static NOINLINE void create_field_op(struct run *r, int op)
{
    struct aml_value v = {0};
    uint64_t at, bits = 0;
    struct name n;
    bool ok = value_operand(r, &v) && integer_operand(r, &at) &&
              (op != 0x13 || integer_operand(r, &bits)) && name_string(r, &n);
    if (!ok) {
        aml_release(&v);
        return;
    }
    switch (op) {
    case 0x8A: at *= 8, bits = 32; break;
    case 0x8B: at *= 8, bits = 16; break;
    case 0x8C: at *= 8, bits = 8; break;
    case 0x8D: bits = 1; break;
    case 0x8F: at *= 8, bits = 64; break;
    }
    if (v.type != AML_BUFFER) {
        fail(r, "a buffer field of a value that is not a buffer");
    } else if (!bits || at + bits > (uint64_t)v.heap->length * 8) {
        fail(r, "a buffer field past its buffer's end");
    } else {
        bool existed;
        struct aml_node *f = define(r, &n, AML_BUFFER_FIELD, &existed);
        if (f && existed) {
            already(f);
        } else if (f) {
            f->buffer_field.buffer = v.heap;
            v.heap->refs++;
            f->buffer_field.bit_offset = (uint32_t)at;
            f->buffer_field.bit_length = (uint32_t)bits;
        }
    }
    aml_release(&v);
}

/* An If's or While's condition is a result too (ACPICA). */
static void predicate(struct run *r, uint64_t yes)
{
    struct aml_value v = {0};
    aml_set_integer(&v, yes);
    remember(r, &v);
}

static NOINLINE void if_op(struct run *r)
{
    const uint8_t *end, *outer = r->end;
    if (!pkg_length(r, &end))
        return;
    r->end = end;
    uint64_t yes;
    bool ok = integer_operand(r, &yes);
    r->end = outer;
    if (!ok)
        return;
    predicate(r, yes);
    if (yes)
        run_block(r, end, 0);
    else
        r->p = end;
    if (r->flow != FLOW_NORMAL || r->p >= r->end || *r->p != 0xA1)
        return;
    r->p++;
    const uint8_t *else_end;
    if (!pkg_length(r, &else_end))
        return;
    if (!yes)
        run_block(r, else_end, 0);
    else
        r->p = else_end;
}

static NOINLINE void while_op(struct run *r)
{
    const uint8_t *end, *outer = r->end;
    if (!pkg_length(r, &end))
        return;
    const uint8_t *start = r->p;
    uint64_t began = aml_now_ns();
    for (uint32_t count = 0;; count++) {
        if (count >= LOOP_ITERATIONS ||
            ((count & 1023) == 1023 && began && aml_now_ns() - began > LOOP_NS)) {
            fail(r, "a While that never ends");
            return;
        }
        r->p = start;
        r->end = end;
        uint64_t yes;
        bool ok = integer_operand(r, &yes);
        r->end = outer;
        if (!ok)
            return;
        predicate(r, yes);
        if (!yes)
            break;
        run_block(r, end, 0);
        if (r->flow == FLOW_BREAK) {
            r->flow = FLOW_NORMAL;
            break;
        }
        if (r->flow == FLOW_CONTINUE)
            r->flow = FLOW_NORMAL;
        if (r->flow != FLOW_NORMAL)
            return;
    }
    r->p = end;
}

static NOINLINE void notify_op(struct run *r)
{
    struct aml_value ref = {0};
    uint64_t value;
    if (!super_ref(r, &ref, false) || !integer_operand(r, &value)) {
        aml_release(&ref);
        return;
    }
    struct aml_node *n = ref.ref == REF_NODE ? aml_alias_target(ref.node) : 0;
    if (!n)
        fail(r, "Notify of something not named");
    else if (aml_machine && aml_machine->notify)
        aml_machine->notify(n, value);
    aml_release(&ref);
}

static NOINLINE void ext_term(struct run *r)
{
    int op = r->end - r->p >= 2 ? r->p[1] : -1;
    struct aml_value ref = {0};
    uint64_t n;
    const uint8_t *at;
    switch (op) {
    case 0x01: r->p += 2; simple_op(r, AML_MUTEX); return;
    case 0x02: r->p += 2; simple_op(r, AML_EVENT); return;
    case 0x13: r->p += 2; create_field_op(r, 0x13); return;
    case 0x80: r->p += 2; region_op(r); return;
    case 0x81: r->p += 2; field_op(r, AML_REGION_FIELD); return;
    case 0x82: r->p += 2; scope_op(r, AML_DEVICE); return;
    case 0x83: r->p += 2; scope_op(r, AML_PROCESSOR); return;
    case 0x84: r->p += 2; scope_op(r, AML_POWER); return;
    case 0x85: r->p += 2; scope_op(r, AML_THERMAL); return;
    case 0x86: r->p += 2; field_op(r, AML_INDEX_FIELD); return;
    case 0x87: r->p += 2; field_op(r, AML_BANK_FIELD); return;
    case 0x88: r->p += 2; data_region_op(r); return;
    case 0x21:                          /* Stall(us) */
        r->p += 2;
        if (integer_operand(r, &n) && aml_machine && aml_machine->stall_us)
            aml_machine->stall_us(n > 1000 ? 1000 : (uint32_t)n);
        return;
    case 0x22:                          /* Sleep(ms) */
        r->p += 2;
        if (integer_operand(r, &n) && aml_machine && aml_machine->sleep_ms)
            aml_machine->sleep_ms(n > 10000 ? 10000 : (uint32_t)n);
        return;
    case 0x24:                          /* Signal(Event), Reset(Event), Release(Mutex) */
    case 0x26:
    case 0x27: {
        r->p += 2;
        if (super_ref(r, &ref, false)) {
            struct aml_node *x = ref_node(r, &ref, op == 0x27 ? AML_MUTEX : AML_EVENT);
            if (x && op == 0x24)
                x->event.signals++;
            else if (x && op == 0x26)
                x->event.signals = 0;
            else if (x && x->mutex.depth) {
                x->mutex.depth--;
                for (int i = held_count - 1; i >= 0; i--)
                    if (held[i] == x) {
                        held[i] = held[--held_count];
                        break;
                    }
            }
            else if (x)                 /* (ACPICA: AE_AML_MUTEX_NOT_ACQUIRED) */
                fail(r, "Release of a mutex not acquired");
        }
        aml_release(&ref);
        return;
    }
    case 0x2A: {                        /* Unload(DDBHandle): tables stay */
        r->p += 2;
        struct aml_value v = {0};
        eval(r, &v);
        aml_release(&v);
        return;
    }
    case 0x32:                          /* Fatal(Type, Code, Arg) */
        r->p += 2;
        if (take(r, 5, &at) && integer_operand(r, &n))
            aml_note("Fatal: type 0x%X code 0x%X argument 0x%lX", (unsigned)at[0],
                     (unsigned)(at[1] | at[2] << 8 | at[3] << 16 | (uint32_t)at[4] << 24),
                     (unsigned long)n);
        return;
    }
    struct aml_value v = {0};
    eval(r, &v);
    aml_release(&v);
}

static NOINLINE void return_op(struct run *r)
{
    struct aml_value v = {0};
    if (eval(r, &v)) {
        aml_release(&r->result);
        r->result = v;
        r->flow = FLOW_RETURN;
    }
}

static NOINLINE void external_op(struct run *r)
{
    struct name n;
    const uint8_t *at;
    if (name_string(r, &n))
        take(r, 2, &at);
}

static NOINLINE void statement_value(struct run *r)
{
    struct aml_value v = {0};
    eval(r, &v);
    aml_release(&v);
}

static void run_term(struct run *r)
{
    if (!enter(r))
        return;
    const uint8_t *end;
    switch (*r->p) {
    case 0x06: r->p++; alias_op(r); break;
    case 0x08: r->p++; name_op(r); break;
    case 0x10: r->p++; scope_op(r, AML_SCOPE); break;
    case 0x14: r->p++; method_op(r); break;
    case 0x15: r->p++; external_op(r); break; /* External: for compilers */
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F:
        create_field_op(r, *r->p++);
        break;
    case 0xA0: r->p++; if_op(r); break;
    case 0xA1:                          /* an Else without its If */
        r->p++;
        if (pkg_length(r, &end))
            r->p = end;
        break;
    case 0xA2: r->p++; while_op(r); break;
    case 0xA3:
    case 0xCC:
        r->p++;
        break;
    case 0xA4: r->p++; return_op(r); break; /* Return(ArgObject) */
    case 0xA5: r->p++; r->flow = FLOW_BREAK; break;
    case 0x9F: r->p++; r->flow = FLOW_CONTINUE; break;
    case 0x86: r->p++; notify_op(r); break;
    case 0x5B: ext_term(r); break;
    default: statement_value(r);
    }
    nesting--;
}

/* ---- Skimming: reading past a term without running it (a term that
 * failed while loading, to go on with the next one as ACPICA does) ----
 *
 * Each opcode's operands as letters: T a TermArg, S a SuperName or Target
 * (a name there is not called), N a NameString, b w d q a byte, word,
 * dword, qword, z a string's bytes, P the rest of a PkgLength. */

static const char *operands(int op, int ext)
{
    if (op == 0x5B) {
        switch (ext) {
        case 0x01: return "Nb";
        case 0x02: return "N";
        case 0x12: return "SS";
        case 0x13: return "TTTN";
        case 0x1F: return "TTTTTT";
        case 0x20: return "NS";
        case 0x21: case 0x22: return "T";
        case 0x23: return "Sw";
        case 0x24: case 0x26: case 0x27: case 0x2A: return "S";
        case 0x25: return "ST";
        case 0x28: case 0x29: return "TS";
        case 0x30: case 0x31: case 0x33: return "";
        case 0x32: return "bdT";
        case 0x80: return "NbTT";
        case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
            return "P";
        case 0x88: return "NTTT";
        }
        return 0;
    }
    switch (op) {
    case 0x00: case 0x01: case 0xFF: return "";
    case 0x0A: return "b";
    case 0x0B: return "w";
    case 0x0C: return "d";
    case 0x0E: return "q";
    case 0x0D: return "z";
    case 0x06: return "NN";
    case 0x08: return "NT";
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0xA0: case 0xA1: case 0xA2: return "P";
    case 0x15: return "Nbb";
    case 0x70: return "TS";
    case 0x71: case 0x75: case 0x76: case 0x87: case 0x8E: return "S";
    case 0x72: case 0x73: case 0x74: case 0x77: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0x84: case 0x85: case 0x88:
    case 0x9C: return "TTS";
    case 0x78: return "TTSS";
    case 0x80: case 0x81: case 0x82: case 0x96: case 0x97: case 0x98: case 0x99:
    case 0x9D: return "TS";
    case 0x83: case 0x92: case 0xA4: return "T";
    case 0x86: return "ST";
    case 0x89: return "TbTbTT";
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F: return "TTN";
    case 0x90: case 0x91: case 0x93: case 0x94: case 0x95: return "TT";
    case 0x9E: return "TTTS";
    case 0x9F: case 0xA3: case 0xA5: case 0xCC: return "";
    }
    if (op >= 0x60 && op <= 0x6E)
        return "";
    return 0;
}

static bool skim_bytes(struct run *r, size_t n)
{
    if ((size_t)(r->end - r->p) < n)
        return false;
    r->p += n;
    return true;
}

static bool skim_name(struct run *r, struct name *n)
{
    memset(n, 0, sizeof(*n));
    if (r->p < r->end && *r->p == '\\')
        n->root = true, r->p++;
    while (r->p < r->end && *r->p == '^')
        n->parents++, r->p++;
    if (r->p >= r->end)
        return false;
    int c = *r->p;
    uint32_t count;
    if (c == 0x00)
        count = 0, r->p++;
    else if (c == 0x2E)
        count = 2, r->p++;
    else if (c == 0x2F && r->end - r->p >= 2)
        count = r->p[1], r->p += 2;
    else if (lead_char(c))
        count = 1;
    else
        return false;
    n->count = (uint8_t)count;
    n->segs = r->p;
    return skim_bytes(r, 4 * count);
}

static NOINLINE bool skim_term(struct run *r, bool super, int depth)
{
    if (depth > 64 || r->p >= r->end)
        return false;
    int op = *r->p;
    if (name_start(op)) {
        struct name n;
        if (!skim_name(r, &n))
            return false;
        struct aml_node *node = super ? 0 : aml_alias_target(resolve(r, &n));
        int count = node && node->type == AML_METHOD ? node->method.args : 0;
        for (int i = 0; i < count; i++)
            if (!skim_term(r, false, depth + 1))
                return false;
        return true;
    }
    r->p++;
    int ext = -1;
    if (op == 0x5B) {
        if (r->p >= r->end)
            return false;
        ext = *r->p++;
    }
    const char *kinds = operands(op, ext);
    if (!kinds)
        return false;
    for (; *kinds; kinds++) {
        struct name n;
        bool ok;
        switch (*kinds) {
        case 'T': ok = skim_term(r, false, depth + 1); break;
        case 'S': ok = skim_term(r, true, depth + 1); break;
        case 'N': ok = skim_name(r, &n); break;
        case 'b': ok = skim_bytes(r, 1); break;
        case 'w': ok = skim_bytes(r, 2); break;
        case 'd': ok = skim_bytes(r, 4); break;
        case 'q': ok = skim_bytes(r, 8); break;
        case 'z':
            while (r->p < r->end && *r->p)
                r->p++;
            ok = skim_bytes(r, 1);
            break;
        default: {                      /* P: to the package's end */
            const uint8_t *q = r->p;
            if (q >= r->end)
                return false;
            int more = *q >> 6;
            uint32_t length = more ? (uint32_t)(*q & 0xF) : (uint32_t)(*q & 0x3F);
            if (r->end - q <= more)
                return false;
            for (int i = 0; i < more; i++)
                length |= (uint32_t)q[1 + i] << (4 + 8 * i);
            if (length > (size_t)(r->end - q))
                return false;
            r->p = q + length;
            ok = true;
        }
        }
        if (!ok)
            return false;
    }
    return true;
}

static void run_block(struct run *r, const uint8_t *end, struct aml_node *scope)
{
    const uint8_t *outer = r->end;
    struct aml_node *outer_scope = r->scope;
    bool loading = !r->frame->method;
    r->end = end;
    if (scope)
        r->scope = scope;
    while (r->p < end && r->flow == FLOW_NORMAL) {
        const uint8_t *start = r->p;
        run_term(r);
        if (r->flow == FLOW_ERROR && loading) {
            /* the table goes on past the term that failed (skimmed from
             * its start), else past the block it is in */
            r->flow = FLOW_NORMAL;
            r->p = start;
            if (!skim_term(r, false, 0))
                break;
        }
    }
    if (r->flow == FLOW_NORMAL)
        r->p = end;
    r->end = outer;
    r->scope = outer_scope;
}

/* ---- Methods and tables ---- */

/* A returned value must not point into the frame that ends. */
static bool keep_result(struct aml_frame *f, struct aml_value *v)
{
    if (v->type != AML_REFERENCE)
        return true;
    bool slot = v->ref == REF_SLOT && v->slot >= f->locals && v->slot < f->args + 7;
    bool own = v->ref == REF_NODE && v->node->temporary;
    for (uint32_t i = 0; own && i < f->made_count; i++)
        if (f->made[i] == v->node) {
            struct aml_value c = {0};
            bool ok = aml_value_of(v->node, &c);
            aml_release(v);
            *v = c;
            return ok;
        }
    if (slot) {
        struct aml_value c = {0};
        aml_copy(&c, v->slot);
        aml_release(v);
        *v = c;
    }
    return true;
}

static void frame_end(struct aml_frame *f)
{
    for (int i = 0; i < 8; i++)
        aml_release(&f->locals[i]);
    for (int i = 0; i < 7; i++)
        aml_release(&f->args[i]);
    aml_release(&f->implicit);
    while (f->made_count)
        aml_node_free(f->made[--f->made_count]);
    kfree(f->made);
    kfree(f);
}

/* \_OSI: the interpreter's own, its arguments padded to 7. */
static NOINLINE bool run_native(struct aml_node *method, const struct aml_value *args, int nargs,
                                struct aml_value *out)
{
    struct aml_value given[7];
    memset(given, 0, sizeof(given));
    for (int i = 0; i < nargs; i++)
        aml_copy(&given[i], &args[i]);
    bool ok = method->method.native(given, out);
    for (int i = 0; i < 7; i++)
        aml_release(&given[i]);
    return ok;
}

bool aml_run_method(struct aml_node *method, const struct aml_value *args, int nargs,
                    struct aml_value *out, struct aml_frame *caller)
{
    method = aml_alias_target(method);
    if (!method || method->type != AML_METHOD)
        return false;
    if (nargs > 7)
        nargs = 7;
    if (method->method.native)
        return run_native(method, args, nargs, out);
    if (calls >= CALLS_DEEPEST) {
        char path[96];
        aml_path(method, path, sizeof(path));
        aml_fail("%s: methods called %d deep", path, calls);
        return false;
    }
    struct aml_frame *f = kzalloc(sizeof(*f));
    if (!f)
        return false;
    uintptr_t here = (uintptr_t)&here;
    if (!calls && !nesting)
        stack_top = here;
    calls++;
    f->method = method;
    f->caller = caller;
    f->depth = calls;
    for (int i = 0; i < nargs; i++)
        aml_copy(&f->args[i], &args[i]);
    struct run r = {
        .p = method->method.code, .end = method->method.code + method->method.length,
        .base = method->method.code, .scope = method, .frame = f,
    };
    aml_counters.methods_run++;
    while (r.p < r.end && r.flow == FLOW_NORMAL)
        run_term(&r);
    if (r.flow != FLOW_RETURN && r.flow != FLOW_ERROR) {
        aml_release(&r.result);         /* (no Return: the last result, or 0) */
        r.result = f->implicit;
        memset(&f->implicit, 0, sizeof(f->implicit));
        if (r.result.type == AML_UNINIT)
            aml_set_integer(&r.result, 0);
    }
    bool ok = r.flow != FLOW_ERROR && keep_result(f, &r.result);
    if (ok) {
        aml_release(out);
        *out = r.result;
    } else {
        aml_release(&r.result);
    }
    frame_end(f);
    if (--calls == 0)
        release_all();
    return ok;
}

int aml_load(const void *table, uint32_t length)
{
    const uint8_t *t = table;
    if (!aml_root || length < 36)
        return -1;
    uint32_t declared = (uint32_t)t[4] | (uint32_t)t[5] << 8 | (uint32_t)t[6] << 16 |
                        (uint32_t)t[7] << 24;
    if (declared < 36 || declared > length) {
        aml_fail("a table of %u bytes says %u", length, declared);
        return -1;
    }
    /* the DSDT's revision sets the integers' width for all (as ACPICA) */
    if (!memcmp(t, "DSDT", 4) && t[8] < 2)
        aml_ones = 0xFFFFFFFF;
    struct aml_frame *f = kzalloc(sizeof(*f));
    if (!f)
        return -1;
    uintptr_t here = (uintptr_t)&here;
    if (!calls && !nesting)
        stack_top = here;
    struct run r = {.p = t + 36, .end = t + declared, .base = t, .scope = aml_root, .frame = f};
    uint32_t before = aml_counters.nodes;
    run_block(&r, r.end, aml_root);
    aml_release(&r.result);
    frame_end(f);
    aml_counters.loads++;
    return (int)(aml_counters.nodes - before);
}
