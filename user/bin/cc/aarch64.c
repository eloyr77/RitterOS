/*
 * The AArch64 backend (60d): the intermediate code as GNU assembly for
 * 64-bit ARM, with AAPCS64's calls (integers in x0-x7, floating values in
 * v0-v7, homogeneous floating aggregates in consecutive v registers, other
 * structures up to 16 bytes in one or two x registers, larger ones by
 * reference to a copy, a large result through x8) and its va_list. long
 * double is IEEE binary128; its operations call the C library's helpers
 * (__addtf3 and the rest, user/lib/longdouble.c).
 *
 * As on x86-64, every value has a stack slot; slots are addressed from
 * x19, which holds the bottom of the frame (sp moves with VLAs and calls).
 * Scratch registers: x9-x15, x16 for addresses out of reach, v16 and v17.
 */
#include "regalloc.h"

static FILE *out;
static struct ir_func *F;
static int fn_number;
static int *slot_of;                            /* value -> offset from x19 */
static struct regalloc ra;                      /* (61a) values in registers: x20-x28, d8-d15 */
static bool gpr_used[9], fpr_used[8];

/* (61d) a value register's x number: 0-8 are x20-x28 (kept for the
 * caller, saved when used), 9-14 are x2-x7 (overwritten by calls: given to
 * values with no call, helper call, TLS access or return in their life) */
enum { SAVED_GPRS = 9, VALUE_GPRS = 15 };
static int xreg(int reg)
{
    return reg < SAVED_GPRS ? 20 + reg : reg - SAVED_GPRS + 2;
}


/* (61b) as on x86-64: how often each value is set and used, where, and
 * which instructions are folded into their uses (see analyze) */
static int *defs, *def_at, *uses, *nonfolded;
static bool *skip, *chained;
/* (62f) the callee-saved registers a function uses get a word each after
 * its value slots (save_at: x20-x28, then d8-d15), not a fixed 136-byte
 * area in every frame: the kernel's 16 KiB stacks overflowed with it */
static int save_at[18], frame_top;           /* ([17]: x19's own) */
#define FRAME_HEADER 16                         /* (61d) x29 and x30 at [x29]; stack arguments above */
static int frame_size;
static int sret_slot;                           /* where x8 (a large result's place) is kept */
static int gr_area, vr_area;                    /* a variadic function's save areas */
static int va_gr, va_vr, va_stack;
static int unique;

static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputc('\t', out);
    vfprintf(out, fmt, ap);
    fputc('\n', out);
    va_end(ap);
}

static enum ir_class cls(int v)
{
    return F->values[v].cls;
}

static int align_up(int n, int a)
{
    return (n + a - 1) / a * a;
}

/* A register name of the right width: r(9, 8) is x9, r(9, 4) w9. */
static const char *r(int n, int size)
{
    static char buf[8][8];
    static int k;
    char *b = buf[k++ & 7];
    snprintf(b, 8, "%c%d", size == 8 ? 'x' : 'w', n);
    return b;
}

/* [x19, #off] for an access of size bytes, or [x16] after computing it. */
static const char *mem(int off, int size)
{
    static char buf[4][32];
    static int k;
    char *b = buf[k++ & 3];
    if (off >= 0 && off % size == 0 && off / size < 4096) {
        snprintf(b, 32, "[x19, #%d]", off);
        return b;
    }
    if (off >= 0 && off < 4096)
        line("add x16, x19, #%d", off);
    else {
        line("mov x16, #%d", off & 0xFFFF);
        if (off >> 16)
            line("movk x16, #%d, lsl #16", (off >> 16) & 0xFFFF);
        line("add x16, x19, x16");
    }
    snprintf(b, 32, "[x16]");
    return b;
}

static int vsize(int v)
{
    return value_size(cls(v));
}

static bool in_gpr(int v)
{
    return ra.reg && ra.reg[v] >= 0 && (cls(v) == V_I32 || cls(v) == V_I64);
}

static bool in_fpr(int v)
{
    return ra.reg && ra.reg[v] >= 0 && (cls(v) == V_F32 || cls(v) == V_F64);
}

/* size bytes of value v into xN/wN (raw bits: a float's too) */
static void ld_bits(int n, int v, int size)
{
    if (in_gpr(v))
        line("mov %s, %s", r(n, size), r(xreg(ra.reg[v]), size));
    else if (in_fpr(v))
        line("fmov %s, %c%d", r(n, size), size == 8 ? 'd' : 's', 8 + ra.reg[v]);
    else
        line("ldr %s, %s", r(n, size), mem(slot_of[v], size));
}

static void st_bits(int n, int v, int size)
{
    if (in_gpr(v))
        line("mov %s, %s", r(xreg(ra.reg[v]), size), r(n, size));
    else if (in_fpr(v))
        line("fmov %c%d, %s", size == 8 ? 'd' : 's', 8 + ra.reg[v], r(n, size));
    else
        line("str %s, %s", r(n, size), mem(slot_of[v], size));
}

/* load an integer value into xN/wN */
static void ld(int n, int v)
{
    ld_bits(n, v, vsize(v) == 8 ? 8 : 4);
}

static void st(int n, int v)
{
    int size = vsize(v) >= 8 ? 8 : 4;
    if (cls(v) == V_LD)
        size = 8;
    st_bits(n, v, size);
}

static char fpr(int v)
{
    return cls(v) == V_F32 ? 's' : cls(v) == V_F64 ? 'd' : 'q';
}

static void fld(int n, int v)
{
    if (in_fpr(v))
        line("fmov %c%d, %c%d", fpr(v), n, fpr(v), 8 + ra.reg[v]);
    else
        line("ldr %c%d, %s", fpr(v), n, mem(slot_of[v], vsize(v)));
}

/* (61b) v's register at size bytes, or scratch n after loading it */
static const char *src(int n, int v, int size)
{
    if (in_gpr(v))
        return r(xreg(ra.reg[v]), size);
    ld_bits(n, v, size);
    return r(n, size);
}

/* where d is computed: its own register, or scratch n */
static const char *dstr(int n, int d, int size)
{
    return in_gpr(d) ? r(xreg(ra.reg[d]), size) : r(n, size);
}

/* (after computing d in dstr(n, d, ...)) */
static void done(int n, int d)
{
    if (!in_gpr(d))
        st(n, d);
}

/* (61b) a float value's register (sN/dN), or scratch n after loading it */
static const char *fsrc(int n, int v)
{
    static char buf[4][8];
    static int k;
    char *b = buf[k++ & 3];
    if (in_fpr(v))
        snprintf(b, 8, "%c%d", fpr(v), 8 + ra.reg[v]);
    else {
        fld(n, v);
        snprintf(b, 8, "%c%d", fpr(v), n);
    }
    return b;
}

static const char *fdst(int n, int v)
{
    static char buf[4][8];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, 8, "%c%d", fpr(v), in_fpr(v) ? 8 + ra.reg[v] : n);
    return b;
}

static void fst(int n, int v);

static void fdone(int n, int v)
{
    if (!in_fpr(v))
        fst(n, v);
}

static void fst(int n, int v)
{
    if (in_fpr(v))
        line("fmov %c%d, %c%d", fpr(v), 8 + ra.reg[v], fpr(v), n);
    else
        line("str %c%d, %s", fpr(v), n, mem(slot_of[v], vsize(v)));
}

static void load_imm(int n, int64_t value, int size)
{
    uint64_t u = (uint64_t)value;
    if (size == 4)
        u &= 0xFFFFFFFFull;
    int chunks = size == 8 ? 4 : 2;
    /* movn for small negative numbers, else movz and movk for each part */
    if (size == 8 && value < 0 && value >= -65536) {
        line("movn %s, #%llu", r(n, 8), (unsigned long long)(~u & 0xFFFF));
        return;
    }
    bool first = true;
    for (int i = 0; i < chunks; i++) {
        unsigned part = (unsigned)((u >> (16 * i)) & 0xFFFF);
        if (!part && !(first && i == chunks - 1))
            continue;
        if (first)
            line("movz %s, #%u, lsl #%d", r(n, size), part, 16 * i);
        else
            line("movk %s, #%u, lsl #%d", r(n, size), part, 16 * i);
        first = false;
    }
    if (first)
        line("movz %s, #0", r(n, size));
}

static const char *label_name(int id)
{
    static char buf[4][32];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, 32, ".L%d_%d", fn_number, id);
    return b;
}

static void global_address(int n, const char *sym, struct obj *o, int64_t addend)
{
    if (symbol_is_local(o)) {
        line("adrp x%d, %s", n, sym);
        line("add x%d, x%d, :lo12:%s", n, n, sym);
    } else {
        line("adrp x%d, :got:%s", n, sym);
        line("ldr x%d, [x%d, :got_lo12:%s]", n, n, sym);
    }
    if (addend) {
        load_imm(10, addend, 8);
        line("add x%d, x%d, x10", n, n);
    }
}

/* the address held by value v into xN */
static void ld_addr(int n, int v)
{
    ld_bits(n, v, 8);
}

/* --- classification (AAPCS64) -------------------------------------------- */

/* A homogeneous floating-point aggregate: 1-4 members of one floating type. */
static bool hfa_walk(struct type *ty, struct type **base, int *count)
{
    ty = unqual(ty);
    if (is_flonum(ty)) {
        if (*base && (*base)->kind != ty->kind)
            return false;
        *base = ty;
        (*count)++;
        return *count <= 4;
    }
    if (ty->kind == TY_ARRAY) {
        for (int i = 0; i < ty->array_len; i++)
            if (!hfa_walk(ty->base, base, count))
                return false;
        return true;
    }
    if (is_memory_value(ty)) {
        if (ty->kind == TY_UNION) {
            /* (a union of one kind: each member the same) */
            int best = 0;
            for (struct member *m = ty->members; m; m = m->next) {
                int c = 0;
                if (m->is_bitfield || !hfa_walk(m->type, base, &c))
                    return false;
                if (c > best)
                    best = c;
            }
            *count += best;
            return *count <= 4 && *count > 0;
        }
        for (struct member *m = ty->members; m; m = m->next)
            if (m->is_bitfield || !hfa_walk(m->type, base, count))
                return false;
        return true;
    }
    return false;
}

