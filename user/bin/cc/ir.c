/*
 * From the tree to the intermediate code (60c), one function at a time.
 * Expressions give a value (gen_expr) or an address (gen_addr); conditions
 * of if, loops, && and || become branches (gen_cond). A structure is
 * handled by its address throughout: assignment copies its bytes.
 */
#include "ir.h"

static struct ir_func *F;
int opt_level = 1;
bool opt_no_pic, opt_general_regs;

int value_size(enum ir_class cls)
{
    switch (cls) {
    case V_I32: case V_F32: return 4;
    case V_LD: case V_V16: return 16;
    case V_V32: return 32;
    default: return 8;
    }
}

static int new_value(enum ir_class cls)
{
    if (F->nvalues == F->capvalues) {
        F->capvalues = F->capvalues ? F->capvalues * 2 : 256;
        F->values = realloc(F->values, sizeof(struct ir_value) * (size_t)F->capvalues);
        if (!F->values)
            fatal("out of memory");
    }
    if (opt_general_regs && (cls == V_F32 || cls == V_F64 || cls == V_LD))
        fatal("%s: floating point with -mgeneral-regs-only", F->fn->name);
    F->values[F->nvalues].cls = cls;
    return F->nvalues++;
}

static struct ir *emit(enum ir_op op, struct token *tok)
{
    if (F->ncode == F->capcode) {
        F->capcode = F->capcode ? F->capcode * 2 : 1024;
        F->code = realloc(F->code, sizeof(struct ir) * (size_t)F->capcode);
        if (!F->code)
            fatal("out of memory");
    }
    struct ir *i = &F->code[F->ncode++];
    memset(i, 0, sizeof(*i));
    i->op = op;
    i->tok = tok;
    return i;
}

static int new_label_id(void)
{
    return ++F->nlabels;
}

static void label(int id)
{
    emit(IR_LABEL, 0)->label = id;
}

static void jump(int id)
{
    emit(IR_JMP, 0)->label = id;
}

static void branch(int cond, int if_true, int if_false)
{
    struct ir *i = emit(IR_BR, 0);
    i->a = cond;
    i->label = if_true;
    i->label2 = if_false;
}

static enum ir_class class_of(struct type *ty, struct token *tok)
{
    (void)tok;
    ty = unqual(ty);
    switch (ty->kind) {
    case TY_VOID: return V_NONE;
    case TY_FLOAT: return V_F32;
    case TY_DOUBLE: return V_F64;
    case TY_LDOUBLE: return V_LD;
    case TY_STRUCT:
        if (ty->is_vector)
            return ty->size == 16 ? V_V16 : V_V32;
        return V_I64;
    case TY_INT128:                             /* (62a: its address, as a structure's) */
    case TY_UNION: case TY_ARRAY: case TY_VLA: case TY_FUNC: case TY_PTR: return V_I64;
    default: return ty->size == 8 ? V_I64 : V_I32;
    }
}

static int imm(int64_t v, enum ir_class cls)
{
    int d = new_value(cls);
    struct ir *i = emit(IR_IMM, 0);
    i->dst = d;
    i->imm = v;
    return d;
}

static int bin(int sub, int a, int b, enum ir_class cls, struct token *tok)
{
    int d = new_value(cls);
    struct ir *i = emit(IR_BIN, tok);
    i->sub = sub;
    i->dst = d;
    i->a = a;
    i->b = b;
    return d;
}

static int cmp(int sub, int a, int b)
{
    int d = new_value(V_I32);
    struct ir *i = emit(IR_CMP, 0);
    i->sub = sub;
    i->dst = d;
    i->a = a;
    i->b = b;
    return d;
}

static int conv(int sub, int a, int from_size, enum ir_class to)
{
    int d = new_value(to);
    struct ir *i = emit(IR_CONV, 0);
    i->sub = sub;
    i->dst = d;
    i->a = a;
    i->size = from_size;
    return d;
}

static void set(int dst, int a)
{
    struct ir *i = emit(IR_COPY, 0);
    i->dst = dst;
    i->a = a;
}

static int add_offset(int addr, int64_t offset)
{
    if (offset == 0)
        return addr;
    return bin(OP_ADD, addr, imm(offset, V_I64), V_I64, 0);
}

/* *addr as a value of type ty (a structure or array: the address itself). */
static int load(struct type *ty, int addr, struct token *tok)
{
    bool vol = (ty->qual & Q_VOLATILE) != 0;
    ty = unqual(ty);
    if (is_memory_value(ty) || ty->kind == TY_ARRAY || ty->kind == TY_FUNC || ty->kind == TY_VLA)
        return addr;
    enum ir_class cls = class_of(ty, tok);
    int d = new_value(cls);
    struct ir *i = emit(IR_LOAD, tok);
    i->dst = d;
    i->a = addr;
    i->size = ty->kind == TY_LDOUBLE ? 16 : ty->size;
    i->is_signed = !ty->is_unsigned && is_integer(ty);
    i->is_volatile = vol;
    return d;
}

static void store(struct type *ty, int addr, int v, struct token *tok)
{
    bool vol = (ty->qual & Q_VOLATILE) != 0;
    ty = unqual(ty);
    if (is_memory_value(ty) || ty->kind == TY_ARRAY) {
        struct ir *i = emit(IR_MEMCPY, tok);
        i->a = addr;
        i->b = v;
        i->imm = ty->size;
        return;
    }
    struct ir *i = emit(IR_STORE, tok);
    i->a = addr;
    i->b = v;
    i->size = ty->kind == TY_LDOUBLE ? 16 : ty->size;
    i->is_volatile = vol;
}

/* --- conversions --------------------------------------------------------- */

static int extend_narrow(int v, struct type *from)
{
    /* a char, short or _Bool value made a proper 32-bit one */
    from = unqual(from);
    if (from->kind == TY_ENUM)
        from = from->enum_int;
    if (is_integer(from) && from->size < 4)
        return conv(from->is_unsigned ? CONV_ZEXT : CONV_SEXT, v, from->size, V_I32);
    return v;
}

static int local_addr(struct obj *o);

/* --- __int128 (62a) -------------------------------------------------------- */

/* A 128-bit value is the address of its 16 bytes (the low half first), as
 * a structure's; each operation reads the two 64-bit halves and writes a
 * new temporary. Division and conversions to and from floating types call
 * the helpers clang calls (compiler-rt's names). */

static bool is_int128(struct type *t)
{
    return unqual(t)->kind == TY_INT128;
}

/* the address of a new temporary local of type ty */
static int temp_of(struct type *ty)
{
    struct obj *o = arena_alloc(sizeof(*o));
    o->name = ".t";
    o->type = ty;
    o->is_local = true;
    o->align = ty->align;
    o->address_taken = true;
    o->next = F->fn->locals;
    F->fn->locals = o;
    return local_addr(o);
}

static int half(int addr, int k)
{
    return load(ty_ulong, add_offset(addr, 8 * k), 0);
}

static int pair(int lo, int hi)
{
    int t = temp_of(ty_uint128);
    store(ty_ulong, t, lo, 0);
    store(ty_ulong, add_offset(t, 8), hi, 0);
    return t;
}

/* a call of a helper: rt name(types[0] vals[0], ...) */
static int helper(const char *name, struct type *rt, struct type **types, int *vals, int n, struct token *tok)
{
    struct ir_call *c = arena_alloc(sizeof(*c));
    struct type *ft = func_type(rt);
    ft->has_prototype = true;
    c->func_type = ft;
    c->args = arena_alloc(sizeof(struct ir_arg) * (size_t)(n + 1));
    c->nargs = c->nfixed = n;
    for (int k = 0; k < n; k++) {
        c->args[k].value = vals[k];
        c->args[k].type = unqual(types[k]);
    }
    if (is_memory_value(rt))
        c->ret_buffer = temp_of(rt);
    /* (operands first: emit() may move the code) */
    /* (the name may be the caller's buffer: the IR keeps its own copy) */
    char *sym = arena_alloc(strlen(name) + 1);
    memcpy(sym, name, strlen(name) + 1);
    struct ir *i = emit(IR_CALL, tok);
    i->call = c;
    i->sym = sym;
    i->type = unqual(rt);
    if (is_memory_value(rt))
        return c->ret_buffer;
    int d = new_value(class_of(rt, tok));
    F->code[F->ncode - 1].dst = d;
    return d;
}

static const char *float_suffix(struct type *t)
{
    switch (unqual(t)->kind) {
    case TY_FLOAT: return "sf";
    case TY_DOUBLE: return "df";
    default: return target.arch == ARCH_X86_64 ? "xf" : "tf";
    }
}

/* an integer (or pointer) value of type from as 128 bits */
static int widen128(int v, struct type *from)
{
    v = extend_narrow(v, from);
    bool sign = is_integer(from) && !from->is_unsigned && from->kind != TY_BOOL;
    if (class_of(from, 0) == V_I32)
        v = conv(sign ? CONV_SEXT : CONV_ZEXT, v, 4, V_I64);
    return pair(v, sign ? bin(OP_SAR, v, imm(63, V_I64), V_I64, 0) : imm(0, V_I64));
}

static int nonzero128(int v)
{
    return cmp(OP_NE, bin(OP_OR, half(v, 0), half(v, 1), V_I64, 0), imm(0, V_I64));
}

static int cast128(int v, struct type *from, struct type *to, struct token *tok)
{
    char name[32];
    if (is_int128(from) && is_int128(to))
        return v;
    if (is_int128(to)) {
        if (is_flonum(from)) {
            snprintf(name, sizeof(name), "__fix%s%sti", to->is_unsigned ? "uns" : "", float_suffix(from));
            return helper(name, to, &from, &v, 1, tok);
        }
        return widen128(v, from);
    }
    if (to->kind == TY_BOOL)
        return nonzero128(v);
    if (is_flonum(to)) {
        snprintf(name, sizeof(name), "__float%sti%s", from->is_unsigned ? "un" : "", float_suffix(to));
        return helper(name, to, &from, &v, 1, tok);
    }
    int lo = half(v, 0);
    return class_of(to, tok) == V_I32 ? conv(CONV_TRUNC, lo, 8, V_I32) : lo;
}

/* a << n, a >> n (n 0-127 as a 64-bit value) */
static int shift128(bool left, bool arith, int a, int n)
{
    int al = half(a, 0), ah = half(a, 1);
    int t = temp_of(ty_uint128), hi_addr = add_offset(t, 8);
    int big = new_label_id(), small = new_label_id(), end = new_label_id();
    branch(cmp(OP_ULT, imm(63, V_I64), n), big, small);
    label(small);
    /* (x >> 1 >> (63 - n) is x >> (64 - n), and 0 for n = 0) */
    int back = bin(OP_SUB, imm(63, V_I64), n, V_I64, 0);
    if (left) {
        store(ty_ulong, t, bin(OP_SHL, al, n, V_I64, 0), 0);
        int carry = bin(OP_SHR, bin(OP_SHR, al, imm(1, V_I64), V_I64, 0), back, V_I64, 0);
        store(ty_ulong, hi_addr, bin(OP_OR, bin(OP_SHL, ah, n, V_I64, 0), carry, V_I64, 0), 0);
    } else {
        int carry = bin(OP_SHL, bin(OP_SHL, ah, imm(1, V_I64), V_I64, 0), back, V_I64, 0);
        store(ty_ulong, t, bin(OP_OR, bin(OP_SHR, al, n, V_I64, 0), carry, V_I64, 0), 0);
        store(ty_ulong, hi_addr, bin(arith ? OP_SAR : OP_SHR, ah, n, V_I64, 0), 0);
    }
    jump(end);
    label(big);
    int m = bin(OP_SUB, n, imm(64, V_I64), V_I64, 0);
    if (left) {
        store(ty_ulong, hi_addr, bin(OP_SHL, al, m, V_I64, 0), 0);
        store(ty_ulong, t, imm(0, V_I64), 0);
    } else {
        store(ty_ulong, t, bin(arith ? OP_SAR : OP_SHR, ah, m, V_I64, 0), 0);
        store(ty_ulong, hi_addr, arith ? bin(OP_SAR, ah, imm(63, V_I64), V_I64, 0) : imm(0, V_I64), 0);
    }
    label(end);
    return t;
}

static int binary128(enum node_kind kind, struct type *ty, int a, int b, struct token *tok)
{
    bool u = ty->is_unsigned;
    if (kind == ND_DIV || kind == ND_MOD) {
        struct type *types[2] = { ty, ty };
        int vals[2] = { a, b };
        const char *name = kind == ND_DIV ? (u ? "__udivti3" : "__divti3") : (u ? "__umodti3" : "__modti3");
        return helper(name, ty, types, vals, 2, tok);
    }
    int al = half(a, 0), ah = half(a, 1), bl = half(b, 0), bh = half(b, 1), lo, hi;
    switch (kind) {
    case ND_ADD:
        lo = bin(OP_ADD, al, bl, V_I64, tok);
        hi = bin(OP_ADD, bin(OP_ADD, ah, bh, V_I64, tok), conv(CONV_ZEXT, cmp(OP_ULT, lo, al), 4, V_I64), V_I64,
                 tok);
        break;
    case ND_SUB:
        lo = bin(OP_SUB, al, bl, V_I64, tok);
        hi = bin(OP_SUB, bin(OP_SUB, ah, bh, V_I64, tok), conv(CONV_ZEXT, cmp(OP_ULT, al, bl), 4, V_I64), V_I64,
                 tok);
        break;
    case ND_MUL:
        lo = bin(OP_MUL, al, bl, V_I64, tok);
        hi = bin(OP_ADD, bin(OP_UMULH, al, bl, V_I64, tok),
                 bin(OP_ADD, bin(OP_MUL, al, bh, V_I64, tok), bin(OP_MUL, ah, bl, V_I64, tok), V_I64, tok), V_I64,
                 tok);
        break;
    case ND_BITAND: lo = bin(OP_AND, al, bl, V_I64, tok); hi = bin(OP_AND, ah, bh, V_I64, tok); break;
    case ND_BITOR: lo = bin(OP_OR, al, bl, V_I64, tok); hi = bin(OP_OR, ah, bh, V_I64, tok); break;
    default: lo = bin(OP_XOR, al, bl, V_I64, tok); hi = bin(OP_XOR, ah, bh, V_I64, tok); break;
    }
    return pair(lo, hi);
}

static int compare128(enum node_kind kind, bool u, int a, int b)
{
    int al = half(a, 0), ah = half(a, 1), bl = half(b, 0), bh = half(b, 1);
    if (kind == ND_EQ || kind == ND_NE) {
        int x = bin(OP_OR, bin(OP_XOR, al, bl, V_I64, 0), bin(OP_XOR, ah, bh, V_I64, 0), V_I64, 0);
        return cmp(kind == ND_EQ ? OP_EQ : OP_NE, x, imm(0, V_I64));
    }
    int high = cmp(u ? OP_ULT : OP_SLT, ah, bh);
    int low = bin(OP_AND, cmp(OP_EQ, ah, bh), cmp(kind == ND_LT ? OP_ULT : OP_ULE, al, bl), V_I32, 0);
    return bin(OP_OR, high, low, V_I32, 0);
}

/* --- complex (62a) --------------------------------------------------------- */

/* A complex value is the address of its two parts, like a structure's;
 * + and - work part by part, * and / of two complex values call the
 * helpers clang calls (__mulsc3 ... in the C library), a real operand is
 * used part by part (x * z, z / x). */

static int float_zero(enum ir_class cls);
static int gen_cast(int v, struct type *from, struct type *to, struct token *tok);

static int part(int addr, struct type *real, int k)
{
    return load(real, add_offset(addr, k * real->size), 0);
}

static int make_complex(struct type *real, int re, int im)
{
    int t = temp_of(complex_of(real));
    store(real, t, re, 0);
    store(real, add_offset(t, real->size), im, 0);
    return t;
}

static int fimm_of(enum ir_class cls, double v)
{
    int d = new_value(cls);
    struct ir *i = emit(IR_FIMM, 0);
    i->dst = d;
    i->fimm = v;
    return d;
}

static int fneg(int v, enum ir_class cls)
{
    int d = new_value(cls);
    struct ir *i = emit(IR_UN, 0);
    i->sub = OP_FNEG;
    i->dst = d;
    i->a = v;
    return d;
}

static int nonzero_complex(int v, struct type *real)
{
    enum ir_class c = class_of(real, 0);
    int re = cmp(OP_FNE, part(v, real, 0), float_zero(c)), im = cmp(OP_FNE, part(v, real, 1), float_zero(c));
    return bin(OP_OR, re, im, V_I32, 0);
}

static int cast_complex(int v, struct type *from, struct type *to, struct token *tok)
{
    if (is_complex(from) && is_complex(to)) {
        struct type *rf = from->base, *rt = to->base;
        if (rf->kind == rt->kind)
            return v;
        return make_complex(rt, gen_cast(part(v, rf, 0), rf, rt, tok), gen_cast(part(v, rf, 1), rf, rt, tok));
    }
    if (is_complex(to))
        return make_complex(to->base, gen_cast(v, from, to->base, tok), float_zero(class_of(to->base, tok)));
    if (to->kind == TY_BOOL)
        return nonzero_complex(v, from->base);
    return gen_cast(part(v, from->base, 0), from->base, to, tok);
}

static int binary_complex(enum node_kind kind, struct type *ty, struct node *l, int a, struct node *r, int b,
                          struct token *tok)
{
    struct type *rt = ty->base;
    enum ir_class rc = class_of(rt, tok);
    bool ac = is_complex(l->type), bc = is_complex(r->type);
    int ar = ac ? part(a, rt, 0) : a, ai = ac ? part(a, rt, 1) : 0;
    int br = bc ? part(b, rt, 0) : b, bi = bc ? part(b, rt, 1) : 0;
    if ((kind == ND_MUL && ac && bc) || (kind == ND_DIV && bc)) {
        const char *k = rt->kind == TY_FLOAT ? "sc3" : rt->kind == TY_DOUBLE ? "dc3"
                        : target.arch == ARCH_X86_64 ? "xc3" : "tc3";
        char name[16];
        snprintf(name, sizeof(name), "__%s%s", kind == ND_MUL ? "mul" : "div", k);
        struct type *types[4] = { rt, rt, rt, rt };
        int vals[4] = { ar, ac ? ai : float_zero(rc), br, bi };
        return helper(name, ty, types, vals, 4, tok);
    }
    int re, im;
    switch (kind) {
    case ND_ADD:
        re = bin(OP_FADD, ar, br, rc, tok);
        im = ac && bc ? bin(OP_FADD, ai, bi, rc, tok) : ac ? ai : bi;
        break;
    case ND_SUB:
        re = bin(OP_FSUB, ar, br, rc, tok);
        im = ac && bc ? bin(OP_FSUB, ai, bi, rc, tok) : ac ? ai : fneg(bi, rc);
        break;
    case ND_MUL:
        if (ac) {
            re = bin(OP_FMUL, ar, b, rc, tok);
            im = bin(OP_FMUL, ai, b, rc, tok);
        } else {
            re = bin(OP_FMUL, a, br, rc, tok);
            im = bin(OP_FMUL, a, bi, rc, tok);
        }
        break;
    default:                                    /* (complex / real) */
        re = bin(OP_FDIV, ar, b, rc, tok);
        im = bin(OP_FDIV, ai, b, rc, tok);
        break;
    }
    return make_complex(rt, re, im);
}

static int compare_complex(enum node_kind kind, struct type *rt, struct node *l, int a, struct node *r, int b)
{
    bool ac = is_complex(l->type), bc = is_complex(r->type);
    enum ir_class rc = class_of(rt, 0);
    int ar = ac ? part(a, rt, 0) : a, ai = ac ? part(a, rt, 1) : float_zero(rc);
    int br = bc ? part(b, rt, 0) : b, bi = bc ? part(b, rt, 1) : float_zero(rc);
    if (kind == ND_EQ)
        return bin(OP_AND, cmp(OP_FEQ, ar, br), cmp(OP_FEQ, ai, bi), V_I32, 0);
    return bin(OP_OR, cmp(OP_FNE, ar, br), cmp(OP_FNE, ai, bi), V_I32, 0);
}

static int gen_cast(int v, struct type *from, struct type *to, struct token *tok)
{
    from = unqual(from);
    to = unqual(to);
    if (to->kind == TY_VOID)
        return 0;
    if (from->kind == TY_ENUM)
        from = from->enum_int;
    if (to->kind == TY_ENUM)
        to = to->enum_int;
    if (is_complex(from) || is_complex(to))
        return cast_complex(v, from, to, tok);
    if (is_int128(from) || is_int128(to))
        return cast128(v, from, to, tok);
    enum ir_class fc = class_of(from, tok), tc = class_of(to, tok);
    if (to->kind == TY_BOOL) {
        if (is_flonum(from)) {
            int zero = new_value(fc);
            struct ir *i = emit(IR_FIMM, tok);
            i->dst = zero;
            i->fimm = 0;
            return cmp(OP_FNE, v, zero);
        }
        if (from->kind == TY_BOOL)
            return v;
        v = extend_narrow(v, from);
        return cmp(OP_NE, v, imm(0, fc == V_I64 ? V_I64 : V_I32));
    }
    if (is_flonum(to)) {
        if (is_flonum(from))
            return fc == tc ? v : conv(CONV_F2F, v, value_size(fc), tc);
        v = extend_narrow(v, from);
        return conv(from->is_unsigned ? CONV_U2F : CONV_S2F, v, fc == V_I64 ? 8 : 4, tc);
    }
    if (is_flonum(from)) {
        /* to a narrow integer: through int (the value must fit, 6.3.1.4) */
        int r = conv(to->is_unsigned && to->size >= 4 ? CONV_F2U : CONV_F2S, v, value_size(fc),
                     to->size == 8 ? V_I64 : V_I32);
        return r;
    }
    /* integers and pointers */
    if (to->size > from->size)
        return conv(from->is_unsigned || from->kind == TY_PTR ? CONV_ZEXT : CONV_SEXT, v, from->size, tc);
    if (tc != fc)
        return conv(CONV_TRUNC, v, from->size, tc);
    return v;
}

