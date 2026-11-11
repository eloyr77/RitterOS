/*
 * The engine's heap and collector (27o): mark and sweep.
 *
 * Cells come from blocks of 64 KiB, each holding cells of one size class
 * (16 to 2048 bytes); larger cells are malloc'd one by one ("big" cells).
 * Both lists are kept sorted by address, so any word can be asked "is
 * this inside a live cell?": that is how the C stack is looked at. The
 * roots are the realm's intrinsics, the interpreter's value stack, frames
 * and handlers (exact), ranges native code registered with js_root_push,
 * and every word of the C stack between js_set_stack_base's address and
 * the collector's own frame, the registers spilled there by setjmp first
 * (conservative: a word that looks like a pointer into a live cell, or a
 * value tagged with one, keeps that cell). The interpreter's value stack
 * is scanned up to the highest point it ever reached, conservatively too,
 * so the loop need not keep the stack pointer stored for the collector.
 *
 * Atoms are weak: an atom nothing else holds is dropped from the table
 * when its cell is swept. Compilation runs with the collector held off
 * (gc_inhibit), since the syntax tree in malloc'd memory holds atoms.
 */
#include <setjmp.h>
#include "jsint.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define JS_NO_ASAN __attribute__((no_sanitize("address")))
#endif
#endif
#ifndef JS_NO_ASAN
#define JS_NO_ASAN
#endif

#define BLOCK_SIZE (64 * 1024)
#define MIN_THRESHOLD (8u << 20)

static const uint32_t class_size[16] = {
    16, 32, 48, 64, 80, 96, 128, 160, 192, 256, 320, 384, 512, 768, 1024, 2048,
};

struct block {
    char *base;                                 /* BLOCK_SIZE bytes */
    uint32_t size, ncells;
};

struct bigcell {
    size_t size;
    uint64_t pad;                               /* keeps the cell 16-aligned */
    struct cell c[];
};

struct freecell {
    struct cell h;
    struct freecell *next;
};

static int size_class(size_t n)
{
    for (int i = 0; i < 16; i++)
        if (n <= class_size[i])
            return i;
    return -1;
}

void gc_note_external(struct js *J, size_t bytes)
{
    J->allocated += bytes;
}

static void collect_if_due(struct js *J)
{
#ifdef JS_GC_STRESS
    /* (a test build: collect every JS_GC_STRESS allocations) */
    static unsigned long count;
    if (++count % JS_GC_STRESS == 0 && !J->gc_inhibit) {
        js_gc(J);
        return;
    }
#endif
    if (J->allocated > J->threshold && !J->gc_inhibit)
        js_gc(J);
}

static bool new_block(struct js *J, int cls)
{
    struct block *b = malloc(sizeof *b);
    char *base = malloc(BLOCK_SIZE);
    if (!b || !base) {
        free(b);
        free(base);
        return false;
    }
    b->base = base;
    b->size = class_size[cls];
    b->ncells = BLOCK_SIZE / b->size;
    if (J->nblocks == J->blocks_cap) {
        uint32_t cap = J->blocks_cap ? J->blocks_cap * 2 : 64;
        struct block **nb = realloc(J->blocks, cap * sizeof *nb);
        if (!nb) {
            free(b);
            free(base);
            return false;
        }
        J->blocks = nb;
        J->blocks_cap = cap;
    }
    uint32_t at = J->nblocks;                   /* sorted by address */
    while (at > 0 && J->blocks[at - 1]->base > base) {
        J->blocks[at] = J->blocks[at - 1];
        at--;
    }
    J->blocks[at] = b;
    J->nblocks++;
    struct freecell *list = (struct freecell *)J->free_lists[cls];
    for (uint32_t i = b->ncells; i-- > 0;) {
        struct freecell *f = (struct freecell *)(base + (size_t)i * b->size);
        f->h.kind = CELL_FREE;
        f->next = list;
        list = f;
    }
    J->free_lists[cls] = &list->h;
    return true;
}