static bool is_hfa(struct type *ty, struct type **base, int *count)
{
    if (!is_memory_value(unqual(ty)))
        return false;
    *base = 0;
    *count = 0;
    return hfa_walk(ty, base, count) && *count >= 1 && *count <= 4 && (*base)->size * *count == ty->size;
}

enum place_kind { P_GP, P_FP, P_HFA, P_STACK };

struct place {
    enum place_kind kind;
    int reg;                                    /* first x or v register */
    int nregs;
    struct type *hfa_base;
    bool by_reference;                          /* a structure over 16 bytes: its copy's address */
    int stack;                                  /* offset in the stack area */
    int stack_size;
    int copy;                                   /* (by reference: the caller's copy, from x19) */
};

struct counters {
    int ngrn, nsrn, nsaa;
};

static void place_of(struct type *ty, struct counters *c, struct place *p)
{
    ty = unqual(ty);
    memset(p, 0, sizeof(*p));
    struct type *base;
    int count;
    if (is_flonum(ty)) {
        if (c->nsrn < 8) {
            p->kind = P_FP;
            p->reg = c->nsrn++;
            return;
        }
        c->nsrn = 8;
        int a = ty->size == 16 ? 16 : 8;
        c->nsaa = align_up(c->nsaa, a);
        p->kind = P_STACK;
        p->stack = c->nsaa;
        p->stack_size = align_up(ty->size, 8);
        c->nsaa += p->stack_size;
        return;
    }
    if (is_hfa(ty, &base, &count)) {
        if (c->nsrn + count <= 8) {
            p->kind = P_HFA;
            p->reg = c->nsrn;
            p->nregs = count;
            p->hfa_base = base;
            c->nsrn += count;
            return;
        }
        c->nsrn = 8;
        int a = ty->align > 8 ? 16 : 8;
        c->nsaa = align_up(c->nsaa, a);
        p->kind = P_STACK;
        p->stack = c->nsaa;
        p->stack_size = align_up(ty->size, 8);
        c->nsaa += p->stack_size;
        return;
    }
    if (is_memory_value(ty) && ty->size > 16) {
        /* by reference: a pointer, like an integer argument */
        p->by_reference = true;
        if (c->ngrn < 8) {
            p->kind = P_GP;
            p->reg = c->ngrn++;
            p->nregs = 1;
            return;
        }
        p->kind = P_STACK;
        p->stack = c->nsaa;
        p->stack_size = 8;
        c->nsaa += 8;
        return;
    }
    int n = is_memory_value(ty) ? (ty->size + 7) / 8 : 1;
    if (n == 0)
        n = 1;
    if (ty->align == 16 && (c->ngrn & 1))
        c->ngrn++;
    if (c->ngrn + n <= 8) {
        p->kind = P_GP;
        p->reg = c->ngrn;
        p->nregs = n;
        c->ngrn += n;
        return;
    }
    c->ngrn = 8;
    int a = ty->align > 8 ? 16 : 8;
    c->nsaa = align_up(c->nsaa, a);
    p->kind = P_STACK;
    p->stack = c->nsaa;
    p->stack_size = align_up(ty->size, 8);
    c->nsaa += p->stack_size;
}

static bool returns_in_memory(struct type *ty)
{
    struct type *base;
    int count;
    ty = unqual(ty);
    return is_memory_value(ty) && ty->size > 16 && !is_hfa(ty, &base, &count);
}

/* --- the frame ----------------------------------------------------------- */

static void assign_slots(int *top_io)
{
    int n = F->nvalues;
    /* (61a) where each value is live, and which are in registers */
    bool *folded_away = calloc((size_t)n + 1, sizeof(bool));
    for (int v = 1; v < n; v++)
        folded_away[v] = defs[v] == 1 && skip[def_at[v]];
    /* (62c) no value in a register over inline assembly */
    unsigned *barriers = calloc((size_t)F->ncode + 1, sizeof(unsigned));
    unsigned x2_x7 = ((1u << (VALUE_GPRS - SAVED_GPRS)) - 1) << SAVED_GPRS;
    for (int k = 0; k < F->ncode; k++) {
        struct ir *i = &F->code[k];
        if (i->op == IR_ASM)
            barriers[k] = 1u << 31;
        /* (61d) what overwrites x2-x7: calls (also the TLS descriptor's and
         * long double's helpers) and a return (x0-x7 hold its result) */
        else if (i->op == IR_CALL || i->op == IR_TLS_ADDR || i->op == IR_RET ||
                 (i->dst > 0 && cls(i->dst) == V_LD) || (i->a > 0 && i->a < F->nvalues && cls(i->a) == V_LD) ||
                 (i->b > 0 && i->b < F->nvalues && cls(i->b) == V_LD))
            barriers[k] = x2_x7;
    }
    regalloc_run(F, opt_level ? VALUE_GPRS : 0, opt_level ? 8 : 0, 8, 1u << V_F32 | 1u << V_F64, folded_away, barriers,
                 SAVED_GPRS, &ra);
    free(barriers);
    free(folded_away);
    int *first = ra.start, *last = ra.end;
    for (int v = 1; v < n; v++)
        if (ra.reg[v] >= 0) {
            if (cls(v) == V_I32 || cls(v) == V_I64) {
                if (ra.reg[v] < SAVED_GPRS)
                    gpr_used[ra.reg[v]] = true;
            } else
                fpr_used[ra.reg[v]] = true;
            first[v] = -1;                      /* (no slot) */
        }
    struct free_slot {
        int offset, size, busy_until;
    } *slots = malloc(sizeof(*slots) * 64);
    int cap = 64, count = 0;
    /* values by first use */
    int *start = calloc((size_t)F->ncode + 2, sizeof(int));
    for (int v = 1; v < n; v++)
        if (first[v] >= 0)
            start[first[v] + 1]++;
    for (int k = 0; k < F->ncode; k++)
        start[k + 1] += start[k];
    int *order = malloc(sizeof(int) * (size_t)n);
    int *fill = calloc((size_t)F->ncode + 1, sizeof(int));
    for (int v = 1; v < n; v++)
        if (first[v] >= 0)
            order[start[first[v]] + fill[first[v]]++] = v;
    int m = start[F->ncode];
    int top = *top_io;
    for (int j = 0; j < m; j++) {
        int v = order[j];
        int size = vsize(v) > 8 ? 16 : 8;
        int found = -1;
        for (int s = 0; s < count; s++)
            if (slots[s].size == size && slots[s].busy_until < first[v]) {
                found = s;
                break;
            }
        if (found < 0) {
            if (count == cap) {
                cap *= 2;
                slots = realloc(slots, sizeof(*slots) * (size_t)cap);
            }
            top = align_up(top, size);
            slots[count].offset = top;
            slots[count].size = size;
            top += size;
            found = count++;
        }
        slots[found].busy_until = last[v];
        slot_of[v] = slots[found].offset;
    }
    *top_io = top;
    free(slots);
    free(start);
    free(order);
    free(fill);
}

/* Offsets from x19 upwards: locals, save areas, copies, then values. */
static void layout_frame(struct obj *fn)
{
    int top = 0;
    sret_slot = -1;
    if (returns_in_memory(fn->type->return_type)) {
        sret_slot = top;
        top += 8;
    }
    if (fn->type->is_variadic) {
        top = align_up(top, 16);
        gr_area = top;
        top += 64;
        vr_area = top;
        top += 128;
    }
    struct counters c = { 0 };
    for (struct obj *p = fn->params; p; p = p->next) {
        struct place pl;
        place_of(p->type, &c, &pl);
        int size = align_up(p->type->size, 8);
        int a = p->align > 8 ? 16 : 8;
        top = align_up(top, a);
        p->offset = top;
        top += size;
    }
    va_gr = c.ngrn;
    va_vr = c.nsrn;
    va_stack = c.nsaa;
    top = place_locals(fn, top, false);
    /* room for va_arg's copies of HFAs */
    for (int k = 0; k < F->ncode; k++) {
        struct ir *i = &F->code[k];
        if (i->op == IR_VA_ARG && is_memory_value(i->type)) {
            top = align_up(top, 16);
            i->imm = top;
            top += align_up(i->type->size, 16);
        }
    }
    assign_slots(&top);
    frame_top = top;
    frame_size = align_up(top, 16);
}

/* after place_asm: the registers the function keeps for its caller */
static void place_saves(void)
{
    int top = align_up(frame_top, 8);
    save_at[17] = top;                          /* x19, the frame's base, always */
    top += 8;
    for (int k = 0; k < 9; k++)
        if (gpr_used[k]) {
            save_at[k] = top;
            top += 8;
        }
    for (int k = 0; k < 8; k++)
        if (fpr_used[k]) {
            save_at[9 + k] = top;
            top += 8;
        }
    frame_size = align_up(top, 16);
}

/* --- copying bytes ------------------------------------------------------- */

/* n bytes from [xs + so] to [xd + sd], with x12 as the scratch register. */
static void copy_bytes(int xd, int sd, int xs, int so, int n)
{
    if (n > 256) {
        int again = unique++;
        line("add x13, x%d, #%d", xs, so);
        line("add x14, x%d, #%d", xd, sd);
        load_imm(15, n / 8, 8);
        fprintf(out, ".Lc%d:\n", again);
        line("ldr x12, [x13], #8");
        line("str x12, [x14], #8");
        line("subs x15, x15, #1");
        line("b.ne .Lc%d", again);
        for (int k = n / 8 * 8; k < n; k++) {
            line("ldrb w12, [x13], #1");
            line("strb w12, [x14], #1");
        }
        return;
    }
    int at = 0;
    for (; n - at >= 8; at += 8) {
        line("ldr x12, [x%d, #%d]", xs, so + at);
        line("str x12, [x%d, #%d]", xd, sd + at);
    }
    if (n - at >= 4) {
        line("ldr w12, [x%d, #%d]", xs, so + at);
        line("str w12, [x%d, #%d]", xd, sd + at);
        at += 4;
    }
    if (n - at >= 2) {
        line("ldrh w12, [x%d, #%d]", xs, so + at);
        line("strh w12, [x%d, #%d]", xd, sd + at);
        at += 2;
    }
    if (n - at >= 1)
        line("ldrb w12, [x%d, #%d]", xs, so + at), line("strb w12, [x%d, #%d]", xd, sd + at);
}