/* --- expressions --------------------------------------------------------- */

/* --- inlining (63c) ---------------------------------------------------------- */

/* A call to a small static function of this file is replaced by its body:
 * its parameters and locals get copies in the caller's frame (made when
 * first named: `var`), the arguments go into the parameters' copies, and a
 * return sets the call's value and jumps past the body. What clang gains
 * from it and we did not: a frame less on every call of a short helper
 * (kernel/pmm.c's is_used, arch_irq_save), and the stack and the time a
 * call costs. */
struct inline_frame {
    struct obj *fn;
    int result, end;
    struct obj *ret_obj;                        /* (39b) a returned structure's place: the call's own */
    struct type *rt;
    int depth;
    int at;                                     /* the outermost inlined call's place in the blocks */
    struct inline_frame *up;
};
static struct inline_frame *inlining;

static bool promotable(struct obj *o);
static void mark_taken(struct node *n);
static int gen_expr(struct node *n);
static void gen_stmt(struct node *n);

static struct obj *var(struct obj *o)
{
    if (!o || !o->inline_pending)
        return o;
    if (!o->inline_copy) {
        struct obj *c = arena_alloc(sizeof(*c));
        *c = *o;
        c->inline_copy = 0;
        c->inline_pending = false;
        c->ir_value = 0;
        c->offset = 0;
        /* (it lives at the call: so it shares room with what lives in other
         * blocks and with the copies of other calls, never with a local
         * of a block around the call; nested calls take the outer one's) */
        c->block_lo = c->block_hi = inlining ? inlining->at : 0;
        c->is_local = true;
        c->next = F->fn->locals;
        F->fn->locals = c;
        if (promotable(c))
            c->ir_value = new_value(class_of(c->type, c->tok));
        o->inline_copy = c;
    }
    return o->inline_copy;
}

#define INLINE_NODES 40                         /* (a callee's size at most, in tree nodes) */
#define INLINE_ONCE_NODES 600                   /* (one called only once: as clang, its frame saved) */
#define INLINE_MEMORY 128                       /* (a callee's locals in memory at most, in bytes) */
#define INLINE_DEPTH 4

/* is the body small enough and free of what a copy cannot have (labels,
 * VLAs, va_list, alloca, its own frame's or return address)? */
static int inline_limit;

static bool inline_ok(struct node *n, int *count)
{
    for (; n; n = n->next) {
        if (++*count > inline_limit)
            return false;
        switch (n->kind) {
        case ND_GOTO: case ND_LABEL: case ND_VLA_ALLOC: case ND_VA_START: case ND_VA_ARG: case ND_VA_COPY:
        case ND_VA_END:
            return false;
        case ND_BUILTIN:
            if (n->builtin == B_FRAME_ADDRESS || n->builtin == B_RETURN_ADDRESS || n->builtin == B_ALLOCA)
                return false;
            break;
        case ND_BLOCK:
            if (n->has_vla)
                return false;
            break;
        case ND_ASM:
            for (int k = 0; k < n->asm_info->nops; k++)
                if (!inline_ok(n->asm_info->ops[k].expr, count))
                    return false;
            break;
        default:
            break;
        }
        if (!inline_ok(n->lhs, count) || !inline_ok(n->rhs, count) || !inline_ok(n->cond, count) ||
            !inline_ok(n->then, count) || !inline_ok(n->els, count) || !inline_ok(n->init, count) ||
            !inline_ok(n->inc, count) || !inline_ok(n->body, count) || !inline_ok(n->args, count))
            return false;
    }
    return true;
}

/* what may be inlined, whatever the caller: a static function of this file
 * with a body small enough and free of what a copy cannot have */
static bool inlinable(struct obj *f)
{
    if (!f->is_function || !f->is_definition || !f->body || !f->is_static || f->is_noinline)
        return false;
    struct type *ft = f->type;
    /* (39b) structures, unions, __int128 and _Complex too: copied into the
     * parameters' places, a returned one into the call's buffer */
    if (ft->is_variadic || !ft->has_prototype || ft->is_ms_abi)
        return false;
    int count = 0;
    inline_limit = f->calls == 1 ? INLINE_ONCE_NODES : INLINE_NODES;
    if (!inline_ok(f->body, &count))
        return false;
    /* not a callee with buffers: its frame is made only when it is called,
     * inlined they would be in every call of the caller (a packet buffer
     * in arp_learn: 1.5 KiB on every path through it) */
    mark_taken(f->body);
    int memory = 0;
    for (struct obj *o = f->locals; o; o = o->next)
        if (!promotable(o) && o->type->size > 0)
            memory += o->type->size;
    for (struct obj *p = f->params; p; p = p->next)
        if (!promotable(p))
            memory += p->type->size;
    return memory <= INLINE_MEMORY;
}

bool inline_only(struct obj *f)
{
    return opt_level && f->calls == 1 && f->refs == 1 && !f->is_constructor && !f->is_destructor && inlinable(f);
}

static struct obj *inline_target(struct node *n)
{
    if (!opt_level || n->lhs->kind != ND_ADDR || n->lhs->lhs->kind != ND_VAR)
        return 0;
    struct obj *f = n->lhs->lhs->var;
    if (f == F->fn || !inlinable(f) || (f->is_avx && !F->fn->is_avx))
        return 0;                               /* (32h: AVX code stays out of SSE code) */
    int depth = 0;
    for (struct inline_frame *u = inlining; u; u = u->up, depth++)
        if (u->fn == f)
            return 0;                           /* (never into itself) */
    if (depth >= INLINE_DEPTH)
        return 0;
    int nparams = 0, nargs = 0;
    for (struct obj *p = f->params; p; p = p->next)
        nparams++;
    for (struct node *a = n->args; a; a = a->next)
        nargs++;
    return nparams == nargs ? f : 0;
}

static int inline_call(struct node *n, struct obj *f)
{
    int nargs = 0;
    for (struct node *a = n->args; a; a = a->next)
        nargs++;
    int *values = arena_alloc(sizeof(int) * (size_t)(nargs + 1)), k = 0;
    for (struct node *a = n->args; a; a = a->next)
        values[k++] = extend_narrow(gen_expr(a), a->type);
    mark_taken(f->body);
    for (struct obj *o = f->locals; o; o = o->next)
        o->inline_pending = true;
    for (struct obj *p = f->params; p; p = p->next)
        p->inline_pending = true;
    struct type *rt = unqual(f->type->return_type);
    int depth = inlining ? inlining->depth + 1 : 1;
    struct obj *ret_obj = 0;
    if (is_memory_value(rt)) {
        if (!n->ret_buffer)
            error_at(n->tok, "a call returning a structure outside a function");
        ret_obj = var(n->ret_buffer);       /* (the caller's, before the callee's names are copies) */
    }
    struct inline_frame frame = { f, 0, new_label_id(), ret_obj, rt, depth, inlining ? inlining->at : n->block_at,
                                  inlining };
    if (rt->kind != TY_VOID && !ret_obj)
        frame.result = new_value(class_of(rt, n->tok));
    inlining = &frame;                          /* (before the parameters' copies: their place) */
    k = 0;
    for (struct obj *p = f->params; p; p = p->next, k++) {
        struct obj *c = var(p);
        if (c->ir_value)
            set(c->ir_value, values[k]);
        else
            store(p->type, local_addr(c), values[k], n->tok);
    }
    gen_stmt(f->body);
    label(frame.end);
    inlining = frame.up;
    for (struct obj *o = f->locals; o; o = o->next)
        o->inline_pending = false, o->inline_copy = 0;
    for (struct obj *p = f->params; p; p = p->next)
        p->inline_pending = false, p->inline_copy = 0;
    if (ret_obj)
        return local_addr(ret_obj);
    return rt->kind == TY_VOID ? 0 : extend_narrow(frame.result, rt);
}

static int gen_expr(struct node *n);
static int gen_addr(struct node *n);
static void gen_cond(struct node *n, int if_true, int if_false);
static void gen_stmt(struct node *n);

static int local_addr(struct obj *o)
{
    int d = new_value(V_I64);
    struct ir *i = emit(IR_LOCAL, 0);
    i->dst = d;
    i->obj = o;
    return d;
}

static int global_addr(const char *sym, int64_t addend, struct obj *o)
{
    int d = new_value(V_I64);
    struct ir *i = emit(IR_GLOBAL, 0);
    i->dst = d;
    i->sym = (char *)sym;
    i->imm = addend;
    i->obj = o;
    return d;
}

static int gen_addr(struct node *n)
{
    switch (n->kind) {
    case ND_VAR:
        if (var(n->var)->ir_value)
            error_at(n->tok, "internal error: the address of a variable kept in a register");
        if (var(n->var)->is_local) {
            int a = local_addr(var(n->var));
            if (var(n->var)->type->kind == TY_VLA)
                return load(ty_ulong, a, n->tok);  /* (the slot holds the array's address) */
            return a;
        }
        if (var(n->var)->is_tls) {
            /* (62b) through the TLS ABI's call: __tls_get_addr or a descriptor */
            int d = new_value(V_I64);
            struct ir *i = emit(IR_TLS_ADDR, n->tok);
            i->dst = d;
            i->sym = var(n->var)->name;
            i->obj = var(n->var);
            return d;
        }
        return global_addr(var(n->var)->name, 0, var(n->var));
    case ND_DEREF:
        return gen_expr(n->lhs);
    case ND_MEMBER:
        /* (the structure's value is its address) */
        return add_offset(gen_expr(n->lhs), n->member->offset);
    case ND_COMMA:
        gen_expr(n->lhs);
        return gen_addr(n->rhs);
    case ND_CALL: case ND_COND: case ND_STMT_EXPR: case ND_ASSIGN: case ND_VA_ARG:
        if (is_memory_value(n->type))
            return gen_expr(n);
        break;
    case ND_CAST:
        if (is_struct_union(n->type))
            return gen_addr(n->lhs);
        break;
    default:
        break;
    }
    /* (62a) __real__ of a computed complex value: its temporary */
    if (is_memory_value(n->type))
        return gen_expr(n);
    error_at(n->tok, "not an lvalue");
}

/* an expression that reads at most (nothing volatile) and changes nothing:
 * which of two such comes first cannot be seen */
static bool effect_free(struct node *n)
{
    if (!n)
        return true;
    if (n->type && (n->type->qual & Q_VOLATILE))
        return false;
    switch (n->kind) {
    case ND_NUM:
        return true;
    case ND_VAR:
        return !(var(n->var)->type->qual & Q_VOLATILE);
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR:
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_NEG: case ND_NOT: case ND_BITNOT: case ND_MEMBER:
    case ND_ADDR: case ND_DEREF: case ND_CAST:
        return effect_free(n->lhs) && effect_free(n->rhs);
    default:
        return false;
    }
}

/* an operand that needs no instructions of its own (or only a constant's) */
static bool simple_value(struct node *n)
{
    while (n->kind == ND_CAST && is_scalar(unqual(n->type)))
        n = n->lhs;
    return n->kind == ND_NUM || (n->kind == ND_VAR && var(n->var)->ir_value);
}

static int gen_bitfield_load(struct member *m, int addr, struct token *tok)
{
    struct type *ty = unqual(m->type);
    if (ty->kind == TY_ENUM)
        ty = ty->enum_int;
    enum ir_class cls = ty->size == 8 ? V_I64 : V_I32;
    int bits = cls == V_I64 ? 64 : 32;
    int d = new_value(cls);
    struct ir *i = emit(IR_LOAD, tok);
    i->dst = d;
    i->a = addr;
    i->size = ty->size;
    i->is_signed = false;
    if (!ty->is_unsigned && ty->kind != TY_BOOL) {
        int up = bits - m->bit_offset - m->bit_width;
        int v = up ? bin(OP_SHL, d, imm(up, cls), cls, tok) : d;
        return bin(OP_SAR, v, imm(bits - m->bit_width, cls), cls, tok);
    }
    int v = m->bit_offset ? bin(OP_SHR, d, imm(m->bit_offset, cls), cls, tok) : d;
    if (m->bit_width == bits)
        return v;
    return bin(OP_AND, v, imm((int64_t)((1ull << m->bit_width) - 1), cls), cls, tok);
}

/* Stores v into the bit-field; the value the assignment has is returned. */
static int gen_bitfield_store(struct member *m, int addr, int v, struct token *tok)
{
    struct type *ty = unqual(m->type);
    if (ty->kind == TY_ENUM)
        ty = ty->enum_int;
    enum ir_class cls = ty->size == 8 ? V_I64 : V_I32;
    int bits = cls == V_I64 ? 64 : 32;
    uint64_t field = m->bit_width == 64 ? ~0ull : ((1ull << m->bit_width) - 1);
    uint64_t mask = field << m->bit_offset;
    if (cls == V_I32)
        mask &= 0xFFFFFFFFull;
    int unit = new_value(cls);
    struct ir *i = emit(IR_LOAD, tok);
    i->dst = unit;
    i->a = addr;
    i->size = ty->size;
    int kept = bin(OP_AND, unit, imm((int64_t)~mask, cls), cls, tok);
    int shifted = m->bit_offset ? bin(OP_SHL, v, imm(m->bit_offset, cls), cls, tok) : v;
    int put = bin(OP_AND, shifted, imm((int64_t)mask, cls), cls, tok);
    int merged = bin(OP_OR, kept, put, cls, tok);
    struct ir *s = emit(IR_STORE, tok);
    s->a = addr;
    s->b = merged;
    s->size = ty->size;
    /* the value: v cut to the field's width (and sign) */
    if (!ty->is_unsigned && ty->kind != TY_BOOL) {
        int up = bits - m->bit_width;
        int x = up ? bin(OP_SHL, v, imm(up, cls), cls, tok) : v;
        return up ? bin(OP_SAR, x, imm(up, cls), cls, tok) : x;
    }
    if (m->bit_width == bits)
        return v;
    return bin(OP_AND, v, imm((int64_t)field, cls), cls, tok);
}

static int gen_compare(struct node *n)
{
    struct type *ty = unqual(n->lhs->type);
    int a = gen_expr(n->lhs), b = gen_expr(n->rhs);
    int sub;
    if (is_int128(ty))
        return compare128(n->kind, ty->is_unsigned, a, b);
    if (is_complex(n->lhs->type) || is_complex(n->rhs->type))
        return compare_complex(n->kind, real_of(is_complex(ty) ? ty : n->rhs->type), n->lhs, a, n->rhs, b);
    if (is_flonum(ty))
        sub = n->kind == ND_EQ ? OP_FEQ : n->kind == ND_NE ? OP_FNE : n->kind == ND_LT ? OP_FLT : OP_FLE;
    else {
        bool u = ty->is_unsigned || ty->kind == TY_PTR;
        sub = n->kind == ND_EQ ? OP_EQ : n->kind == ND_NE ? OP_NE : n->kind == ND_LT ? (u ? OP_ULT : OP_SLT)
                                                                                      : (u ? OP_ULE : OP_SLE);
    }
    return cmp(sub, a, b);
}

static int float_zero(enum ir_class cls)
{
    int z = new_value(cls);
    struct ir *i = emit(IR_FIMM, 0);
    i->dst = z;
    i->fimm = 0;
    return z;
}

static void gen_cond(struct node *n, int if_true, int if_false)
{
    switch (n->kind) {
    case ND_LOGAND: {
        int mid = new_label_id();
        gen_cond(n->lhs, mid, if_false);
        label(mid);
        gen_cond(n->rhs, if_true, if_false);
        return;
    }
    case ND_LOGOR: {
        int mid = new_label_id();
        gen_cond(n->lhs, if_true, mid);
        label(mid);
        gen_cond(n->rhs, if_true, if_false);
        return;
    }
    case ND_NOT:
        gen_cond(n->lhs, if_false, if_true);
        return;
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
        branch(gen_compare(n), if_true, if_false);
        return;
    case ND_NUM:
        if (!is_flonum(n->type)) {
            jump(n->value ? if_true : if_false);
            return;
        }
        break;
    default:
        break;
    }
    int v = gen_expr(n);
    struct type *ty = unqual(n->type);
    if (is_flonum(ty))
        v = cmp(OP_FNE, v, float_zero(class_of(ty, n->tok)));
    else if (is_int128(ty))
        v = nonzero128(v);
    else if (is_complex(ty))
        v = nonzero_complex(v, ty->base);
    else
        v = extend_narrow(v, ty);
    branch(v, if_true, if_false);
}

static int gen_call(struct node *n)
{
    struct obj *inline_fn = inline_target(n);
    if (inline_fn)
        return inline_call(n, inline_fn);
    struct ir_call *c = arena_alloc(sizeof(*c));
    c->func_type = n->func_type;
    int count = 0;
    for (struct node *a = n->args; a; a = a->next)
        count++;
    c->args = arena_alloc(sizeof(struct ir_arg) * (size_t)(count + 1));
    c->nargs = count;
    int fixed = 0;
    for (struct param *p = n->func_type->params; p; p = p->next)
        fixed++;
    c->nfixed = n->func_type->has_prototype ? fixed : count;
    int k = 0;
    for (struct node *a = n->args; a; a = a->next, k++) {
        int v = gen_expr(a);
        v = extend_narrow(v, a->type);
        c->args[k].value = v;
        c->args[k].type = unqual(a->type);
    }
    struct ir *i;
    char *sym = 0;
    struct obj *callee = 0;
    int fv = 0;
    if (n->lhs->kind == ND_ADDR && n->lhs->lhs->kind == ND_VAR && n->lhs->lhs->var->is_function) {
        callee = n->lhs->lhs->var;
        sym = callee->name;
        callee->call_emitted = true;            /* (63c: so its code is written) */
    } else
        fv = gen_expr(n->lhs);
    struct type *rt = unqual(n->func_type->return_type);
    if (is_memory_value(rt)) {
        if (!n->ret_buffer)
            error_at(n->tok, "a call returning a structure outside a function");
        c->ret_buffer = local_addr(var(n->ret_buffer));
    }
    i = emit(IR_CALL, n->tok);
    i->call = c;
    i->sym = sym;
    i->obj = callee;
    i->a = fv;
    i->type = rt;
    if (is_memory_value(rt))
        return c->ret_buffer;
    if (rt->kind == TY_VOID)
        return 0;
    i->dst = new_value(class_of(rt, n->tok));
    /* a char or short result: made a proper int here, whatever the callee did */
    return extend_narrow(i->dst, rt);
}