static void *big_alloc(struct js *J, size_t size)
{
    struct bigcell *b = malloc(sizeof *b + size);
    if (!b)
        return 0;
    b->size = size;
    if (J->nbigs == J->bigs_cap) {
        uint32_t cap = J->bigs_cap ? J->bigs_cap * 2 : 64;
        struct bigcell **nb = realloc(J->bigs, cap * sizeof *nb);
        if (!nb) {
            free(b);
            return 0;
        }
        J->bigs = nb;
        J->bigs_cap = cap;
    }
    uint32_t at = J->nbigs;
    while (at > 0 && J->bigs[at - 1] > b) {
        J->bigs[at] = J->bigs[at - 1];
        at--;
    }
    J->bigs[at] = b;
    J->nbigs++;
    return b->c;
}

void *gc_alloc(struct js *J, enum cell_kind kind, size_t size)
{
    collect_if_due(J);
    int cls = size_class(size);
    struct cell *c;
    if (cls < 0) {
        c = big_alloc(J, size);
        if (!c) {
            js_gc(J);
            c = big_alloc(J, size);
            if (!c)
                return 0;
        }
    } else {
        if (!J->free_lists[cls] && !new_block(J, cls)) {
            js_gc(J);
            if (!J->free_lists[cls] && !new_block(J, cls))
                return 0;
        }
        struct freecell *f = (struct freecell *)J->free_lists[cls];
        J->free_lists[cls] = f->next ? &f->next->h : 0;
        c = &f->h;
        size = class_size[cls];
    }
    memset(c, 0, size);
    c->kind = (uint8_t)kind;
    J->allocated += size;
    return c;
}

/* --- finding cells by address -------------------------------------------- */