/* n (1-8) bytes at [xs + off] into xN, not reading past them */
static void load_partial(int n_reg, int xs, int off, int n)
{
    switch (n) {
    case 8: line("ldr x%d, [x%d, #%d]", n_reg, xs, off); return;
    case 4: line("ldr w%d, [x%d, #%d]", n_reg, xs, off); return;
    case 2: line("ldrh w%d, [x%d, #%d]", n_reg, xs, off); return;
    case 1: line("ldrb w%d, [x%d, #%d]", n_reg, xs, off); return;
    }
    line("mov x%d, #0", n_reg);
    for (int i = n - 1; i >= 0; i--) {
        line("ldrb w12, [x%d, #%d]", xs, off + i);
        line("orr x%d, x12, x%d, lsl #8", n_reg, n_reg);
    }
}

static void store_partial(int xd, int off, int n_reg, int n)
{
    /* (xN is changed) */
    int at = 0;
    while (at < n) {
        if (n - at >= 8) {
            line("str x%d, [x%d, #%d]", n_reg, xd, off + at);
            at += 8;
        } else if (n - at >= 4) {
            line("str w%d, [x%d, #%d]", n_reg, xd, off + at);
            line("lsr x%d, x%d, #32", n_reg, n_reg);
            at += 4;
        } else if (n - at >= 2) {
            line("strh w%d, [x%d, #%d]", n_reg, xd, off + at);
            line("lsr x%d, x%d, #16", n_reg, n_reg);
            at += 2;
        } else {
            line("strb w%d, [x%d, #%d]", n_reg, xd, off + at);
            line("lsr x%d, x%d, #8", n_reg, n_reg);
            at += 1;
        }
    }
}

/* sp += n (n may be negative) */
static void add_sp(int n)
{
    int m = n < 0 ? -n : n;
    if (m < 4096)
        line("%s sp, sp, #%d", n < 0 ? "sub" : "add", m);
    else {
        load_imm(16, m, 8);
        line("%s sp, sp, x16", n < 0 ? "sub" : "add");
    }
}

/* --- long double through the library's helpers --------------------------- */

static void call_helper(const char *name)
{
    line("bl %s", name);
}

static void ld_binary(const char *helper, int d, int a, int b)
{
    fld(0, a);
    fld(1, b);
    call_helper(helper);
    fst(0, d);
}

/* --- instructions -------------------------------------------------------- */

/* --- folding (61b) ------------------------------------------------------- */

/* the instruction before or after k, over the copies coalescing removed */
static int before(int k)
{
    int j = k - 1;
    while (j >= 0 && F->code[j].op == IR_NOP)
        j--;
    return j;
}

static int after(int k)
{
    int j = k + 1;
    while (j < F->ncode && F->code[j].op == IR_NOP)
        j++;
    return j;
}

static struct ir *def_of(int v)
{
    return v > 0 && defs[v] == 1 ? &F->code[def_at[v]] : 0;
}

static bool int_cls(int v)
{
    return cls(v) == V_I32 || cls(v) == V_I64;
}

static bool constant(int v, int64_t *c)
{
    struct ir *d = def_of(v);
    if (!d || d->op != IR_IMM)
        return false;
    *c = cls(v) == V_I32 ? (int64_t)(int32_t)d->imm : d->imm;
    return true;
}

/* a logical instruction's immediate: a rotated run of ones, repeated */
static bool logical_imm(uint64_t v, int width)
{
    if (width == 32)
        v = (v & 0xFFFFFFFFull) | (v << 32);
    if (v == 0 || v == ~0ull)
        return false;
    int e = 64;
    while (e > 2 && (v & ((1ull << (e / 2)) - 1)) == ((v >> (e / 2)) & ((1ull << (e / 2)) - 1)))
        e /= 2;
    uint64_t mask = e == 64 ? ~0ull : (1ull << e) - 1, elem = v & mask;
    /* ones contiguous somewhere in the rotation: at most one 0->1 step */
    int rises = 0;
    for (int b = 0; b < e; b++)
        if (((elem >> b) & 1) && !((elem >> ((b + e - 1) % e)) & 1))
            rises++;
    return rises == 1;
}

/* a constant a cmp can take: 0-4095 */
static bool cmp_imm(int v, int64_t *c)
{
    return int_cls(v) && constant(v, c) && *c >= 0 && *c < 4096;
}

static bool add_imm_ok(int64_t c)
{
    return c > -4096 && c < 4096;
}

/* b's address plus c said in the operand itself: a frame object
 * ([x19, #off]), or this file's global as [x16, :lo12:sym+off], whose
 * scaled offset needs the address aligned for the access (as emit.c places
 * the object) */
static bool fold_base(struct ir *b, int64_t c, int size)
{
    if (!b || c <= -(1 << 20) || c >= (1 << 20))
        return false;
    if (b->op == IR_LOCAL)
        return true;
    if (b->op != IR_GLOBAL || !symbol_is_local(b->obj) || b->imm <= -(1 << 20) || b->imm >= (1 << 20))
        return false;
    int align = b->obj->align > b->obj->type->align ? b->obj->align : b->obj->type->align;
    return size == 1 || (align % size == 0 && (b->imm + c) % size == 0);
}

/* An address base + index said as [xB, xI, lsl #s] or [xB, wI, sxtw #s]:
 * the addition right before k, its index scaled by the access size
 * (a shift or multiplication right before that) and widened from a 32-bit
 * int (right before that), each used only there. Nothing else is set
 * between them, so the registers of base and index are still theirs. */
struct index_fold {
    int base, index;
    bool sxtw, scaled;
    int scale_at, widen_at;                     /* (-1: none) */
};

static int log2_of(int n)
{
    int k = 0;
    while ((1 << k) < n)
        k++;
    return k;
}

static bool index_folds(int k, int v, int size, struct index_fold *f)
{
    struct ir *d = def_of(v);
    int64_t c;
    if (!d || d->op != IR_BIN || d->sub != OP_ADD || cls(v) != V_I64 || def_at[v] != before(k) || uses[v] != 1 ||
        constant(d->b, &c) || d->b <= 0 || d->a <= 0 || cls(d->a) != V_I64 || cls(d->b) != V_I64)
        return false;
    struct index_fold g = { d->a, d->b, false, false, -1, -1 };
    int at = before(before(k));
    struct ir *m = def_of(g.index);
    if (size > 1 && m && m->op == IR_BIN && def_at[g.index] == at && uses[g.index] == 1 && constant(m->b, &c) &&
        ((m->sub == OP_SHL && c == log2_of(size)) || (m->sub == OP_MUL && c == size))) {
        g.scaled = true;
        g.scale_at = at;
        at = before(at);
        /* (the scale's own constant may come in between: it folds away) */
        if (at >= 0 && F->code[at].op == IR_IMM && F->code[at].dst == m->b && uses[m->b] == 1)
            at = before(at);
        g.index = m->a;
    }
    struct ir *w = def_of(g.index);
    if (w && w->op == IR_CONV && w->sub == CONV_SEXT && w->size == 4 && def_at[g.index] == at && uses[g.index] == 1) {
        g.sxtw = true;
        g.widen_at = at;
        g.index = w->a;
    }
    if (g.index <= 0 || !int_cls(g.index) || (!g.sxtw && cls(g.index) != V_I64))
        return false;
    if (f)
        *f = g;
    return true;
}

/* an address the instruction at k (size bytes) can say in its operand: a
 * frame or global address, or an addition of a constant right before k
 * (its base's register is not reused before k: nothing else is set there) */
static bool address_folds(int k, int v, int size)
{
    struct ir *d = def_of(v);
    if (!d)
        return false;
    if (fold_base(d, 0, size))
        return true;
    int64_t c;
    if (index_folds(k, v, size, 0))
        return true;
    /* (40a) only as its one use: an addition also computed for another may
     * have taken its base's register */
    if (d->op != IR_BIN || d->sub != OP_ADD || cls(v) != V_I64 || def_at[v] != before(k) || uses[v] != 1 ||
        !constant(d->b, &c))
        return false;
    return fold_base(def_of(d->a), c, size) || (c >= 0 && c % size == 0 && c / size < 4096) ||
           (c >= -256 && c < 256);
}

static bool fused(int k)
{
    struct ir *b = &F->code[k];
    if (b->op != IR_BR || k == 0)
        return false;
    struct ir *d = def_of(b->a);
    return d && d->op == IR_CMP && def_at[b->a] == before(k) && uses[b->a] == 1;
}

static bool add_folds_next(int k)
{
    struct ir *i = &F->code[k];
    if (i->op != IR_BIN || i->sub != OP_ADD || after(k) >= F->ncode || uses[i->dst] != 1)
        return false;
    struct ir *n = &F->code[after(k)];
    return (n->op == IR_LOAD || n->op == IR_STORE) && n->a == i->dst && address_folds(after(k), i->dst, n->size);
}

static bool use_folds(int k, int which, int v)
{
    struct ir *i = &F->code[k];
    int64_t c;
    if (defs[v] == 1 && chained[def_at[v]])
        return true;                            /* (a scaled or widened index: its only use) */
    switch (i->op) {
    case IR_LOAD:
        return which == 0 && address_folds(k, v, i->size);
    case IR_STORE:
        if (which == 0)
            return address_folds(k, v, i->size);
        return which == 1 && i->size <= 8 && int_cls(v) && constant(v, &c) && c == 0;
    case IR_BIN:
        if (!int_cls(i->dst))
            return false;
        if (which == 1 && constant(v, &c))
            switch (i->sub) {
            case OP_ADD: case OP_SUB: return add_imm_ok(c);
            case OP_AND: case OP_OR: case OP_XOR: return logical_imm((uint64_t)c, vsize(i->dst) == 8 ? 64 : 32);
            case OP_SHL: case OP_SHR: case OP_SAR: return true;
            case OP_MUL: return c > 0 && (c & (c - 1)) == 0;
            }
        return which == 0 && add_folds_next(k) && constant(i->b, &c) && fold_base(def_of(v), c, F->code[after(k)].size);
    case IR_CMP:
        if (which == 0)
            return i->sub <= OP_ULE && int_cls(v) && cmp_imm(v, &c) && !cmp_imm(i->b, &c);
        return which == 1 && i->sub <= OP_ULE && int_cls(v) && cmp_imm(v, &c);
    case IR_BR:
        return fused(k);
    default:
        return false;
    }
}