static int builtin_value(struct node *n)
{
    struct node *a0 = n->args;
    switch (n->builtin) {
    case B_CLZ: case B_CLZL: case B_CTZ: case B_CTZL: case B_POPCOUNT: case B_POPCOUNTL: {
        int v = gen_expr(a0);
        struct ir *i = emit(IR_BUILTIN, n->tok);
        i->sub = n->builtin == B_CLZ || n->builtin == B_CLZL ? BI_CLZ
                 : n->builtin == B_CTZ || n->builtin == B_CTZL ? BI_CTZ : BI_POPCOUNT;
        i->a = v;
        i->size = unqual(a0->type)->size;
        i->dst = new_value(V_I32);
        return i->dst;
    }
    case B_BSWAP16: case B_BSWAP32: case B_BSWAP64: {
        int v = gen_expr(a0);
        struct ir *i = emit(IR_BUILTIN, n->tok);
        i->sub = n->builtin == B_BSWAP16 ? BI_BSWAP16 : n->builtin == B_BSWAP32 ? BI_BSWAP32 : BI_BSWAP64;
        i->a = v;
        i->dst = new_value(n->builtin == B_BSWAP64 ? V_I64 : V_I32);
        return i->dst;
    }
    case B_COMPLEX: {
        int re = gen_expr(a0), im = gen_expr(a0->next);
        return make_complex(unqual(n->type)->base, re, im);
    }
    case B_SQRT: case B_FABS: {
        int v = gen_expr(a0);
        struct ir *i = emit(IR_BUILTIN, n->tok);
        i->sub = n->builtin == B_SQRT ? BI_SQRT : BI_FABS;
        i->a = v;
        i->dst = new_value(class_of(n->type, n->tok));
        return i->dst;
    }
    case B_TRAP: case B_UNREACHABLE:
        emit(IR_TRAP, n->tok);
        return 0;
    case B_ALLOCA: {
        int size = gen_expr(a0);
        struct ir *i = emit(IR_ALLOCA, n->tok);
        i->a = size;
        i->imm = 16;
        i->dst = new_value(V_I64);
        return i->dst;
    }
    case B_ATOMIC_FENCE: {
        struct ir *i = emit(IR_ATOMIC, n->tok);
        i->sub = AT_FENCE;
        return 0;
    }
    default:
        break;
    }
    /* the atomics: (pointer, ...) */
    int p = gen_expr(a0);
    struct type *ty = unqual(a0->type->base);
    struct ir *i;
    int b = 0, c = 0;
    switch (n->builtin) {
    case B_ATOMIC_LOAD:
        i = emit(IR_ATOMIC, n->tok);
        i->sub = AT_LOAD;
        break;
    case B_ATOMIC_STORE:
        b = gen_expr(a0->next);
        i = emit(IR_ATOMIC, n->tok);
        i->sub = AT_STORE;
        break;
    case B_ATOMIC_EXCHANGE:
        b = gen_expr(a0->next);
        i = emit(IR_ATOMIC, n->tok);
        i->sub = AT_XCHG;
        break;
    case B_ATOMIC_CAS:
        b = gen_expr(a0->next);                 /* the expected value's address */
        c = gen_expr(a0->next->next);           /* the desired value */
        i = emit(IR_ATOMIC, n->tok);
        i->sub = AT_CAS;
        break;
    case B_ATOMIC_TEST_AND_SET:
        b = imm(1, V_I32);
        ty = ty_uchar;
        i = emit(IR_ATOMIC, n->tok);
        i->sub = AT_XCHG;
        break;
    case B_ATOMIC_CLEAR:
        b = imm(0, V_I32);
        ty = ty_uchar;
        i = emit(IR_ATOMIC, n->tok);
        i->sub = AT_STORE;
        break;
    default: {
        static const struct {
            enum builtin b;
            int sub;
            bool new_value;
        } ops[] = {
            { B_ATOMIC_ADD_FETCH, AT_ADD, true }, { B_ATOMIC_SUB_FETCH, AT_SUB, true },
            { B_ATOMIC_AND_FETCH, AT_AND, true }, { B_ATOMIC_OR_FETCH, AT_OR, true },
            { B_ATOMIC_XOR_FETCH, AT_XOR, true }, { B_ATOMIC_FETCH_ADD, AT_ADD, false },
            { B_ATOMIC_FETCH_SUB, AT_SUB, false }, { B_ATOMIC_FETCH_AND, AT_AND, false },
            { B_ATOMIC_FETCH_OR, AT_OR, false }, { B_ATOMIC_FETCH_XOR, AT_XOR, false },
        };
        b = gen_expr(a0->next);
        i = 0;
        for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++)
            if (ops[k].b == n->builtin) {
                i = emit(IR_ATOMIC, n->tok);
                i->sub = ops[k].sub;
                i->fetch_new = ops[k].new_value;
            }
        if (!i)
            error_at(n->tok, "internal error: builtin %d", n->builtin);
    }
    }
    i->a = p;
    i->b = b;
    i->c = c;
    i->size = ty->size;
    if (ty->size != 1 && ty->size != 2 && ty->size != 4 && ty->size != 8)
        error_at(n->tok, "atomic operation on a type of %d bytes", ty->size);
    if (n->builtin == B_ATOMIC_TEST_AND_SET) {
        int old = i->dst = new_value(V_I32);
        int zero = imm(0, V_I32);
        return cmp(OP_NE, old, zero);
    }
    if (unqual(n->type)->kind != TY_VOID)
        i->dst = new_value(n->builtin == B_ATOMIC_CAS ? V_I32 : class_of(ty, n->tok));
    return i->dst;
}

static int vla_stack[256];
static int vla_depth;

static int gen_expr(struct node *n)
{
    switch (n->kind) {
    case ND_NUM: {
        enum ir_class cls = class_of(n->type, n->tok);
        if (is_flonum(n->type)) {
            int d = new_value(cls);
            struct ir *i = emit(IR_FIMM, n->tok);
            i->dst = d;
            i->fimm = n->fvalue;
            return d;
        }
        if (is_complex(n->type)) {
            struct type *rt = unqual(n->type)->base;
            return make_complex(rt, float_zero(class_of(rt, n->tok)), fimm_of(class_of(rt, n->tok), n->fvalue));
        }
        if (is_int128(n->type))
            return pair(imm(n->value, V_I64), imm(n->value < 0 && !unqual(n->type)->is_unsigned ? -1 : 0, V_I64));
        return imm(n->value, cls);
    }
    case ND_VAR:
    case ND_DEREF:
        if (n->kind == ND_VAR && var(n->var)->ir_value)
            return var(n->var)->ir_value;            /* (a promoted local: its value itself) */
        if (n->kind == ND_VAR && var(n->var)->is_function)
            return gen_addr(n);
        return load(n->type, gen_addr(n), n->tok);
    case ND_MEMBER: {
        int addr = gen_addr(n);
        if (n->member->is_bitfield)
            return gen_bitfield_load(n->member, addr, n->tok);
        return load(n->type, addr, n->tok);
    }
    case ND_ADDR:
        if (n->lhs->kind == ND_VAR && n->lhs->var->is_function)
            return global_addr(n->lhs->var->name, 0, n->lhs->var);
        return gen_addr(n->lhs);
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR: {
        struct type *ty = unqual(n->type);
        enum ir_class cls = class_of(ty, n->tok);
        int a = gen_expr(n->lhs), b = gen_expr(n->rhs);
        bool u = ty->is_unsigned || ty->kind == TY_PTR;
        if (is_complex(ty))
            return binary_complex(n->kind, ty, n->lhs, a, n->rhs, b, n->tok);
        if ((n->kind == ND_SHL || n->kind == ND_SHR) && is_int128(n->rhs->type))
            b = cast128(b, unqual(n->rhs->type), is_int128(ty) ? ty_ulong : ty, n->tok);
        if (is_int128(ty)) {
            if (n->kind == ND_SHL || n->kind == ND_SHR) {
                if (!is_int128(n->rhs->type))
                    b = gen_cast(b, n->rhs->type, ty_ulong, n->tok);
                return shift128(n->kind == ND_SHL, !u, a, b);
            }
            return binary128(n->kind, ty, a, b, n->tok);
        }
        int sub;
        if (is_flonum(ty))
            sub = n->kind == ND_ADD ? OP_FADD : n->kind == ND_SUB ? OP_FSUB : n->kind == ND_MUL ? OP_FMUL : OP_FDIV;
        else
            switch (n->kind) {
            case ND_ADD: sub = OP_ADD; break;
            case ND_SUB: sub = OP_SUB; break;
            case ND_MUL: sub = OP_MUL; break;
            case ND_DIV: sub = u ? OP_UDIV : OP_SDIV; break;
            case ND_MOD: sub = u ? OP_UMOD : OP_SMOD; break;
            case ND_BITAND: sub = OP_AND; break;
            case ND_BITOR: sub = OP_OR; break;
            case ND_BITXOR: sub = OP_XOR; break;
            case ND_SHL: sub = OP_SHL; break;
            default: sub = u ? OP_SHR : OP_SAR; break;
            }
        if (n->kind == ND_SHL || n->kind == ND_SHR) {
            /* (the count in the shifted value's class) */
            enum ir_class bc = is_int128(n->rhs->type) ? cls : class_of(n->rhs->type, n->tok);
            if (bc != cls)
                b = conv(bc == V_I64 ? CONV_TRUNC : CONV_ZEXT, b, value_size(bc), cls);
        }
        int d = bin(sub, a, b, cls, n->tok);
        /* (39c-2) a signed integer's +, - and *: C says they never overflow
         * (an induction variable may become a pointer) */
        if (!u && is_integer(ty) && (sub == OP_ADD || sub == OP_SUB || sub == OP_MUL))
            F->code[F->ncode - 1].is_signed = true;
        return d;
    }
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
        return gen_compare(n);
    case ND_NEG: {
        struct type *ty = unqual(n->type);
        int a = gen_expr(n->lhs);
        if (is_complex(ty)) {
            enum ir_class rc = class_of(ty->base, n->tok);
            return make_complex(ty->base, fneg(part(a, ty->base, 0), rc), fneg(part(a, ty->base, 1), rc));
        }
        if (is_int128(ty)) {
            int al = half(a, 0), ah = half(a, 1);
            int lo = bin(OP_SUB, imm(0, V_I64), al, V_I64, n->tok);
            int borrow = conv(CONV_ZEXT, cmp(OP_NE, al, imm(0, V_I64)), 4, V_I64);
            return pair(lo, bin(OP_SUB, bin(OP_SUB, imm(0, V_I64), ah, V_I64, n->tok), borrow, V_I64, n->tok));
        }
        int d = new_value(class_of(ty, n->tok));
        struct ir *i = emit(IR_UN, n->tok);
        i->sub = is_flonum(ty) ? OP_FNEG : OP_NEG;
        i->dst = d;
        i->a = a;
        return d;
    }
    case ND_BITNOT: {
        int a = gen_expr(n->lhs);
        if (is_int128(n->type)) {
            int all = imm(-1, V_I64);
            return pair(bin(OP_XOR, half(a, 0), all, V_I64, n->tok), bin(OP_XOR, half(a, 1), all, V_I64, n->tok));
        }
        int d = new_value(class_of(n->type, n->tok));
        struct ir *i = emit(IR_UN, n->tok);
        i->sub = OP_NOT;
        i->dst = d;
        i->a = a;
        return d;
    }
    case ND_NOT: case ND_LOGAND: case ND_LOGOR: {
        int d = new_value(V_I32);
        int t = new_label_id(), f = new_label_id(), end = new_label_id();
        gen_cond(n, t, f);
        label(t);
        set(d, imm(1, V_I32));
        jump(end);
        label(f);
        set(d, imm(0, V_I32));
        label(end);
        return d;
    }
    case ND_COND: {
        struct type *ty = unqual(n->type);
        int t = new_label_id(), f = new_label_id(), end = new_label_id();
        gen_cond(n->cond, t, f);
        if (ty->kind == TY_VOID) {
            label(t);
            gen_expr(n->then);
            jump(end);
            label(f);
            gen_expr(n->els);
            label(end);
            return 0;
        }
        int d = new_value(class_of(ty, n->tok));
        label(t);
        set(d, gen_expr(n->then));
        jump(end);
        label(f);
        set(d, gen_expr(n->els));
        label(end);
        return d;
    }
    case ND_COMMA:
        gen_expr(n->lhs);
        return gen_expr(n->rhs);
    case ND_ASSIGN: {
        if (n->lhs->kind == ND_MEMBER && n->lhs->member->is_bitfield) {
            int addr = gen_addr(n->lhs);
            int v = gen_expr(n->rhs);
            return gen_bitfield_store(n->lhs->member, addr, v, n->tok);
        }
        if (n->lhs->kind == ND_VAR && var(n->lhs->var)->ir_value) {
            int v = gen_expr(n->rhs);
            set(var(n->lhs->var)->ir_value, v);
            return v;
        }
        /* (61c) C leaves the operands' order open: a constant or a
         * variable in a register first, so the address is computed right
         * before the store, where the backends can fold it into it */
        if (!is_memory_value(n->type) && (simple_value(n->rhs) || (effect_free(n->lhs) && effect_free(n->rhs)))) {
            int v = gen_expr(n->rhs);
            store(n->lhs->type, gen_addr(n->lhs), v, n->tok);
            return v;
        }
        int addr = gen_addr(n->lhs);
        int v = gen_expr(n->rhs);
        store(n->lhs->type, addr, v, n->tok);
        if (is_memory_value(n->type))
            return addr;
        return v;
    }
    case ND_CAST: {
        int v = gen_expr(n->lhs);
        if (unqual(n->type)->kind == TY_VOID)
            return 0;
        if (is_struct_union(n->type))
            return v;
        return gen_cast(v, n->lhs->type, n->type, n->tok);
    }
    case ND_CALL:
        return gen_call(n);
    case ND_STMT_EXPR: {
        struct node *last = n->body;
        while (last && last->next)
            last = last->next;
        for (struct node *s = n->body; s && s != last; s = s->next)
            gen_stmt(s);
        if (!last)
            return 0;
        if (last->kind == ND_EXPR_STMT)
            return gen_expr(last->lhs);
        gen_stmt(last);
        return 0;
    }
    case ND_MEMZERO: {
        struct ir *i;
        if (var(n->var)->ir_value) {
            enum ir_class cls = class_of(var(n->var)->type, n->tok);
            set(var(n->var)->ir_value, is_flonum(var(n->var)->type) ? float_zero(cls) : imm(0, cls));
            return 0;
        }
        int a = local_addr(var(n->var));
        i = emit(IR_MEMZERO, n->tok);
        i->a = a;
        i->imm = var(n->var)->type->size;
        return 0;
    }
    case ND_VLA_ALLOC: {
        int size = gen_expr(n->lhs);
        struct ir *i = emit(IR_ALLOCA, n->tok);
        i->a = size;
        i->imm = var(n->var)->align > 16 ? var(n->var)->align : 16;
        int p = i->dst = new_value(V_I64);
        /* (operands first: emit() may move the code, and i with it) */
        int slot = local_addr(var(n->var));
        struct ir *s = emit(IR_STORE, n->tok);
        s->a = slot;
        s->b = p;
        s->size = 8;
        return 0;
    }
    case ND_VA_START: {
        int a = gen_expr(n->lhs);
        emit(IR_VA_START, n->tok)->a = a;
        return 0;
    }
    case ND_VA_ARG: {
        int a = gen_expr(n->lhs);
        struct ir *i = emit(IR_VA_ARG, n->tok);
        i->a = a;
        i->type = n->type->base;
        i->dst = new_value(V_I64);
        return i->dst;
    }
    case ND_VA_COPY: {
        int d = gen_expr(n->lhs), s = gen_expr(n->rhs);
        struct ir *i = emit(IR_MEMCPY, n->tok);
        i->a = d;
        i->b = s;
        i->imm = target.arch == ARCH_X86_64 ? 24 : 32;
        return 0;
    }
    case ND_VA_END:
        gen_expr(n->lhs);
        return 0;
    case ND_BUILTIN:
        return builtin_value(n);
    case ND_NOP:
        return 0;
    default:
        error_at(n->tok, "internal error: expression %d", n->kind);
    }
}

/* --- statements ---------------------------------------------------------- */

static int return_label;

/* The stack pointer back to where it was outside the VLA blocks being
 * left: those deeper than depth. */
static void leave_vla_blocks(int depth)
{
    if (vla_depth > depth) {
        struct ir *i = emit(IR_SET_SP, 0);
        i->a = vla_stack[depth];
    }
}

struct label_id {
    struct label_id *next;
    char *name;
    int id;
};
static struct label_id *label_ids;

static int named_label(char *unique)
{
    for (struct label_id *l = label_ids; l; l = l->next)
        if (strcmp(l->name, unique) == 0)
            return l->id;
    struct label_id *l = arena_alloc(sizeof(*l));
    l->name = unique;
    l->id = new_label_id();
    l->next = label_ids;
    label_ids = l;
    return l->id;
}

/* The VLA depth of every label (gotos may come before them). */
static void label_depths(struct node *n, int depth)
{
    for (; n; n = n->next) {
        switch (n->kind) {
        case ND_BLOCK:
            label_depths(n->body, depth + n->has_vla);
            break;
        case ND_LABEL:
            n->ir_vla_depth = depth;
            label_depths(n->lhs, depth);
            break;
        case ND_IF:
            label_depths(n->then, depth);
            label_depths(n->els, depth);
            break;
        case ND_FOR: case ND_DO: case ND_SWITCH:
            label_depths(n->init, depth);
            label_depths(n->then, depth);
            break;
        case ND_CASE:
            label_depths(n->lhs, depth);
            break;
        default:
            break;
        }
    }
}

static void gen_switch(struct node *n)
{
    if (is_int128(n->cond->type))
        error_at(n->tok, "switch on __int128 is not supported yet");
    int v = gen_expr(n->cond);
    enum ir_class cls = class_of(n->cond->type, n->tok);
    bool u = unqual(n->cond->type)->is_unsigned;
    n->ir_break = new_label_id();
    n->ir_vla_depth = vla_depth;
    for (struct node *c = n->case_next; c; c = c->case_next) {
        c->ir_label = new_label_id();
        int next = new_label_id();
        if (c->case_begin == c->case_end)
            branch(cmp(OP_EQ, v, imm(c->case_begin, cls)), c->ir_label, next);
        else {
            /* begin <= v <= end as (v - begin) <= (end - begin), unsigned */
            int d = bin(OP_SUB, v, imm(c->case_begin, cls), cls, c->tok);
            int64_t span = c->case_end - c->case_begin;
            if (cls == V_I32)
                span = (uint32_t)span;
            branch(cmp(OP_ULE, d, imm(span, cls)), c->ir_label, next);
        }
        label(next);
    }
    (void)u;
    if (n->default_case) {
        n->default_case->ir_label = new_label_id();
        jump(n->default_case->ir_label);
    } else
        jump(n->ir_break);
    gen_stmt(n->then);
    label(n->ir_break);
}

static void gen_stmt(struct node *n)
{
    switch (n->kind) {
    case ND_IF: {
        int t = new_label_id(), f = new_label_id(), end = new_label_id();
        gen_cond(n->cond, t, f);
        label(t);
        gen_stmt(n->then);
        jump(end);
        label(f);
        if (n->els)
            gen_stmt(n->els);
        label(end);
        return;
    }
    case ND_FOR: {
        int begin = new_label_id(), body = new_label_id();
        n->ir_break = new_label_id();
        n->ir_continue = new_label_id();
        int saved_depth = vla_depth;
        if (n->init)
            gen_stmt(n->init);
        n->ir_vla_depth = vla_depth;
        label(begin);
        if (n->cond)
            gen_cond(n->cond, body, n->ir_break);
        label(body);
        gen_stmt(n->then);
        label(n->ir_continue);
        if (n->inc)
            gen_expr(n->inc);
        jump(begin);
        label(n->ir_break);
        vla_depth = saved_depth;
        return;
    }
    case ND_DO: {
        int body = new_label_id();
        n->ir_break = new_label_id();
        n->ir_continue = new_label_id();
        n->ir_vla_depth = vla_depth;
        label(body);
        gen_stmt(n->then);
        label(n->ir_continue);
        gen_cond(n->cond, body, n->ir_break);
        label(n->ir_break);
        return;
    }
    case ND_SWITCH:
        gen_switch(n);
        return;
    case ND_CASE:
        label(n->ir_label);
        gen_stmt(n->lhs);
        return;
    case ND_BLOCK: {
        int saved_depth = vla_depth;
        if (n->has_vla) {
            if (vla_depth >= 256)
                error_at(n->tok, "too many nested blocks with variable length arrays");
            int sp = new_value(V_I64);
            emit(IR_GET_SP, n->tok)->dst = sp;
            vla_stack[vla_depth++] = sp;
        }
        for (struct node *s = n->body; s; s = s->next)
            gen_stmt(s);
        if (n->has_vla) {
            leave_vla_blocks(saved_depth);
            vla_depth = saved_depth;
        }
        return;
    }
    case ND_BREAK:
        leave_vla_blocks(n->target->ir_vla_depth);
        jump(n->target->ir_break);
        return;
    case ND_CONTINUE:
        leave_vla_blocks(n->target->ir_vla_depth);
        jump(n->target->ir_continue);
        return;
    case ND_GOTO:
        leave_vla_blocks(n->target->ir_vla_depth);
        jump(named_label(n->unique_label));
        return;
    case ND_LABEL:
        label(named_label(n->unique_label));
        gen_stmt(n->lhs);
        return;
    case ND_RETURN: {
        if (inlining) {
            /* (63c) the inlined call's value, then past its body */
            if (n->lhs) {
                int v = gen_expr(n->lhs);
                if (inlining->ret_obj)
                    store(inlining->rt, local_addr(inlining->ret_obj), v, n->tok);   /* (39b) */
                else if (inlining->rt->kind != TY_VOID)
                    set(inlining->result, extend_narrow(v, inlining->rt));
            }
            jump(inlining->end);
            return;
        }
        struct type *rt = unqual(F->fn->type->return_type);
        int v = 0;
        if (n->lhs) {
            v = gen_expr(n->lhs);
            if (rt->kind == TY_VOID)
                v = 0;
            else if (!is_memory_value(rt))
                v = extend_narrow(v, rt);
        }
        struct ir *i = emit(IR_RET, n->tok);
        i->a = v;
        i->type = rt;
        return;
    }
    case ND_EXPR_STMT:
        gen_expr(n->lhs);
        return;
    case ND_ASM: {
        /* (62c) operands as a call's arguments, so every pass sees them used:
         * inputs' values, outputs' and memory operands' addresses; (32h) one
         * given registers: inputs' values, its output the instruction's
         * value, then assigned */
        struct asm_stmt *a = n->asm_info;
        if (asm_in_registers(a)) {
            struct ir_call *c = arena_alloc(sizeof(*c));
            c->args = arena_alloc(sizeof(struct ir_arg) * (size_t)(a->nops + 1));
            c->nargs = c->nfixed = a->nops;
            for (int k = 0; k < a->nops; k++) {
                struct asm_operand *op = &a->ops[k];
                c->args[k].value = op->has_imm || op->is_output ? 0 : extend_narrow(gen_expr(op->expr), op->expr->type);
                c->args[k].type = unqual(op->expr->type);
            }
            int d = a->noutputs ? new_value(class_of(a->ops[0].expr->type, n->tok)) : 0;
            struct ir *i = emit(IR_ASM, n->tok);
            i->call = c;
            i->asm_info = a;
            i->dst = d;
            if (d) {
                struct node *lhs = a->ops[0].expr;
                if (lhs->kind == ND_VAR && var(lhs->var)->ir_value)
                    set(var(lhs->var)->ir_value, d);
                else
                    store(lhs->type, gen_addr(lhs), d, n->tok);
            }
            return;
        }
        struct ir_call *c = arena_alloc(sizeof(*c));
        c->args = arena_alloc(sizeof(struct ir_arg) * (size_t)(a->nops + 1));
        c->nargs = c->nfixed = a->nops;
        for (int k = 0; k < a->nops; k++) {
            struct asm_operand *op = &a->ops[k];
            bool memory = strchr(op->constraint, 'm') && !strchr(op->constraint, 'r');
            int v = 0;
            if (op->has_imm)
                v = 0;
            else if (op->is_output || memory)
                v = gen_addr(op->expr);
            else
                v = extend_narrow(gen_expr(op->expr), op->expr->type);
            c->args[k].value = v;
            c->args[k].type = unqual(op->expr->type);
        }
        struct ir *i = emit(IR_ASM, n->tok);
        i->call = c;
        i->asm_info = a;
        return;
    }
    default:
        error_at(n->tok, "internal error: statement %d", n->kind);
    }
}