static struct cell *find_cell(struct js *J, uintptr_t a)
{
    uint32_t lo = 0, hi = J->nblocks;
    while (lo < hi) {                           /* the last block starting at or below a */
        uint32_t mid = (lo + hi) / 2;
        if ((uintptr_t)J->blocks[mid]->base <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo > 0) {
        struct block *b = J->blocks[lo - 1];
        uintptr_t off = a - (uintptr_t)b->base;
        if (off < BLOCK_SIZE) {
            uint32_t i = (uint32_t)(off / b->size);
            if (i < b->ncells) {
                struct cell *c = (struct cell *)(b->base + (size_t)i * b->size);
                return c->kind != CELL_FREE ? c : 0;
            }
            return 0;
        }
    }
    lo = 0, hi = J->nbigs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if ((uintptr_t)J->bigs[mid]->c <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo > 0) {
        struct bigcell *b = J->bigs[lo - 1];
        if (a - (uintptr_t)b->c < b->size)
            return b->c;
    }
    return 0;
}

/* --- marking -------------------------------------------------------------- */

void gc_mark_cell(struct js *J, void *p)
{
    struct cell *c = p;
    if (!c || c->mark)
        return;
    c->mark = 1;
    if (J->mark_n == J->mark_cap) {
        uint32_t cap = J->mark_cap ? J->mark_cap * 2 : 1024;
        struct cell **ns = realloc(J->mark_stack, cap * sizeof *ns);
        if (!ns)
            abort();                            /* (out of memory while collecting: nothing sane is left) */
        J->mark_stack = ns;
        J->mark_cap = cap;
    }
    J->mark_stack[J->mark_n++] = c;
}

void gc_mark_value(struct js *J, jsval v)
{
    if (js_tag(v) >= JS_T_OBJECT)
        gc_mark_cell(J, js_pointer(v));
}

static void mark_values(struct js *J, const jsval *v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        gc_mark_value(J, v[i]);
}

static void mark_propmap(struct js *J, struct propmap *m)
{
    for (uint32_t i = 0; i < m->n; i++)
        if (m->e[i].key != JS_EMPTY) {
            gc_mark_value(J, m->e[i].key);
            gc_mark_value(J, m->e[i].value);
        }
}

static void trace(struct js *J, struct cell *c)
{
    switch (c->kind) {
    case CELL_STRING: {
        struct jsstring *s = (struct jsstring *)c;
        if (s->h.flags & STR_ROPE) {
            if (s->h.flags & STR_FLATTENED)
                gc_mark_cell(J, s->flat);
            else {
                gc_mark_cell(J, s->left);
                gc_mark_cell(J, s->right);
            }
        }
        break;
    }
    case CELL_SYMBOL:
        gc_mark_cell(J, ((struct jssymbol *)c)->description);
        break;
    case CELL_SCOPE: {
        struct scope *s = (struct scope *)c;
        gc_mark_cell(J, s->parent);
        mark_values(J, s->vars, s->h.extra);
        break;
    }
    case CELL_FUNCDEF: {
        struct funcdef *f = (struct funcdef *)c;
        if (f->consts)
            mark_values(J, f->consts, f->nconsts);
        gc_mark_cell(J, f->name);
        gc_mark_cell(J, f->file);
        gc_mark_cell(J, f->source);
        break;
    }
    case CELL_ACCESSOR: {
        struct accessor *a = (struct accessor *)c;
        gc_mark_value(J, a->get);
        gc_mark_value(J, a->set);
        break;
    }
    case CELL_OBJECT: {
        struct jsobject *o = (struct jsobject *)c;
        gc_mark_cell(J, o->proto);
        mark_propmap(J, &o->props);
        if (o->elems)
            mark_values(J, o->elems, o->length < o->cap ? o->length : o->cap);
        switch (obj_class(o)) {
        case CLASS_FUNCTION:
            gc_mark_cell(J, o->u.fn.def);
            gc_mark_cell(J, o->u.fn.env);
            gc_mark_cell(J, o->u.fn.home);
            gc_mark_value(J, o->u.fn.fields);
            break;
        case CLASS_NATIVE:
            gc_mark_value(J, o->u.native.data);
            break;
        case CLASS_BOUND:
            gc_mark_value(J, o->u.bound.target);
            gc_mark_value(J, o->u.bound.this_val);
            if (o->u.bound.args)
                mark_values(J, o->u.bound.args, o->u.bound.argc);
            break;
        case CLASS_BOOLEAN: case CLASS_NUMBER: case CLASS_STRING: case CLASS_SYMBOL: case CLASS_DATE:
            gc_mark_value(J, o->u.prim);
            break;
        case CLASS_REGEXP:
            gc_mark_cell(J, o->u.re.source);
            gc_mark_cell(J, o->u.re.flags);
            break;
        case CLASS_MAP: case CLASS_SET: case CLASS_WEAKMAP: case CLASS_WEAKSET: {
            struct mapdata *m = o->u.map;
            if (m)
                for (uint32_t i = 0; i < m->n; i++)
                    if (m->e[i].key != JS_EMPTY) {
                        gc_mark_value(J, m->e[i].key);
                        gc_mark_value(J, m->e[i].value);
                    }
            break;
        }
        case CLASS_ITERATOR:
            gc_mark_value(J, o->u.iter.target);
            gc_mark_value(J, o->u.iter.extra);
            break;
        case CLASS_PROXY:                       /* (64r) */
            gc_mark_value(J, o->u.proxy.target);
            gc_mark_value(J, o->u.proxy.handler);
            break;
        case CLASS_FOR_IN:
            if (o->u.forin.keys)
                mark_values(J, o->u.forin.keys, o->u.forin.n);
            gc_mark_value(J, o->u.forin.object);
            break;
        case CLASS_ERROR:
            gc_mark_cell(J, o->u.err.file);
            break;
        case CLASS_ARGUMENTS:
            gc_mark_cell(J, o->u.args.scope);
            break;
        case CLASS_HOST:
            if (o->u.host.cls && o->u.host.cls->trace)
                o->u.host.cls->trace(J, o->u.host.data);
            break;
        case CLASS_GENERATOR: {                 /* (27r) */
            struct genstate *st = o->u.gen;
            if (!st)
                break;
            gc_mark_cell(J, st->callee);
            gc_mark_value(J, st->this_val);
            gc_mark_value(J, st->new_target);
            gc_mark_value(J, st->promise);
            gc_mark_cell(J, st->scope);
            if (st->saved)
                mark_values(J, st->saved, st->argc + st->nregs + st->depth);
            for (uint32_t i = 0; i < st->nhandlers; i++)
                gc_mark_cell(J, st->handlers[i].scope);
            break;
        }
        case CLASS_PROMISE:
            if (o->u.promise) {
                gc_mark_value(J, o->u.promise->result);
                gc_mark_value(J, o->u.promise->reactions);
            }
            break;
        }
        break;
    }
    }
}

static void drain(struct js *J)
{
    while (J->mark_n)
        trace(J, J->mark_stack[--J->mark_n]);
}

/* A word that may be a pointer or a value holding one. */
static void JS_NO_ASAN mark_maybe(struct js *J, uint64_t w)
{
    if (w >= 4096) {
        struct cell *c = find_cell(J, (uintptr_t)w);
        if (c)
            gc_mark_cell(J, c);
    }
    if ((w >> 51) == 0x1FFF && ((w >> JS_TAG_SHIFT) & 15) >= JS_T_OBJECT) {
        struct cell *c = find_cell(J, (uintptr_t)js_pointer(w));
        if (c)
            gc_mark_cell(J, c);
    }
}

static void JS_NO_ASAN scan_range(struct js *J, const void *from, const void *to)
{
    uintptr_t a = ((uintptr_t)from + 7) & ~(uintptr_t)7;
    for (; a + 8 <= (uintptr_t)to; a += 8) {
        uint64_t w;
        memcpy(&w, (const void *)a, 8);
        mark_maybe(J, w);
    }
}

static JS_NOINLINE void JS_NO_ASAN scan_c_stack(struct js *J)
{
    jmp_buf regs;
    setjmp(regs);                               /* the callee-saved registers, onto this frame */
    volatile char here = 0;
    const char *lo = (const char *)&here;
    const char *r = (const char *)&regs;
    if (r < lo)
        lo = r;
    if (J->stack_base && (const char *)J->stack_base > lo)
        scan_range(J, lo, (const char *)J->stack_base + sizeof(void *));
    scan_range(J, &regs, (const char *)&regs + sizeof regs);
}

static void mark_roots(struct js *J)
{
    gc_mark_cell(J, J->global);
    for (int i = 0; i < P_COUNT; i++)
        gc_mark_cell(J, J->proto[i]);
    for (int i = 0; i < C_COUNT; i++)
        gc_mark_cell(J, J->ctor[i]);
    mark_values(J, J->symbol, SYM_COUNT);
    gc_mark_cell(J, J->global_lex);
    gc_mark_cell(J, J->symbol_registry);
    gc_mark_cell(J, J->throw_type_error);
    gc_mark_value(J, J->array_proto_values);
    gc_mark_value(J, J->array_iter_next);
    gc_mark_value(J, J->eval_fn);
    gc_mark_value(J, J->jobs);
    gc_mark_value(J, J->intl);
    gc_mark_value(J, J->reaction_job);
    gc_mark_value(J, J->thenable_job);
    gc_mark_cell(J, J->native_callee);
    gc_mark_value(J, J->exception);
    gc_mark_cell(J, J->current_file);
    for (int i = 0; i < A_COUNT; i++)
        gc_mark_cell(J, J->atom[i]);
    for (int i = 0; i < 256; i++)
        gc_mark_cell(J, J->char_strings[i]);
    for (uint32_t i = 0; i < J->nframes; i++) {
        struct frame *f = &J->frames[i];
        gc_mark_cell(J, f->def);
        gc_mark_cell(J, f->callee);
        gc_mark_value(J, f->this_val);
        gc_mark_value(J, f->new_target);
        gc_mark_cell(J, f->scope);
        gc_mark_cell(J, f->gen);
    }
    for (uint32_t i = 0; i < J->nhandlers; i++)
        gc_mark_cell(J, J->handlers[i].scope);
    for (uint32_t i = 0; i < J->nroots; i++)
        mark_values(J, J->roots[i].v, J->roots[i].n);
    for (uint32_t i = 0; i < J->nvisiting; i++)
        gc_mark_cell(J, J->visiting[i]);
    if (J->stack)
        scan_range(J, J->stack, J->sp_high);
    scan_c_stack(J);
}

/* --- sweeping ------------------------------------------------------------- */

static void finalize(struct js *J, struct cell *c)
{
    (void)J;
    switch (c->kind) {
    case CELL_OBJECT: {
        struct jsobject *o = (struct jsobject *)c;
        propmap_free(&o->props);
        free(o->elems);
        switch (obj_class(o)) {
        case CLASS_BOUND:
            free(o->u.bound.args);
            break;
        case CLASS_MAP: case CLASS_SET: case CLASS_WEAKMAP: case CLASS_WEAKSET:
            if (o->u.map) {
                free(o->u.map->e);
                free(o->u.map->buckets);
                free(o->u.map);
            }
            break;
        case CLASS_REGEXP: {
            extern void regex_free(struct regex *);
            if (o->u.re.prog)
                regex_free(o->u.re.prog);
            break;
        }
        case CLASS_FOR_IN:
            free(o->u.forin.keys);
            break;
        case CLASS_ARGUMENTS:
            free(o->u.args.map);
            break;
        case CLASS_HOST:
            if (o->u.host.cls && o->u.host.cls->finalize)
                o->u.host.cls->finalize(o->u.host.data);
            break;
        case CLASS_GENERATOR:
            gen_free(o->u.gen);
            break;
        case CLASS_PROMISE:
            promise_free(o->u.promise);
            break;
        }
        break;
    }
    case CELL_FUNCDEF: {
        struct funcdef *f = (struct funcdef *)c;
        free(f->code);
        free(f->consts);
        free(f->lines);
        break;
    }
    }
}

static void sweep(struct js *J)
{
    size_t live = 0;
    for (int i = 0; i < 16; i++)
        J->free_lists[i] = 0;
    struct freecell *lists[16] = { 0 };
    for (uint32_t bi = 0; bi < J->nblocks; bi++) {
        struct block *b = J->blocks[bi];
        int cls = size_class(b->size);
        for (uint32_t i = b->ncells; i-- > 0;) {
            struct cell *c = (struct cell *)(b->base + (size_t)i * b->size);
            if (c->kind != CELL_FREE) {
                if (c->mark) {
                    c->mark = 0;
                    live += b->size;
                    continue;
                }
                finalize(J, c);
                c->kind = CELL_FREE;
            }
            struct freecell *f = (struct freecell *)c;
            f->next = lists[cls];
            lists[cls] = f;
        }
    }
    for (int i = 0; i < 16; i++)
        J->free_lists[i] = lists[i] ? &lists[i]->h : 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < J->nbigs; i++) {
        struct bigcell *b = J->bigs[i];
        if (b->c->mark) {
            b->c->mark = 0;
            live += b->size;
            J->bigs[k++] = b;
        } else {
            finalize(J, b->c);
            free(b);
        }
    }
    J->nbigs = k;
    J->live_after = live;
}