static void analyze(void)
{
    int nv = F->nvalues, nc = F->ncode;
    defs = calloc((size_t)nv, sizeof(int));
    def_at = calloc((size_t)nv, sizeof(int));
    uses = calloc((size_t)nv, sizeof(int));
    nonfolded = calloc((size_t)nv, sizeof(int));
    skip = calloc((size_t)nc + 1, sizeof(bool));
    chained = calloc((size_t)nc + 1, sizeof(bool));
    if (!opt_level)
        return;
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        if (i->dst) {
            defs[i->dst]++;
            def_at[i->dst] = k;
        }
        int ops[3] = { i->a, i->b, i->c };
        for (int j = 0; j < 3; j++)
            if (ops[j] > 0)
                uses[ops[j]]++;
        if (i->call) {
            for (int j = 0; j < i->call->nargs; j++)
                uses[i->call->args[j].value]++;
            if (i->call->ret_buffer > 0)
                uses[i->call->ret_buffer]++;
        }
    }
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        struct index_fold f;
        if ((i->op == IR_LOAD || i->op == IR_STORE) && index_folds(k, i->a, i->op == IR_LOAD && cls(i->dst) == V_LD ? 16 : i->size, &f)) {
            if (f.scale_at >= 0)
                chained[f.scale_at] = true;
            if (f.widen_at >= 0)
                chained[f.widen_at] = true;
        }
    }
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        int ops[3] = { i->a, i->b, i->c };
        for (int j = 0; j < 3; j++)
            if (ops[j] > 0 && !use_folds(k, j, ops[j]))
                nonfolded[ops[j]]++;
        if (i->call) {
            for (int j = 0; j < i->call->nargs; j++)
                nonfolded[i->call->args[j].value]++;
            if (i->call->ret_buffer > 0)
                nonfolded[i->call->ret_buffer]++;
        }
    }
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        if (!i->dst || defs[i->dst] != 1 || nonfolded[i->dst])
            continue;
        if (chained[k] || i->op == IR_IMM || i->op == IR_LOCAL || (i->op == IR_GLOBAL && symbol_is_local(i->obj)) ||
            i->op == IR_CMP || (i->op == IR_BIN && i->sub == OP_ADD && add_folds_next(k)))
            skip[k] = true;
    }
}

/* the memory operand for *(v + extra), an access of size bytes at k
 * (x10 and x16 may be taken for it) */
static const char *address(int v, int64_t extra, int k, int size)
{
    static char buf[4][160];
    static int n;
    char *b = buf[n++ & 3];
    struct ir *d = def_of(v);
    if (d && opt_level && address_folds(k, v, size)) {
        if (d->op == IR_LOCAL)
            return mem((int)(d->obj->offset + extra), size);
        if (d->op == IR_GLOBAL) {
            /* (the page of the address itself: an offset may cross pages) */
            line("adrp x16, %s%+lld", d->sym, (long long)(d->imm + extra));
            snprintf(b, 160, "[x16, :lo12:%s%+lld]", d->sym, (long long)(d->imm + extra));
            return b;
        }
        int64_t c;
        struct index_fold f;
        if (index_folds(k, v, size, &f)) {
            const char *B = src(10, f.base, 8);
            const char *I = src(11, f.index, f.sxtw ? 4 : 8);
            if (f.scaled)
                snprintf(b, 160, "[%s, %s, %s #%d]", B, I, f.sxtw ? "sxtw" : "lsl", log2_of(size));
            else if (f.sxtw)
                snprintf(b, 160, "[%s, %s, sxtw]", B, I);
            else
                snprintf(b, 160, "[%s, %s]", B, I);
            return b;
        }
        constant(d->b, &c);
        struct ir *base = def_of(d->a);
        if (fold_base(base, c, size)) {
            if (base->op == IR_LOCAL)
                return mem((int)(base->obj->offset + extra + c), size);
            line("adrp x16, %s%+lld", base->sym, (long long)(base->imm + extra + c));
            snprintf(b, 160, "[x16, :lo12:%s%+lld]", base->sym, (long long)(base->imm + extra + c));
            return b;
        }
        v = d->a;
        extra += c;
    }
    const char *base = in_gpr(v) ? r(xreg(ra.reg[v]), 8) : (ld_bits(10, v, 8), "x10");
    if ((extra >= 0 && extra % size == 0 && extra / size < 4096) || (extra >= -256 && extra < 256))
        snprintf(b, 160, "[%s, #%lld]", base, (long long)extra);
    else {
        load_imm(16, extra, 8);
        line("add x16, %s, x16", base);
        snprintf(b, 160, "[x16]");
    }
    return b;
}

static const char *inverse_cond(const char *c)
{
    static const char *pairs[][2] = { { "eq", "ne" }, { "lt", "ge" }, { "le", "gt" }, { "lo", "hs" },
                                      { "ls", "hi" }, { "mi", "pl" } };
    for (size_t k = 0; k < sizeof(pairs) / sizeof(pairs[0]); k++) {
        if (strcmp(c, pairs[k][0]) == 0)
            return pairs[k][1];
        if (strcmp(c, pairs[k][1]) == 0)
            return pairs[k][0];
    }
    return c;
}

static void call_helper(const char *name);

/* the flags of a comparison, and the condition that means true */
static const char *compare_flags(struct ir *i)
{
    int a = i->a, b = i->b;
    if (i->sub >= OP_FEQ && cls(a) == V_LD) {
        static const char *helpers[] = { [OP_FEQ] = "__eqtf2", [OP_FNE] = "__netf2",
                                         [OP_FLT] = "__lttf2", [OP_FLE] = "__letf2" };
        static const char *cc[] = { [OP_FEQ] = "eq", [OP_FNE] = "ne", [OP_FLT] = "lt", [OP_FLE] = "le" };
        fld(0, a);
        fld(1, b);
        call_helper(helpers[i->sub]);
        line("cmp w0, #0");
        return cc[i->sub];
    }
    if (i->sub >= OP_FEQ) {
        char k = fpr(a);
        const char *fa = in_fpr(a) ? 0 : "16", *fb = in_fpr(b) ? 0 : "17";
        char ra_[8], rb_[8];
        if (fa)
            fld(16, a);
        if (fb)
            fld(17, b);
        snprintf(ra_, sizeof(ra_), "%c%d", k, fa ? 16 : 8 + ra.reg[a]);
        snprintf(rb_, sizeof(rb_), "%c%d", k, fb ? 17 : 8 + ra.reg[b]);
        line("fcmp %s, %s", ra_, rb_);
        /* eq and mi and ls are false when unordered, ne true */
        static const char *cc[] = { [OP_FEQ] = "eq", [OP_FNE] = "ne", [OP_FLT] = "mi", [OP_FLE] = "ls" };
        return cc[i->sub];
    }
    int s = vsize(a) == 8 ? 8 : 4;
    int64_t c;
    static const char *cc[] = { [OP_EQ] = "eq", [OP_NE] = "ne", [OP_SLT] = "lt", [OP_SLE] = "le",
                                [OP_ULT] = "lo", [OP_ULE] = "ls" };
    /* (61c) a constant first: compared the other way round */
    static const char *swapped[] = { [OP_EQ] = "eq", [OP_NE] = "ne", [OP_SLT] = "gt", [OP_SLE] = "ge",
                                     [OP_ULT] = "hi", [OP_ULE] = "hs" };
    if (cmp_imm(a, &c) && !cmp_imm(b, &c)) {
        cmp_imm(a, &c);
        line("cmp %s, #%lld", src(10, b, s), (long long)c);
        return swapped[i->sub];
    }
    const char *A = src(9, a, s);
    if (cmp_imm(b, &c))
        line("cmp %s, #%lld", A, (long long)c);
    else
        line("cmp %s, %s", A, src(10, b, s));
    return cc[i->sub];
}

static void gen_bin(struct ir *i)
{
    int d = i->dst, a = i->a, b = i->b;
    if (cls(d) == V_LD) {
        static const char *helpers[] = { [OP_FADD] = "__addtf3", [OP_FSUB] = "__subtf3",
                                         [OP_FMUL] = "__multf3", [OP_FDIV] = "__divtf3" };
        ld_binary(helpers[i->sub], d, a, b);
        return;
    }
    if (cls(d) == V_F32 || cls(d) == V_F64) {
        const char *A = fsrc(16, a), *B = fsrc(17, b);
        static const char *names[] = { [OP_FADD] = "fadd", [OP_FSUB] = "fsub", [OP_FMUL] = "fmul", [OP_FDIV] = "fdiv" };
        line("%s %s, %s, %s", names[i->sub], fdst(16, d), A, B);
        fdone(16, d);
        return;
    }
    int s = vsize(d) == 8 ? 8 : 4;
    const char *A = src(9, a, s);
    const char *D = dstr(9, d, s);
    int64_t c;
    static const char *names[] = { [OP_ADD] = "add", [OP_SUB] = "sub", [OP_MUL] = "mul", [OP_SDIV] = "sdiv",
                                   [OP_UDIV] = "udiv", [OP_AND] = "and", [OP_OR] = "orr", [OP_XOR] = "eor",
                                   [OP_SHL] = "lsl", [OP_SHR] = "lsr", [OP_SAR] = "asr" };
    if (opt_level && constant(b, &c) && use_folds((int)(i - F->code), 1, b)) {
        switch (i->sub) {
        case OP_ADD: case OP_SUB: {
            bool sub = (i->sub == OP_SUB) != (c < 0);
            line("%s %s, %s, #%lld", sub ? "sub" : "add", D, A, (long long)(c < 0 ? -c : c));
            break;
        }
        case OP_AND: case OP_OR: case OP_XOR:
            line("%s %s, %s, #%lld", names[i->sub], D, A, (long long)(s == 4 ? (int64_t)(uint32_t)c : c));
            break;
        case OP_SHL: case OP_SHR: case OP_SAR:
            line("%s %s, %s, #%d", names[i->sub], D, A, (int)(c & (s == 8 ? 63 : 31)));
            break;
        case OP_MUL: {
            int sh = 0;
            while ((1ll << sh) < c)
                sh++;
            line("lsl %s, %s, #%d", D, A, sh);
            break;
        }
        }
        done(9, d);
        return;
    }
    const char *B = src(10, b, s);
    if (i->sub == OP_UMULH) {
        line("umulh %s, %s, %s", D, A, B);
        done(9, d);
        return;
    }
    switch (i->sub) {
    case OP_SMOD:
        line("sdiv %s, %s, %s", r(11, s), A, B);
        line("msub %s, %s, %s, %s", D, r(11, s), B, A);
        break;
    case OP_UMOD:
        line("udiv %s, %s, %s", r(11, s), A, B);
        line("msub %s, %s, %s, %s", D, r(11, s), B, A);
        break;
    default:
        line("%s %s, %s, %s", names[i->sub], D, A, B);
    }
    done(9, d);
}