/* (32h) Inline assembly the x86-64 backend gives registers as it does an
 * instruction's (no barrier: values stay in their registers around it):
 * at most one output, "=x" or "=r"; inputs "x", "r" or immediates; no
 * memory operand, no "memory" clobber, clobbers only xmm0-xmm7 (no value
 * lives there) or "cc"; and an "x" operand at all (other statements keep
 * 62c's barrier, as before). */
bool asm_in_registers(struct asm_stmt *a)
{
    if (target.arch != ARCH_X86_64 || a->is_basic || a->noutputs > 1)
        return false;
    bool vector = false;
    for (int k = 0; k < a->nops; k++) {
        struct asm_operand *op = &a->ops[k];
        if (op->has_imm)
            continue;
        if (op->is_inout || (strcmp(op->constraint, "x") != 0 && strcmp(op->constraint, "r") != 0))
            return false;
        if (op->is_output && k != 0)
            return false;
        vector |= strcmp(op->constraint, "x") == 0;
    }
    if (!vector)
        return false;
    for (int j = 0; j < a->nclobbers; j++) {
        const char *c = a->clobbers[j];
        if (strcmp(c, "cc") == 0)
            continue;
        if ((strncmp(c, "xmm", 3) == 0 || strncmp(c, "ymm", 3) == 0) && c[3] >= '0' && c[3] <= '7' && !c[4])
            continue;
        return false;
    }
    return true;
}

/* --- locals in registers (61a) ------------------------------------------- */

static void mark_taken(struct node *n)
{
    for (; n; n = n->next) {
        if (n->kind == ND_ADDR && n->lhs) {
            /* (a compound literal is (init, var): its variable, 62d) */
            struct node *x = n->lhs;
            while (x->kind == ND_COMMA && x->rhs)
                x = x->rhs;
            if (x->kind == ND_VAR)
                x->var->address_taken = true;
        }
        if (n->kind == ND_ASM)
            for (int k = 0; k < n->asm_info->nops; k++) {
                /* (62c) an output's or memory operand's variable lives in memory */
                struct asm_operand *op = &n->asm_info->ops[k];
                struct node *e = op->expr;
                while (e->kind == ND_MEMBER || (e->kind == ND_CAST && e->lhs))
                    e = e->lhs;
                if ((op->is_output || (strchr(op->constraint, 'm') && !strchr(op->constraint, 'r'))) &&
                    e->kind == ND_VAR && !asm_in_registers(n->asm_info))
                    e->var->address_taken = true;
                mark_taken(op->expr);
            }
        mark_taken(n->lhs);
        mark_taken(n->rhs);
        mark_taken(n->cond);
        mark_taken(n->then);
        mark_taken(n->els);
        mark_taken(n->init);
        mark_taken(n->inc);
        mark_taken(n->body);
        mark_taken(n->args);
    }
}

/* A scalar local (or parameter) whose address is never taken lives in a
 * value of its own, which the backend may keep in a register; volatile
 * ones stay in memory (setjmp's rule). */
static bool promotable(struct obj *o)
{
    struct type *t = o->type;
    if (o->address_taken || (t->qual & Q_VOLATILE) || !o->name)
        return false;
    t = unqual(t);
    return is_integer(t) ? t->kind != TY_INT128
                         : t->kind == TY_PTR || t->kind == TY_FLOAT || t->kind == TY_DOUBLE || t->is_vector;
}

static void promote_locals(struct obj *fn)
{
    mark_taken(fn->body);
    for (struct obj *o = fn->locals; o; o = o->next)
        if (promotable(o))
            o->ir_value = new_value(class_of(o->type, o->tok));
    /* a parameter's value comes from where the prologue stored it */
    for (struct obj *p = fn->params; p; p = p->next)
        if (promotable(p)) {
            int v = load(p->type, local_addr(p), p->tok);
            p->ir_value = new_value(class_of(p->type, p->tok));
            set(p->ir_value, v);
        }
}

/* how often each value is set and used, and where it was set last */
static void count_values(int *defs, int *def_at, int *uses)
{
    for (int k = 0; k < F->ncode; k++) {
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
}

/* --- constants (61c) ------------------------------------------------------ */

static bool int_class(int v)
{
    return F->values[v].cls == V_I32 || F->values[v].cls == V_I64;
}

/* a value's bits as its class holds them: 32-bit ones sign-extended, as
 * IR_IMM keeps them */
static int64_t in_class(int v, uint64_t x)
{
    return F->values[v].cls == V_I32 ? (int64_t)(int32_t)(uint32_t)x : (int64_t)x;
}

static bool known(const int *defs, const int *def_at, int v, int64_t *c)
{
    if (v <= 0 || defs[v] != 1 || !int_class(v))
        return false;
    struct ir *d = &F->code[def_at[v]];
    if (d->op != IR_IMM)
        return false;
    *c = d->imm;
    return true;
}

/* what instruction i makes of constant operands, when it can be said */
static bool evaluate(struct ir *i, int64_t a, int64_t b, bool have_b, int64_t *out)
{
    int width = F->values[i->dst].cls == V_I64 ? 64 : 32;
    uint64_t mask = width == 64 ? ~0ull : 0xFFFFFFFFull;
    uint64_t ua = (uint64_t)a & mask, ub = (uint64_t)b & mask;
    int64_t sa = in_class(i->dst, ua), sb = in_class(i->dst, ub);
    switch (i->op) {
    case IR_CONV: {
        int bits = i->size * 8;
        uint64_t low = bits >= 64 ? (uint64_t)a : (uint64_t)a & ((1ull << bits) - 1);
        if (i->sub == CONV_ZEXT)
            *out = in_class(i->dst, low);
        else if (i->sub == CONV_SEXT)
            *out = in_class(i->dst, bits >= 64 ? low : (uint64_t)(((int64_t)(low << (64 - bits))) >> (64 - bits)));
        else if (i->sub == CONV_TRUNC)
            *out = in_class(i->dst, (uint64_t)a);
        else
            return false;
        return true;
    }
    case IR_UN:
        if (i->sub == OP_NEG)
            *out = in_class(i->dst, 0 - ua);
        else if (i->sub == OP_NOT)
            *out = in_class(i->dst, ~ua);
        else
            return false;
        return true;
    case IR_CMP: {
        if (!have_b)
            return false;
        /* (the operands' class, not the result's) */
        int ow = F->values[i->a].cls == V_I64 ? 64 : 32;
        uint64_t m = ow == 64 ? ~0ull : 0xFFFFFFFFull, x = (uint64_t)a & m, y = (uint64_t)b & m;
        int64_t sx = ow == 64 ? (int64_t)x : (int64_t)(int32_t)x, sy = ow == 64 ? (int64_t)y : (int64_t)(int32_t)y;
        switch (i->sub) {
        case OP_EQ: *out = x == y; break;
        case OP_NE: *out = x != y; break;
        case OP_SLT: *out = sx < sy; break;
        case OP_SLE: *out = sx <= sy; break;
        case OP_ULT: *out = x < y; break;
        case OP_ULE: *out = x <= y; break;
        default: return false;
        }
        return true;
    }
    case IR_BIN:
        if (!have_b)
            return false;
        switch (i->sub) {
        case OP_ADD: *out = in_class(i->dst, ua + ub); return true;
        case OP_SUB: *out = in_class(i->dst, ua - ub); return true;
        case OP_MUL: *out = in_class(i->dst, ua * ub); return true;
        case OP_AND: *out = in_class(i->dst, ua & ub); return true;
        case OP_OR: *out = in_class(i->dst, ua | ub); return true;
        case OP_XOR: *out = in_class(i->dst, ua ^ ub); return true;
        case OP_SHL: if (ub >= (uint64_t)width) return false; *out = in_class(i->dst, ua << ub); return true;
        case OP_SHR: if (ub >= (uint64_t)width) return false; *out = in_class(i->dst, ua >> ub); return true;
        case OP_SAR:
            if (ub >= (uint64_t)width)
                return false;
            *out = in_class(i->dst, (uint64_t)(sa >> ub));
            return true;
        case OP_UDIV: case OP_UMOD:
            if (!ub)
                return false;
            *out = in_class(i->dst, i->sub == OP_UDIV ? ua / ub : ua % ub);
            return true;
        case OP_SDIV: case OP_SMOD:
            if (!sb || (sb == -1 && sa == (width == 64 ? INT64_MIN : INT32_MIN)))
                return false;
            *out = in_class(i->dst, (uint64_t)(i->sub == OP_SDIV ? sa / sb : sa % sb));
            return true;
        }
        return false;
    default:
        return false;
    }
}

static void make_imm(struct ir *i, int64_t c)
{
    i->op = IR_IMM;
    i->imm = c;
    i->a = i->b = i->c = 0;
    i->sub = 0;
}

static bool pure(struct ir *i)
{
    switch (i->op) {
    case IR_IMM: case IR_FIMM: case IR_LOCAL: case IR_GLOBAL: case IR_COPY: case IR_UN: case IR_CMP:
        return true;
    case IR_BIN:
        /* (a division may trap: it stays) */
        return i->sub != OP_SDIV && i->sub != OP_UDIV && i->sub != OP_SMOD && i->sub != OP_UMOD;
    case IR_CONV:
        return true;
    default:
        return false;
    }
}

/* Constant operands worked out at compile time, additions of constants
 * merged ((p + 8) + 4 is p + 12; a global's address plus a constant is
 * one address), multiplications by 1 and additions of 0 made copies, then
 * whatever is no longer used dropped. Only values set once count. */
static void fold_constants(void)
{
    int nv = F->nvalues;
    int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
    int *uses = calloc((size_t)nv, sizeof(int));
    count_values(defs, def_at, uses);
    for (int k = 0; k < F->ncode; k++) {
        struct ir *i = &F->code[k];
        int64_t a, b, c;
        /* (39a) an integer constant made a float or double (float s = 0):
         * the constant itself, when a double holds it exactly (then one
         * rounding to float, as the conversion's) */
        if (i->dst && defs[i->dst] == 1 && i->op == IR_CONV && (i->sub == CONV_S2F || i->sub == CONV_U2F) &&
            (F->values[i->dst].cls == V_F32 || F->values[i->dst].cls == V_F64) && known(defs, def_at, i->a, &a)) {
            bool u = i->sub == CONV_U2F;
            uint64_t x = i->size == 4 ? (u ? (uint64_t)(uint32_t)a : (uint64_t)(int64_t)(int32_t)a) : (uint64_t)a;
            bool neg = !u && (int64_t)x < 0;
            uint64_t mag = neg ? -x : x;
            if (mag <= (1ull << 53)) {
                i->op = IR_FIMM;
                i->fimm = neg ? -(double)mag : (double)mag;
                i->a = 0;
                i->sub = 0;
                i->size = 0;
                continue;
            }
        }
        if (!i->dst || defs[i->dst] != 1 || !int_class(i->dst))
            continue;
        if (i->op == IR_CONV || i->op == IR_UN || i->op == IR_BIN || i->op == IR_CMP) {
            if (i->op == IR_CONV && !int_class(i->a))
                continue;
            if (i->op == IR_CMP && !int_class(i->a))
                continue;
            bool have_a = known(defs, def_at, i->a, &a), have_b = i->b > 0 && known(defs, def_at, i->b, &b);
            if (have_a && evaluate(i, a, have_b ? b : 0, have_b, &c)) {
                make_imm(i, c);
                continue;
            }
        }
        /* a constant goes second in what does not care about the order */
        if (i->op == IR_BIN && known(defs, def_at, i->a, &a) && !known(defs, def_at, i->b, &b) &&
            (i->sub == OP_ADD || i->sub == OP_MUL || i->sub == OP_AND || i->sub == OP_OR || i->sub == OP_XOR) &&
            i->b > 0) {
            int t = i->a;
            i->a = i->b;
            i->b = t;
        }
        if (i->op != IR_BIN || !known(defs, def_at, i->b, &b))
            continue;
        if ((i->sub == OP_ADD || i->sub == OP_SUB || i->sub == OP_OR || i->sub == OP_XOR || i->sub == OP_SHL ||
             i->sub == OP_SHR || i->sub == OP_SAR) && b == 0) {
            i->op = IR_COPY;
            i->b = 0;
            continue;
        }
        if ((i->sub == OP_MUL || i->sub == OP_SDIV || i->sub == OP_UDIV) && b == 1) {
            i->op = IR_COPY;
            i->b = 0;
            continue;
        }
        if (i->sub != OP_ADD || F->values[i->dst].cls != V_I64 || i->a <= 0 || defs[i->a] != 1)
            continue;
        struct ir *p = &F->code[def_at[i->a]];
        int64_t inner;
        if (p->op == IR_GLOBAL && uses[i->a] == 1) {
            /* the global's address with the sum: one instruction */
            int64_t sum = (int64_t)((uint64_t)p->imm + (uint64_t)b);
            if (b < INT32_MIN / 2 || b > INT32_MAX / 2 || sum < INT32_MIN / 2 || sum > INT32_MAX / 2)
                continue;
            i->op = IR_GLOBAL;
            i->sym = p->sym;
            i->obj = p->obj;
            i->imm = sum;
            i->a = i->b = 0;
            i->sub = 0;
            p->op = IR_NOP;
            p->dst = 0;
            continue;
        }
        if (p->op == IR_BIN && p->sub == OP_ADD && known(defs, def_at, p->b, &inner) &&
            F->values[p->dst].cls == V_I64 && p->a > 0) {
            /* (p + c1) + c2: the constant sum made in place of c2's value;
             * (40e) p + c1 kept when it has other uses, if p is not set
             * between (an unrolled copy's p - 8 and its .im's p - 4) */
            struct ir *cb = &F->code[def_at[i->b]];
            if (uses[i->b] != 1 || F->values[i->b].cls != V_I64)
                continue;
            bool keep = uses[i->a] != 1;
            if (keep) {
                bool clear = true;
                for (int q = def_at[i->a] + 1; q < k && clear; q++)
                    clear = F->code[q].dst != p->a;
                if (!clear)
                    continue;
            }
            cb->imm = (int64_t)((uint64_t)inner + (uint64_t)b);
            uses[i->a]--;
            i->a = p->a;
            uses[p->a]++;
            if (!keep) {
                p->op = IR_NOP;
                p->dst = p->a = p->b = 0;
            }
        }
    }
    /* unused results of instructions without effects: gone, last first so
     * their operands' counts fall too */
    memset(uses, 0, sizeof(int) * (size_t)nv);
    memset(defs, 0, sizeof(int) * (size_t)nv);
    count_values(defs, def_at, uses);
    for (int k = F->ncode - 1; k >= 0; k--) {
        struct ir *i = &F->code[k];
        if (!i->dst || uses[i->dst] || !pure(i))
            continue;
        int ops[3] = { i->a, i->b, i->c };
        for (int j = 0; j < 3; j++)
            if (ops[j] > 0)
                uses[ops[j]]--;
        i->op = IR_NOP;
        i->dst = i->a = i->b = i->c = 0;
    }
    free(defs);
    free(def_at);
    free(uses);
}

/* (61c) an extension of what a load extended the same way already (a
 * signed char loaded and widened to int again) is a copy */
static void drop_extensions(void)
{
    int nv = F->nvalues;
    int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
    int *uses = calloc((size_t)nv, sizeof(int));
    count_values(defs, def_at, uses);
    for (int k = 0; k < F->ncode; k++) {
        struct ir *c = &F->code[k];
        if (c->op != IR_CONV || (c->sub != CONV_SEXT && c->sub != CONV_ZEXT) || c->a <= 0 || defs[c->a] != 1 ||
            F->values[c->a].cls != F->values[c->dst].cls)
            continue;
        struct ir *l = &F->code[def_at[c->a]];
        if (l->op == IR_LOAD && l->size <= c->size && l->is_signed == (c->sub == CONV_SEXT))
            c->op = IR_COPY;
        else if (l->op == IR_LOAD && l->size == c->size && l->size < 4 && uses[c->a] == 1) {
            /* the other extension is the load's only use: the load does it */
            l->is_signed = c->sub == CONV_SEXT;
            c->op = IR_COPY;
        }
    }
    free(defs);
    free(def_at);
    free(uses);
}

static bool reads(struct ir *x, int v)
{
    if (x->a == v || x->b == v || x->c == v)
        return true;
    if (x->call) {
        for (int q = 0; q < x->call->nargs; q++)
            if (x->call->args[q].value == v)
                return true;
        if (x->call->ret_buffer == v)
            return true;
    }
    return false;
}

static void replace_use(struct ir *x, int from, int to)
{
    if (x->a == from)
        x->a = to;
    if (x->b == from)
        x->b = to;
    if (x->c == from)
        x->c = to;
    if (x->call) {
        for (int q = 0; q < x->call->nargs; q++)
            if (x->call->args[q].value == from)
                x->call->args[q].value = to;
        if (x->call->ret_buffer == from)
            x->call->ret_buffer = to;
    }
}

/* (61c) "d = a; ... OP d" with d used only there, later in the block and
 * a not set in between: OP reads a itself ("old = i; i = old + 1" of i++
 * becomes "i = i + 1"; OP may set a, as operands are read first) */
static void forward_copies(void)
{
    int nv = F->nvalues;
    int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
    int *uses = calloc((size_t)nv, sizeof(int));
    count_values(defs, def_at, uses);
    for (int k = 0; k < F->ncode; k++) {
        struct ir *c = &F->code[k];
        if (c->op != IR_COPY || c->a <= 0 || defs[c->dst] != 1 || uses[c->dst] != 1 ||
            F->values[c->a].cls != F->values[c->dst].cls)
            continue;
        int a = c->a, d = c->dst;
        for (int m = k + 1; m < F->ncode; m++) {
            struct ir *x = &F->code[m];
            if (x->op == IR_LABEL)
                break;
            if (reads(x, d)) {
                replace_use(x, d, a);
                c->op = IR_NOP;
                c->dst = c->a = 0;
                break;
            }
            if (x->dst == a || x->op == IR_JMP || x->op == IR_BR || x->op == IR_RET)
                break;
        }
    }
    free(defs);
    free(def_at);
    free(uses);
}

/* (61b) "a = OP ...; d = a" with a used only there: OP writes d itself.
 * Every backend reads an instruction's operands before it writes its
 * result, so d may also be an operand of OP. Only inside one block: with
 * a label between them a loop may come back to the copy after writing d
 * elsewhere (awk's `for (r = repl; ...)` inside a `while`: r was set once,
 * before the while, and the second gsub match found it at repl's end). */
static void coalesce_copies(void)
{
    int nv = F->nvalues;
    int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
    int *uses = calloc((size_t)nv, sizeof(int));
    count_values(defs, def_at, uses);
    for (int k = 0; k < F->ncode; k++) {
        struct ir *c = &F->code[k];
        if (c->op != IR_COPY)
            continue;
        int a = c->a, d = c->dst;
        if (defs[a] != 1 || uses[a] != 1 || F->values[a].cls != F->values[d].cls)
            continue;
        int j = def_at[a];
        if (j >= k)
            continue;
        struct ir *p = &F->code[j];
        switch (p->op) {
        case IR_BIN: case IR_UN: case IR_CMP: case IR_CONV: case IR_LOAD: case IR_IMM: case IR_FIMM:
        case IR_LOCAL: case IR_GLOBAL: case IR_COPY:
            break;
        default:
            continue;
        }
        /* d untouched in between */
        bool clear = true;
        for (int m = j + 1; m < k && clear; m++) {
            struct ir *x = &F->code[m];
            if (x->dst == d || x->a == d || x->b == d || x->c == d || x->op == IR_LABEL || x->op == IR_JMP ||
                x->op == IR_BR || x->op == IR_RET)
                clear = false;
            if (x->call) {
                for (int q = 0; q < x->call->nargs; q++)
                    if (x->call->args[q].value == d)
                        clear = false;
                if (x->call->ret_buffer == d)
                    clear = false;
            }
        }
        if (!clear)
            continue;
        p->dst = d;
        c->op = IR_NOP;
        c->dst = c->a = 0;
        defs[d]++;
    }
    free(defs);
    free(def_at);
    free(uses);
}

/* (39b-2) Small structures (and unions, __int128, _Complex) as values: a
 * local whose address only reaches loads, stores, copies and zeroing of
 * its own bytes (no call, no pointer stored, no other arithmetic; a
 * value set once to it, as an inlined callee's pointer parameter, is the
 * address too) becomes a
 * value per field, so a struct { float re, im; } lives in two registers
 * (an inlined complex multiply then needs no memory at all). The fields
 * are the places the code loads and stores; the bytes between them (and
 * members never named) get integer fields of their own, so a copy still
 * carries every byte. Locals copied into each other share one layout (an
 * inlined call's parameters and result buffer). Not the function's own
 * parameters: the prologue writes those to memory. */
#define SPLIT_SIZE 64
#define SPLIT_FIELDS 16

struct split_field {
    int off, size;
    enum ir_class cls;
};

struct split {
    struct obj *o;
    int size;
    bool bad;
    int group;                                  /* (union-find: the group's first) */
    struct split_field *fields;                 /* (the group's layout, on its first) */
    int nfields;
    int *values;                                /* per field of the layout */
};

static int split_find(struct split *s, int c)
{
    while (s[c].group != c)
        c = s[c].group = s[s[c].group].group;
    return c;
}

/* a field (off, size, cls) into a layout: false when it overlaps another
 * differently (a union's members, a char of an int) */
static bool split_add(struct split_field **f, int *n, int *cap, int off, int size, enum ir_class cls)
{
    for (int k = 0; k < *n; k++) {
        struct split_field *g = &(*f)[k];
        if (g->off == off && g->size == size && g->cls == cls)
            return true;
        if (off < g->off + g->size && g->off < off + size)
            return false;
    }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *f = realloc(*f, sizeof(**f) * (size_t)*cap);
    }
    (*f)[(*n)++] = (struct split_field){ off, size, cls };
    return true;
}