void js_gc(struct js *J)
{
    if (J->gc_inhibit)
        return;
    J->gc_inhibit++;
    mark_roots(J);
    drain(J);
    atoms_sweep(J);                             /* before the cells go: the table looks at their marks */
    sweep(J);
    J->allocated = 0;
    J->threshold = J->live_after > MIN_THRESHOLD ? J->live_after : MIN_THRESHOLD;
    J->gc_inhibit--;
}

void js_set_stack_base(struct js *J, void *base)
{
    J->stack_base = base;
}

void js_root_push(struct js *J, jsval *values, size_t count)
{
    if (J->nroots == J->roots_cap) {
        uint32_t cap = J->roots_cap ? J->roots_cap * 2 : 32;
        struct root_range *nr = realloc(J->roots, cap * sizeof *nr);
        if (!nr)
            abort();
        J->roots = nr;
        J->roots_cap = cap;
    }
    J->roots[J->nroots].v = values;
    J->roots[J->nroots].n = count;
    J->nroots++;
}

void js_root_pop(struct js *J)
{
    if (J->nroots)
        J->nroots--;
}

void gc_free_all(struct js *J)
{
    for (uint32_t bi = 0; bi < J->nblocks; bi++) {
        struct block *b = J->blocks[bi];
        for (uint32_t i = 0; i < b->ncells; i++) {
            struct cell *c = (struct cell *)(b->base + (size_t)i * b->size);
            if (c->kind != CELL_FREE)
                finalize(J, c);
        }
        free(b->base);
        free(b);
    }
    for (uint32_t i = 0; i < J->nbigs; i++) {
        finalize(J, J->bigs[i]->c);
        free(J->bigs[i]);
    }
    free(J->blocks);
    free(J->bigs);
    free(J->mark_stack);
    free(J->roots);
    J->blocks = 0;
    J->bigs = 0;
    J->nblocks = J->nbigs = 0;
}