static void gen_cmp(struct ir *i)
{
    const char *cc = compare_flags(i);
    const char *D = dstr(9, i->dst, 4);
    line("cset %s, %s", D, cc);
    done(9, i->dst);
}

static void gen_conv(struct ir *i)
{
    int d = i->dst, a = i->a;
    int ds = vsize(d) == 8 ? 8 : 4;
    switch (i->sub) {
    case CONV_SEXT: {
        const char *A = src(9, a, 4), *D = dstr(9, d, ds);
        switch (i->size) {
        case 1: line("sxtb %s, %s", D, A); break;
        case 2: line("sxth %s, %s", D, A); break;
        default:
            if (ds == 8)
                line("sxtw %s, %s", D, A);
            else if (strcmp(D, A) != 0)
                line("mov %s, %s", D, A);
        }
        done(9, d);
        return;
    }
    case CONV_ZEXT: {
        const char *A = src(9, a, 4), *W = dstr(9, d, 4);
        switch (i->size) {
        case 1: line("and %s, %s, #0xff", W, A); break;
        case 2: line("and %s, %s, #0xffff", W, A); break;
        default: line("mov %s, %s", W, A);
        }
        done(9, d);
        return;
    }
    case CONV_TRUNC: {
        const char *A = src(9, a, 4), *W = dstr(9, d, 4);
        line("mov %s, %s", W, A);
        done(9, d);
        return;
    }
    case CONV_S2F: case CONV_U2F:
        if (cls(d) == V_LD) {
            ld_bits(0, a, i->size);
            call_helper(i->sub == CONV_S2F ? (i->size == 8 ? "__floatditf" : "__floatsitf")
                                           : (i->size == 8 ? "__floatunditf" : "__floatunsitf"));
            fst(0, d);
            return;
        }
        {
            const char *A = src(9, a, i->size);
            line("%s %s, %s", i->sub == CONV_S2F ? "scvtf" : "ucvtf", fdst(16, d), A);
            fdone(16, d);
        }
        return;
    case CONV_F2S: case CONV_F2U:
        if (cls(a) == V_LD) {
            fld(0, a);
            call_helper(i->sub == CONV_F2S ? (ds == 8 ? "__fixtfdi" : "__fixtfsi")
                                           : (ds == 8 ? "__fixunstfdi" : "__fixunstfsi"));
            st(0, d);
            return;
        }
        {
            const char *A = fsrc(16, a);
            line("%s %s, %s", i->sub == CONV_F2S ? "fcvtzs" : "fcvtzu", dstr(9, d, ds), A);
            done(9, d);
        }
        return;
    case CONV_F2F:
        if (cls(d) == V_LD) {
            fld(0, a);
            call_helper(cls(a) == V_F32 ? "__extendsftf2" : "__extenddftf2");
            fst(0, d);
            return;
        }
        if (cls(a) == V_LD) {
            fld(0, a);
            call_helper(cls(d) == V_F32 ? "__trunctfsf2" : "__trunctfdf2");
            fst(0, d);
            return;
        }
        {
            const char *A = fsrc(16, a);
            line("fcvt %s, %s", fdst(16, d), A);
            fdone(16, d);
        }
        return;
    }
}

static int hfa_member_size(struct type *base)
{
    return base->size;
}

static char hfa_reg(struct type *base)
{
    return base->size == 4 ? 's' : base->size == 8 ? 'd' : 'q';
}

static void gen_call(struct ir *i)
{
    struct ir_call *c = i->call;
    struct type *rt = unqual(i->type);
    struct place *places = arena_alloc(sizeof(struct place) * (size_t)(c->nargs + 1));
    struct counters cnt = { 0 };
    for (int k = 0; k < c->nargs; k++)
        place_of(c->args[k].type, &cnt, &places[k]);
    /* (unprototyped and variadic calls pass arguments as fixed ones do:
     * AAPCS64, unlike Apple's variant) */
    /* sp moves once: the arguments in memory at [sp, area), and above them
     * the copies of structures passed by reference */
    int area = align_up(cnt.nsaa, 16), copies = 0;
    for (int k = 0; k < c->nargs; k++)
        if (places[k].by_reference) {
            places[k].copy = area + copies;
            copies += align_up(c->args[k].type->size, 16);
        }
    int total = area + copies;
    if (total)
        add_sp(-total);
    for (int k = 0; k < c->nargs; k++) {
        struct place *p = &places[k];
        struct type *ty = c->args[k].type;
        int v = c->args[k].value;
        if (p->by_reference) {
            ld_addr(9, v);
            line("mov x11, sp");
            copy_bytes(11, p->copy, 9, 0, ty->size);
            if (p->kind == P_STACK) {
                line("add x14, sp, #%d", p->copy);
                line("str x14, [sp, #%d]", p->stack);
            }
            continue;
        }
        if (p->kind != P_STACK)
            continue;
        if (is_memory_value(ty)) {
            ld_addr(9, v);
            line("mov x11, sp");
            copy_bytes(11, p->stack, 9, 0, ty->size);
        } else if (cls(v) == V_LD) {
            fld(16, v);
            line("str q16, [sp, #%d]", p->stack);
        } else if (cls(v) == V_F32 || cls(v) == V_F64) {
            fld(16, v);
            line("str %c16, [sp, #%d]", fpr(v), p->stack);
        } else {
            ld(9, v);
            line("str %s, [sp, #%d]", r(9, vsize(v) == 8 ? 8 : 4), p->stack);
        }
    }
    /* registers */
    for (int k = 0; k < c->nargs; k++) {
        struct place *p = &places[k];
        struct type *ty = c->args[k].type;
        int v = c->args[k].value;
        if (p->kind == P_STACK)
            continue;
        if (p->by_reference) {
            line("add x%d, sp, #%d", p->reg, p->copy);
            continue;
        }
        if (p->kind == P_FP) {
            fld(p->reg, v);
            continue;
        }
        if (p->kind == P_HFA) {
            ld_addr(9, v);
            int ms = hfa_member_size(p->hfa_base);
            for (int j = 0; j < p->nregs; j++)
                line("ldr %c%d, [x9, #%d]", hfa_reg(p->hfa_base), p->reg + j, j * ms);
            continue;
        }
        if (is_memory_value(ty)) {
            ld_addr(9, v);
            for (int j = 0; j < p->nregs; j++) {
                int n = ty->size - 8 * j < 8 ? ty->size - 8 * j : 8;
                load_partial(p->reg + j, 9, 8 * j, n);
            }
            continue;
        }
        ld(p->reg, v);
    }
    if (returns_in_memory(rt))
        ld_addr(8, c->ret_buffer);
    if (i->sym) {
        line("bl %s", i->sym);
    } else {
        ld_addr(9, i->a);
        line("blr x9");
    }
    if (total)
        add_sp(total);
    /* the result */
    if (is_memory_value(rt)) {
        if (returns_in_memory(rt))
            return;
        struct type *base;
        int count;
        ld_addr(9, c->ret_buffer);
        if (is_hfa(rt, &base, &count)) {
            for (int j = 0; j < count; j++)
                line("str %c%d, [x9, #%d]", hfa_reg(base), j, j * base->size);
            return;
        }
        int n = (rt->size + 7) / 8;
        for (int j = 0; j < n; j++) {
            int size = rt->size - 8 * j < 8 ? rt->size - 8 * j : 8;
            if (size == 8)
                line("str x%d, [x9, #%d]", j, 8 * j);
            else
                store_partial(9, 8 * j, j, size);
        }
        return;
    }
    if (!i->dst)
        return;
    switch (cls(i->dst)) {
    case V_F32: case V_F64: case V_LD: fst(0, i->dst); break;
    default: st(0, i->dst);
    }
}

static void epilogue(void)
{
    for (int k = 0; k < 9; k++)
        if (gpr_used[k])
            line("ldr x%d, %s", 20 + k, mem(save_at[k], 8));
    for (int k = 0; k < 8; k++)
        if (fpr_used[k])
            line("ldr d%d, %s", 8 + k, mem(save_at[9 + k], 8));
    line("ldr x19, %s", mem(save_at[17], 8));
    line("mov sp, x29");
    line("ldp x29, x30, [sp], #16");
    line("ret");
}

static void gen_ret(struct ir *i)
{
    struct type *rt = unqual(i->type);
    if (i->a) {
        if (is_memory_value(rt)) {
            ld_addr(9, i->a);
            struct type *base;
            int count;
            if (returns_in_memory(rt)) {
                line("ldr x10, %s", mem(sret_slot, 8));
                copy_bytes(10, 0, 9, 0, rt->size);
            } else if (is_hfa(rt, &base, &count)) {
                for (int j = 0; j < count; j++)
                    line("ldr %c%d, [x9, #%d]", hfa_reg(base), j, j * base->size);
            } else {
                int n = (rt->size + 7) / 8;
                for (int j = 0; j < n; j++) {
                    int size = rt->size - 8 * j < 8 ? rt->size - 8 * j : 8;
                    load_partial(j, 9, 8 * j, size);
                }
            }
        } else
            switch (cls(i->a)) {
            case V_F32: case V_F64: case V_LD: fld(0, i->a); break;
            default: ld(0, i->a);
            }
    }
    epilogue();
}

static void gen_va_start(struct ir *i)
{
    ld_addr(9, i->a);
    line("add x10, x29, #%d", FRAME_HEADER + va_stack);
    line("str x10, [x9]");                      /* __stack */
    line("add x10, x19, #%d", gr_area + 64);
    line("str x10, [x9, #8]");                  /* __gr_top */
    line("add x10, x19, #%d", vr_area + 128);
    line("str x10, [x9, #16]");                 /* __vr_top */
    load_imm(10, -(8 - va_gr) * 8, 4);
    line("str w10, [x9, #24]");                 /* __gr_offs */
    load_imm(10, -(8 - va_vr) * 16, 4);
    line("str w10, [x9, #28]");                 /* __vr_offs */
}