static int split_cmp(const void *a, const void *b)
{
    return ((const struct split_field *)a)->off - ((const struct split_field *)b)->off;
}

/* the class a field of this access has, or V_NONE when it cannot be one */
static enum ir_class split_class(int size, enum ir_class cls)
{
    if (size < 4)
        return cls == V_I32 ? V_I32 : V_NONE;
    if (cls == V_LD || value_size(cls) != size)
        return V_NONE;
    return cls;
}

static void split_structs(void)
{
    int nv = F->nvalues, nc = F->ncode;
    int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
    int *uses = calloc((size_t)nv, sizeof(int));
    count_values(defs, def_at, uses);
    /* the candidates: every local named by IR_LOCAL, small, not volatile,
     * not a parameter */
    struct split *s = 0;
    int ns = 0, caps = 0;
    int *cand = calloc((size_t)nv, sizeof(int)), *off = calloc((size_t)nv, sizeof(int));  /* (per address value: candidate + 1) */
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        if (i->op != IR_LOCAL)
            continue;
        struct obj *o = i->obj;
        struct type *t = unqual(o->type);
        bool param = false;
        for (struct obj *p = F->fn->params; p; p = p->next)
            param |= p == o;
        if (!is_memory_value(t) || t->size <= 0 || t->size > SPLIT_SIZE || (o->type->qual & Q_VOLATILE) || param)
            continue;
        int c = -1;
        for (int j = 0; j < ns && c < 0; j++)
            if (s[j].o == o)
                c = j;
        if (c < 0) {
            if (ns == caps) {
                caps = caps ? caps * 2 : 16;
                s = realloc(s, sizeof(*s) * (size_t)caps);
            }
            s[ns] = (struct split){ o, t->size, false, ns, 0, 0, 0 };
            c = ns++;
        }
        if (defs[i->dst] != 1)
            s[c].bad = true;                    /* (an address not followed: its memory stays) */
        else {
            cand[i->dst] = c + 1;
            off[i->dst] = 0;
        }
    }
    if (!ns) {
        free(defs), free(def_at), free(uses), free(cand), free(off);
        return;
    }
    /* addresses inside them: one of theirs plus a constant, or a copy of
     * one (an assignment operator's place, named once) */
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        int64_t c;
        if (i->op == IR_COPY && i->a > 0 && cand[i->a] && defs[i->dst] == 1) {
            cand[i->dst] = cand[i->a];
            off[i->dst] = off[i->a];
        } else if (i->op == IR_BIN && i->sub == OP_ADD && i->a > 0 && cand[i->a] && defs[i->dst] == 1 &&
            known(defs, def_at, i->b, &c) && c >= 0 && c < SPLIT_SIZE) {
            cand[i->dst] = cand[i->a];
            off[i->dst] = off[i->a] + (int)c;
        }
    }
    /* every use of such an address: an access, a copy or a zeroing of a
     * whole field range, or the addition above; anything else keeps the
     * local in memory */
    struct split_field **acc = calloc((size_t)ns, sizeof(*acc));
    int *nacc = calloc((size_t)ns, sizeof(int)), *capacc = calloc((size_t)ns, sizeof(int));
#define BAD(v) (s[cand[v] - 1].bad = true)
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        int ops[3] = { i->a, i->b, i->c };
        for (int j = 0; j < 3; j++) {
            int v = ops[j];
            if (v <= 0 || !cand[v])
                continue;
            struct split *x = &s[cand[v] - 1];
            int at = off[v];
            if (i->op == IR_LOAD && j == 0) {
                enum ir_class cls = split_class(i->size, F->values[i->dst].cls);
                if (cls == V_NONE || at + i->size > x->size ||
                    !split_add(&acc[cand[v] - 1], &nacc[cand[v] - 1], &capacc[cand[v] - 1], at, i->size, cls))
                    x->bad = true;
            } else if (i->op == IR_STORE && j == 0) {
                enum ir_class cls = split_class(i->size, F->values[i->b].cls);
                if (cls == V_NONE || at + i->size > x->size ||
                    !split_add(&acc[cand[v] - 1], &nacc[cand[v] - 1], &capacc[cand[v] - 1], at, i->size, cls))
                    x->bad = true;
            } else if ((i->op == IR_MEMCPY && j < 2) || (i->op == IR_MEMZERO && j == 0)) {
                if (at + i->imm > x->size)
                    x->bad = true;
                if (i->op == IR_MEMCPY && i->a > 0 && i->b > 0 && cand[i->a] && cand[i->b] &&
                    (cand[i->a] == cand[i->b] || off[i->a] != off[i->b]))
                    BAD(i->a), BAD(i->b);       /* (into itself, or shifted: memory) */
            } else if (((i->op == IR_BIN && i->sub == OP_ADD) || i->op == IR_COPY) && j == 0 && i->dst > 0 &&
                       cand[i->dst])
                ;                               /* (an address inside it, looked at by itself) */
            else
                x->bad = true;
        }
        if (i->call) {
            for (int q = 0; q < i->call->nargs; q++)
                if (i->call->args[q].value > 0 && cand[i->call->args[q].value])
                    BAD(i->call->args[q].value);
            if (i->call->ret_buffer > 0 && cand[i->call->ret_buffer])
                BAD(i->call->ret_buffer);
        }
    }
    /* groups: locals copied into each other */
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        if (i->op == IR_MEMCPY && i->a > 0 && i->b > 0 && cand[i->a] && cand[i->b] && !s[cand[i->a] - 1].bad &&
            !s[cand[i->b] - 1].bad) {
            int a = split_find(s, cand[i->a] - 1), b = split_find(s, cand[i->b] - 1);
            if (a != b)
                s[b].group = a;
        }
    }
    /* each group's layout: every member's accesses, then the bytes between
     * as integer fields; a group with a member in memory, overlapping
     * accesses or too many fields stays in memory */
    for (int c = 0; c < ns; c++) {
        int g = split_find(s, c);
        if (s[c].bad)
            s[g].bad = true;
    }
    for (int c = 0; c < ns; c++) {
        int g = split_find(s, c);
        if (s[g].bad)
            continue;
        int capf = s[g].nfields;            /* (at most what is allocated: split_add grows it) */
        for (int k = 0; k < nacc[c] && !s[g].bad; k++)
            if (!split_add(&s[g].fields, &s[g].nfields, &capf, acc[c][k].off, acc[c][k].size, acc[c][k].cls))
                s[g].bad = true;
        if (s[c].size > s[g].size)
            s[g].size = s[c].size;
    }
    for (int g = 0; g < ns; g++) {
        if (split_find(s, g) != g || s[g].bad)
            continue;
        int capf = s[g].nfields;
        qsort(s[g].fields, (size_t)s[g].nfields, sizeof(struct split_field), split_cmp);
        int n = s[g].nfields, pos = 0;
        for (int k = 0; k <= n; k++) {
            int end = k < n ? s[g].fields[k].off : s[g].size;
            while (pos < end) {
                int c = 8;
                while (c > 1 && (pos % c != 0 || pos + c > end))
                    c /= 2;
                split_add(&s[g].fields, &s[g].nfields, &capf, pos, c, c == 8 ? V_I64 : V_I32);
                pos += c;
            }
            if (k < n)
                pos = s[g].fields[k].off + s[g].fields[k].size;
        }
        qsort(s[g].fields, (size_t)s[g].nfields, sizeof(struct split_field), split_cmp);
        if (s[g].nfields > SPLIT_FIELDS)
            s[g].bad = true;
    }
    /* a copy or zeroing must take whole fields */
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        if (i->op != IR_MEMCPY && i->op != IR_MEMZERO)
            continue;
        for (int j = 0; j < (i->op == IR_MEMCPY ? 2 : 1); j++) {
            int v = j ? i->b : i->a;
            if (v <= 0 || !cand[v])
                continue;
            struct split *g = &s[split_find(s, cand[v] - 1)];
            if (g->bad)
                continue;
            int lo = off[v], hi = off[v] + (int)i->imm;
            for (int f = 0; f < g->nfields; f++) {
                int a = g->fields[f].off, b = a + g->fields[f].size;
                if (a < hi && lo < b && (a < lo || b > hi))
                    g->bad = true;
            }
        }
    }
    int nsplit = 0;
    for (int c = 0; c < ns; c++) {
        struct split *g = &s[split_find(s, c)];
        s[c].bad = g->bad;
        if (g->bad)
            continue;
        s[c].values = malloc(sizeof(int) * (size_t)(g->nfields + 1));
        for (int f = 0; f < g->nfields; f++)
            s[c].values[f] = new_value(g->fields[f].cls);
        nsplit++;
    }
    if (nsplit) {
        /* the code again, the split locals' addresses gone */
        struct ir *old = F->code;
        F->code = 0;
        F->ncode = F->capcode = 0;
        for (int k = 0; k < nc; k++) {
            struct ir *i = &old[k];
            int ca = i->a > 0 && i->a < nv && cand[i->a] && !s[cand[i->a] - 1].bad ? cand[i->a] - 1 : -1;
            int cb = i->b > 0 && i->b < nv && cand[i->b] && !s[cand[i->b] - 1].bad ? cand[i->b] - 1 : -1;
            int cd = i->dst > 0 && i->dst < nv && cand[i->dst] && !s[cand[i->dst] - 1].bad ? cand[i->dst] - 1 : -1;
            if (cd >= 0 && (i->op == IR_LOCAL || i->op == IR_BIN || i->op == IR_COPY))
                continue;                       /* (an address of a split local) */
            struct split_field *lay = 0;
            int nf = 0;
            if (ca >= 0) {
                lay = s[split_find(s, ca)].fields;
                nf = s[split_find(s, ca)].nfields;
            } else if (cb >= 0) {
                lay = s[split_find(s, cb)].fields;
                nf = s[split_find(s, cb)].nfields;
            }
            int f0 = -1;
            if ((i->op == IR_LOAD || i->op == IR_STORE) && ca >= 0)
                for (int f = 0; f < nf; f++)
                    if (lay[f].off == off[i->a] && lay[f].size == i->size)
                        f0 = f;
            if (i->op == IR_LOAD && ca >= 0) {
                int fv = s[ca].values[f0];
                if (i->size < 4)
                    emit(IR_CONV, i->tok)->sub = i->is_signed ? CONV_SEXT : CONV_ZEXT;
                else
                    emit(IR_COPY, i->tok);
                struct ir *n = &F->code[F->ncode - 1];
                n->dst = i->dst;
                n->a = fv;
                n->size = i->size;
                continue;
            }
            if (i->op == IR_STORE && ca >= 0) {
                struct ir *n = emit(IR_COPY, i->tok);
                n->dst = s[ca].values[f0];
                n->a = i->b;
                continue;
            }
            if (i->op == IR_MEMZERO && ca >= 0) {
                int lo = off[i->a], hi = lo + (int)i->imm;
                for (int f = 0; f < nf; f++)
                    if (lay[f].off >= lo && lay[f].off < hi) {
                        bool fl = lay[f].cls == V_F32 || lay[f].cls == V_F64;
                        struct ir *n = emit(fl ? IR_FIMM : IR_IMM, i->tok);
                        n->dst = s[ca].values[f];
                    }
                continue;
            }
            if (i->op == IR_MEMCPY && (ca >= 0 || cb >= 0)) {
                int lo = off[ca >= 0 ? i->a : i->b], hi = lo + (int)i->imm;
                for (int f = 0; f < nf; f++) {
                    if (lay[f].off < lo || lay[f].off >= hi)
                        continue;
                    int delta = lay[f].off - lo;
                    if (ca >= 0 && cb >= 0) {
                        struct ir *n = emit(IR_COPY, i->tok);
                        n->dst = s[ca].values[f];
                        n->a = s[cb].values[f];
                    } else if (ca >= 0) {
                        int p = delta ? bin(OP_ADD, i->b, imm(delta, V_I64), V_I64, i->tok) : i->b;
                        struct ir *n = emit(IR_LOAD, i->tok);
                        n->dst = s[ca].values[f];
                        n->a = p;
                        n->size = lay[f].size;
                    } else {
                        int p = delta ? bin(OP_ADD, i->a, imm(delta, V_I64), V_I64, i->tok) : i->a;
                        struct ir *n = emit(IR_STORE, i->tok);
                        n->a = p;
                        n->b = s[cb].values[f];
                        n->size = lay[f].size;
                    }
                }
                continue;
            }
            *emit(i->op, i->tok) = *i;
        }
        free(old);
    }
#undef BAD
    for (int c = 0; c < ns; c++) {
        free(acc[c]);
        free(s[c].values);
        if (split_find(s, c) == c)
            free(s[c].fields);
    }
    free(acc), free(nacc), free(capacc), free(s);
    free(defs), free(def_at), free(uses), free(cand), free(off);
}

/* (39c) Common subexpressions inside a block: an arithmetic operation,
 * conversion or builtin whose operands (a constant counts by its value)
 * are those of one made before in the block, none set again in between,
 * is that one (x[k - i].re and x[k - i].im: k - i, its extension and the
 * scaling once). Both values set only there; the later one is not used
 * between the two (a variable read before its only assignment in a loop
 * still has the last pass's value). Not scalings an address folds. (40c)
 * Loads too, of the same address, size and sign, when nothing between may
 * write memory; never a volatile one. */
struct cse_entry {
    int k;                                      /* the instruction */
    bool ca, cb;                                /* operands constant? */
    int64_t va, vb;                             /* their constants or values */
    int base, index, scale;                     /* (40e) a load's address: base + index * scale + disp */
    int64_t disp;
};

static bool cse_scaling(const int *defs, const int *def_at, struct ir *i);

/* (40e) a load's address by its shape: the same base, index, scale and
 * displacement is the same place (each access keeps its own additions,
 * folded into it, so their values differ) */
static void cse_address(const int *defs, const int *def_at, int v, struct cse_entry *e)
{
    int64_t c;
    e->base = v, e->index = 0, e->scale = 0, e->disp = 0;
    struct ir *d = v > 0 && defs[v] == 1 ? &F->code[def_at[v]] : 0;
    if (d && d->op == IR_BIN && d->sub == OP_ADD && F->values[v].cls == V_I64 && d->a > 0 &&
        known(defs, def_at, d->b, &c)) {
        e->disp = c;
        v = e->base = d->a;
        d = defs[v] == 1 ? &F->code[def_at[v]] : 0;
    }
    if (d && d->op == IR_BIN && d->sub == OP_ADD && F->values[v].cls == V_I64 && d->a > 0 && d->b > 0 &&
        defs[d->b] == 1 && cse_scaling(defs, def_at, &F->code[def_at[d->b]])) {
        struct ir *m = &F->code[def_at[d->b]];
        int64_t k = 0;
        known(defs, def_at, m->b, &k);
        if (m->a > 0 && defs[m->a] == 1) {
            e->base = d->a;
            e->index = m->a;
            e->scale = (int)(m->sub == OP_SHL ? 64 + k : k);
        }
    }
}

/* (40e) an addition of a scaled index: free inside an address, kept apart
 * so each access folds its own */
static bool cse_indexed(const int *defs, const int *def_at, struct ir *i)
{
    return i->op == IR_BIN && i->sub == OP_ADD && F->values[i->dst].cls == V_I64 && i->b > 0 && defs[i->b] == 1 &&
           cse_scaling(defs, def_at, &F->code[def_at[i->b]]);
}

static bool cse_kind(struct ir *i)
{
    return i->op == IR_BIN || i->op == IR_UN || i->op == IR_CONV || i->op == IR_BUILTIN ||
           (i->op == IR_LOAD && !i->is_volatile);      /* (40c) */
}

/* (40c) an instruction that may write memory: the loads seen before it
 * may read something else now (no alias analysis: any of them) */
static bool cse_writes(struct ir *i)
{
    switch (i->op) {
    case IR_STORE: case IR_MEMCPY: case IR_MEMZERO: case IR_CALL: case IR_ATOMIC: case IR_ASM: case IR_VA_START:
    case IR_VA_ARG: case IR_TLS_ADDR:
        return true;
    default:
        return false;
    }
}

/* an index scaled by 2, 4 or 8: free inside an address, so made again
 * where it is used (merged, it is a register's copy and a shift) */
static bool cse_scaling(const int *defs, const int *def_at, struct ir *i)
{
    int64_t c;
    if (i->op != IR_BIN || F->values[i->dst].cls != V_I64 || !known(defs, def_at, i->b, &c))
        return false;
    return (i->sub == OP_SHL && c >= 1 && c <= 3) || (i->sub == OP_MUL && (c == 2 || c == 4 || c == 8));
}

static bool cse_scaling(const int *defs, const int *def_at, struct ir *i);

static void cse_operand(const int *defs, const int *def_at, int v, bool *c, int64_t *x)
{
    struct ir *d = v > 0 && defs[v] == 1 ? &F->code[def_at[v]] : 0;
    *c = d && d->op == IR_IMM;
    *x = *c ? d->imm : v;
    /* (40c) a scaling (never merged) as what it scales and by what: two
     * additions of the same scaled index are the same address, so two
     * loads of h[i] one */
    int64_t k;
    if (d && !*c && cse_scaling(defs, def_at, d) && d->a > 0 && defs[d->a] == 1 && known(defs, def_at, d->b, &k))
        *x = -((int64_t)d->a * 64 + (d->sub == OP_SHL ? k : 16 + k)) - 1;
}

static void common_subexpressions(void)
{
    int nv = F->nvalues, nc = F->ncode;
    int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
    int *uses = calloc((size_t)nv, sizeof(int)), *ren = calloc((size_t)nv, sizeof(int));
    count_values(defs, def_at, uses);
    struct cse_entry *t = malloc(sizeof(*t) * 64);
    int nt = 0, capt = 64;
    bool renamed = false;
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        if (i->op == IR_LABEL)
            nt = 0;
        /* operands renamed by merges before */
        if (renamed) {
            if (i->a > 0 && ren[i->a])
                i->a = ren[i->a];
            if (i->b > 0 && ren[i->b])
                i->b = ren[i->b];
            if (i->c > 0 && ren[i->c])
                i->c = ren[i->c];
            if (i->call) {
                for (int q = 0; q < i->call->nargs; q++)
                    if (i->call->args[q].value > 0 && ren[i->call->args[q].value])
                        i->call->args[q].value = ren[i->call->args[q].value];
                if (i->call->ret_buffer > 0 && ren[i->call->ret_buffer])
                    i->call->ret_buffer = ren[i->call->ret_buffer];
            }
        }
        if (cse_kind(i) && i->dst > 0 && defs[i->dst] == 1 && i->a != i->dst && i->b != i->dst && !i->call &&
            !cse_scaling(defs, def_at, i) && !cse_indexed(defs, def_at, i)) {
            struct cse_entry e = { k, false, false, 0, 0, 0, 0, 0, 0 };
            cse_operand(defs, def_at, i->a, &e.ca, &e.va);
            cse_operand(defs, def_at, i->b, &e.cb, &e.vb);
            if (i->op == IR_LOAD)
                cse_address(defs, def_at, i->a, &e);
            int found = -1;
            for (int j = 0; j < nt && found < 0; j++) {
                struct ir *p = &F->code[t[j].k];
                bool same = i->op == IR_LOAD
                                ? t[j].base == e.base && t[j].index == e.index && t[j].scale == e.scale && t[j].disp == e.disp
                                : t[j].ca == e.ca && t[j].va == e.va && t[j].cb == e.cb && t[j].vb == e.vb;
                if (p->op == i->op && p->sub == i->sub && p->size == i->size && p->is_signed == i->is_signed &&
                    F->values[p->dst].cls == F->values[i->dst].cls && (p->b > 0) == (i->b > 0) && same)
                    found = j;
            }
            if (found >= 0) {
                int was = F->code[t[found].k].dst, d = i->dst;
                bool read = false;
                for (int m = t[found].k + 1; m < k && !read; m++)
                    read = reads(&F->code[m], d);
                if (!read) {
                    ren[d] = was;
                    renamed = true;
                    i->op = IR_NOP;
                    i->dst = i->a = i->b = i->c = 0;
                    continue;
                }
            }
        }
        if (cse_writes(i)) {
            int keep = 0;
            for (int j = 0; j < nt; j++)
                if (F->code[t[j].k].op != IR_LOAD)
                    t[keep++] = t[j];
            nt = keep;
        }
        /* a value set here: what was made from its old value is stale */
        if (i->dst > 0) {
            int keep = 0;
            for (int j = 0; j < nt; j++)
                if (!((!t[j].ca && t[j].va == i->dst) || (!t[j].cb && t[j].vb == i->dst) || t[j].base == i->dst ||
                      t[j].index == i->dst))
                    t[keep++] = t[j];
            nt = keep;
        }
        if (cse_kind(i) && i->dst > 0 && defs[i->dst] == 1 && i->a != i->dst && i->b != i->dst && !i->call &&
            !cse_scaling(defs, def_at, i) && !cse_indexed(defs, def_at, i)) {
            if (nt == capt) {
                capt *= 2;
                t = realloc(t, sizeof(*t) * (size_t)capt);
            }
            struct cse_entry e = { k, false, false, 0, 0, 0, 0, 0, 0 };
            cse_operand(defs, def_at, i->a, &e.ca, &e.va);
            cse_operand(defs, def_at, i->b, &e.cb, &e.vb);
            if (i->op == IR_LOAD)
                cse_address(defs, def_at, i->a, &e);
            t[nt++] = e;
        }
        if (i->op == IR_JMP || i->op == IR_BR || i->op == IR_RET || i->op == IR_TRAP)
            nt = 0;
    }
    /* uses before the merged instruction (a loop's earlier part) too */
    if (renamed)
        for (int k = 0; k < nc; k++) {
            struct ir *i = &F->code[k];
            for (int round = 0; round < 2; round++) {
                int *ops[3] = { &i->a, &i->b, &i->c };
                for (int j = 0; j < 3; j++)
                    if (*ops[j] > 0 && ren[*ops[j]])
                        *ops[j] = ren[*ops[j]];
            }
            if (i->call) {
                for (int q = 0; q < i->call->nargs; q++)
                    while (i->call->args[q].value > 0 && ren[i->call->args[q].value])
                        i->call->args[q].value = ren[i->call->args[q].value];
                while (i->call->ret_buffer > 0 && ren[i->call->ret_buffer])
                    i->call->ret_buffer = ren[i->call->ret_buffer];
            }
        }
    free(t);
    free(defs), free(def_at), free(uses), free(ren);
}