/* x10 = the next stack argument's address (size, alignment), __stack moved on */
static void va_stack_arg(int size, int align)
{
    line("ldr x10, [x9]");
    if (align > 8) {
        line("add x10, x10, #15");
        line("and x10, x10, #-16");
    }
    line("add x11, x10, #%d", align_up(size, 8));
    line("str x11, [x9]");
}

static void gen_va_arg(struct ir *i)
{
    struct type *ty = unqual(i->type);
    ld_addr(9, i->a);
    int stack = unique++, done = unique++;
    struct type *base;
    int count;
    if (is_flonum(ty) || is_hfa(ty, &base, &count)) {
        int n = is_flonum(ty) ? 1 : count;
        struct type *mt = is_flonum(ty) ? ty : base;
        line("ldrsw x11, [x9, #28]");           /* __vr_offs */
        line("cmp x11, #0");
        line("b.ge .Lc%d", stack);
        line("add x12, x11, #%d", 16 * n);
        line("str w12, [x9, #28]");
        line("cmp x12, #0");
        line("b.gt .Lc%d", stack);
        line("ldr x10, [x9, #16]");
        line("add x10, x10, x11");              /* the first register's place */
        if (n > 1 || !is_flonum(ty)) {
            /* each member had a 16-byte register: gather them */
            int room = (int)i->imm;
            line("add x13, x19, #%d", room);
            for (int j = 0; j < n; j++) {
                line("ldr %c16, [x10, #%d]", hfa_reg(mt), 16 * j);
                line("str %c16, [x13, #%d]", hfa_reg(mt), j * mt->size);
            }
            line("mov x10, x13");
        }
        line("b .Lc%d", done);
        fprintf(out, ".Lc%d:\n", stack);
        va_stack_arg(is_flonum(ty) ? ty->size : ty->size, ty->align);
        fprintf(out, ".Lc%d:\n", done);
        st_bits(10, i->dst, 8);
        return;
    }
    bool by_ref = is_memory_value(ty) && ty->size > 16;
    int nregs = by_ref ? 1 : is_memory_value(ty) ? (ty->size + 7) / 8 : 1;
    line("ldrsw x11, [x9, #24]");               /* __gr_offs */
    line("cmp x11, #0");
    line("b.ge .Lc%d", stack);
    if (!by_ref && ty->align == 16) {
        line("add x11, x11, #15");
        line("and x11, x11, #-16");
    }
    line("add x12, x11, #%d", 8 * nregs);
    line("str w12, [x9, #24]");
    line("cmp x12, #0");
    line("b.gt .Lc%d", stack);
    line("ldr x10, [x9, #8]");
    line("add x10, x10, x11");
    line("b .Lc%d", done);
    fprintf(out, ".Lc%d:\n", stack);
    va_stack_arg(by_ref ? 8 : ty->size, by_ref ? 8 : ty->align);
    fprintf(out, ".Lc%d:\n", done);
    if (by_ref)
        line("ldr x10, [x10]");                 /* (the caller's copy) */
    st_bits(10, i->dst, 8);
}

static const char *sz_suffix(int size)
{
    return size == 1 ? "b" : size == 2 ? "h" : "";
}

static void gen_atomic(struct ir *i)
{
    int size = i->size, s = size == 8 ? 8 : 4;
    const char *k = sz_suffix(size);
    if (i->sub == AT_FENCE) {
        line("dmb ish");
        return;
    }
    ld_addr(10, i->a);
    if (i->b && i->sub != AT_CAS)
        ld(11, i->b);
    int again = unique++;
    switch (i->sub) {
    case AT_LOAD:
        line("ldar%s %s, [x10]", k, r(9, s));
        break;
    case AT_STORE:
        line("stlr%s %s, [x10]", k, r(11, s));
        line("dmb ish");
        return;
    case AT_XCHG:
        fprintf(out, ".Lc%d:\n", again);
        line("ldaxr%s %s, [x10]", k, r(9, s));
        line("stlxr%s w12, %s, [x10]", k, r(11, s));
        line("cbnz w12, .Lc%d", again);
        break;
    case AT_ADD: case AT_SUB: case AT_AND: case AT_OR: case AT_XOR: {
        static const char *names[] = { [AT_ADD] = "add", [AT_SUB] = "sub", [AT_AND] = "and", [AT_OR] = "orr", [AT_XOR] = "eor" };
        fprintf(out, ".Lc%d:\n", again);
        line("ldaxr%s %s, [x10]", k, r(9, s));
        line("%s %s, %s, %s", names[i->sub], r(13, s), r(9, s), r(11, s));
        line("stlxr%s w12, %s, [x10]", k, r(13, s));
        line("cbnz w12, .Lc%d", again);
        if (i->fetch_new)
            line("mov %s, %s", r(9, s), r(13, s));
        break;
    }
    case AT_CAS: {
        int fail = unique++, done = unique++;
        ld_addr(15, i->b);
        line("ldr%s %s, [x15]", k, r(14, s));
        ld(11, i->c);
        fprintf(out, ".Lc%d:\n", again);
        line("ldaxr%s %s, [x10]", k, r(9, s));
        line("cmp %s, %s", r(9, s), r(14, s));
        line("b.ne .Lc%d", fail);
        line("stlxr%s w12, %s, [x10]", k, r(11, s));
        line("cbnz w12, .Lc%d", again);
        line("mov w9, #1");
        line("b .Lc%d", done);
        fprintf(out, ".Lc%d:\n", fail);
        line("clrex");
        line("str%s %s, [x15]", k, r(9, s));
        line("mov w9, #0");
        fprintf(out, ".Lc%d:\n", done);
        st(9, i->dst);
        return;
    }
    }
    if (i->dst)
        st(9, i->dst);
}

static void gen_builtin(struct ir *i)
{
    int s = i->size == 8 ? 8 : 4;
    if (i->sub == BI_SQRT || i->sub == BI_FABS) {
        const char *A = fsrc(16, i->a);
        line("%s %s, %s", i->sub == BI_SQRT ? "fsqrt" : "fabs", fdst(16, i->dst), A);
        fdone(16, i->dst);
        return;
    }
    switch (i->sub) {
    case BI_CLZ:
        ld_bits(9, i->a, s);
        line("clz %s, %s", r(9, s), r(9, s));
        break;
    case BI_CTZ:
        ld_bits(9, i->a, s);
        line("rbit %s, %s", r(9, s), r(9, s));
        line("clz %s, %s", r(9, s), r(9, s));
        break;
    case BI_POPCOUNT:
        ld_bits(9, i->a, s);
        line("lsr x10, x9, #1");
        line("and x10, x10, #0x5555555555555555");
        line("sub x9, x9, x10");
        line("and x10, x9, #0x3333333333333333");
        line("lsr x9, x9, #2");
        line("and x9, x9, #0x3333333333333333");
        line("add x9, x9, x10");
        line("add x9, x9, x9, lsr #4");
        line("and x9, x9, #0x0f0f0f0f0f0f0f0f");
        line("mov x10, #0x0101010101010101");
        line("mul x9, x9, x10");
        line("lsr x9, x9, #56");
        break;
    case BI_BSWAP16:
        ld(9, i->a);
        line("rev16 w9, w9");
        line("and w9, w9, #0xffff");
        break;
    case BI_BSWAP32:
        ld(9, i->a);
        line("rev w9, w9");
        break;
    case BI_BSWAP64:
        ld(9, i->a);
        line("rev x9, x9");
        break;
    }
    st(9, i->dst);
}

static void long_double_bytes(double v, uint64_t *lo, uint64_t *hi)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    uint64_t sign = bits >> 63;
    int exp = (int)((bits >> 52) & 0x7FF);
    uint64_t frac = bits & 0xFFFFFFFFFFFFFull;
    int e;
    if (exp == 0 && frac == 0)
        e = 0;
    else if (exp == 0x7FF)
        e = 0x7FFF;
    else if (exp == 0) {
        int shift = 0;
        while (!(frac & (1ull << 52))) {
            frac <<= 1;
            shift++;
        }
        frac &= ~(1ull << 52);
        e = 1 - 1023 - shift + 16383;
    } else
        e = exp - 1023 + 16383;
    *lo = frac << 60;
    *hi = (frac >> 4) | ((uint64_t)e << 48) | (sign << 63);
}

static void gen_asm(int k);

static void gen_insn(int k)
{
    struct ir *i = &F->code[k], *next = after(k) < F->ncode ? &F->code[after(k)] : 0;
    if (skip[k])
        return;
    switch (i->op) {
    case IR_LABEL:
        fprintf(out, "%s:\n", label_name(i->label));
        return;
    case IR_JMP:
        if (next && next->op == IR_LABEL && next->label == i->label)
            return;
        line("b %s", label_name(i->label));
        return;
    case IR_BR:
        if (fused(k)) {
            const char *cc = compare_flags(def_of(i->a));
            if (next && next->op == IR_LABEL && next->label == i->label) {
                line("b.%s %s", inverse_cond(cc), label_name(i->label2));
                return;
            }
            line("b.%s %s", cc, label_name(i->label));
            if (!(next && next->op == IR_LABEL && next->label == i->label2))
                line("b %s", label_name(i->label2));
            return;
        }
        {
            /* (61c) the value's own register when it has one */
            const char *A = src(9, i->a, vsize(i->a) == 8 ? 8 : 4);
            if (next && next->op == IR_LABEL && next->label == i->label) {
                line("cbz %s, %s", A, label_name(i->label2));
                return;
            }
            line("cbnz %s, %s", A, label_name(i->label));
        }
        if (!(next && next->op == IR_LABEL && next->label == i->label2))
            line("b %s", label_name(i->label2));
        return;
    case IR_IMM: {
        int s = vsize(i->dst) == 8 ? 8 : 4;
        load_imm(in_gpr(i->dst) ? xreg(ra.reg[i->dst]) : 9, i->imm, s);
        done(9, i->dst);
        return;
    }
    case IR_FIMM:
        if (cls(i->dst) == V_F32) {
            float f = (float)i->fimm;
            uint32_t bits;
            memcpy(&bits, &f, 4);
            load_imm(9, bits, 4);
            st_bits(9, i->dst, 4);
        } else if (cls(i->dst) == V_F64) {
            uint64_t bits;
            memcpy(&bits, &i->fimm, 8);
            load_imm(9, (int64_t)bits, 8);
            st_bits(9, i->dst, 8);
        } else {
            uint64_t lo, hi;
            long_double_bytes(i->fimm, &lo, &hi);
            load_imm(9, (int64_t)lo, 8);
            line("str x9, %s", mem(slot_of[i->dst], 8));
            load_imm(9, (int64_t)hi, 8);
            line("str x9, %s", mem(slot_of[i->dst] + 8, 8));
        }
        return;
    case IR_LOCAL: {
        const char *D = dstr(9, i->dst, 8);
        if (i->obj->offset < 4096)
            line("add %s, x19, #%d", D, i->obj->offset);
        else {
            load_imm(9, i->obj->offset, 8);
            line("add %s, x19, x9", D);
        }
        done(9, i->dst);
        return;
    }
    case IR_GLOBAL:
        global_address(in_gpr(i->dst) ? xreg(ra.reg[i->dst]) : 9, i->sym, i->obj, i->imm);
        done(9, i->dst);
        return;
    case IR_LOAD: {
        if (cls(i->dst) == V_LD) {
            const char *at = address(i->a, 0, k, 16);
            line("ldr q16, %s", at);
            fst(16, i->dst);
            return;
        }
        const char *at = address(i->a, 0, k, i->size);
        if (in_fpr(i->dst) && i->size == vsize(i->dst)) {
            line("ldr %c%d, %s", fpr(i->dst), 8 + ra.reg[i->dst], at);
            return;
        }
        int ds = vsize(i->dst) == 8 ? 8 : 4;
        const char *D = dstr(9, i->dst, ds);
        const char *W = dstr(9, i->dst, 4);
        switch (i->size) {
        case 1:
            if (i->is_signed)
                line("ldrsb %s, %s", D, at);
            else
                line("ldrb %s, %s", W, at);
            break;
        case 2:
            if (i->is_signed)
                line("ldrsh %s, %s", D, at);
            else
                line("ldrh %s, %s", W, at);
            break;
        case 4:
            if (ds == 8 && i->is_signed)
                line("ldrsw %s, %s", D, at);
            else
                line("ldr %s, %s", W, at);
            break;
        default: line("ldr %s, %s", dstr(9, i->dst, 8), at);
        }
        if (!in_gpr(i->dst))
            st(9, i->dst);
        return;
    }
    case IR_STORE: {
        int64_t c;
        if (i->size > 8) {
            line("ldr q16, %s", mem(slot_of[i->b], 16));
            line("str q16, %s", address(i->a, 0, k, 16));
            return;
        }
        int s = i->size == 8 ? 8 : 4;
        const char *V;
        if (opt_level && int_cls(i->b) && constant(i->b, &c) && c == 0)
            V = s == 8 ? "xzr" : "wzr";
        else if (in_fpr(i->b) && i->size == vsize(i->b)) {
            static char fv[8];
            snprintf(fv, sizeof(fv), "%c%d", fpr(i->b), 8 + ra.reg[i->b]);
            V = fv;
        } else if (i->size >= 4)
            V = src(9, i->b, s);
        else
            V = src(9, i->b, vsize(i->b) == 8 ? 8 : 4);
        /* (a w name for strb and strh) */
        if (i->size < 4 && V[0] == 'x')
            V = r(atoi(V + 1), 4);
        const char *at = address(i->a, 0, k, i->size);
        line("%s %s, %s", i->size == 1 ? "strb" : i->size == 2 ? "strh" : "str", V, at);
        return;
    }
    case IR_COPY:
        if (cls(i->dst) == V_LD) {
            fld(16, i->a);
            fst(16, i->dst);
            return;
        }
        if (in_gpr(i->dst) && in_gpr(i->a)) {
            line("mov %s, %s", r(xreg(ra.reg[i->dst]), 8), r(xreg(ra.reg[i->a]), 8));
            return;
        }
        if (in_gpr(i->dst)) {
            ld_bits(xreg(ra.reg[i->dst]), i->a, 8);
            return;
        }
        if (in_gpr(i->a)) {
            st_bits(xreg(ra.reg[i->a]), i->dst, 8);
            return;
        }
        ld_bits(9, i->a, 8);
        st_bits(9, i->dst, 8);
        return;
    case IR_BIN:
        gen_bin(i);
        return;
    case IR_UN: {
        int s = vsize(i->dst) == 8 ? 8 : 4;
        if (i->sub == OP_FNEG) {
            if (cls(i->dst) == V_LD) {
                line("ldr x9, %s", mem(slot_of[i->a], 8));
                line("str x9, %s", mem(slot_of[i->dst], 8));
                line("ldr x9, %s", mem(slot_of[i->a] + 8, 8));
                line("eor x9, x9, #0x8000000000000000");
                line("str x9, %s", mem(slot_of[i->dst] + 8, 8));
                return;
            }
            const char *A = fsrc(16, i->a);
            line("fneg %s, %s", fdst(16, i->dst), A);
            fdone(16, i->dst);
            return;
        }
        const char *A = src(9, i->a, s);
        line("%s %s, %s", i->sub == OP_NEG ? "neg" : "mvn", dstr(9, i->dst, s), A);
        done(9, i->dst);
        return;
    }
    case IR_CMP:
        gen_cmp(i);
        return;
    case IR_CONV:
        gen_conv(i);
        return;
    case IR_CALL:
        gen_call(i);
        return;
    case IR_RET:
        gen_ret(i);
        return;
    case IR_MEMCPY:
        ld_addr(10, i->a);
        ld_addr(11, i->b);
        copy_bytes(10, 0, 11, 0, (int)i->imm);
        return;
    case IR_MEMZERO: {
        ld_addr(10, i->a);
        int n = (int)i->imm, at = 0;
        if (n > 256) {
            int again = unique++;
            load_imm(15, n / 8, 8);
            line("mov x14, x10");
            fprintf(out, ".Lc%d:\n", again);
            line("str xzr, [x14], #8");
            line("subs x15, x15, #1");
            line("b.ne .Lc%d", again);
            at = n / 8 * 8;
        }
        for (; n - at >= 8; at += 8)
            line("str xzr, [x10, #%d]", at);
        for (; at < n; at++)
            line("strb wzr, [x10, #%d]", at);
        return;
    }
    case IR_VA_START:
        gen_va_start(i);
        return;
    case IR_VA_ARG:
        gen_va_arg(i);
        return;
    case IR_ALLOCA:
        ld(9, i->a);
        line("add x9, x9, #15");
        line("and x9, x9, #-16");
        line("mov x10, sp");
        line("sub x10, x10, x9");
        if (i->imm > 16)
            line("and x10, x10, #%lld", -(long long)i->imm);
        line("mov sp, x10");
        st(10, i->dst);
        return;
    case IR_GET_SP:
        line("mov x9, sp");
        st(9, i->dst);
        return;
    case IR_SET_SP:
        ld(9, i->a);
        line("mov sp, x9");
        return;
    case IR_BUILTIN:
        gen_builtin(i);
        return;
    case IR_ATOMIC:
        gen_atomic(i);
        return;
    case IR_NOP:
        return;
    case IR_ASM:
        gen_asm(k);
        return;
    case IR_TLS_ADDR:
        /* (62b) a TLS descriptor: its function gives x0 = the offset from
         * the thread pointer (keeping every other register but x30) */
        line("adrp x0, :tlsdesc:%s", i->sym);
        line("ldr x1, [x0, :tlsdesc_lo12:%s]", i->sym);
        line("add x0, x0, :tlsdesc_lo12:%s", i->sym);
        line(".tlsdesccall %s", i->sym);
        line("blr x1");
        line("mrs x9, tpidr_el0");
        line("add x9, x9, x0");
        st(9, i->dst);
        return;
    case IR_TRAP:
        line("brk #1000");
        return;
    case IR_VSPLAT: case IR_VHADD: case IR_VMADD: case IR_VREVW:   /* (39f: made for x86-64 only) */
        error_at(i->tok, "internal error: a packed operation on AArch64");
    }
}

static void gen_prologue(struct obj *fn)
{
    line("stp x29, x30, [sp, #-16]!");
    line("mov x29, sp");
    if (frame_size < 4096)
        line("sub sp, sp, #%d", frame_size);
    else {
        load_imm(16, frame_size, 8);
        line("sub sp, sp, x16");
    }
    /* (61d) x19 kept in the frame; then it is the frame's base */
    if (save_at[17] < 32760)
        line("str x19, [sp, #%d]", save_at[17]);
    else {
        load_imm(16, save_at[17], 8);
        line("str x19, [sp, x16]");
    }
    line("mov x19, sp");
    for (int k = 0; k < 9; k++)                 /* (61a) the registers values use, kept for the caller */
        if (gpr_used[k])
            line("str x%d, %s", 20 + k, mem(save_at[k], 8));
    for (int k = 0; k < 8; k++)
        if (fpr_used[k])
            line("str d%d, %s", 8 + k, mem(save_at[9 + k], 8));
    if (sret_slot >= 0)
        line("str x8, %s", mem(sret_slot, 8));
    if (fn->type->is_variadic) {
        for (int k = 0; k < 8; k += 2)
            line("stp x%d, x%d, [x19, #%d]", k, k + 1, gr_area + 8 * k);
        for (int k = 0; k < 8 && !opt_general_regs; k++)
            line("str q%d, %s", k, mem(vr_area + 16 * k, 16));
    }
    struct counters c = { 0 };
    for (struct obj *p = fn->params; p; p = p->next) {
        struct place pl;
        struct type *ty = unqual(p->type);
        place_of(ty, &c, &pl);
        int off = p->offset;
        if (pl.kind == P_STACK) {
            /* from the caller's area above our frame record */
            line("add x10, x29, #%d", FRAME_HEADER + pl.stack);
            if (pl.by_reference)
                line("ldr x10, [x10]");
            line("add x11, x19, #%d", off);
            copy_bytes(11, 0, 10, 0, ty->size);
            continue;
        }
        if (pl.by_reference) {
            line("add x11, x19, #%d", off);
            copy_bytes(11, 0, pl.reg, 0, ty->size);
            continue;
        }
        if (pl.kind == P_FP) {
            char k = ty->kind == TY_FLOAT ? 's' : ty->kind == TY_DOUBLE ? 'd' : 'q';
            line("str %c%d, %s", k, pl.reg, mem(off, ty->size));
            continue;
        }
        if (pl.kind == P_HFA) {
            for (int j = 0; j < pl.nregs; j++)
                line("str %c%d, %s", hfa_reg(pl.hfa_base), pl.reg + j,
                     mem(off + j * pl.hfa_base->size, pl.hfa_base->size));
            continue;
        }
        if (is_memory_value(ty)) {
            for (int j = 0; j < pl.nregs; j++)
                line("str x%d, %s", pl.reg + j, mem(off + 8 * j, 8));
            continue;
        }
        switch (ty->size) {
        case 1: line("strb w%d, %s", pl.reg, mem(off, 1)); break;
        case 2: line("strh w%d, %s", pl.reg, mem(off, 2)); break;
        case 4: line("str w%d, %s", pl.reg, mem(off, 4)); break;
        default: line("str x%d, %s", pl.reg, mem(off, 8));
        }
    }
}