/* (39f) SSE2 packed operations for simple loops (x86-64; never in an AVX
 * function): a loop H: (cmp i < n or i <= n; br B, X) B: body ... jmp H
 * whose straight body (i a signed int stepped by 1 at its end, n not set
 * in it) only loads and stores base[i] of 4-byte elements (base not set in
 * the loop), combines them element by element (int: + - & | ^ and shifts
 * by a constant; float: + - * /; with values the loop does not set,
 * broadcast) and adds ints into a variable read nowhere else in the loop
 * gets a loop of 4 elements a pass in front of it:
 *
 *       lim = sext(n) - 3; each sum's vector = 0
 *       if two of its arrays (one stored) start 1-15 bytes apart: to VX
 *       the broadcasts
 *   VH: if sext(i) < lim (<=): VB else VX
 *   VB: the body on 16 bytes at a time, i += 4, jmp VH
 *   VX: each sum += its vector's lanes (an int sum's order does not matter)
 *       the loop itself: what is left
 *
 * A float sum stays a loop of one element (its rounding is its order), and
 * an int multiply too (packed 32-bit products need SSE4.1). */
enum vkind { VK_NONE, VK_INDEX, VK_SCALED, VK_ADDR, VK_VEC, VK_CONST, VK_BASE };   /* (VK_BASE: a frame or global address) */

/* the loops a packed one runs before: they do at most 3 passes after it,
 * not worth unrolling */
static int vector_rest[64], nvector_rest;

static bool vector_once(int H, int J)
{
    int nv = F->nvalues, nc = F->ncode;
    /* the test: constants, a compare of i with n, the branch into the body */
    int T = H + 1;
    while (T < J && (F->code[T].op == IR_IMM || F->code[T].op == IR_FIMM))
        T++;
    struct ir *t = &F->code[T], *br = &F->code[T + 1];
    if (T + 2 >= J || t->op != IR_CMP || br->op != IR_BR || br->a != t->dst || F->code[T + 2].op != IR_LABEL ||
        F->code[T + 2].label != br->label || (t->sub != OP_SLT && t->sub != OP_SLE))
        return false;
    int iv = t->a, bound = t->b, B = T + 2;
    if (iv <= 0 || bound <= 0 || F->values[iv].cls != V_I32 || F->values[bound].cls != V_I32)
        return false;
    int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
    int *uses = calloc((size_t)nv, sizeof(int)), *inside = calloc((size_t)nv, sizeof(int));
    count_values(defs, def_at, uses);
    unsigned char *kind = calloc((size_t)nv, 1);
    int *base = calloc((size_t)nv, sizeof(int));
    bool *set_in = calloc((size_t)nv, sizeof(bool));
    for (int k = H; k <= J; k++)
        if (F->code[k].dst > 0)
            set_in[F->code[k].dst] = true;
    for (int k = H; k <= J; k++) {
        struct ir *x = &F->code[k];
        int ops[3] = { x->a, x->b, x->c };
        for (int o = 0; o < 3; o++)
            if (ops[o] > 0)
                inside[ops[o]]++;
    }
    bool ok = !set_in[bound] || known(defs, def_at, bound, &(int64_t){ 0 });
    int step_at = -1, loads = 0, outs = 0, nred = 0;
    int red[8];
    int64_t c;
    for (int k = B + 1; k < J && ok; k++) {
        struct ir *x = &F->code[k];
        if (x->op == IR_LABEL || x->op == IR_NOP)
            continue;
        if (step_at >= 0) {
            ok = false;                         /* (the step is the body's last) */
            break;
        }
        if (x->call || (x->dst > 0 && x->dst != iv && x->dst != bound && defs[x->dst] != 1 &&
                        !(x->op == IR_BIN && x->a == x->dst)))
            ok = false;
        /* every operand of a vector kind used only here, in the body */
        int ops[3] = { x->a, x->b, x->c };
        for (int o = 0; o < 3; o++)
            if (ops[o] > 0 && kind[ops[o]] && kind[ops[o]] != VK_CONST && inside[ops[o]] != uses[ops[o]])
                ok = false;
        enum vkind ka = x->a > 0 ? kind[x->a] : VK_NONE, kb = x->b > 0 ? kind[x->b] : VK_NONE;
        bool inv_b = x->b > 0 && !set_in[x->b];
        switch (x->op) {
        case IR_IMM: case IR_FIMM:
            kind[x->dst] = VK_CONST;
            break;
        case IR_LOCAL: case IR_GLOBAL:
            kind[x->dst] = VK_BASE;             /* (made again each pass, the same each time) */
            break;
        case IR_CONV:
            if (x->sub == CONV_SEXT && x->size == 4 && x->a == iv && F->values[x->dst].cls == V_I64)
                kind[x->dst] = VK_INDEX;
            else
                ok = false;
            break;
        case IR_LOAD:
            if (ka == VK_ADDR && x->size == 4 && !x->is_volatile &&
                (F->values[x->dst].cls == V_I32 || F->values[x->dst].cls == V_F32)) {
                kind[x->dst] = VK_VEC;
                loads++;
            } else
                ok = false;
            break;
        case IR_STORE:
            if (ka == VK_ADDR && x->size == 4 && kb == VK_VEC && !x->is_volatile)
                outs++;
            else
                ok = false;
            break;
        case IR_BIN: {
            enum ir_class cl = F->values[x->dst].cls;
            if (x->dst == iv) {
                if (x->sub == OP_ADD && x->a == iv && known(defs, def_at, x->b, &c) && c == 1 && x->is_signed)
                    step_at = k;
                else
                    ok = false;
                break;
            }
            if ((x->sub == OP_SHL || x->sub == OP_MUL) && ka == VK_INDEX && known(defs, def_at, x->b, &c) &&
                (x->sub == OP_SHL ? c == 2 : c == 4) && cl == V_I64) {
                kind[x->dst] = VK_SCALED;
                break;
            }
            if (x->sub == OP_ADD && cl == V_I64 && (kb == VK_SCALED || ka == VK_SCALED)) {
                int bs = kb == VK_SCALED ? x->a : x->b;
                if (bs > 0 && (!set_in[bs] || kind[bs] == VK_BASE) && F->values[bs].cls == V_I64) {
                    kind[x->dst] = VK_ADDR;
                    base[x->dst] = bs;
                } else
                    ok = false;
                break;
            }
            /* an int sum: s = s + v, s read nowhere else in the loop */
            if (x->a == x->dst && cl == V_I32 && x->sub == OP_ADD && kb == VK_VEC && inside[x->dst] == 1 &&
                nred < 8) {
                red[nred++] = k;
                break;
            }
            bool fl = cl == V_F32;
            bool sub_ok = fl ? (x->sub == OP_FADD || x->sub == OP_FSUB || x->sub == OP_FMUL || x->sub == OP_FDIV)
                             : cl == V_I32 && (x->sub == OP_ADD || x->sub == OP_SUB || x->sub == OP_AND ||
                                               x->sub == OP_OR || x->sub == OP_XOR);
            bool shift = cl == V_I32 && (x->sub == OP_SHL || x->sub == OP_SHR || x->sub == OP_SAR);
            if (shift && ka == VK_VEC && known(defs, def_at, x->b, &c) && c >= 0 && c < 32) {
                kind[x->dst] = VK_VEC;
                break;
            }
            bool a_ok = ka == VK_VEC || ka == VK_CONST || (x->a > 0 && !set_in[x->a]);
            bool b_ok = kb == VK_VEC || kb == VK_CONST || inv_b;
            if (sub_ok && a_ok && b_ok && (ka == VK_VEC || kb == VK_VEC) && F->values[x->a].cls == cl &&
                F->values[x->b].cls == cl)
                kind[x->dst] = VK_VEC;
            else
                ok = false;
            break;
        }
        default:
            ok = false;
        }
    }
    if (step_at < 0 || !loads || (!outs && !nred))
        ok = false;
    /* labels inside the body: nothing jumps to them */
    for (int k = 0; k < nc && ok; k++) {
        struct ir *x = &F->code[k];
        int tg[2] = { x->op == IR_JMP || x->op == IR_BR ? x->label : 0, x->op == IR_BR ? x->label2 : 0 };
        for (int j = 0; j < 2; j++)
            for (int q = B + 1; q < J && tg[j]; q++)
                if (F->code[q].op == IR_LABEL && F->code[q].label == tg[j])
                    ok = false;
    }
    if (!ok) {
        free(defs), free(def_at), free(uses), free(inside), free(kind), free(base), free(set_in);
        return false;
    }
    /* the arrays: bases stored to, bases read */
    int sb[16], ns = 0, ab[32], na = 0;
    for (int k = B + 1; k < J; k++) {
        struct ir *x = &F->code[k];
        int b = (x->op == IR_LOAD || x->op == IR_STORE) && x->a > 0 ? base[x->a] : 0;
        if (!b)
            continue;
        bool seen = false;
        for (int q = 0; q < na; q++)
            seen |= ab[q] == b;
        if (!seen && na < 32)
            ab[na++] = b;
        seen = false;
        for (int q = 0; q < ns; q++)
            seen |= sb[q] == b;
        if (x->op == IR_STORE && !seen && ns < 16)
            sb[ns++] = b;
    }
    struct ir *old = F->code;
    F->code = 0;
    F->ncode = F->capcode = 0;
    int *map = calloc((size_t)nv, sizeof(int)), *splat = calloc((size_t)nv, sizeof(int));
    int vacc[8];
    for (int k = 0; k < nc; k++) {
        if (k != H) {
            *emit(old[k].op, old[k].tok) = old[k];
            continue;
        }
        struct token *tok = old[T].tok;
        int VS = new_label_id(), VH = new_label_id(), VB = new_label_id(), VX = new_label_id();
        int64_t bc;
        int wn = known(defs, def_at, bound, &bc) ? imm(bc, V_I64) : conv(CONV_SEXT, bound, 4, V_I64);
        int lim = bin(OP_SUB, wn, imm(3, V_I64), V_I64, tok);
        for (int r = 0; r < nred; r++) {
            vacc[r] = new_value(V_V16);
            emit(IR_IMM, tok)->dst = vacc[r];
        }
        /* two arrays 1-15 bytes apart, one of them stored: no vectors */
        int bad = 0;
        for (int s = 0; s < ns; s++)
            for (int q = 0; q < na; q++) {
                if (ab[q] == sb[s])
                    continue;
                int d = bin(OP_SUB, sb[s], ab[q], V_I64, tok);
                int near = cmp(OP_ULT, bin(OP_ADD, d, imm(15, V_I64), V_I64, tok), imm(31, V_I64));
                int apart = cmp(OP_NE, d, imm(0, V_I64));
                int both = bin(OP_AND, near, apart, V_I32, tok);
                bad = bad ? bin(OP_OR, bad, both, V_I32, tok) : both;
            }
        if (bad)
            branch(bad, VX, VS);
        label(VS);
        /* the broadcasts of what the loop does not set */
        for (int q = B + 1; q < J; q++) {
            struct ir *x = &old[q];
            if (x->op != IR_BIN || x->dst <= 0 || kind[x->dst] != VK_VEC || x->sub == OP_SHL || x->sub == OP_SHR ||
                x->sub == OP_SAR)
                continue;
            int ops[2] = { x->a, x->b };
            for (int o = 0; o < 2; o++) {
                int v = ops[o];
                if (kind[v] == VK_VEC || splat[v])
                    continue;
                int s = v;
                if (kind[v] == VK_CONST) {
                    struct ir *cd = &old[def_at[v]];
                    s = cd->op == IR_FIMM ? fimm_of(F->values[v].cls, cd->fimm) : imm(cd->imm, F->values[v].cls);
                }
                splat[v] = new_value(V_V16);
                struct ir *n = emit(IR_VSPLAT, tok);
                n->dst = splat[v];
                n->a = s;
                n->size = 4;
            }
        }
        label(VH);
        int wi = conv(CONV_SEXT, iv, 4, V_I64);
        branch(cmp(old[T].sub, wi, lim), VB, VX);
        label(VB);
        for (int q = B + 1; q < J; q++) {
            struct ir *x = &old[q];
            if (x->op == IR_LABEL || x->op == IR_NOP)
                continue;
            if (q == step_at) {
                int four = imm(4, V_I32);
                struct ir *s = emit(IR_BIN, x->tok);
                s->sub = OP_ADD;
                s->dst = iv;
                s->a = iv;
                s->b = four;
                s->is_signed = true;
                continue;
            }
            bool is_red = false;
            for (int r = 0; r < nred; r++)
                if (red[r] == q) {
                    struct ir *n = emit(IR_BIN, x->tok);
                    n->sub = OP_ADD;
                    n->dst = vacc[r];
                    n->a = vacc[r];
                    n->b = map[x->b];
                    n->size = 4;
                    is_red = true;
                }
            if (is_red)
                continue;
            struct ir *n = emit(x->op, x->tok);
            *n = *x;
            /* a packed operation's scalar operands are their broadcasts (a
             * shift's count stays the constant); anything else the body's
             * own copies */
            bool vop = x->op == IR_BIN && x->dst > 0 && kind[x->dst] == VK_VEC && x->sub != OP_SHL &&
                       x->sub != OP_SHR && x->sub != OP_SAR;
            int *ops[3] = { &n->a, &n->b, &n->c };
            for (int o = 0; o < 3; o++) {
                int v = *ops[o];
                if (v <= 0 || v >= nv)
                    continue;
                if (vop && splat[v])
                    *ops[o] = splat[v];
                else if (map[v])
                    *ops[o] = map[v];
            }
            if (x->dst > 0) {
                enum vkind kd = kind[x->dst];
                map[x->dst] = new_value(kd == VK_VEC ? V_V16 : F->values[x->dst].cls);
                n->dst = map[x->dst];
            }
            if (x->op == IR_LOAD || x->op == IR_STORE)
                n->size = 16;
            if (x->op == IR_BIN && kind[x->dst] == VK_VEC)
                n->size = 4;                    /* (the lanes' width) */
        }
        jump(VH);
        label(VX);
        for (int r = 0; r < nred; r++) {
            int s = old[red[r]].dst;
            int sum = new_value(V_I32);
            struct ir *h = emit(IR_VHADD, tok);
            h->dst = sum;
            h->a = vacc[r];
            struct ir *n = emit(IR_BIN, tok);
            n->sub = OP_ADD;
            n->dst = s;
            n->a = s;
            n->b = sum;
        }
        if (nvector_rest < 64)
            vector_rest[nvector_rest++] = old[k].label;
        *emit(old[k].op, old[k].tok) = old[k];  /* H: the loop itself, for the rest */
    }
    free(old);
    free(map), free(splat);
    free(defs), free(def_at), free(uses), free(inside), free(kind), free(base), free(set_in);
    return true;
}

/* (39f-2) Sums of int16 products, 8 a pass (pmaddwd): a loop as above
 * whose body is acc += a[e1] * b[e2], a and b arrays of signed 2-byte
 * elements, each index i or v - i (v not set in the loop), acc an int read
 * nowhere else in it. A pass loads 8 elements of each (one going down: the
 * 8 below and including its element, its words reversed when only one
 * goes down, so the lanes pair as the plain loop's steps), multiplies them
 * in pairs into 4 int sums and adds those into a vector; after it acc gets
 * the vector's lanes, and the loop itself does the rest. */
enum mkind { MK_NONE, MK_INDEX, MK_DOWN, MK_RINDEX, MK_SCALED, MK_RSCALED, MK_ADDR, MK_RADDR, MK_LOAD, MK_RLOAD, MK_PROD,
             MK_BASE };