/* --- inline assembly (62c) ------------------------------------------------ */

/* As on x86-64: no value is in a register over an asm statement, so its
 * operands may take any register but x19 (the frame), x29, x30 and sp;
 * loaded from their slots right before, outputs stored right after;
 * callee-saved ones it uses marked for the prologue. */
struct asm_place {
    int *reg;
    int temp;
};
static struct asm_place *asm_places;

static int x_by_name(const char *name)
{
    if ((name[0] == 'x' || name[0] == 'w') && name[1] >= '0' && name[1] <= '9') {
        int n = atoi(name + 1);
        return n <= 30 ? n : -1;
    }
    if (strcmp(name, "lr") == 0)
        return 30;
    return -1;
}

static bool is_mem_constraint(const char *c)
{
    return strchr(c, 'm') && !strchr(c, 'r');
}

static void place_asm(int k)
{
    struct asm_stmt *a = F->code[k].asm_info;
    struct asm_place *p = &asm_places[k];
    p->reg = calloc((size_t)a->nops + 1, sizeof(int));
    bool taken[32] = { 0 };
    taken[19] = taken[29] = taken[30] = taken[31] = true;
    for (int j = 0; j < a->nclobbers; j++) {
        const char *c = a->clobbers[j];
        int r = x_by_name(c);
        if (r == 19 || r == 29)
            error_at(F->code[k].tok, "asm: %s is the frame's register here", c);
        if (r >= 0)
            taken[r] = true;
        /* (v8-v15, d8-d15, s8-s15: kept for the caller) */
        if ((c[0] == 'v' || c[0] == 'd' || c[0] == 's' || c[0] == 'q') && c[1] >= '0' && c[1] <= '9') {
            int f = atoi(c + 1);
            if (f >= 8 && f <= 15)
                fpr_used[f - 8] = true;
        }
    }
    for (int j = 0; j < a->nops; j++) {
        struct asm_operand *op = &a->ops[j];
        p->reg[j] = -1;
        if (op->has_imm)
            continue;
        struct node *e = op->expr;
        if (e->kind == ND_VAR && e->var->asm_reg) {
            int r = x_by_name(e->var->asm_reg);
            if (r < 0 || r == 19 || r == 29)
                error_at(F->code[k].tok, "asm: register '%s' cannot be used", e->var->asm_reg);
            p->reg[j] = r;
            taken[r] = true;
        }
    }
    static const int pool[] = { 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 17,
                                20, 21, 22, 23, 24, 25, 26, 27, 28 };
    for (int j = 0; j < a->nops; j++) {
        if (a->ops[j].has_imm || p->reg[j] >= 0)
            continue;
        for (size_t q = 0; q < sizeof(pool) / sizeof(pool[0]); q++)
            if (!taken[pool[q]]) {
                p->reg[j] = pool[q];
                taken[pool[q]] = true;
                break;
            }
        if (p->reg[j] < 0)
            error_at(F->code[k].tok, "asm: too many operands for the registers");
    }
    p->temp = -1;
    for (size_t q = 0; q < sizeof(pool) / sizeof(pool[0]) && p->temp < 0; q++) {
        bool out_reg = false;
        for (int j = 0; j < a->noutputs; j++)
            if (p->reg[j] == pool[q] && !is_mem_constraint(a->ops[j].constraint))
                out_reg = true;
        if (!out_reg)
            p->temp = pool[q];
    }
    for (int r = 20; r <= 28; r++)
        if (taken[r] || r == p->temp)
            gpr_used[r - 20] = true;
}

static void gen_asm(int k)
{
    struct ir *i = &F->code[k];
    struct asm_stmt *a = i->asm_info;
    if (a->is_basic) {
        fprintf(out, "\t%s\n", a->text);
        return;
    }
    struct asm_place *p = &asm_places[k];
    struct ir_call *c = i->call;
    for (int j = 0; j < a->nops; j++) {
        struct asm_operand *op = &a->ops[j];
        int v = c->args[j].value, rg = p->reg[j], size = c->args[j].type->size;
        if (op->has_imm)
            continue;
        if (is_mem_constraint(op->constraint))
            ld_bits(rg, v, 8);
        else if (op->is_output && op->is_inout) {
            ld_bits(rg, v, 8);
            line("%s %s, [x%d]", size == 1 ? "ldrb" : size == 2 ? "ldrh" : "ldr", r(rg, size == 8 ? 8 : 4), rg);
        } else if (!op->is_output)
            ld_bits(rg, v, vsize(v) == 8 ? 8 : 4);
    }
    struct buffer b = { 0 };
    for (const char *t = a->text; *t; t++) {
        if (*t != '%') {
            buf_printf(&b, "%c", *t);
            continue;
        }
        t++;
        if (*t == '%') {
            buf_printf(&b, "%%");
            continue;
        }
        if (*t == '=') {
            buf_printf(&b, "%d", unique++);
            continue;
        }
        char mod = 0;
        if ((*t >= 'a' && *t <= 'z') && (t[1] == '[' || (t[1] >= '0' && t[1] <= '9')))
            mod = *t++;
        int j = -1;
        if (*t == '[') {
            const char *end = strchr(t, ']');
            if (!end)
                error_at(i->tok, "asm: a [name] without ]");
            for (int q = 0; q < a->nops; q++)
                if (a->ops[q].name && (size_t)(end - t - 1) == strlen(a->ops[q].name) &&
                    strncmp(t + 1, a->ops[q].name, (size_t)(end - t - 1)) == 0)
                    j = q;
            t = end;
        } else if (*t >= '0' && *t <= '9') {
            j = 0;
            while (t[0] >= '0' && t[0] <= '9')
                j = j * 10 + (*t++ - '0');
            t--;
        }
        if (j < 0 || j >= a->nops)
            error_at(i->tok, "asm: an operand that does not exist");
        struct asm_operand *op = &a->ops[j];
        if (op->has_imm) {
            buf_printf(&b, "%lld", (long long)op->imm);
            continue;
        }
        int rg = p->reg[j];
        if (is_mem_constraint(op->constraint)) {
            buf_printf(&b, "[x%d]", rg);
            continue;
        }
        int size = c->args[j].type->size;
        bool x = mod == 'x' || (mod != 'w' && size == 8);
        buf_printf(&b, "%c%d", x ? 'x' : 'w', rg);
    }
    fprintf(out, "\t%s\n", b.data ? b.data : "");
    free(b.data);
    for (int j = 0; j < a->noutputs; j++) {
        struct asm_operand *op = &a->ops[j];
        if (is_mem_constraint(op->constraint))
            continue;
        int v = c->args[j].value, rg = p->reg[j], size = c->args[j].type->size, t = p->temp;
        ld_bits(t, v, 8);
        line("%s %s, [x%d]", size == 1 ? "strb" : size == 2 ? "strh" : "str", r(rg, size == 8 ? 8 : 4), t);
    }
}

static void emit_function(struct obj *fn)
{
    F = ir_function(fn);
    if (getenv("CC_DUMP_IR"))
        ir_dump(stderr, F);
    fn_number++;
    slot_of = calloc((size_t)F->nvalues + 1, sizeof(int));
    memset(gpr_used, 0, sizeof(gpr_used));
    memset(fpr_used, 0, sizeof(fpr_used));
    analyze();
    layout_frame(fn);
    asm_places = calloc((size_t)F->ncode + 1, sizeof(*asm_places));
    for (int k = 0; k < F->ncode; k++)
        if (F->code[k].op == IR_ASM && !F->code[k].asm_info->is_basic)
            place_asm(k);
    place_saves();
    fprintf(out, "\t.text\n");
    if (!fn->is_static)
        fprintf(out, "\t.globl %s\n", fn->name);
    if (fn->is_weak)
        fprintf(out, "\t.weak %s\n", fn->name);
    if (fn->section)
        fprintf(out, "\t.section %s,\"ax\",@progbits\n", fn->section);
    fprintf(out, "\t.p2align 2\n");
    fprintf(out, "\t.type %s, %%function\n", fn->name);
    fprintf(out, "%s:\n", fn->name);
    gen_prologue(fn);
    for (int k = 0; k < F->ncode; k++)
        gen_insn(k);
    fprintf(out, "\t.size %s, .-%s\n", fn->name, fn->name);
    free(slot_of);
    free(defs);
    free(def_at);
    free(uses);
    free(nonfolded);
    free(skip);
    free(chained);
    for (int k = 0; k < F->ncode; k++)
        free(asm_places[k].reg);
    free(asm_places);
    free(ra.reg);
    free(ra.start);
    free(ra.end);
    ra.reg = 0;
    free(F->code);
    free(F->values);
    free(F);
}

void emit_aarch64(FILE *o, struct obj *program)
{
    out = o;
    for (struct obj *fn = program; fn; fn = fn->next)
        if (fn->is_function && fn->is_definition && !inline_only(fn)) {
            fn->emitted = true;
            emit_function(fn);
        }
    /* (63c) a function inlined at its only call is written only when that
     * call was not inlined after all (too deep), and so on for its calls */
    for (bool more = true; more;) {
        more = false;
        for (struct obj *fn = program; fn; fn = fn->next)
            if (fn->is_function && fn->is_definition && !fn->emitted && fn->call_emitted) {
                fn->emitted = more = true;
                emit_function(fn);
            }
    }
    emit_data(out, program, true);
    fprintf(out, "\t.section .note.GNU-stack,\"\",@progbits\n");
}