static bool madd_once(int H, int J)
{
    int nv = F->nvalues, nc = F->ncode;
    int T = H + 1;
    while (T < J && (F->code[T].op == IR_IMM || F->code[T].op == IR_FIMM))
        T++;
    struct ir *t = &F->code[T], *br = &F->code[T + 1];
    if (T + 2 >= J || t->op != IR_CMP || br->op != IR_BR || br->a != t->dst || F->code[T + 2].op != IR_LABEL ||
        F->code[T + 2].label != br->label || (t->sub != OP_SLT && t->sub != OP_SLE))
        return false;
    int iv = t->a, bound = t->b, B = T + 2;
    if (iv <= 0 || bound <= 0 || F->values[iv].cls != V_I32 || F->values[bound].cls != V_I32)
        return false;
    int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
    int *uses = calloc((size_t)nv, sizeof(int)), *inside = calloc((size_t)nv, sizeof(int));
    count_values(defs, def_at, uses);
    unsigned char *mk = calloc((size_t)nv, 1);
    int *base = calloc((size_t)nv, sizeof(int)), *down = calloc((size_t)nv, sizeof(int));
    bool *set_in = calloc((size_t)nv, sizeof(bool));
    for (int k = H; k <= J; k++)
        if (F->code[k].dst > 0)
            set_in[F->code[k].dst] = true;
    for (int k = H; k <= J; k++) {
        struct ir *x = &F->code[k];
        int ops[3] = { x->a, x->b, x->c };
        for (int o = 0; o < 3; o++)
            if (ops[o] > 0)
                inside[ops[o]]++;
    }
    int64_t c;
    bool ok = !set_in[bound] || known(defs, def_at, bound, &c);
    int step_at = -1, red = -1, prod = 0;
    for (int k = B + 1; k < J && ok; k++) {
        struct ir *x = &F->code[k];
        if (x->op == IR_LABEL || x->op == IR_NOP)
            continue;
        if (step_at >= 0) {
            ok = false;
            break;
        }
        int ops[3] = { x->a, x->b, x->c };
        for (int o = 0; o < 3; o++)
            if (ops[o] > 0 && mk[ops[o]] && inside[ops[o]] != uses[ops[o]])
                ok = false;
        if (x->dst > 0 && x->dst != iv && defs[x->dst] != 1 && !(x->op == IR_BIN && x->a == x->dst))
            ok = false;
        int ka = x->a > 0 ? mk[x->a] : MK_NONE, kb = x->b > 0 ? mk[x->b] : MK_NONE;
        enum ir_class cl = x->dst > 0 ? F->values[x->dst].cls : V_NONE;
        switch (x->op) {
        case IR_IMM:
            break;
        case IR_LOCAL: case IR_GLOBAL:
            mk[x->dst] = MK_BASE;
            break;
        case IR_CONV:
            if (x->sub == CONV_SEXT && x->size == 4 && cl == V_I64 && (x->a == iv || ka == MK_DOWN)) {
                mk[x->dst] = x->a == iv ? MK_INDEX : MK_RINDEX;
                down[x->dst] = down[x->a];
            } else
                ok = false;
            break;
        case IR_LOAD:
            if ((ka == MK_ADDR || ka == MK_RADDR) && x->size == 2 && x->is_signed && cl == V_I32 && !x->is_volatile) {
                mk[x->dst] = ka == MK_ADDR ? MK_LOAD : MK_RLOAD;
                base[x->dst] = base[x->a];
                down[x->dst] = down[x->a];
            } else
                ok = false;
            break;
        case IR_BIN:
            if (x->dst == iv) {
                if (x->sub == OP_ADD && x->a == iv && known(defs, def_at, x->b, &c) && c == 1 && x->is_signed)
                    step_at = k;
                else
                    ok = false;
            } else if (x->sub == OP_SUB && cl == V_I32 && x->is_signed && x->b == iv && x->a > 0 &&
                       (!set_in[x->a] || known(defs, def_at, x->a, &c))) {
                mk[x->dst] = MK_DOWN;
                down[x->dst] = x->a;
            } else if ((x->sub == OP_MUL || x->sub == OP_SHL) && cl == V_I64 && (ka == MK_INDEX || ka == MK_RINDEX) &&
                       known(defs, def_at, x->b, &c) && (x->sub == OP_MUL ? c == 2 : c == 1)) {
                mk[x->dst] = ka == MK_INDEX ? MK_SCALED : MK_RSCALED;
                down[x->dst] = down[x->a];
            } else if (x->sub == OP_ADD && cl == V_I64 && (kb == MK_SCALED || kb == MK_RSCALED || ka == MK_SCALED ||
                                                             ka == MK_RSCALED)) {
                bool bsc = kb == MK_SCALED || kb == MK_RSCALED;
                int bs = bsc ? x->a : x->b, sc = bsc ? x->b : x->a;
                if (bs > 0 && (!set_in[bs] || mk[bs] == MK_BASE) && F->values[bs].cls == V_I64) {
                    mk[x->dst] = mk[sc] == MK_SCALED ? MK_ADDR : MK_RADDR;
                    base[x->dst] = bs;
                    down[x->dst] = down[sc];
                } else
                    ok = false;
            } else if (x->sub == OP_MUL && cl == V_I32 && (ka == MK_LOAD || ka == MK_RLOAD) &&
                       (kb == MK_LOAD || kb == MK_RLOAD) && !prod) {
                mk[x->dst] = MK_PROD;
                prod = k;
            } else if (x->sub == OP_ADD && cl == V_I32 && x->a == x->dst && kb == MK_PROD && inside[x->dst] == 1 &&
                       red < 0)
                red = k;
            else
                ok = false;
            break;
        default:
            ok = false;
        }
    }
    if (step_at < 0 || red < 0 || !prod)
        ok = false;
    for (int k = 0; k < nc && ok; k++) {
        struct ir *x = &F->code[k];
        int tg[2] = { x->op == IR_JMP || x->op == IR_BR ? x->label : 0, x->op == IR_BR ? x->label2 : 0 };
        for (int j = 0; j < 2; j++)
            for (int q = B + 1; q < J && tg[j]; q++)
                if (F->code[q].op == IR_LABEL && F->code[q].label == tg[j])
                    ok = false;
    }
    if (!ok) {
        free(defs), free(def_at), free(uses), free(inside), free(mk), free(base), free(down), free(set_in);
        return false;
    }
    struct ir *pr = &F->code[prod];
    int la = pr->a, lb = pr->b;
    bool ra = mk[la] == MK_RLOAD, rb = mk[lb] == MK_RLOAD;
    int ba = base[la], bb = base[lb], da = down[la], db = down[lb];
    int64_t dac = 0, dbc = 0;
    bool da_k = ra && known(defs, def_at, da, &dac), db_k = rb && known(defs, def_at, db, &dbc);
    int acc = F->code[red].dst;
    struct ir *old = F->code;
    F->code = 0;
    F->ncode = F->capcode = 0;
    for (int k = 0; k < nc; k++) {
        if (k != H) {
            *emit(old[k].op, old[k].tok) = old[k];
            continue;
        }
        struct token *tok = old[T].tok;
        int VH = new_label_id(), VB = new_label_id(), VX = new_label_id();
        int64_t bc;
        int wn = known(defs, def_at, bound, &bc) ? imm(bc, V_I64) : conv(CONV_SEXT, bound, 4, V_I64);
        int lim = bin(OP_SUB, wn, imm(7, V_I64), V_I64, tok);
        int vacc = new_value(V_V16);
        emit(IR_IMM, tok)->dst = vacc;
        label(VH);
        branch(cmp(old[T].sub, conv(CONV_SEXT, iv, 4, V_I64), lim), VB, VX);
        label(VB);
        int vl[2];
        for (int s = 0; s < 2; s++) {
            bool r = s ? rb : ra;
            int bs = s ? bb : ba;
            if (mk[bs] == MK_BASE) {            /* (a frame or global address: made here again) */
                struct ir *n = emit(old[def_at[bs]].op, old[def_at[bs]].tok);
                *n = old[def_at[bs]];
                n->dst = new_value(V_I64);
                bs = n->dst;
            }
            int e = iv;
            if (r) {
                bool kk = s ? db_k : da_k;
                int dv = kk ? imm(s ? dbc : dac, V_I32) : (s ? db : da);
                e = bin(OP_SUB, dv, iv, V_I32, tok);
                F->code[F->ncode - 1].is_signed = true;
            }
            int ad = bin(OP_ADD, bs, bin(OP_MUL, conv(CONV_SEXT, e, 4, V_I64), imm(2, V_I64), V_I64, tok), V_I64, tok);
            if (r)
                ad = bin(OP_ADD, ad, imm(-14, V_I64), V_I64, tok);    /* (the 8 below and including it) */
            vl[s] = new_value(V_V16);
            struct ir *l = emit(IR_LOAD, tok);
            l->dst = vl[s];
            l->a = ad;
            l->size = 16;
            if (r && ra != rb) {                /* (only one goes down: its words the other way round) */
                int w = new_value(V_V16);
                struct ir *v = emit(IR_VREVW, tok);
                v->dst = w;
                v->a = vl[s];
                vl[s] = w;
            }
        }
        int m = new_value(V_V16);
        struct ir *ma = emit(IR_VMADD, tok);
        ma->dst = m;
        ma->a = vl[0];
        ma->b = vl[1];
        struct ir *ad = emit(IR_BIN, tok);
        ad->sub = OP_ADD;
        ad->dst = vacc;
        ad->a = vacc;
        ad->b = m;
        ad->size = 4;
        int eight = imm(8, V_I32);
        struct ir *st = emit(IR_BIN, tok);
        st->sub = OP_ADD;
        st->dst = iv;
        st->a = iv;
        st->b = eight;
        st->is_signed = true;
        jump(VH);
        label(VX);
        int sum = new_value(V_I32);
        struct ir *h = emit(IR_VHADD, tok);
        h->dst = sum;
        h->a = vacc;
        struct ir *n = emit(IR_BIN, tok);
        n->sub = OP_ADD;
        n->dst = acc;
        n->a = acc;
        n->b = sum;
        if (nvector_rest < 64)
            vector_rest[nvector_rest++] = old[k].label;
        *emit(old[k].op, old[k].tok) = old[k];
    }
    free(old);
    free(defs), free(def_at), free(uses), free(inside), free(mk), free(base), free(down), free(set_in);
    return true;
}

static void vectorize_loops(void)
{
    nvector_rest = 0;
    if (target.arch != ARCH_X86_64 || F->fn->is_avx || opt_general_regs)
        return;
    for (int v = 1; v < F->nvalues; v++)
        if (F->values[v].cls == V_V32)
            return;
    int cap = F->nlabels + 64;
    bool *done = calloc((size_t)cap, sizeof(bool));
    for (int rounds = 0; rounds < 64; rounds++) {
        int nlab = F->nlabels + 2, nc = F->ncode;
        if (nlab > cap) {
            done = realloc(done, (size_t)nlab * 2);
            memset(done + cap, 0, (size_t)(nlab * 2 - cap));
            cap = nlab * 2;
        }
        int *label_at = malloc(sizeof(int) * (size_t)nlab);
        for (int l = 0; l < nlab; l++)
            label_at[l] = -1;
        for (int k = 0; k < nc; k++)
            if (F->code[k].op == IR_LABEL)
                label_at[F->code[k].label] = k;
        int H = -1, J = -1;
        for (int k = 0; k < nc && H < 0; k++) {
            struct ir *i = &F->code[k];
            if (i->op == IR_JMP && label_at[i->label] >= 0 && label_at[i->label] < k && !done[i->label]) {
                H = label_at[i->label];
                J = k;
            }
        }
        free(label_at);
        if (H < 0)
            break;
        done[F->code[H].label] = true;
        int before = F->nlabels;
        if (vector_once(H, J) || madd_once(H, J))
            for (int l = before + 1; l <= F->nlabels && l < cap; l++)
                done[l] = true;
    }
    free(done);
}

/* (39c-2) Loops. A loop is a jump back to a label (H, the jump J): entered
 * only by falling into H (every jump to H or to a label inside comes from
 * inside), so code put right before H runs once, before it.
 *
 * What does not change in the loop goes before it: an operation, conversion
 * or builtin (not a division: the loop may run no time) whose operands are
 * not set in the loop, a constant, a frame or global address; its value set
 * only there.
 *
 * Induction variables: a counter i set in the loop only by i = i + c (c a
 * constant; for int, an addition C says cannot overflow) and an address
 * base + sext(e) * scale with base not set in the loop and e one of i,
 * i + v, v + i, i - v, v - i (v not set in it) become a pointer p: p is
 * made before the loop from i's first value and moves by c * scale (- for
 * v - i) right after i's step, so p and i move together and the address,
 * used only right after it is made, is p (x[k - i] of a FIR filter: a
 * pointer going back 8 bytes a pass, the subtraction, the extension and the
 * scaling gone). Not for e = i: one extension serves every array there,
 * and the addressing scales it. One pointer per distinct address. */
#define LOOP_POINTERS 4

struct loop_iv {
    int addr;                                   /* the address instruction */
    int same;                                   /* an earlier one with the same pointer, or -1 */
    int iv, step_at;                            /* i, i's step */
    int e_op, e_sub, v, v_imm;                  /* e: IR_NOP (e = i), or the operation, v its other operand */
    bool v_first, v_const, has_conv;
    int64_t v_c, scale, delta;
    int base, p;
};

static bool loop_pure(struct ir *i)
{
    switch (i->op) {
    case IR_IMM: case IR_FIMM: case IR_UN: case IR_CONV: case IR_BUILTIN: case IR_LOCAL: case IR_GLOBAL:
        return true;
    case IR_BIN:
        return i->sub != OP_SDIV && i->sub != OP_UDIV && i->sub != OP_SMOD && i->sub != OP_UMOD;
    default:
        return false;
    }
}

/* one loop at a time: true when it changed the code */
static bool loop_once(int H, int J, bool with_ivs)
{
    int nv = F->nvalues, nc = F->ncode;
    int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
    int *uses = calloc((size_t)nv, sizeof(int)), *in_loop = calloc((size_t)nv, sizeof(int));
    count_values(defs, def_at, uses);
    for (int k = H; k <= J; k++)
        if (F->code[k].dst > 0)
            in_loop[F->code[k].dst]++;
    bool *hoist = calloc((size_t)nc + 1, sizeof(bool));
    int nhoist = 0;
    for (int k = H; k <= J; k++) {
        struct ir *i = &F->code[k];
        if (!loop_pure(i) || i->dst <= 0 || defs[i->dst] != 1 || i->call)
            continue;
        int ops[3] = { i->a, i->b, i->c };
        bool inv = true;
        for (int j = 0; j < 3; j++)
            if (ops[j] > 0 && in_loop[ops[j]])
                inv = false;
        if (!inv)
            continue;
        hoist[k] = true;
        in_loop[i->dst] = 0;
        nhoist++;
    }
    /* induction variables */
    struct loop_iv ivs[LOOP_POINTERS];
    int niv = 0;
    for (int k = H; k <= J && niv < LOOP_POINTERS && with_ivs; k++) {
        struct ir *a = &F->code[k];
        if (hoist[k] || a->op != IR_BIN || a->sub != OP_ADD || F->values[a->dst].cls != V_I64 || defs[a->dst] != 1 ||
            a->a <= 0 || a->b <= 0 || in_loop[a->a] || !in_loop[a->b] || defs[a->b] != 1)
            continue;
        struct loop_iv x = { .addr = k, .base = a->a, .scale = 1 };
        int s = a->b;
        struct ir *m = &F->code[def_at[s]];
        int64_t c;
        if (m->op == IR_BIN && (m->sub == OP_SHL || m->sub == OP_MUL) && known(defs, def_at, m->b, &c) && m->a > 0 &&
            defs[m->a] == 1 && c >= 0 && (m->sub == OP_MUL || c < 16)) {
            x.scale = m->sub == OP_SHL ? (int64_t)1 << c : c;
            s = m->a;
            m = &F->code[def_at[s]];
        }
        /* s: sext(e) of an int, or an I64 e itself */
        int e = s;
        if (m->op == IR_CONV && m->sub == CONV_SEXT && m->size == 4 && m->a > 0 && F->values[m->a].cls == V_I32) {
            x.has_conv = true;
            e = m->a;
        } else if (F->values[s].cls != V_I64)
            continue;
        enum ir_class ec = F->values[e].cls;
        /* e: i itself, or i with an invariant */
        int iv = e;
        x.e_op = IR_NOP;
        if (defs[e] == 1 && in_loop[e]) {
            struct ir *o = &F->code[def_at[e]];
            if (o->op != IR_BIN || (o->sub != OP_ADD && o->sub != OP_SUB) || o->a <= 0 || o->b <= 0 ||
                (ec == V_I32 && !o->is_signed))
                continue;
            int other;
            if (in_loop[o->b] == 0 || known(defs, def_at, o->b, &c)) {
                iv = o->a;
                other = o->b;
                x.v_first = false;
            } else if (o->sub == OP_ADD || in_loop[o->a] == 0 || known(defs, def_at, o->a, &c)) {
                iv = o->b;
                other = o->a;
                x.v_first = true;
            } else
                continue;
            if (in_loop[other] && !known(defs, def_at, other, &c))
                continue;
            x.e_op = IR_BIN;
            x.e_sub = o->sub;
            x.v = other;
            x.v_const = known(defs, def_at, other, &x.v_c);
        }
        /* (e = i itself: one extension and the addressing's own scaling,
         * cheaper than a pointer per array) */
        if (x.e_op != IR_BIN)
            continue;
        /* i: set once in the loop, by a constant step */
        if (iv <= 0 || F->values[iv].cls != ec || in_loop[iv] != 1 || defs[iv] < 2)
            continue;                           /* (set before the loop too) */
        int st = -1;
        for (int q = H; q <= J; q++)
            if (F->code[q].dst == iv)
                st = q;
        struct ir *step = &F->code[st];
        if (step->op != IR_BIN || (step->sub != OP_ADD && step->sub != OP_SUB) || step->a != iv ||
            !known(defs, def_at, step->b, &c) || (ec == V_I32 && !step->is_signed))
            continue;
        int64_t sign = x.e_op == IR_BIN && x.e_sub == OP_SUB && x.v_first ? -1 : 1;
        x.delta = (step->sub == OP_ADD ? c : -c) * x.scale * sign;
        x.iv = iv;
        x.step_at = st;
        /* the address used only right after it is made, before i's step */
        bool ok = true;
        int seen = 0;
        for (int q = k + 1; q < nc && seen < uses[a->dst] && ok; q++) {
            struct ir *u = &F->code[q];
            if (u->op == IR_LABEL || q == st || u->op == IR_JMP || u->op == IR_BR || u->op == IR_RET)
                ok = false;
            else if (reads(u, a->dst))
                seen++;
        }
        if (!ok || seen != uses[a->dst])
            continue;
        /* the same pointer as one before (x[k - i] loaded twice) */
        x.same = -1;
        for (int n = 0; n < niv && x.same < 0; n++) {
            struct loop_iv *y = &ivs[n];
            if (y->same < 0 && y->base == x.base && y->iv == x.iv && y->e_sub == x.e_sub && y->v_first == x.v_first &&
                y->v_const == x.v_const && (x.v_const ? y->v_c == x.v_c : y->v == x.v) && y->scale == x.scale &&
                y->has_conv == x.has_conv)
                x.same = n;
        }
        ivs[niv++] = x;
    }
    if (!nhoist && !niv) {
        free(defs), free(def_at), free(uses), free(in_loop), free(hoist);
        return false;
    }
    /* the code again: the hoisted instructions and the pointers' first
     * values before H, each pointer's step after its counter's */
    struct ir *old = F->code;
    int *ren = calloc((size_t)nv + LOOP_POINTERS + 1, sizeof(int));
    F->code = 0;
    F->ncode = F->capcode = 0;
    for (int k = 0; k < nc; k++) {
        struct ir *i = &old[k];
        if (k == H) {
            for (int q = H; q <= J; q++)
                if (hoist[q])
                    *emit(old[q].op, old[q].tok) = old[q];
            for (int n = 0; n < niv; n++) {
                struct loop_iv *x = &ivs[n];
                if (x->same >= 0)
                    continue;
                struct token *tok = old[x->addr].tok;
                enum ir_class ec = F->values[x->iv].cls;
                int e = x->iv;
                if (x->e_op == IR_BIN) {
                    int v = x->v_const ? imm(x->v_c, ec) : x->v;
                    e = x->v_first ? bin(x->e_sub, v, x->iv, ec, tok) : bin(x->e_sub, x->iv, v, ec, tok);
                }
                int s = x->has_conv ? conv(CONV_SEXT, e, 4, V_I64) : e;
                if (x->scale != 1)
                    s = bin(OP_MUL, s, imm(x->scale, V_I64), V_I64, tok);
                x->p = new_value(V_I64);
                struct ir *p = emit(IR_BIN, tok);
                p->sub = OP_ADD;
                p->dst = x->p;
                p->a = x->base;
                p->b = s;
            }
        }
        if (hoist[k])
            continue;
        bool is_addr = false;
        for (int n = 0; n < niv; n++)
            if (ivs[n].addr == k) {
                ren[i->dst] = ivs[ivs[n].same >= 0 ? ivs[n].same : n].p;
                is_addr = true;
            }
        if (is_addr)
            continue;
        *emit(i->op, i->tok) = *i;
        for (int n = 0; n < niv; n++)
            if (ivs[n].step_at == k && ivs[n].same < 0) {
                int d = imm(ivs[n].delta, V_I64);
                struct ir *p = emit(IR_BIN, i->tok);
                p->sub = OP_ADD;
                p->dst = ivs[n].p;
                p->a = ivs[n].p;
                p->b = d;
            }
    }
    free(old);
    for (int k = 0; k < F->ncode; k++) {
        struct ir *i = &F->code[k];
        int *ops[3] = { &i->a, &i->b, &i->c };
        for (int j = 0; j < 3; j++)
            if (*ops[j] > 0 && *ops[j] < nv && ren[*ops[j]])
                *ops[j] = ren[*ops[j]];
        if (i->call) {
            for (int q = 0; q < i->call->nargs; q++)
                if (i->call->args[q].value > 0 && i->call->args[q].value < nv && ren[i->call->args[q].value])
                    i->call->args[q].value = ren[i->call->args[q].value];
            if (i->call->ret_buffer > 0 && i->call->ret_buffer < nv && ren[i->call->ret_buffer])
                i->call->ret_buffer = ren[i->call->ret_buffer];
        }
    }
    free(ren);
    free(defs), free(def_at), free(uses), free(in_loop), free(hoist);
    return true;
}

/* every loop, the innermost first (what one loop hoists may leave the
 * next one out too); a loop's header label is done once */
static void loops(bool with_ivs)
{
    int nlab = F->nlabels + 2;
    bool *done = calloc((size_t)nlab, sizeof(bool));
    for (int rounds = 0; rounds < 256; rounds++) {
        int nc = F->ncode;
        int *label_at = malloc(sizeof(int) * (size_t)nlab);
        for (int l = 0; l < nlab; l++)
            label_at[l] = -1;
        for (int k = 0; k < nc; k++)
            if (F->code[k].op == IR_LABEL)
                label_at[F->code[k].label] = k;
        /* the smallest loop not done */
        int bestH = -1, bestJ = -1;
        for (int k = 0; k < nc; k++) {
            struct ir *i = &F->code[k];
            if (i->op != IR_JMP || label_at[i->label] < 0 || label_at[i->label] >= k || done[i->label])
                continue;
            if (bestH < 0 || k - label_at[i->label] < bestJ - bestH)
                bestH = label_at[i->label], bestJ = k;
        }
        if (bestH < 0) {
            free(label_at);
            break;
        }
        done[F->code[bestH].label] = true;
        /* entered only by falling into H */
        bool ok = bestH > 0;
        for (int k = 0; k < nc && ok; k++) {
            struct ir *i = &F->code[k];
            if (k >= bestH && k <= bestJ)
                continue;
            int t[2] = { i->op == IR_JMP || i->op == IR_BR ? i->label : 0, i->op == IR_BR ? i->label2 : 0 };
            for (int j = 0; j < 2; j++)
                if (t[j] && label_at[t[j]] >= bestH && label_at[t[j]] <= bestJ)
                    ok = false;
        }
        free(label_at);
        if (ok)
            loop_once(bestH, bestJ, with_ivs);
    }
    free(done);
}

/* (39e) Unrolling: a loop H: (cmp i < n or i <= n; br B, X) B: body ...
 * jmp H, its body straight code (no label anything jumps to, no branch,
 * call or assembly) with i, a signed int, stepped once by a constant c > 0
 * and n not set in it, gets a loop of U bodies in front of it: while
 * sext(i) < sext(n) - (U - 1) * c (in 64 bits: never an overflow), U copies
 * of the body, each with values of its own for what it alone sets and
 * uses; then the loop itself does the rest. Same steps in the same order,
 * so float sums keep their rounding. */
#define UNROLL_BODY 48

static bool unroll_once(int H, int J, int *done_label)
{
    int nv = F->nvalues, nc = F->ncode, nlab = F->nlabels + 2;
    struct ir *t = &F->code[H + 1], *br = &F->code[H + 2];
    if (H + 3 >= J || t->op != IR_CMP || br->op != IR_BR || br->a != t->dst || F->code[H + 3].op != IR_LABEL ||
        F->code[H + 3].label != br->label || (t->sub != OP_SLT && t->sub != OP_SLE))
        return false;
    int iv = t->a, bound = t->b, B = H + 3;
    if (iv <= 0 || bound <= 0 || F->values[iv].cls != V_I32 || F->values[bound].cls != V_I32)
        return false;
    int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
    int *uses = calloc((size_t)nv, sizeof(int));
    count_values(defs, def_at, uses);
    bool ok = J - B <= UNROLL_BODY;
    int steps = 0;
    int64_t c = 0;
    for (int k = B + 1; k < J && ok; k++) {
        struct ir *i = &F->code[k];
        switch (i->op) {
        case IR_LABEL: case IR_JMP: case IR_BR: case IR_RET: case IR_TRAP: case IR_CALL: case IR_ASM:
        case IR_TLS_ADDR: case IR_VA_START: case IR_VA_ARG: case IR_ALLOCA: case IR_GET_SP: case IR_SET_SP:
            ok = i->op == IR_LABEL;             /* (a label only when nothing jumps to it: below) */
            break;
        default:
            break;
        }
        if (i->dst == bound)
            ok = false;
        if (i->dst == iv) {
            steps++;
            if (i->op != IR_BIN || i->sub != OP_ADD || i->a != iv || !i->is_signed || !known(defs, def_at, i->b, &c) ||
                c <= 0 || c > 1 << 20)
                ok = false;
        }
    }
    if (ok && steps != 1)
        ok = false;
    /* labels inside the body: nothing may jump to them */
    for (int k = 0; k < nc && ok; k++) {
        struct ir *i = &F->code[k];
        int tg[2] = { i->op == IR_JMP || i->op == IR_BR ? i->label : 0, i->op == IR_BR ? i->label2 : 0 };
        for (int j = 0; j < 2; j++)
            for (int q = B + 1; q < J && tg[j]; q++)
                if (F->code[q].op == IR_LABEL && F->code[q].label == tg[j])
                    ok = false;
    }
    /* the bound: not set in the loop (a constant or a value from before) */
    for (int k = H; k <= J && ok; k++)
        if (F->code[k].dst == bound)
            ok = false;
    if (!ok) {
        free(defs), free(def_at), free(uses);
        return false;
    }
    int U = J - B <= UNROLL_BODY / 2 ? 4 : 2;
    /* the values each copy makes for itself: set once in the body, every
     * use inside it */
    int *inside = calloc((size_t)nv, sizeof(int)), *map = calloc((size_t)nv, sizeof(int));
    bool *own = calloc((size_t)nv, sizeof(bool));
    for (int k = B + 1; k < J; k++) {
        struct ir *i = &F->code[k];
        int ops[3] = { i->a, i->b, i->c };
        for (int o = 0; o < 3; o++)
            if (ops[o] > 0)
                inside[ops[o]]++;
    }
    for (int k = B + 1; k < J; k++) {
        int d = F->code[k].dst;
        if (d <= 0 || defs[d] != 1 || inside[d] != uses[d])
            continue;
        /* (and not read before it is set: a pass's value for the next) */
        bool early = false;
        for (int q = B + 1; q <= k && !early; q++)
            early = reads(&F->code[q], d);
        own[d] = !early;
    }
    /* (40e) the body ending in its steps (i's, and the pointers 39c-2 steps
     * after it; their constants): each copy u uses w + u * step for every
     * stepped w, the steps made once after the copies, so a copy's index is
     * i plus a constant (an address's displacement after reassociate) */
    int tail = J;
    while (tail - 1 > B && (F->code[tail - 1].op == IR_IMM || F->code[tail - 1].op == IR_NOP ||
                            F->code[tail - 1].op == IR_LABEL ||
                            (F->code[tail - 1].op == IR_BIN && F->code[tail - 1].sub == OP_ADD &&
                             F->code[tail - 1].a == F->code[tail - 1].dst)))
        tail--;
    int wv[8], nw = 0;
    int64_t wc[8];
    bool offsets = true, iv_in_tail = false;
    for (int k = tail; k < J && offsets; k++) {
        struct ir *x = &F->code[k];
        if (x->op != IR_BIN)
            continue;
        int64_t st;
        int sets = 0;
        for (int q = B + 1; q < J; q++)
            sets += F->code[q].dst == x->dst;
        if (sets != 1 || !known(defs, def_at, x->b, &st) || nw == 8 || (F->values[x->dst].cls == V_I32 && !x->is_signed))
            offsets = false;
        else {
            wv[nw] = x->dst;
            wc[nw++] = st;
            iv_in_tail |= x->dst == iv;
        }
    }
    if (!iv_in_tail)
        offsets = false;
    for (int k = B + 1; k < tail && offsets; k++)
        if (F->code[k].op == IR_IMM && F->code[k].dst > 0)
            for (int q = tail; q < J; q++)
                if (F->code[q].op == IR_BIN && F->code[q].b == F->code[k].dst)
                    offsets = false;            /* (a step's constant made before the tail: keep it simple) */
    int H2 = new_label_id(), B2 = new_label_id();
    struct ir *old = F->code;
    F->code = 0;
    F->ncode = F->capcode = 0;
    (void)nlab;
    int *wmap = calloc((size_t)nv, sizeof(int));
    for (int k = 0; k < nc; k++) {
        if (k == H) {
            /* the unrolled loop: sext(i) < sext(n) - (U - 1) * c, its right
             * side made once before it; U bodies; back */
            struct token *tok = old[H + 1].tok;
            int wn = conv(CONV_SEXT, bound, 4, V_I64);
            int lim = bin(OP_SUB, wn, imm((U - 1) * c, V_I64), V_I64, tok);
            label(H2);
            int wi = conv(CONV_SEXT, iv, 4, V_I64);
            int go = cmp(old[H + 1].sub, wi, lim);
            branch(go, B2, old[H].label);
            label(B2);
            for (int u = 0; u < U; u++) {
                if (offsets)
                    for (int w = 0; w < nw; w++) {
                        enum ir_class wcl = F->values[wv[w]].cls;
                        if (!u) {
                            wmap[wv[w]] = 0;
                            continue;
                        }
                        wmap[wv[w]] = bin(OP_ADD, wv[w], imm(u * wc[w], wcl), wcl, tok);
                        F->code[F->ncode - 1].is_signed = wcl == V_I32;
                    }
                for (int q = B + 1; q < (offsets ? tail : J); q++) {
                    struct ir *x = &old[q];
                    if (x->op == IR_LABEL)
                        continue;
                    struct ir *n = emit(x->op, x->tok);
                    *n = *x;
                    if (n->dst > 0 && own[n->dst]) {
                        map[n->dst] = new_value(F->values[n->dst].cls);
                        n->dst = map[n->dst];
                    }
                    int *ops[3] = { &n->a, &n->b, &n->c };
                    for (int o = 0; o < 3; o++) {
                        if (*ops[o] > 0 && *ops[o] < nv && own[*ops[o]])
                            *ops[o] = map[*ops[o]];
                        else if (*ops[o] > 0 && *ops[o] < nv && wmap[*ops[o]])
                            *ops[o] = wmap[*ops[o]];
                    }
                }
            }
            for (int w = 0; w < nw && offsets; w++) {
                enum ir_class wcl = F->values[wv[w]].cls;
                int d = imm(U * wc[w], wcl);
                struct ir *st = emit(IR_BIN, tok);
                st->sub = OP_ADD;
                st->dst = st->a = wv[w];
                st->b = d;
                st->is_signed = wcl == V_I32;
            }
            jump(H2);
        }
        *emit(old[k].op, old[k].tok) = old[k];
    }
    free(old);
    *done_label = H2;
    free(wmap);
    free(inside), free(map), free(own);
    free(defs), free(def_at), free(uses);
    return true;
}

/* every innermost loop once (the new ones are marked done) */
static void unroll_loops(void)
{
    int cap = F->nlabels + 64;
    bool *done = calloc((size_t)cap, sizeof(bool));
    for (int r = 0; r < nvector_rest; r++)
        if (vector_rest[r] < cap)
            done[vector_rest[r]] = true;
    for (int rounds = 0; rounds < 64; rounds++) {
        int nlab = F->nlabels + 2, nc = F->ncode;
        if (nlab > cap) {
            done = realloc(done, (size_t)nlab * 2);
            memset(done + cap, 0, (size_t)(nlab * 2 - cap));
            cap = nlab * 2;
        }
        int *label_at = malloc(sizeof(int) * (size_t)nlab);
        for (int l = 0; l < nlab; l++)
            label_at[l] = -1;
        for (int k = 0; k < nc; k++)
            if (F->code[k].op == IR_LABEL)
                label_at[F->code[k].label] = k;
        int H = -1, J = -1;
        for (int k = 0; k < nc && H < 0; k++) {
            struct ir *i = &F->code[k];
            if (i->op == IR_JMP && label_at[i->label] >= 0 && label_at[i->label] < k && !done[i->label]) {
                H = label_at[i->label];
                J = k;
            }
        }
        free(label_at);
        if (H < 0)
            break;
        done[F->code[H].label] = true;
        int made = 0;
        if (unroll_once(H, J, &made) && made < cap)
            done[made] = true;
    }
    free(done);
}

/* (40e) Constants moved out of addresses, so they end as displacements:
 * sext(x + c) is sext(x) + c (x + c a signed int's: C says it cannot
 * overflow), (y + c) * k or << k is y * k + c * k, base + (z + c) is
 * (base + z) + c (64 bits: exact); each only when the inner value has no
 * other use (a scaling: any) and its operand is not set between; and
 * (p + c1) + c2 is p + (c1 + c2) when p is not set between. An unrolled copy's
 * x[i + 2] is then x[i]'s address plus 8. */
static bool not_set_between(int v, int from, int to)
{
    for (int q = from + 1; q < to; q++)
        if (F->code[q].dst == v)
            return false;
    return true;
}

static void reassociate(void)
{
    for (int round = 0; round < 3; round++) {
        int nv = F->nvalues, nc = F->ncode;
        int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
        int *uses = calloc((size_t)nv, sizeof(int));
        count_values(defs, def_at, uses);
        bool *redo = calloc((size_t)nc + 1, sizeof(bool));
        int changed = 0;
        bool *sum = calloc((size_t)nc + 1, sizeof(bool));       /* (rule 4 at k) */
        for (int k = 0; k < nc; k++) {
            struct ir *i = &F->code[k];
            int64_t c, m;
            /* 4: (p + c1) + c2 is p + (c1 + c2), p + c1 kept for its other uses */
            if (i->op == IR_BIN && i->sub == OP_ADD && i->dst > 0 && F->values[i->dst].cls == V_I64 && i->a > 0 &&
                defs[i->a] == 1 && known(defs, def_at, i->b, &c)) {
                struct ir *d = &F->code[def_at[i->a]];
                if (d->op == IR_BIN && d->sub == OP_ADD && d->a > 0 && F->values[d->a].cls == V_I64 &&
                    known(defs, def_at, d->b, &m) && not_set_between(d->a, def_at[i->a], k)) {
                    sum[k] = true;
                    changed++;
                    continue;
                }
            }
            int in = i->op == IR_CONV ? i->a : i->op == IR_BIN ? (i->sub == OP_ADD ? i->b : i->a) : 0;
            /* (a scaling of y + c even when y + c has other uses: each its
             * own y * k, free inside an address) */
            bool scaling = i->op == IR_BIN && (i->sub == OP_MUL || i->sub == OP_SHL);
            if (in <= 0 || defs[in] != 1 || (uses[in] != 1 && !scaling) || !i->dst)
                continue;
            struct ir *d = &F->code[def_at[in]];
            if (d->op != IR_BIN || d->sub != OP_ADD || d->a <= 0 || !known(defs, def_at, d->b, &c) ||
                !not_set_between(d->a, def_at[in], k))
                continue;
            if (i->op == IR_CONV && i->sub == CONV_SEXT && i->size == 4 && F->values[i->dst].cls == V_I64 &&
                F->values[in].cls == V_I32 && d->is_signed)
                redo[k] = true;
            else if (i->op == IR_BIN && (i->sub == OP_MUL || i->sub == OP_SHL) && F->values[i->dst].cls == V_I64 &&
                     F->values[in].cls == V_I64 && known(defs, def_at, i->b, &m) && (i->sub == OP_MUL || (m >= 0 && m < 63)))
                redo[k] = true;
            else if (i->op == IR_BIN && i->sub == OP_ADD && F->values[i->dst].cls == V_I64 && F->values[in].cls == V_I64 &&
                     i->a > 0 && i->a != in && not_set_between(i->a, def_at[in], k))
                redo[k] = true;
            if (redo[k])
                changed++;
        }
        if (changed) {
            struct ir *old = F->code;
            F->code = 0;
            F->ncode = F->capcode = 0;
            for (int k = 0; k < nc; k++) {
                struct ir *i = &old[k];
                if (sum[k]) {
                    struct ir *d = &old[def_at[i->a]];
                    int64_t c2 = old[def_at[i->b]].imm, c1 = old[def_at[d->b]].imm;
                    int cv = imm((int64_t)((uint64_t)c1 + (uint64_t)c2), V_I64);
                    struct ir *n = emit(IR_BIN, i->tok);
                    n->sub = OP_ADD;
                    n->dst = i->dst;
                    n->a = d->a;
                    n->b = cv;
                    continue;
                }
                if (!redo[k]) {
                    *emit(i->op, i->tok) = *i;
                    continue;
                }
                int in = i->op == IR_CONV ? i->a : i->sub == OP_ADD ? i->b : i->a;
                struct ir *d = &old[def_at[in]];
                int64_t c = old[def_at[d->b]].imm, m = 0;
                int x;
                if (i->op == IR_CONV) {
                    x = conv(CONV_SEXT, d->a, 4, V_I64);
                    c = (int32_t)c;
                } else if (i->sub == OP_ADD) {
                    x = bin(OP_ADD, i->a, d->a, V_I64, i->tok);
                } else {
                    m = old[def_at[i->b]].imm;
                    x = bin(i->sub, d->a, imm(m, V_I64), V_I64, i->tok);
                    c = i->sub == OP_MUL ? (int64_t)((uint64_t)c * (uint64_t)m) : (int64_t)((uint64_t)c << m);
                }
                int cv = imm(c, V_I64);
                struct ir *n = emit(IR_BIN, i->tok);
                n->sub = OP_ADD;
                n->dst = i->dst;
                n->a = x;
                n->b = cv;
            }
            free(old);
        }
        free(redo);
        free(sum);
        free(defs), free(def_at), free(uses);
        if (!changed)
            break;
    }
}

/* (39d) A loop tests at its bottom: the jump back to the test (H: a label,
 * straight code, a branch into the body right after it or out) becomes a
 * copy of the test, so a pass costs one jump less; the test at H stays as
 * the guard before the first pass (and for jumps to H: a while's
 * continue). In the copy the test's own values (set and used only there)
 * are new ones; anything else it sets is a variable, set twice now. */
#define ROTATE_TEST 16

static void rotate_loops(void)
{
    int nv = F->nvalues, nc = F->ncode, nlab = F->nlabels + 2;
    int *defs = calloc((size_t)nv, sizeof(int)), *def_at = calloc((size_t)nv, sizeof(int));
    int *uses = calloc((size_t)nv, sizeof(int));
    count_values(defs, def_at, uses);
    int *label_at = malloc(sizeof(int) * (size_t)nlab);
    for (int l = 0; l < nlab; l++)
        label_at[l] = -1;
    for (int k = 0; k < nc; k++)
        if (F->code[k].op == IR_LABEL)
            label_at[F->code[k].label] = k;
    int *test_end = calloc((size_t)nc + 1, sizeof(int));     /* per back jump: its test's branch + 1 */
    int found = 0;
    for (int k = 0; k < nc; k++) {
        struct ir *j = &F->code[k];
        int H = j->op == IR_JMP ? label_at[j->label] : -1;
        if (H < 0 || H >= k)
            continue;
        int B = H + 1;
        while (B < nc && B - H <= ROTATE_TEST && F->code[B].op != IR_BR) {
            enum ir_op op = F->code[B].op;
            if (op == IR_LABEL || op == IR_JMP || op == IR_RET || op == IR_TRAP || op == IR_CALL || op == IR_ASM ||
                op == IR_TLS_ADDR || op == IR_VA_START || op == IR_VA_ARG || op == IR_ALLOCA)
                break;
            B++;
        }
        if (B >= nc || F->code[B].op != IR_BR || B >= k || B + 1 >= nc || F->code[B + 1].op != IR_LABEL)
            continue;
        struct ir *br = &F->code[B];
        int body = F->code[B + 1].label;
        if (br->label != body && br->label2 != body)
            continue;
        test_end[k] = B + 1;
        found++;
    }
    if (found) {
        struct ir *old = F->code;
        F->code = 0;
        F->ncode = F->capcode = 0;
        int *map = calloc((size_t)nv, sizeof(int)), *inside = calloc((size_t)nv, sizeof(int));
        for (int k = 0; k < nc; k++) {
            if (!test_end[k]) {
                *emit(old[k].op, old[k].tok) = old[k];
                continue;
            }
            int H = label_at[old[k].label], B1 = test_end[k];
            /* the test's own values: set once, every use inside it */
            for (int q = H + 1; q < B1; q++) {
                struct ir *x = &old[q];
                int ops[3] = { x->a, x->b, x->c };
                for (int o = 0; o < 3; o++)
                    if (ops[o] > 0)
                        inside[ops[o]]++;
            }
            for (int q = H + 1; q < B1; q++) {
                int d = old[q].dst;
                if (d > 0 && defs[d] == 1 && inside[d] == uses[d])
                    map[d] = new_value(F->values[d].cls);
            }
            for (int q = H + 1; q < B1; q++) {
                struct ir *n = emit(old[q].op, old[q].tok);
                *n = old[q];
                if (n->dst > 0 && map[n->dst])
                    n->dst = map[n->dst];
                int *ops[3] = { &n->a, &n->b, &n->c };
                for (int o = 0; o < 3; o++)
                    if (*ops[o] > 0 && map[*ops[o]])
                        *ops[o] = map[*ops[o]];
            }
            for (int q = H + 1; q < B1; q++) {
                struct ir *x = &old[q];
                int ops[3] = { x->a, x->b, x->c };
                for (int o = 0; o < 3; o++)
                    if (ops[o] > 0)
                        inside[ops[o]] = 0;
                if (x->dst > 0)
                    map[x->dst] = 0;
            }
        }
        free(old);
        free(map);
        free(inside);
    }
    free(test_end);
    free(label_at);
    free(defs), free(def_at), free(uses);
}

struct ir_func *ir_function(struct obj *fn)
{
    F = calloc(1, sizeof(*F));
    if (!F)
        fatal("out of memory");
    F->fn = fn;
    new_value(V_NONE);                          /* (value 0 is none) */
    label_ids = 0;
    vla_depth = 0;
    return_label = 0;
    label_depths(fn->body, 0);
    if (opt_level)
        promote_locals(fn);
    gen_stmt(fn->body);
    /* the end of main returns 0 (5.1.2.2.3); other functions return nothing */
    struct type *rt = unqual(fn->type->return_type);
    struct ir *i;
    if (strcmp(fn->name, "main") == 0 && rt->kind == TY_INT) {
        int z = imm(0, V_I32);
        i = emit(IR_RET, 0);
        i->a = z;
    } else {
        i = emit(IR_RET, 0);
        i->a = 0;
    }
    i->type = rt;
    if (opt_level) {
        fold_constants();
        split_structs();
        fold_constants();                       /* (39b-2: what the split left unused) */
        drop_extensions();
        common_subexpressions();
        forward_copies();
        coalesce_copies();
        loops(false);                           /* (40e: invariants first: n - 1 of b[n - 1 - i] for the packing) */
        vectorize_loops();
        loops(true);
        fold_constants();                       /* (39c-2: the chains a pointer replaced) */
        unroll_loops();
        reassociate();
        common_subexpressions();                /* (40e: the copies' i and addresses once) */
        fold_constants();
        rotate_loops();
    }
    return F;
}

/* --- a readable form (tests) --------------------------------------------- */

static const char *op_names[] = {
    "label", "jmp", "br", "imm", "fimm", "local", "global", "load", "store", "copy", "bin", "un",
    "cmp", "conv", "call", "ret", "memcpy", "memzero", "va_start", "va_arg", "alloca", "get_sp",
    "set_sp", "builtin", "atomic", "trap", "nop", "tls_addr", "asm", "vsplat", "vhadd", "vmadd", "vrevw",
};

void ir_dump(FILE *out, struct ir_func *f)
{
    fprintf(out, "function %s\n", f->fn->name);
    for (int k = 0; k < f->ncode; k++) {
        struct ir *i = &f->code[k];
        if (i->op == IR_LABEL) {
            fprintf(out, "L%d:\n", i->label);
            continue;
        }
        fprintf(out, "    %s", op_names[i->op]);
        if (i->dst)
            fprintf(out, " v%d", i->dst);
        if (i->a)
            fprintf(out, " a=v%d", i->a);
        if (i->b)
            fprintf(out, " b=v%d", i->b);
        if (i->sub)
            fprintf(out, " sub=%d", i->sub);
        if (i->size)
            fprintf(out, " size=%d", i->size);
        if (i->op == IR_IMM || i->op == IR_MEMCPY || i->op == IR_MEMZERO)
            fprintf(out, " %lld", (long long)i->imm);
        if (i->op == IR_FIMM)
            fprintf(out, " %g", i->fimm);
        if (i->sym)
            fprintf(out, " %s", i->sym);
        if (i->obj && i->op == IR_LOCAL)
            fprintf(out, " %s", i->obj->name);
        if (i->op == IR_JMP || i->op == IR_BR)
            fprintf(out, " L%d", i->label);
        if (i->op == IR_BR)
            fprintf(out, " L%d", i->label2);
        fprintf(out, "\n");
    }
}
