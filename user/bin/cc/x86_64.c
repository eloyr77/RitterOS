/*
 * The x86-64 backend (60c): the intermediate code as AT&T assembly, with
 * the System V AMD64 ABI's calls (integer arguments in rdi, rsi, rdx, rcx,
 * r8, r9, floating ones in xmm0-7, small structures split into eightbytes
 * of either kind, the rest on the stack) and its variadic functions (the
 * register save area and va_list).
 *
 * Every value lives in a stack slot of its own; a slot is reused once its
 * value's last use is behind (values never live across a loop's back edge
 * except where ir.c sets them on both paths). An instruction loads its
 * operands into rax, rcx, rdx or xmm0, xmm1 (long doubles: the x87 stack),
 * computes, and stores the result. Simple and slow; registers come later.
 */
#include "regalloc.h"

static FILE *out;
static struct ir_func *F;
static int fn_number;
static int *slot_of;                            /* value -> rbp offset */
static struct regalloc ra;                      /* (61a) values in registers */
static int saves;                               /* where the callee-saved registers are kept */
static int ms_saves;                            /* (62g) an ms_abi function's rsi and rdi (0: none) */
static int scratch;                             /* 16 bytes for x87 integer loads and stores */

/* (61b) what the code says about each value: how often it is set and used,
 * where it is set, and which instructions need not be emitted (their value
 * folded into every use: a constant, a frame or global address, an address
 * plus a constant, a comparison fused with its branch) */
static int *defs, *def_at, *nonfolded, *uses;
static bool *skip, *chained, *lea_at;           /* (39c: lea_at, additions written as leaq) */
static int *memop_of;                           /* (40b) per value: its load's place + 1, read by its user */
static bool *disp_ok;                           /* (40e) per instruction: an added constant over an index form, both folded */
static int *last_use;                           /* (40e) per value: its last use's place */

/* The registers values get (61a): the ones a call keeps, then (61c) four
 * it does not, for values no call (or memcpy, memset or atomic, which use
 * rsi and rdi) comes in the middle of */
enum { SAVED_REGS = 5, VALUE_REGS = 9 };
static const char *const vreg_names[VALUE_REGS][4] = {
    { "%bl", "%bx", "%ebx", "%rbx" }, { "%r12b", "%r12w", "%r12d", "%r12" },
    { "%r13b", "%r13w", "%r13d", "%r13" }, { "%r14b", "%r14w", "%r14d", "%r14" },
    { "%r15b", "%r15w", "%r15d", "%r15" }, { "%r8b", "%r8w", "%r8d", "%r8" },
    { "%r9b", "%r9w", "%r9d", "%r9" }, { "%sil", "%si", "%esi", "%rsi" }, { "%dil", "%di", "%edi", "%rdi" },
};
static bool reg_used[VALUE_REGS];
static int frame_size;
static int sret_slot;                           /* where the hidden result pointer is kept */
static int va_area;                             /* a variadic function's register save area */
static int va_gp, va_fp, va_stack;              /* what the named parameters used */
static int unique;
static bool need_two64, need_two63f;

static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* (32h) A function with 256-bit vector values runs AVX code: an SSE
 * encoding there, while the ymm registers' upper halves hold data, costs
 * dozens of cycles each time on some processors (the laptop's Ryzen: a FIR
 * filter's loop 70 times slower for one movss). So its scalar float code
 * is written in VEX encodings (vexify), and vzeroupper goes before its
 * calls and returns, as clang does. */
static bool avx_fn;

/* text, an instruction, in its VEX form where it is one of the backend's
 * scalar SSE ones (2-operand op src, dst becomes vop src, dst, dst) */
static void vexify(char *text, size_t cap)
{
    char m[24], rest[200];
    if (sscanf(text, "%23s %199[^\n]", m, rest) != 2)
        return;
    bool xmm = strstr(rest, "%xmm") != 0;
    char *comma = strstr(rest, ", ");
    if (!xmm || !comma || strstr(comma + 2, ", "))
        return;                                 /* (two operands, one an xmm register) */
    char a[100], b[100];
    snprintf(a, sizeof(a), "%.*s", (int)(comma - rest), rest);
    snprintf(b, sizeof(b), "%s", comma + 2);
    bool ra = a[0] == '%', rb = b[0] == '%';
    static const char *three[] = { "addss", "addsd", "subss", "subsd", "mulss", "mulsd", "divss", "divsd",
                                   "minss", "minsd", "maxss", "maxsd", "sqrtss", "sqrtsd", "cvtss2sd", "cvtsd2ss",
                                   "cvtsi2ssl", "cvtsi2ssq", "cvtsi2sdl", "cvtsi2sdq" };
    static const char *two[] = { "ucomiss", "ucomisd", "comiss", "comisd", "cvttss2sil", "cvttss2siq",
                                 "cvttsd2sil", "cvttsd2siq", "cvtss2sil", "cvtss2siq", "cvtsd2sil", "cvtsd2siq",
                                 "movq", "movd", "movdqu", "movdqa", "movaps", "movups" };
    for (size_t k = 0; k < sizeof(three) / sizeof(three[0]); k++)
        if (strcmp(m, three[k]) == 0) {
            snprintf(text, cap, "v%s %s, %s, %s", m, a, b, b);
            return;
        }
    for (size_t k = 0; k < sizeof(two) / sizeof(two[0]); k++)
        if (strcmp(m, two[k]) == 0) {
            snprintf(text, cap, "v%s %s, %s", m, a, b);
            return;
        }
    if (strcmp(m, "movss") == 0 || strcmp(m, "movsd") == 0) {
        if (ra && rb)
            snprintf(text, cap, "v%s %s, %s, %s", m, a, b, b);     /* (a register's low lane, the rest kept) */
        else
            snprintf(text, cap, "v%s %s, %s", m, a, b);
        return;
    }
    if (strcmp(m, "pxor") == 0 || strcmp(m, "xorps") == 0)
        snprintf(text, cap, "v%s %s, %s, %s", m, a, b, b);
}

static void line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char text[256];
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (avx_fn)
        vexify(text, sizeof(text));
    fprintf(out, "\t%s\n", text);
}

static enum ir_class cls(int v);
static bool vec(int v);
static bool flt(int v);

/* A value as an operand of width bytes: its register (named for that
 * width; (39a) a float's or double's: its xmm register) or its stack slot. */
static const char *opnd(int v, int width)
{
    if (ra.reg && ra.reg[v] >= 0 && flt(v)) {
        static char xbuf[8][8];
        static int xk;
        char *b = xbuf[xk++ & 7];
        snprintf(b, 8, "%%xmm%d", ra.reg[v] < 8 ? 8 + ra.reg[v] : ra.reg[v] - 6);   /* (40d: xmm_of) */
        return b;
    }
    if (ra.reg && ra.reg[v] >= 0)
        return vreg_names[ra.reg[v]][width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : 3];
    static char buf[4][32];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, 32, "%d(%%rbp)", slot_of[v]);
    return b;
}

/* ... at its own width (32 bits for I32, 64 for I64; a float's register
 * is its xmm one) */
static const char *slot(int v)
{
    return opnd(v, value_size(cls(v)) == 4 ? 4 : 8);
}

static bool in_reg(int v)
{
    return ra.reg && ra.reg[v] >= 0;
}

/* A value's place in memory for an instruction that needs memory (the
 * x87's integer loads): a register value goes through the scratch slot. */
static const char *mem_in(int v)
{
    static char buf[32];
    if (!in_reg(v))
        return slot(v);
    fprintf(out, "\tmovq %s, %d(%%rbp)\n", opnd(v, 8), scratch);
    snprintf(buf, sizeof(buf), "%d(%%rbp)", scratch);
    return buf;
}

static const char *mem_out(int v)
{
    static char buf[32];
    if (!in_reg(v))
        return slot(v);
    snprintf(buf, sizeof(buf), "%d(%%rbp)", scratch);
    return buf;
}

/* (after an instruction that wrote mem_out(v)) */
static void mem_out_done(int v)
{
    if (in_reg(v))
        fprintf(out, "\tmovq %d(%%rbp), %s\n", scratch, opnd(v, 8));
}

static const char *slot_at(int v, int offset)
{
    static char buf[4][32];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, 32, "%d(%%rbp)", slot_of[v] + offset);
    return b;
}

static enum ir_class cls(int v)
{
    return F->values[v].cls;
}

/* (39a) a float or double: kept in xmm8-xmm15 like the vectors, from the
 * allocator's second pool */
static bool flt(int v)
{
    return cls(v) == V_F32 || cls(v) == V_F64;
}

static struct ir *def_of(int v)
{
    return v > 0 && defs[v] == 1 ? &F->code[def_at[v]] : 0;
}

/* (61c) the instruction before or after k, over the copies coalescing
 * removed */
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

/* a constant that fits a 32-bit immediate (sign-extended for 64 bits) */
static bool const32(int v, int64_t *c)
{
    struct ir *d = def_of(v);
    if (!d || d->op != IR_IMM)
        return false;
    int64_t x = d->imm;
    if (cls(v) == V_I32)
        x = (int32_t)x;
    else if (x < INT32_MIN || x > INT32_MAX)
        return false;
    *c = x;
    return true;
}

static bool foldable_global(struct ir *d)
{
    return d && d->op == IR_GLOBAL && symbol_is_local(d->obj) && d->imm >= INT32_MIN / 2 && d->imm <= INT32_MAX / 2;
}

/* (61c) An address base + index * scale (1, 2, 4 or 8) said as
 * (%base,%index,scale): the addition right before k, its index scaled by
 * a shift or multiplication right before that (its constant may come in
 * between), each used only there. Nothing else is set between them, so
 * the registers of base and index are still theirs. */
struct index_fold {
    int base, index, scale, scale_at;           /* (scale_at -1: not scaled) */
};

static bool index_folds(int k, int v, struct index_fold *f)
{
    struct ir *d = def_of(v);
    int64_t c;
    /* (39c-2) only into the access itself: through an added constant
     * (x[k - i].im) the addition between would still be computed, and its
     * register may be the index's (constants made before a loop no longer
     * stand between them) */
    /* (40e) or into an added constant analyze marked (disp_ok): it and this
     * addition are both folded away, so neither takes a register */
    if (F->code[k].op != IR_LOAD && F->code[k].op != IR_STORE && !(disp_ok && disp_ok[k]))
        return false;
    int at0 = before(k);
    if (at0 >= 0 && F->code[k].op == IR_BIN && F->code[at0].op == IR_IMM && F->code[at0].dst == F->code[k].b)
        at0 = before(at0);                      /* (the added constant's own, an immediate) */
    if (!d || d->op != IR_BIN || d->sub != OP_ADD || cls(v) != V_I64 || def_at[v] != at0 || uses[v] != 1 ||
        const32(d->b, &c) || d->a <= 0 || d->b <= 0 || cls(d->a) != V_I64 || cls(d->b) != V_I64)
        return false;
    struct index_fold g = { d->a, d->b, 1, -1 };
    int at = before(at0);
    struct ir *m = def_of(g.index);
    if (m && m->op == IR_BIN && def_at[g.index] == at && uses[g.index] == 1 && const32(m->b, &c) &&
        ((m->sub == OP_SHL && c >= 1 && c <= 3) || (m->sub == OP_MUL && (c == 2 || c == 4 || c == 8)))) {
        g.scale = m->sub == OP_SHL ? 1 << c : (int)c;
        g.scale_at = at;
        g.index = m->a;
        if (g.index <= 0 || cls(g.index) != V_I64)
            return false;
    }
    if (f)
        *f = g;
    return true;
}

/* Value v, an address used by the instruction at k: made of a frame or
 * (this file's) global address and constant offsets, so the memory operand
 * can say it all? An addition counts only right before its use (a register
 * its base was in may be reused after its last use). */
static bool address_folds(int k, int v)
{
    struct ir *d = def_of(v);
    if (!d)
        return false;
    if (d->op == IR_LOCAL || foldable_global(d))
        return true;
    int64_t c;
    if (index_folds(k, v, 0))
        return true;
    /* (40a) and only as its one use: an addition also computed for another
     * may have taken its base's register (the base's life ends there);
     * (40e) or when the base is used after k, so its register is its own */
    if (d->op != IR_BIN || d->sub != OP_ADD || cls(v) != V_I64 || !const32(d->b, &c) || c <= INT32_MIN / 2 ||
        c >= INT32_MAX / 2)
        return false;
    if (def_at[v] == before(k) && uses[v] == 1)
        return true;
    /* (40e: a base used after k and not set since the addition: anywhere) */
    if (d->a <= 0 || last_use[d->a] <= k || def_at[v] > k)
        return false;
    for (int q = def_at[v] + 1; q < k; q++)
        if (F->code[q].dst == d->a || F->code[q].op == IR_LABEL)
            return false;
    return true;
}

/* (39c) The addition at k as base + index * 1, 2, 4 or 8, its scaling right
 * before it and used only there: one leaq (an address used more than once,
 * so not folded into one memory operand: x[k - i].re and .im) */
static bool lea_scaled(int k, struct index_fold *f)
{
    struct ir *i = &F->code[k];
    int64_t c;
    if (i->op != IR_BIN || i->sub != OP_ADD || cls(i->dst) != V_I64 || i->a <= 0 || i->b <= 0 || cls(i->a) != V_I64 ||
        cls(i->b) != V_I64 || const32(i->b, &c))
        return false;
    struct ir *m = def_of(i->b);
    if (!m || m->op != IR_BIN || def_at[i->b] != before(k) || uses[i->b] != 1 || !const32(m->b, &c) || m->a <= 0 ||
        cls(m->a) != V_I64 || !((m->sub == OP_SHL && c >= 1 && c <= 3) || (m->sub == OP_MUL && (c == 2 || c == 4 || c == 8))))
        return false;
    if (f)
        *f = (struct index_fold){ i->a, m->a, m->sub == OP_SHL ? 1 << c : (int)c, before(k) };
    return true;
}

/* The memory operand for *(v + extra), with rcx for a base that is only
 * on the stack. */
static const char *address(int v, int64_t extra, int k)
{
    static char buf[4][128];
    static int n;
    char *b = buf[n++ & 3];
    struct ir *d = def_of(v);
    if (d && address_folds(k, v)) {
        if (d->op == IR_LOCAL) {
            snprintf(b, 128, "%lld(%%rbp)", (long long)(d->obj->offset + extra));
            return b;
        }
        if (d->op == IR_GLOBAL) {
            snprintf(b, 128, "%s%+lld(%%rip)", d->sym, (long long)(d->imm + extra));
            return b;
        }
        int64_t c;
        struct index_fold f;
        if (index_folds(k, v, &f)) {
            const char *B = in_reg(f.base) ? opnd(f.base, 8) : (line("movq %s, %%rcx", opnd(f.base, 8)), "%rcx");
            const char *I = in_reg(f.index) ? opnd(f.index, 8) : (line("movq %s, %%rdx", opnd(f.index, 8)), "%rdx");
            snprintf(b, 128, "%lld(%s,%s,%d)", (long long)extra, B, I, f.scale);
            return b;
        }
        const32(d->b, &c);
        return address(d->a, extra + c, def_at[v]);
    }
    if (in_reg(v))
        snprintf(b, 128, "%lld(%s)", (long long)extra, opnd(v, 8));
    else {
        fprintf(out, "\tmovq %s, %%rcx\n", opnd(v, 8));
        snprintf(b, 128, "%lld(%%rcx)", (long long)extra);
    }
    return b;
}

static bool wide(int v)
{
    return cls(v) == V_I64;
}

static const char *ax(int v)
{
    return wide(v) ? "%rax" : "%eax";
}

static const char *cx(int v)
{
    return wide(v) ? "%rcx" : "%ecx";
}

static char sfx(int v)
{
    return wide(v) ? 'q' : 'l';
}

static int new_unique(void)
{
    return unique++;
}

/* --- the frame ----------------------------------------------------------- */

static int align_up(int n, int a)
{
    return (n + a - 1) / a * a;
}

/* The System V classes of a structure's eightbytes. */
enum { CLASS_NONE, CLASS_INT, CLASS_SSE, CLASS_MEMORY };

static void classify_into(struct type *ty, int offset, int *classes)
{
    ty = unqual(ty);
    if (ty->is_vector) {
        classes[0] = classes[1] = CLASS_MEMORY;  /* (32h: a structure holding a vector goes in memory) */
        return;
    }
    if (ty->kind == TY_ARRAY) {
        for (int i = 0; i < ty->array_len; i++)
            classify_into(ty->base, offset + i * ty->base->size, classes);
        return;
    }
    if (is_memory_value(ty)) {
        for (struct member *m = ty->members; m; m = m->next) {
            if (!m->is_bitfield && m->type->size > 0 && (offset + m->offset) % m->type->align != 0) {
                classes[0] = classes[1] = CLASS_MEMORY;
                return;
            }
            classify_into(m->type, offset + m->offset, classes);
        }
        return;
    }
    if (ty->kind == TY_LDOUBLE) {
        classes[0] = classes[1] = CLASS_MEMORY;
        return;
    }
    int k = offset / 8;
    if (k > 1)
        return;
    int c = is_flonum(ty) ? CLASS_SSE : CLASS_INT;
    if (classes[k] == CLASS_MEMORY)
        return;
    if (classes[k] == CLASS_NONE || (classes[k] == CLASS_SSE && c == CLASS_INT))
        classes[k] = c;
}

/* (62a) long double _Complex: returned in st0 (real) and st1 (imaginary),
 * the ABI's COMPLEX_X87 class; passed in memory like any MEMORY class */
static bool x87_complex(struct type *t)
{
    t = unqual(t);
    return t->kind == TY_COMPLEX && t->base->kind == TY_LDOUBLE;
}

/* A structure in registers: the classes of its eightbytes (n of them), or
 * false when it goes in memory. */
static bool struct_in_registers(struct type *ty, int classes[2], int *n)
{
    classes[0] = classes[1] = CLASS_NONE;
    if (ty->size > 16 || ty->size == 0)
        return false;
    classify_into(ty, 0, classes);
    if (classes[0] == CLASS_MEMORY || classes[1] == CLASS_MEMORY)
        return false;
    *n = ty->size > 8 ? 2 : 1;
    for (int k = 0; k < *n; k++)
        if (classes[k] == CLASS_NONE)
            classes[k] = CLASS_SSE;             /* (padding only: either will do) */
    return true;
}

/* Interval allocation of value slots: each value's first and last use. */
static void assign_slots(int base)
{
    int n = F->nvalues;
    /* (61a) where each value is live, and which are in registers */
    bool *folded_away = calloc((size_t)n + 1, sizeof(bool));
    for (int v = 1; v < n; v++)
        folded_away[v] = defs[v] == 1 && skip[def_at[v]];
    unsigned *clobbers = calloc((size_t)F->ncode + 1, sizeof(unsigned));
    for (int k = 0; k < F->ncode; k++)
        switch (F->code[k].op) {
        case IR_CALL: case IR_TLS_ADDR: clobbers[k] = 0xF << SAVED_REGS | CLOBBERS_FPRS | CLOBBERS_LOW_FPRS; break;
        case IR_ASM:                                    /* (62c: no value in a register over it) */
            clobbers[k] = (asm_in_registers(F->code[k].asm_info) ? 0 : 1u << 31) | CLOBBERS_LOW_FPRS;   /* (32h: or as an instruction) */
            break;
        case IR_MEMCPY:                                 /* (39b: rsi and rdi only for rep movsb) */
            clobbers[k] = F->code[k].imm > 128 ? 3 << (SAVED_REGS + 2) : 0;
            break;
        case IR_MEMZERO: clobbers[k] = F->code[k].imm > 128 ? 1 << (SAVED_REGS + 3) : 0; break;
        case IR_ATOMIC: clobbers[k] = 1 << (SAVED_REGS + 2); break;
        default: break;
        }
    /* (39a) floats and doubles share xmm8-xmm15 with the vectors; not in an
     * ms_abi function, whose caller keeps xmm6-xmm15 */
    unsigned fprs = 1u << V_V16 | 1u << V_V32 | (F->fn->type->is_ms_abi ? 0 : 1u << V_F32 | 1u << V_F64);
    /* (40d) xmm8-xmm15, then xmm2-xmm7 (pool indexes 8-13), which calls and
     * inline assembly use: xmm_of */
    regalloc_run(F, opt_level ? VALUE_REGS : 0, opt_level ? 14 : 0, 8, fprs, folded_away, clobbers, SAVED_REGS, &ra);
    free(clobbers);
    free(folded_away);
    int *first = ra.start, *last = ra.end;
    for (int v = 1; v < n; v++)
        if (ra.reg[v] >= 0) {
            first[v] = -1;                      /* (no slot) */
            if (!vec(v) && !flt(v))
                reg_used[ra.reg[v]] = true;     /* (xmm8-15: nothing to keep for the caller) */
        }
    /* free slots by size (8 or 16), each with the end of its last value */
    int cap = 64, count = 0;
    struct free_slot {
        int offset, size, busy_until;
    } *slots = malloc(sizeof(*slots) * (size_t)cap);
    /* values in the order they begin */
    int *order = malloc(sizeof(int) * (size_t)n), m = 0;
    int *bucket_count = calloc((size_t)F->ncode + 1, sizeof(int));
    for (int v = 1; v < n; v++)
        if (first[v] >= 0)
            bucket_count[first[v]]++;
    int *start = calloc((size_t)F->ncode + 2, sizeof(int));
    for (int k = 0; k < F->ncode; k++)
        start[k + 1] = start[k] + bucket_count[k];
    int *fill = calloc((size_t)F->ncode + 1, sizeof(int));
    for (int v = 1; v < n; v++)
        if (first[v] >= 0)
            order[start[first[v]] + fill[first[v]]++] = v;
    m = start[F->ncode];
    int top = base;
    for (int j = 0; j < m; j++) {
        int v = order[j];
        int size = value_size(cls(v)) > 16 ? 32 : value_size(cls(v)) > 8 ? 16 : 8;
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
            top = align_up(top + size, size);
            slots[count].offset = -top;
            slots[count].size = size;
            found = count++;
        }
        slots[found].busy_until = last[v];
        slot_of[v] = slots[found].offset;
    }
    frame_size = top;
    free(slots);
    free(order);
    free(bucket_count);
    free(start);
    free(fill);
}

/* Places for the locals and parameters, then the values. */
static void layout_frame(struct obj *fn)
{
    int top = 40;                               /* (61a) rbx, r12-r15 when used */
    saves = -top;
    top = align_up(top + 16, 16);
    scratch = -top;
    sret_slot = 0;
    va_area = 0;
    if (is_memory_value(unqual(fn->type->return_type)) && !x87_complex(fn->type->return_type)) {
        int classes[2], n;
        if (!struct_in_registers(unqual(fn->type->return_type), classes, &n)) {
            top += 8;
            sret_slot = -top;
        }
    }
    if (fn->type->is_variadic) {
        top = align_up(top + 176, 16);
        va_area = -top;
    }
    ms_saves = 0;
    if (fn->type->is_ms_abi) {
        /* (62g) called by UEFI (efi_main): the Microsoft convention keeps
         * rsi and rdi for the caller too (and xmm6-xmm15, which nothing
         * here uses); integers and pointers in rcx, rdx, r8, r9 */
        if (fn->type->is_variadic || sret_slot)
            error_at(fn->tok, "an ms_abi function takes fixed arguments and returns a scalar");
        top += 16;
        ms_saves = -top;
        int k = 0;
        for (struct obj *p = fn->params; p; p = p->next, k++) {
            struct type *ty = unqual(p->type);
            if (is_memory_value(ty) || is_flonum(ty) || k >= 4)
                error_at(p->tok, "an ms_abi function takes at most four integer or pointer arguments");
            top = align_up(top + 8, 8);
            p->offset = -top;
        }
        va_gp = va_fp = va_stack = 0;
        goto after_params;
    }
    /* parameters passed on the stack stay where the caller put them */
    int gp = sret_slot ? 1 : 0, sse = 0, stack = 16;
    for (struct obj *p = fn->params; p; p = p->next) {
        struct type *ty = unqual(p->type);
        int classes[2], n = 0;
        bool in_regs;
        if (is_memory_value(ty)) {
            in_regs = struct_in_registers(ty, classes, &n);
            if (in_regs) {
                int need_gp = 0, need_sse = 0;
                for (int k = 0; k < n; k++)
                    classes[k] == CLASS_INT ? need_gp++ : need_sse++;
                if (gp + need_gp > 6 || sse + need_sse > 8)
                    in_regs = false;
                else
                    gp += need_gp, sse += need_sse;
            }
        } else if (ty->kind == TY_LDOUBLE)
            in_regs = false;
        else if (is_flonum(ty) || ty->is_vector)
            in_regs = sse < 8 ? (sse++, true) : false;
        else
            in_regs = gp < 6 ? (gp++, true) : false;
        if (in_regs) {
            int size = align_up(ty->size, 8);
            int a = p->align > 8 ? p->align : 8;
            top = align_up(top + size, a);
            p->offset = -top;
        } else if (ty->is_vector) {
            error_at(p->tok, "a vector parameter after eight in xmm registers is not supported");
        } else {
            int a = ty->align > 8 ? 16 : 8;
            stack = align_up(stack, a);
            p->offset = stack;
            stack += align_up(ty->size, 8);
        }
    }
    va_gp = gp;
    va_fp = sse;
    va_stack = stack;
after_params:
    top = place_locals(fn, top, true);
    /* the scratch room of va_arg for structures in registers */
    for (int k = 0; k < F->ncode; k++)
        if (F->code[k].op == IR_VA_ARG && is_memory_value(F->code[k].type)) {
            top = align_up(top + 16, 16);
            F->code[k].imm = -top;
        }
    assign_slots(top);
    frame_size = align_up(frame_size, 16);
}

/* (32h) Vectors: a value of V_V16 or V_V32 lives in the second pool's
 * register r (xmm8 + r: no other code here uses xmm8-xmm15) or its slot;
 * xmm0-xmm7 are scratch (and the arguments). 32-byte ones are moved with
 * VEX (AVX), 16-byte ones with SSE's encodings: those run on every x86-64. */
static bool vec(int v)
{
    return cls(v) == V_V16 || cls(v) == V_V32;
}

static const char *xname(int x, bool wide)
{
    static char buf[4][8];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, 8, "%%%cmm%d", wide ? 'y' : 'x', x);
    return b;
}

/* v's register, or -1 */
static int vreg(int v)
{
    return in_reg(v) ? (ra.reg[v] < 8 ? 8 + ra.reg[v] : ra.reg[v] - 6) : -1;   /* (40d: pool 8-13 are xmm2-xmm7) */
}

/* v into register x (nothing if it is there) */
static void vec_to(int x, int v)
{
    bool wide = cls(v) == V_V32;            /* (a 16-byte one in VEX too in an AVX function: vexify) */
    if (vreg(v) == x)
        return;
    if (vreg(v) >= 0)
        line("%s %s, %s", wide ? "vmovdqa" : "movdqa", xname(vreg(v), wide), xname(x, wide));
    else
        line("%s %s, %s", wide ? "vmovdqu" : "movdqu", slot(v), xname(x, wide));
}

/* register x into v */
static void vec_from(int v, int x)
{
    bool wide = cls(v) == V_V32;
    if (vreg(v) == x)
        return;
    if (vreg(v) >= 0)
        line("%s %s, %s", wide ? "vmovdqa" : "movdqa", xname(x, wide), xname(vreg(v), wide));
    else
        line("%s %s, %s", wide ? "vmovdqu" : "movdqu", xname(x, wide), slot(v));
}

/* the register v is in, or scratch register x loaded with it */
static int vec_in(int v, int x)
{
    if (vreg(v) >= 0)
        return vreg(v);
    vec_to(x, v);
    return x;
}

/* --- small pieces -------------------------------------------------------- */

static void load_to(const char *reg, int v)
{
    if (wide(v) || cls(v) == V_F64)
        line("movq %s, %s", slot(v), reg);
    else
        line(flt(v) && in_reg(v) ? "movd %s, %s" : "movl %s, %s", slot(v), reg);   /* (39a: a float's bits) */
}

static void store_from(int v, const char *reg64, const char *reg32)
{
    if (value_size(cls(v)) == 8)
        line("movq %s, %s", reg64, slot(v));
    else
        line(flt(v) && in_reg(v) ? "movd %s, %s" : "movl %s, %s", reg32, slot(v));
}

static void load_address(const char *reg, int v)
{
    line("movq %s, %s", slot(v), reg);
}

static const char *fp_suffix(int v)
{
    return cls(v) == V_F32 ? "ss" : "sd";
}

/* (39a) between xmm registers the whole register is copied (movss
 * would merge into the old one: a wait for whatever set it) */
static void movf_to_xmm(const char *xmm, int v)
{
    if (in_reg(v)) {
        if (strcmp(slot(v), xmm) != 0)
            line("movaps %s, %s", slot(v), xmm);
        return;
    }
    line("mov%s %s, %s", cls(v) == V_F32 ? "ss" : "sd", slot(v), xmm);
}

static void movf_from_xmm(int v, const char *xmm)
{
    if (in_reg(v)) {
        if (strcmp(slot(v), xmm) != 0)
            line("movaps %s, %s", xmm, slot(v));
        return;
    }
    line("mov%s %s, %s", cls(v) == V_F32 ? "ss" : "sd", xmm, slot(v));
}

/* (39a) where an SSE instruction computes float d: its own register, or
 * xmm0 (then movf_from_xmm) */
static const char *fwork(int d)
{
    return in_reg(d) ? slot(d) : "%xmm0";
}

/* x87 code needs memory: a float in a register goes through the scratch
 * slot */
static const char *fmem_in(int v)
{
    if (!in_reg(v))
        return slot(v);
    line("mov%s %s, %d(%%rbp)", fp_suffix(v), slot(v), scratch);
    static char buf[32];
    snprintf(buf, sizeof(buf), "%d(%%rbp)", scratch);
    return buf;
}

/* The bytes [src, src + n) to [dst, dst + n), both as "OFF(%REG)" bases
 * given by register names; rax is the scratch register. */
static void copy_bytes(const char *dst, int dst_off, const char *src, int src_off, int n)
{
    int at = 0;
    while (n - at >= 8) {
        line("movq %d(%s), %%rax", src_off + at, src);
        line("movq %%rax, %d(%s)", dst_off + at, dst);
        at += 8;
    }
    if (n - at >= 4) {
        line("movl %d(%s), %%eax", src_off + at, src);
        line("movl %%eax, %d(%s)", dst_off + at, dst);
        at += 4;
    }
    if (n - at >= 2) {
        line("movw %d(%s), %%ax", src_off + at, src);
        line("movw %%ax, %d(%s)", dst_off + at, dst);
        at += 2;
    }
    if (n - at >= 1) {
        line("movb %d(%s), %%al", src_off + at, src);
        line("movb %%al, %d(%s)", dst_off + at, dst);
    }
}

/* n (1-8) bytes at OFF(base) into reg64 without reading past them. */
static void load_partial(const char *reg64, const char *reg32, const char *base, int off, int n)
{
    switch (n) {
    case 8: line("movq %d(%s), %s", off, base, reg64); return;
    case 4: line("movl %d(%s), %s", off, base, reg32); return;
    case 2: line("movzwl %d(%s), %s", off, base, reg32); return;
    case 1: line("movzbl %d(%s), %s", off, base, reg32); return;
    }
    /* 3, 5, 6, 7: byte by byte from the top */
    line("xorl %s, %s", reg32, reg32);
    for (int i = n - 1; i >= 0; i--) {
        line("shlq $8, %s", reg64);
        line("movzbl %d(%s), %%r10d", off + i, base);
        line("orq %%r10, %s", reg64);
    }
}

static void store_partial(const char *base, int off, const char *reg64, int n)
{
    /* (reg64 is changed) */
    static const char *names[][4] = {
        { "%rax", "%eax", "%ax", "%al" }, { "%rdx", "%edx", "%dx", "%dl" },
        { "%rdi", "%edi", "%di", "%dil" }, { "%rsi", "%esi", "%si", "%sil" },
        { "%rcx", "%ecx", "%cx", "%cl" }, { "%r8", "%r8d", "%r8w", "%r8b" },
        { "%r9", "%r9d", "%r9w", "%r9b" }, { "%r10", "%r10d", "%r10w", "%r10b" },
        { "%r11", "%r11d", "%r11w", "%r11b" },
    };
    int r = -1;
    for (int i = 0; i < 9; i++)
        if (strcmp(names[i][0], reg64) == 0)
            r = i;
    int at = 0;
    while (n - at > 0) {
        if (n - at >= 8) {
            line("movq %s, %d(%s)", names[r][0], off + at, base);
            at += 8;
            continue;
        }
        if (n - at >= 4) {
            line("movl %s, %d(%s)", names[r][1], off + at, base);
            line("shrq $32, %s", names[r][0]);
            at += 4;
            continue;
        }
        if (n - at >= 2) {
            line("movw %s, %d(%s)", names[r][2], off + at, base);
            line("shrq $16, %s", names[r][0]);
            at += 2;
            continue;
        }
        line("movb %s, %d(%s)", names[r][3], off + at, base);
        line("shrq $8, %s", names[r][0]);
        at += 1;
    }
}

static void long_double_bytes(double v, unsigned char out16[16]);

static const char *sym_ref(const char *sym, struct obj *o)
{
    (void)o;
    return sym;
}

static void global_address(const char *reg, const char *sym, struct obj *o, int64_t addend)
{
    if (symbol_is_local(o))
        line("leaq %s(%%rip), %s", sym_ref(sym, o), reg);
    else
        line("movq %s@GOTPCREL(%%rip), %s", sym, reg);
    if (addend)
        line("addq $%lld, %s", (long long)addend, reg);
}

static const char *label_name(int id)
{
    static char buf[4][32];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, 32, ".L%d_%d", fn_number, id);
    return b;
}

/* --- x87 for long double ------------------------------------------------- */

static void fld(int v)
{
    line("fldt %s", slot(v));
}

static void fstp(int v)
{
    line("fstpt %s", slot(v));
}

/* The flags as ucomisd x, y would set them: CF for x < y, ZF for equal,
 * PF for unordered. */
static void fcompare(int x, int y)
{
    if (cls(x) == V_LD) {
        fld(y);
        fld(x);
        line("fucomip %%st(1), %%st");
        line("fstp %%st(0)");
        return;
    }
    if (in_reg(x)) {
        line("ucomi%s %s, %s", fp_suffix(x), slot(y), slot(x));
        return;
    }
    movf_to_xmm("%xmm0", x);
    line("ucomi%s %s, %%xmm0", fp_suffix(x), slot(y));
}

/* --- instructions -------------------------------------------------------- */

/* the branch at k tests the comparison right before it, used only there */
static bool fused(int k)
{
    struct ir *b = &F->code[k];
    if (b->op != IR_BR || k == 0)
        return false;
    struct ir *d = def_of(b->a);
    return d && d->op == IR_CMP && def_at[b->a] == before(k) && uses[b->a] == 1;
}

/* the addition at k only makes the address of the load or store after it */
static bool add_folds_next(int k)
{
    struct ir *i = &F->code[k];
    if (i->op != IR_BIN || i->sub != OP_ADD || after(k) >= F->ncode || uses[i->dst] != 1)
        return false;
    struct ir *n = &F->code[after(k)];
    return (n->op == IR_LOAD || n->op == IR_STORE) && n->a == i->dst && address_folds(after(k), i->dst);
}

static bool int_cls(int v)
{
    return cls(v) == V_I32 || cls(v) == V_I64;
}

/* Does the instruction at k use value v (operand which: 0 a, 1 b, 2 c)
 * without needing it in a register or slot? */
static bool use_folds(int k, int which, int v)
{
    struct ir *i = &F->code[k];
    int64_t c;
    struct ir *d;
    if (defs[v] == 1 && chained[def_at[v]])
        return true;                            /* (a scaled index: its only use) */
    switch (i->op) {
    case IR_LOAD:
        return which == 0 && address_folds(k, v);
    case IR_STORE:
        if (which == 0)
            return address_folds(k, v);
        return which == 1 && i->size <= 8 && int_cls(v) && const32(v, &c);
    case IR_BIN:
        if (!int_cls(i->dst))
            return false;
        if (which == 1)
            switch (i->sub) {
            case OP_ADD: case OP_SUB: case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR: case OP_SAR:
                return const32(v, &c);
            case OP_MUL:
                return const32(v, &c);
            }
        d = def_of(v);
        /* (40e) also an addition address() folds through on its way (an
         * added constant over an added constant): left computed, it could
         * hold its own base's register, which the fold reads */
        return which == 0 && add_folds_next(k) && const32(i->b, &c) && d &&
               (d->op == IR_LOCAL || foldable_global(d) || address_folds(k, v));
    case IR_CMP:
        if (which == 0)
            return i->sub <= OP_ULE && int_cls(v) && const32(v, &c) && !const32(i->b, &c);
        return which == 1 && i->sub <= OP_ULE && int_cls(v) && const32(v, &c);
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
    lea_at = calloc((size_t)nc + 1, sizeof(bool));
    memop_of = calloc((size_t)nv + 1, sizeof(int));
    disp_ok = calloc((size_t)nc + 1, sizeof(bool));
    last_use = calloc((size_t)nv + 1, sizeof(int));
    if (!opt_level)
        return;
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        int ops[3] = { i->a, i->b, i->c };
        for (int j = 0; j < 3; j++)
            if (ops[j] > 0)
                last_use[ops[j]] = k;
        if (i->call) {
            for (int j = 0; j < i->call->nargs; j++)
                last_use[i->call->args[j].value] = k;
            if (i->call->ret_buffer > 0)
                last_use[i->call->ret_buffer] = k;
        }
    }
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
        if ((i->op == IR_LOAD || i->op == IR_STORE) && index_folds(k, i->a, &f) && f.scale_at >= 0)
            chained[f.scale_at] = true;
    }
    /* (40e) an access of (base + index * scale) + c, each addition used only
     * by the next: c(%base,%index,scale), both additions folded away */
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        int64_t c;
        if ((i->op != IR_LOAD && i->op != IR_STORE) || i->a <= 0 || defs[i->a] != 1)
            continue;
        int o = def_at[i->a];
        struct ir *d = &F->code[o];
        if (d->op != IR_BIN || d->sub != OP_ADD || cls(i->a) != V_I64 || o != before(k) || uses[i->a] != 1 ||
            !const32(d->b, &c) || c <= INT32_MIN / 2 || c >= INT32_MAX / 2 || d->a <= 0 || defs[d->a] != 1)
            continue;
        struct index_fold f;
        disp_ok[o] = true;
        if (index_folds(o, d->a, &f)) {
            chained[def_at[d->a]] = true;
            if (f.scale_at >= 0)
                chained[f.scale_at] = true;
        } else
            disp_ok[o] = false;
    }
    /* (39c) a scaled address addition no load or store folds: leaq */
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        struct index_fold f;
        if (!lea_scaled(k, &f) || chained[f.scale_at])
            continue;
        struct ir *n = after(k) < nc ? &F->code[after(k)] : 0;
        if (uses[i->dst] == 1 && n && (n->op == IR_LOAD || n->op == IR_STORE) && n->a == i->dst &&
            index_folds(after(k), i->dst, 0))
            continue;
        lea_at[k] = chained[f.scale_at] = true;
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
        int64_t c;
        if (chained[k] || (i->op == IR_IMM && !vec(i->dst)) || i->op == IR_LOCAL || foldable_global(i) || i->op == IR_CMP ||
            (i->op == IR_BIN && i->sub == OP_ADD && add_folds_next(k)) ||
            (i->op == IR_BIN && i->sub == OP_ADD && cls(i->dst) == V_I64 && const32(i->b, &c)))
            skip[k] = true;                     /* (40e: an added constant every use folds) */
    }
    /* (40b) a load whose one use is the instruction right after it: that
     * instruction's memory operand (its right one; a commutative one's left
     * swapped there), the load itself gone. Its address is read one
     * instruction later: nothing is set between them. */
    int *last = calloc((size_t)nv + 1, sizeof(int));     /* (each value's last use) */
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        int ops[3] = { i->a, i->b, i->c };
        for (int j = 0; j < 3; j++)
            if (ops[j] > 0)
                last[ops[j]] = k;
        if (i->call) {
            for (int j = 0; j < i->call->nargs; j++)
                last[i->call->args[j].value] = k;
            if (i->call->ret_buffer > 0)
                last[i->call->ret_buffer] = k;
        }
    }
    for (int k = 0; k < nc; k++) {
        struct ir *i = &F->code[k];
        int j = before(k);
        if (j < 0 || skip[k] || (i->op != IR_BIN && i->op != IR_CMP))
            continue;
        struct ir *l = &F->code[j];
        int v = l->dst;
        int64_t c;
        if (l->op != IR_LOAD || skip[j] || v <= 0 || defs[v] != 1 || uses[v] != 1 || vec(v) || cls(v) == V_LD ||
            l->size != value_size(cls(v)) || i->a == i->b)
            continue;
        bool fl = flt(v), comm;
        if (i->op == IR_CMP) {
            if (fl || i->sub > OP_ULE || i->b != v || const32(i->a, &c))
                continue;
            comm = false;
        } else if (fl) {
            if (!flt(i->dst) || (i->sub != OP_FADD && i->sub != OP_FSUB && i->sub != OP_FMUL && i->sub != OP_FDIV))
                continue;
            comm = i->sub == OP_FADD || i->sub == OP_FMUL;
        } else {
            if (!int_cls(i->dst) || (i->sub != OP_ADD && i->sub != OP_SUB && i->sub != OP_AND && i->sub != OP_OR &&
                                     i->sub != OP_XOR && i->sub != OP_MUL))
                continue;
            comm = i->sub != OP_SUB;
        }
        if (i->a == v && comm && i->b > 0 && !const32(i->b, &c)) {
            int t = i->a;
            i->a = i->b;
            i->b = t;
        }
        if (i->b != v || i->a <= 0 || const32(i->a, &c))
            continue;
        /* a commutative one only when its other operand ends here too (the
         * result then takes its register): else the result needs that
         * operand's copy first, a wait for it the plain load avoids (a
         * loop's recurrence: the rotation's pr * x[k].re) */
        if (comm) {
            /* (40c: its last use, made before in this block) */
            bool ends = defs[i->a] == 1 && last[i->a] == k && def_at[i->a] < k;
            for (int q = def_at[i->a] + 1; q < k && ends; q++)
                if (F->code[q].op == IR_LABEL)
                    ends = false;
            if (!ends)
                continue;
        }
        memop_of[v] = j + 1;
        skip[j] = true;
    }
    free(last);
}

/* (40b) v as a source operand: its register or slot, or the memory its
 * folded load reads */
static const char *src(int v)
{
    if (v > 0 && memop_of[v]) {
        int j = memop_of[v] - 1;
        return address(F->code[j].a, 0, j);
    }
    return slot(v);
}

/* does memory operand m name register R's 64-bit form? */
static bool names_reg(const char *m, const char *R)
{
    static const char *const pairs[][2] = {
        { "%ebx", "%rbx" }, { "%r12d", "%r12" }, { "%r13d", "%r13" }, { "%r14d", "%r14" }, { "%r15d", "%r15" },
        { "%r8d", "%r8" }, { "%r9d", "%r9" }, { "%esi", "%rsi" }, { "%edi", "%rdi" },
    };
    for (size_t k = 0; k < sizeof(pairs) / sizeof(pairs[0]); k++)
        if ((strcmp(R, pairs[k][0]) == 0 || strcmp(R, pairs[k][1]) == 0) && strstr(m, pairs[k][1]))
            return true;
    return false;
}

static const char *inverse_cc(const char *cc)
{
    static const char *pairs[][2] = { { "e", "ne" }, { "l", "ge" }, { "le", "g" }, { "b", "ae" }, { "be", "a" } };
    for (size_t k = 0; k < sizeof(pairs) / sizeof(pairs[0]); k++) {
        if (strcmp(cc, pairs[k][0]) == 0)
            return pairs[k][1];
        if (strcmp(cc, pairs[k][1]) == 0)
            return pairs[k][0];
    }
    return cc;
}

static const char *label_name(int id);

/* if (cc) goto lt else goto lf, the next instruction falling through */
static void jump_if(const char *cc, int lt, int lf, struct ir *next)
{
    if (next && next->op == IR_LABEL && next->label == lt) {
        line("j%s %s", inverse_cc(cc), label_name(lf));
        return;
    }
    line("j%s %s", cc, label_name(lt));
    if (!(next && next->op == IR_LABEL && next->label == lf))
        line("jmp %s", label_name(lf));
}

static void fcompare(int x, int y);

/* a comparison's flags; the condition (a cc) that means true, or for
 * the float == and != (with the parity flag) "feq" and "fne" */
static const char *compare_flags(struct ir *c)
{
    int a = c->a, b = c->b;
    int64_t k;
    switch (c->sub) {
    case OP_FEQ: fcompare(a, b); return "feq";
    case OP_FNE: fcompare(a, b); return "fne";
    case OP_FLT: fcompare(b, a); return "a";
    case OP_FLE: fcompare(b, a); return "ae";
    }
    static const char *cc[] = { [OP_EQ] = "e", [OP_NE] = "ne", [OP_SLT] = "l", [OP_SLE] = "le",
                                [OP_ULT] = "b", [OP_ULE] = "be" };
    /* (61c) a constant first: compared the other way round */
    static const char *swapped[] = { [OP_EQ] = "e", [OP_NE] = "ne", [OP_SLT] = "g", [OP_SLE] = "ge",
                                     [OP_ULT] = "a", [OP_ULE] = "ae" };
    if (const32(a, &k) && !const32(b, &k)) {
        const32(a, &k);
        const char *rb = in_reg(b) ? slot(b) : (load_to(ax(b), b), ax(b));
        if (k == 0 && in_reg(b))
            line("test%c %s, %s", sfx(b), rb, rb);
        else
            line("cmp%c $%lld, %s", sfx(b), (long long)k, rb);
        return swapped[c->sub];
    }
    const char *ra = in_reg(a) ? slot(a) : (load_to(ax(a), a), ax(a));
    if (const32(b, &k) && k == 0 && in_reg(a))
        line("test%c %s, %s", sfx(a), ra, ra);
    else if (const32(b, &k))
        line("cmp%c $%lld, %s", sfx(a), (long long)k, ra);
    else
        line("cmp%c %s, %s", sfx(a), src(b), ra);     /* (40b: a folded load's memory too) */
    return cc[c->sub];
}

/* Where an instruction computes value d: its own register if it has one
 * that is not also operand b's (else rax): then no store afterwards. */
static const char *work_reg(int d, int b)
{
    if (in_reg(d) && !(b > 0 && in_reg(b) && ra.reg[b] == ra.reg[d]))
        return slot(d);
    return value_size(cls(d)) == 8 ? "%rax" : "%eax";  /* (a double too) */
}

/* a into register R (nothing when it is there already) */
static void move_to(const char *R, int a)
{
    /* (a 32-bit register's name: %eXX or %rNNd) */
    bool narrow = R[1] == 'e' || R[strlen(R) - 1] == 'd';
    if (strcmp(slot(a), R) != 0)
        line("mov%c %s, %s", narrow ? 'l' : 'q', slot(a), R);
}

/* (the value computed in R: stored into d unless R is d's register) */
static void finish(int d, const char *R)
{
    if (strcmp(R, slot(d)) != 0)
        store_from(d, "%rax", "%eax");
}

static void gen_vbin(struct ir *i);

static void gen_bin(struct ir *i)
{
    int d = i->dst, a = i->a, b = i->b;
    if (vec(d)) {
        gen_vbin(i);                            /* (39f) */
        return;
    }
    if (cls(d) == V_LD) {
        fld(a);
        fld(b);
        switch (i->sub) {
        case OP_FADD: line("faddp %%st, %%st(1)"); break;
        case OP_FMUL: line("fmulp %%st, %%st(1)"); break;
        /* (GNU's AT&T names of the reversed forms: st(1) = st(1) op st) */
        case OP_FSUB: line("fsubrp %%st, %%st(1)"); break;
        case OP_FDIV: line("fdivrp %%st, %%st(1)"); break;
        }
        fstp(d);
        return;
    }
    if (cls(d) == V_F32 || cls(d) == V_F64) {
        static const char *names[] = { [OP_FADD] = "add", [OP_FSUB] = "sub", [OP_FMUL] = "mul", [OP_FDIV] = "div" };
        const char *s = fp_suffix(d);
        /* (39a) in d's register unless b is there (then, for + and *,
         * the other way round) */
        const char *R = fwork(d);
        int src_ = b;
        if (in_reg(d) && in_reg(b) && ra.reg[b] == ra.reg[d] && a != b) {
            if (i->sub == OP_FADD || i->sub == OP_FMUL)
                src_ = a;
            else
                R = "%xmm0";
        }
        const char *m = src_ == b ? src(b) : slot(src_);
        if (src_ == b)
            movf_to_xmm(R, a);
        line("%s%s %s, %s", names[i->sub], s, m, R);
        movf_from_xmm(d, R);
        return;
    }
    char q = sfx(d);
    if (lea_at[i - F->code]) {
        struct index_fold f;
        lea_scaled((int)(i - F->code), &f);
        const char *B = in_reg(f.base) ? opnd(f.base, 8) : (line("movq %s, %%rcx", opnd(f.base, 8)), "%rcx");
        const char *I = in_reg(f.index) ? opnd(f.index, 8) : (line("movq %s, %%rdx", opnd(f.index, 8)), "%rdx");
        const char *R = in_reg(d) ? opnd(d, 8) : "%rax";
        line("leaq 0(%s,%s,%d), %s", B, I, f.scale, R);
        finish(d, R);
        return;
    }
    switch (i->sub) {
    case OP_ADD: case OP_SUB: case OP_AND: case OP_OR: case OP_XOR: {
        static const char *names[] = { [OP_ADD] = "add", [OP_SUB] = "sub", [OP_AND] = "and", [OP_OR] = "or", [OP_XOR] = "xor" };
        if (i->sub != OP_SUB && in_reg(d) && in_reg(b) && ra.reg[b] == ra.reg[d] && a > 0 &&
            !(in_reg(a) && ra.reg[a] == ra.reg[d])) {
            line("%s%c %s, %s", names[i->sub], q, slot(a), slot(d));   /* (40a-2: d holds b; the order no matter) */
            return;
        }
        const char *m = src(b);                 /* (40b: its address made first: rcx, rdx) */
        const char *R = work_reg(d, b);
        if (memop_of[b] && names_reg(m, R))
            R = wide(d) ? "%rax" : "%eax";      /* (d's register still names the address) */
        move_to(R, a);
        int64_t c;
        if (const32(b, &c))
            line("%s%c $%lld, %s", names[i->sub], q, (long long)c, R);
        else
            line("%s%c %s, %s", names[i->sub], q, m, R);
        finish(d, R);
        return;
    }
    case OP_UMULH:
        /* (62a) rdx:rax = rax * b */
        load_to("%rax", a);
        line("mulq %s", slot(b));
        store_from(d, "%rdx", "%edx");
        return;
    case OP_MUL: {
        int64_t c;
        if (const32(b, &c) && !(c > 0 && (c & (c - 1)) == 0)) {
            /* (61c) imul $c, a, R */
            const char *R = work_reg(d, 0);
            line("imul%c $%lld, %s, %s", q, (long long)c, slot(a), R);
            finish(d, R);
            return;
        }
        if (!const32(b, &c) && in_reg(d) && in_reg(b) && ra.reg[b] == ra.reg[d] && a > 0 &&
            !(in_reg(a) && ra.reg[a] == ra.reg[d])) {
            line("imul%c %s, %s", q, slot(a), slot(d));   /* (40a-2: d holds b) */
            return;
        }
        const char *m = src(b);
        const char *R = work_reg(d, b);
        if (memop_of[b] && names_reg(m, R))
            R = wide(d) ? "%rax" : "%eax";
        move_to(R, a);
        if (const32(b, &c) && c > 0 && (c & (c - 1)) == 0) {
            int sh = 0;
            while ((1ll << sh) < c)
                sh++;
            if (sh)
                line("shl%c $%d, %s", q, sh, R);
        } else
            line("imul%c %s, %s", q, m, R);
        finish(d, R);
        return;
    }
    case OP_SDIV: case OP_SMOD:
        load_to(ax(d), a);
        line(wide(d) ? "cqto" : "cltd");
        line("idiv%c %s", q, slot(b));
        if (i->sub == OP_SMOD) {
            store_from(d, "%rdx", "%edx");
            return;
        }
        break;
    case OP_UDIV: case OP_UMOD:
        load_to(ax(d), a);
        line("xorl %%edx, %%edx");
        line("div%c %s", q, slot(b));
        if (i->sub == OP_UMOD) {
            store_from(d, "%rdx", "%edx");
            return;
        }
        break;
    case OP_SHL: case OP_SHR: case OP_SAR: {
        static const char *names[] = { [OP_SHL] = "shl", [OP_SHR] = "shr", [OP_SAR] = "sar" };
        int64_t c;
        if (const32(b, &c)) {
            const char *R = work_reg(d, 0);
            move_to(R, a);
            line("%s%c $%d, %s", names[i->sub], q, (int)(c & (wide(d) ? 63 : 31)), R);
            finish(d, R);
            return;
        }
        load_to(cx(b), b);
        load_to(ax(d), a);
        line("%s%c %%cl, %s", names[i->sub], q, ax(d));
        break;
    }
    }
    store_from(d, "%rax", "%eax");
}

static void gen_cmp(struct ir *i)
{
    const char *cc = compare_flags(i);
    if (strcmp(cc, "feq") == 0) {
        line("sete %%al");
        line("setnp %%cl");
        line("andb %%cl, %%al");
    } else if (strcmp(cc, "fne") == 0) {
        line("setne %%al");
        line("setp %%cl");
        line("orb %%cl, %%al");
    } else
        line("set%s %%al", cc);
    line("movzbl %%al, %%eax");
    line("movl %%eax, %s", slot(i->dst));
}

static void gen_conv(struct ir *i)
{
    int d = i->dst, a = i->a;
    switch (i->sub) {
    case CONV_SEXT: case CONV_ZEXT: case CONV_TRUNC: {
        /* (61c) computed in d's own register when it has one */
        const char *R = work_reg(d, 0), *R32 = in_reg(d) ? opnd(d, 4) : "%eax";
        if (i->sub == CONV_TRUNC)
            line("movl %s, %s", opnd(a, 4), R32);
        else if (i->sub == CONV_SEXT)
            switch (i->size) {
            case 1: line("movsb%c %s, %s", sfx(d), opnd(a, 1), R); break;
            case 2: line("movsw%c %s, %s", sfx(d), opnd(a, 2), R); break;
            case 4: line(wide(d) ? "movslq %s, %s" : "movl %s, %s", opnd(a, 4), wide(d) ? R : R32); break;
            default: line("movq %s, %s", opnd(a, 8), R);
            }
        else
            switch (i->size) {
            case 1: line("movzbl %s, %s", opnd(a, 1), R32); break;
            case 2: line("movzwl %s, %s", opnd(a, 2), R32); break;
            case 4: line("movl %s, %s", opnd(a, 4), R32); break;
            default: line("movq %s, %s", opnd(a, 8), R);
            }
        finish(d, R);
        return;
    }
    case CONV_S2F:
        if (cls(d) == V_LD) {
            line(i->size == 8 ? "fildll %s" : "fildl %s", mem_in(a));
            fstp(d);
            return;
        }
        line("cvtsi2%s%c %s, %s", fp_suffix(d), i->size == 8 ? 'q' : 'l', slot(a), fwork(d));
        movf_from_xmm(d, fwork(d));
        return;
    case CONV_U2F:
        if (i->size == 4) {
            line("movl %s, %%eax", slot(a));
            if (cls(d) == V_LD) {
                line("movq %%rax, %s", slot(d));
                line("fildll %s", slot(d));
                fstp(d);
                return;
            }
            line("cvtsi2%sq %%rax, %%xmm0", fp_suffix(d));
            movf_from_xmm(d, "%xmm0");
            return;
        }
        if (cls(d) == V_LD) {
            /* fild takes it as signed: 2^64 more when the top bit was set */
            int done = new_unique();
            line("fildll %s", mem_in(a));
            line("cmpq $0, %s", slot(a));
            line("jge .Lc%d", done);
            line("fadds .L.two64(%%rip)");
            need_two64 = true;
            fprintf(out, ".Lc%d:\n", done);
            fstp(d);
            return;
        }
        {
            /* halved (keeping the lowest bit for the rounding), converted, doubled */
            int big = new_unique(), done = new_unique();
            const char *s = fp_suffix(d);
            line("movq %s, %%rax", slot(a));
            line("testq %%rax, %%rax");
            line("js .Lc%d", big);
            line("cvtsi2%sq %%rax, %%xmm0", s);
            line("jmp .Lc%d", done);
            fprintf(out, ".Lc%d:\n", big);
            line("movq %%rax, %%rcx");
            line("shrq %%rcx");
            line("andl $1, %%eax");
            line("orq %%rax, %%rcx");
            line("cvtsi2%sq %%rcx, %%xmm0", s);
            line("add%s %%xmm0, %%xmm0", s);
            fprintf(out, ".Lc%d:\n", done);
            movf_from_xmm(d, "%xmm0");
        }
        return;
    case CONV_F2S:
        if (cls(a) == V_LD) {
            fld(a);
            line(wide(d) ? "fisttpll %s" : "fisttpl %s", mem_out(d));
            mem_out_done(d);
            return;
        }
        line("cvtt%s2si%c %s, %s", fp_suffix(a), sfx(d), slot(a), ax(d));
        store_from(d, "%rax", "%eax");
        return;
    case CONV_F2U:
        if (!wide(d)) {
            /* fits in a signed 64-bit conversion */
            if (cls(a) == V_LD) {
                fld(a);
                line("fisttpll %s", mem_out(d));
                mem_out_done(d);
                return;
            }
            line("cvtt%s2siq %s, %%rax", fp_suffix(a), slot(a));
            store_from(d, "%rax", "%eax");
            return;
        }
        if (cls(a) == V_LD) {
            int big = new_unique(), done = new_unique();
            need_two63f = true;
            fld(a);
            line("flds .L.two63(%%rip)");
            line("fucomip %%st(1), %%st");
            line("jbe .Lc%d", big);
            line("fisttpll %s", mem_out(d));
            line("jmp .Lc%d", done);
            fprintf(out, ".Lc%d:\n", big);
            line("fsubs .L.two63(%%rip)");
            line("fisttpll %s", mem_out(d));
            line("btcq $63, %s", mem_out(d));
            fprintf(out, ".Lc%d:\n", done);
            mem_out_done(d);
            return;
        }
        {
            int big = new_unique(), done = new_unique();
            const char *s = fp_suffix(a);
            movf_to_xmm("%xmm0", a);
            if (cls(a) == V_F32)
                line("movl $0x5f000000, %%eax");
            else
                line("movabsq $0x43e0000000000000, %%rax");
            line("movq %%rax, %%xmm1");
            line("ucomi%s %%xmm1, %%xmm0", s);
            line("jae .Lc%d", big);
            line("cvtt%s2siq %%xmm0, %%rax", s);
            line("jmp .Lc%d", done);
            fprintf(out, ".Lc%d:\n", big);
            line("sub%s %%xmm1, %%xmm0", s);
            line("cvtt%s2siq %%xmm0, %%rax", s);
            line("btcq $63, %%rax");
            fprintf(out, ".Lc%d:\n", done);
            store_from(d, "%rax", "%eax");
        }
        return;
    case CONV_F2F:
        if (cls(d) == V_LD) {
            line(cls(a) == V_F32 ? "flds %s" : "fldl %s", fmem_in(a));
            fstp(d);
            return;
        }
        if (cls(a) == V_LD) {
            fld(a);
            if (in_reg(d)) {
                line(cls(d) == V_F32 ? "fstps %d(%%rbp)" : "fstpl %d(%%rbp)", scratch);
                line("mov%s %d(%%rbp), %s", fp_suffix(d), scratch, slot(d));
            } else
                line(cls(d) == V_F32 ? "fstps %s" : "fstpl %s", slot(d));
            return;
        }
        line("cvt%s2%s %s, %s", fp_suffix(a), fp_suffix(d), slot(a), fwork(d));
        movf_from_xmm(d, fwork(d));
        return;
    }
}

static const char *gp_regs[] = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9" };
static const char *gp_regs32[] = { "%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d" };

struct arg_place {
    bool in_regs;
    int classes[2], n;                          /* (structures) */
    int reg, reg2;                              /* first registers: gp or sse numbers */
    int stack;                                  /* offset in the outgoing area */
};

/* (62f) a call in the Microsoft convention (UEFI's runtime services from the
 * kernel): integers and pointers in rcx, rdx, r8, r9, the rest on the stack
 * above 32 bytes of shadow space the callee may use; the result in rax */
static void gen_ms_call(struct ir *i)
{
    static const char *ms_regs[] = { "%rcx", "%rdx", "%r8", "%r9" };
    static const char *ms_regs32[] = { "%ecx", "%edx", "%r8d", "%r9d" };
    struct ir_call *c = i->call;
    struct type *rt = unqual(i->type);
    if (is_memory_value(rt) || is_flonum(rt) || c->func_type->is_variadic)
        error_at(i->tok, "an ms_abi call returns an integer or a pointer, with fixed arguments");
    int area = align_up(32 + 8 * (c->nargs > 4 ? c->nargs - 4 : 0), 16);
    line("subq $%d, %%rsp", area);
    for (int k = 0; k < c->nargs; k++) {
        struct type *ty = c->args[k].type;
        int v = c->args[k].value;
        if (is_memory_value(ty) || is_flonum(ty))
            error_at(i->tok, "an ms_abi call takes integers and pointers only");
        if (k >= 4) {
            line("movq %s, %%rax", opnd(v, 8));
            line("movq %%rax, %d(%%rsp)", 32 + 8 * (k - 4));
        }
    }
    if (!i->sym)
        line("movq %s, %%r11", slot(i->a));
    for (int k = 0; k < c->nargs && k < 4; k++) {
        int v = c->args[k].value;
        if (wide(v))
            line("movq %s, %s", slot(v), ms_regs[k]);
        else
            line("movl %s, %s", slot(v), ms_regs32[k]);
    }
    if (i->sym)
        line("call %s", i->sym);
    else
        line("call *%%r11");
    line("addq $%d, %%rsp", area);
}

static void gen_call(struct ir *i)
{
    struct ir_call *c = i->call;
    struct type *rt = unqual(i->type);
    if (c->func_type->is_ms_abi) {
        gen_ms_call(i);
        goto scalar_result;
    }
    bool sret = false;
    int ret_classes[2], ret_n = 0;
    if (is_memory_value(rt) && !x87_complex(rt))
        sret = !struct_in_registers(rt, ret_classes, &ret_n);
    struct arg_place *places = arena_alloc(sizeof(struct arg_place) * (size_t)(c->nargs + 1));
    int gp = sret ? 1 : 0, sse = 0, stack = 0;
    for (int k = 0; k < c->nargs; k++) {
        struct type *ty = c->args[k].type;
        struct arg_place *p = &places[k];
        if (is_memory_value(ty)) {
            if (struct_in_registers(ty, p->classes, &p->n)) {
                int need_gp = 0, need_sse = 0;
                for (int j = 0; j < p->n; j++)
                    p->classes[j] == CLASS_INT ? need_gp++ : need_sse++;
                if (gp + need_gp <= 6 && sse + need_sse <= 8) {
                    p->in_regs = true;
                    p->reg = gp;
                    p->reg2 = sse;
                    gp += need_gp;
                    sse += need_sse;
                    continue;
                }
            }
        } else if (ty->kind != TY_LDOUBLE) {
            if ((is_flonum(ty) || unqual(ty)->is_vector) && sse < 8) {
                p->in_regs = true;
                p->reg = sse++;
                continue;
            }
            if (unqual(ty)->is_vector)
                error_at(i->tok, "a vector argument after eight in xmm registers is not supported");
            if (!is_flonum(ty) && gp < 6) {
                p->in_regs = true;
                p->reg = gp++;
                continue;
            }
        }
        int a = ty->align > 8 ? 16 : 8;
        stack = align_up(stack, a);
        p->stack = stack;
        stack += align_up(ty->size, 8);
    }
    int area = align_up(stack, 16);
    if (area)
        line("subq $%d, %%rsp", area);
    /* the arguments in memory first (copies use rax) */
    for (int k = 0; k < c->nargs; k++) {
        struct arg_place *p = &places[k];
        if (p->in_regs)
            continue;
        struct type *ty = c->args[k].type;
        int v = c->args[k].value;
        if (is_memory_value(ty)) {
            load_address("%r11", v);
            copy_bytes("%rsp", p->stack, "%r11", 0, ty->size);
        } else if (ty->kind == TY_LDOUBLE) {
            line("movq %s, %%rax", slot(v));
            line("movq %%rax, %d(%%rsp)", p->stack);
            line("movq %s, %%rax", slot_at(v, 8));
            line("movq %%rax, %d(%%rsp)", p->stack + 8);
        } else {
            line("movq %s, %%rax", opnd(v, 8));
            line("movq %%rax, %d(%%rsp)", p->stack);
        }
    }
    /* then the registers */
    for (int k = 0; k < c->nargs; k++) {
        struct arg_place *p = &places[k];
        if (!p->in_regs)
            continue;
        struct type *ty = c->args[k].type;
        int v = c->args[k].value;
        if (is_memory_value(ty)) {
            load_address("%r11", v);
            int g = p->reg, s = p->reg2;
            for (int j = 0; j < p->n; j++) {
                int n = ty->size - 8 * j < 8 ? ty->size - 8 * j : 8;
                if (p->classes[j] == CLASS_INT) {
                    load_partial(gp_regs[g], gp_regs32[g], "%r11", 8 * j, n);
                    g++;
                } else {
                    if (n == 4)
                        line("movss %d(%%r11), %%xmm%d", 8 * j, s);
                    else if (n == 8)
                        line("movsd %d(%%r11), %%xmm%d", 8 * j, s);
                    else {
                        load_partial("%rax", "%eax", "%r11", 8 * j, n);
                        line("movq %%rax, %%xmm%d", s);
                    }
                    s++;
                }
            }
            continue;
        }
        if (vec(v))
            vec_to(p->reg, v);                  /* (32h) */
        else if (is_flonum(ty))
            movf_to_xmm(xname(p->reg, false), v);
        else if (wide(v))
            line("movq %s, %s", slot(v), gp_regs[p->reg]);
        else
            line("movl %s, %s", slot(v), gp_regs32[p->reg]);
    }
    if (sret)
        line("movq %s, %%rdi", slot(c->ret_buffer));
    if (!i->sym)
        line("movq %s, %%r11", slot(i->a));
    if (c->func_type->is_variadic || !c->func_type->has_prototype)
        line("movl $%d, %%eax", sse);
    if (avx_fn) {
        /* (32h) the upper halves cleared for the callee's SSE code, unless
         * a 256-bit vector goes to it or comes back */
        bool ymm = cls(i->dst ? i->dst : 0) == V_V32;
        for (int k = 0; k < c->nargs; k++)
            ymm |= c->args[k].value > 0 && cls(c->args[k].value) == V_V32;
        if (!ymm)
            line("vzeroupper");
    }
    if (i->sym) {
        if (symbol_is_local(i->obj))
            line("call %s", i->sym);
        else
            line("call %s@PLT", i->sym);
    } else
        line("call *%%r11");
    if (area)
        line("addq $%d, %%rsp", area);
    /* the result */
    if (x87_complex(rt)) {
        line("movq %s, %%r11", slot(c->ret_buffer));
        line("fstpt 0(%%r11)");
        line("fstpt 16(%%r11)");
        return;
    }
    if (is_memory_value(rt)) {
        if (sret)
            return;
        line("movq %s, %%r11", slot(c->ret_buffer));
        int g = 0, s = 0;
        for (int j = 0; j < ret_n; j++) {
            int n = rt->size - 8 * j < 8 ? rt->size - 8 * j : 8;
            if (ret_classes[j] == CLASS_INT) {
                const char *r = g++ ? "%rdx" : "%rax";
                if (n == 8)
                    line("movq %s, %d(%%r11)", r, 8 * j);
                else
                    store_partial("%r11", 8 * j, r, n);
            } else {
                int x = s++;
                if (n == 8)
                    line("movsd %%xmm%d, %d(%%r11)", x, 8 * j);
                else if (n == 4)
                    line("movss %%xmm%d, %d(%%r11)", x, 8 * j);
                else {
                    line("movq %%xmm%d, %%rcx", x);
                    store_partial("%r11", 8 * j, "%rcx", n);
                }
            }
        }
        return;
    }
scalar_result:
    if (!i->dst)
        return;
    switch (cls(i->dst)) {
    case V_F32: case V_F64: movf_from_xmm(i->dst, "%xmm0"); break;
    case V_LD: fstp(i->dst); break;
    case V_V16: case V_V32: vec_from(i->dst, 0); break;
    default: store_from(i->dst, "%rax", "%eax");
    }
}

static void gen_ret(struct ir *i)
{
    struct type *rt = unqual(i->type);
    if (i->a && x87_complex(rt)) {
        load_address("%r11", i->a);
        line("fldt 16(%%r11)");
        line("fldt 0(%%r11)");
    } else if (i->a) {
        if (is_memory_value(rt)) {
            int classes[2], n;
            load_address("%r11", i->a);
            if (!struct_in_registers(rt, classes, &n)) {
                line("movq %d(%%rbp), %%r10", sret_slot);
                copy_bytes("%r10", 0, "%r11", 0, rt->size);
                line("movq %%r10, %%rax");
            } else {
                int g = 0, s = 0;
                for (int j = 0; j < n; j++) {
                    int size = rt->size - 8 * j < 8 ? rt->size - 8 * j : 8;
                    if (classes[j] == CLASS_INT) {
                        const char *r = g ? "%rdx" : "%rax", *r32 = g ? "%edx" : "%eax";
                        g++;
                        if (size == 8)
                            line("movq %d(%%r11), %s", 8 * j, r);
                        else {
                            /* (rax may already hold the first eightbyte) */
                            load_partial(g == 1 ? "%rax" : "%rdx", r32, "%r11", 8 * j, size);
                        }
                    } else {
                        if (size == 4)
                            line("movss %d(%%r11), %%xmm%d", 8 * j, s);
                        else if (size == 8)
                            line("movsd %d(%%r11), %%xmm%d", 8 * j, s);
                        else {
                            load_partial("%rcx", "%ecx", "%r11", 8 * j, size);
                            line("movq %%rcx, %%xmm%d", s);
                        }
                        s++;
                    }
                }
            }
        } else
            switch (cls(i->a)) {
            case V_F32: case V_F64: movf_to_xmm("%xmm0", i->a); break;
            case V_LD: fld(i->a); break;
            case V_V16: case V_V32: vec_to(0, i->a); break;     /* (32h) */
            default: load_to(ax(i->a), i->a);
            }
    }
    for (int k = 0; k < SAVED_REGS; k++)
        if (reg_used[k])
            line("movq %d(%%rbp), %s", saves + 8 * k, vreg_names[k][3]);
    if (ms_saves) {
        line("movq %d(%%rbp), %%rsi", ms_saves);
        line("movq %d(%%rbp), %%rdi", ms_saves + 8);
    }
    if (avx_fn && !(i->a && cls(i->a) == V_V32))
        line("vzeroupper");                     /* (32h: for the caller's SSE code) */
    line("leave");
    line("ret");
}

static void gen_va_start(struct ir *i)
{
    load_address("%rax", i->a);
    line("movl $%d, (%%rax)", va_gp * 8);
    line("movl $%d, 4(%%rax)", 48 + va_fp * 16);
    line("leaq %d(%%rbp), %%rcx", va_stack);
    line("movq %%rcx, 8(%%rax)");
    line("leaq %d(%%rbp), %%rcx", va_area);
    line("movq %%rcx, 16(%%rax)");
}

static void va_from_overflow(int size, int align, int dst)
{
    /* rcx: the va_list */
    line("movq 8(%%rcx), %%rdx");
    if (align > 8) {
        line("addq $15, %%rdx");
        line("andq $-16, %%rdx");
    }
    line("leaq %d(%%rdx), %%rax", align_up(size, 8));
    line("movq %%rax, 8(%%rcx)");
    line("movq %%rdx, %s", slot(dst));
}

static void gen_va_arg(struct ir *i)
{
    struct type *ty = unqual(i->type);
    load_address("%rcx", i->a);
    int mem = new_unique(), done = new_unique();
    if (ty->is_vector)
        error_at(i->tok, "va_arg of a vector is not supported");
    if (is_memory_value(ty)) {
        int classes[2], n;
        if (!struct_in_registers(ty, classes, &n)) {
            va_from_overflow(ty->size, ty->align, i->dst);
            return;
        }
        int need_gp = 0, need_sse = 0;
        for (int j = 0; j < n; j++)
            classes[j] == CLASS_INT ? need_gp++ : need_sse++;
        if (need_gp) {
            line("cmpl $%d, (%%rcx)", 48 - 8 * need_gp);
            line("ja .Lc%d", mem);
        }
        if (need_sse) {
            line("cmpl $%d, 4(%%rcx)", 176 - 16 * need_sse);
            line("ja .Lc%d", mem);
        }
        /* the eightbytes from the save area into this va_arg's room */
        int room = (int)i->imm;
        for (int j = 0; j < n; j++) {
            if (classes[j] == CLASS_INT) {
                line("movl (%%rcx), %%eax");
                line("addq 16(%%rcx), %%rax");
                line("movq (%%rax), %%rax");
                line("addl $8, (%%rcx)");
            } else {
                line("movl 4(%%rcx), %%eax");
                line("addq 16(%%rcx), %%rax");
                line("movq (%%rax), %%rax");
                line("addl $16, 4(%%rcx)");
            }
            line("movq %%rax, %d(%%rbp)", room + 8 * j);
        }
        line("leaq %d(%%rbp), %%rax", room);
        line("movq %%rax, %s", slot(i->dst));
        line("jmp .Lc%d", done);
        fprintf(out, ".Lc%d:\n", mem);
        va_from_overflow(ty->size, ty->align, i->dst);
        fprintf(out, ".Lc%d:\n", done);
        return;
    }
    if (ty->kind == TY_LDOUBLE) {
        va_from_overflow(16, 16, i->dst);
        return;
    }
    bool fp = is_flonum(ty);
    if (fp) {
        line("movl 4(%%rcx), %%eax");
        line("cmpl $176, %%eax");
    } else {
        line("movl (%%rcx), %%eax");
        line("cmpl $48, %%eax");
    }
    line("jae .Lc%d", mem);
    line("movl %%eax, %%edx");
    line("addq 16(%%rcx), %%rdx");
    line("addl $%d, %%eax", fp ? 16 : 8);
    line("movl %%eax, %s(%%rcx)", fp ? "4" : "");
    line("movq %%rdx, %s", slot(i->dst));
    line("jmp .Lc%d", done);
    fprintf(out, ".Lc%d:\n", mem);
    va_from_overflow(8, 8, i->dst);
    fprintf(out, ".Lc%d:\n", done);
}

static char size_suffix(int size)
{
    return size == 1 ? 'b' : size == 2 ? 'w' : size == 4 ? 'l' : 'q';
}

static const char *ax_of(int size)
{
    return size == 1 ? "%al" : size == 2 ? "%ax" : size == 4 ? "%eax" : "%rax";
}

static const char *dx_of(int size)
{
    return size == 1 ? "%dl" : size == 2 ? "%dx" : size == 4 ? "%edx" : "%rdx";
}

static void load_sized(const char *reg64, int v)
{
    /* (enough bytes of a value for an operation of up to 8 bytes) */
    line("movq %s, %s", opnd(v, 8), reg64);
}

static void gen_atomic(struct ir *i)
{
    int size = i->size;
    char s = size_suffix(size);
    if (i->sub == AT_FENCE) {
        line("mfence");
        return;
    }
    load_address("%rcx", i->a);
    switch (i->sub) {
    case AT_LOAD:
        if (size == 1)
            line("movzbl (%%rcx), %%eax");
        else if (size == 2)
            line("movzwl (%%rcx), %%eax");
        else
            line("mov%c (%%rcx), %s", s, ax_of(size));
        break;
    case AT_STORE:
        load_sized("%rax", i->b);
        line("xchg%c %s, (%%rcx)", s, ax_of(size));
        return;
    case AT_XCHG:
        load_sized("%rax", i->b);
        line("xchg%c %s, (%%rcx)", s, ax_of(size));
        break;
    case AT_ADD: case AT_SUB:
        load_sized("%rax", i->b);
        if (i->sub == AT_SUB)
            line("neg%c %s", s, ax_of(size));
        line("movq %%rax, %%rdx");
        line("lock xadd%c %s, (%%rcx)", s, ax_of(size));
        if (i->fetch_new)
            line("add%c %s, %s", s, dx_of(size), ax_of(size));
        break;
    case AT_AND: case AT_OR: case AT_XOR: {
        static const char *names[] = { [AT_AND] = "and", [AT_OR] = "or", [AT_XOR] = "xor" };
        int again = new_unique();
        load_sized("%rsi", i->b);
        line("mov%c (%%rcx), %s", s, ax_of(size));
        fprintf(out, ".Lc%d:\n", again);
        line("movq %%rax, %%rdx");
        line("%s%c %s, %s", names[i->sub], s,
             size == 1 ? "%sil" : size == 2 ? "%si" : size == 4 ? "%esi" : "%rsi", dx_of(size));
        line("lock cmpxchg%c %s, (%%rcx)", s, dx_of(size));
        line("jne .Lc%d", again);
        if (i->fetch_new)
            line("movq %%rdx, %%rax");
        break;
    }
    case AT_CAS: {
        int ok = new_unique();
        load_address("%rsi", i->b);
        line("mov%c (%%rsi), %s", s, ax_of(size));
        load_sized("%rdx", i->c);
        line("lock cmpxchg%c %s, (%%rcx)", s, dx_of(size));
        line("sete %%dl");
        line("je .Lc%d", ok);
        line("mov%c %s, (%%rsi)", s, ax_of(size));
        fprintf(out, ".Lc%d:\n", ok);
        line("movzbl %%dl, %%eax");
        break;
    }
    }
    if (i->dst)
        store_from(i->dst, "%rax", "%eax");
}

static void gen_builtin(struct ir *i)
{
    int d = i->dst, a = i->a;
    switch (i->sub) {
    case BI_SQRT:
        line("sqrt%s %s, %s", fp_suffix(a), slot(a), fwork(d));
        movf_from_xmm(d, fwork(d));
        return;
    case BI_FABS:
        /* (the sign bit cleared in an integer register) */
        load_to(cls(a) == V_F64 ? "%rax" : "%eax", a);
        if (cls(a) == V_F64) {
            line("shlq $1, %%rax");
            line("shrq $1, %%rax");
        } else
            line("andl $0x7fffffff, %%eax");
        break;
    case BI_CLZ:
        if (i->size == 8) {
            line("bsrq %s, %%rax", slot(a));
            line("xorl $63, %%eax");
        } else {
            line("bsrl %s, %%eax", slot(a));
            line("xorl $31, %%eax");
        }
        break;
    case BI_CTZ:
        if (i->size == 8)
            line("bsfq %s, %%rax", slot(a));
        else
            line("bsfl %s, %%eax", slot(a));
        break;
    case BI_POPCOUNT:
        /* (without the popcnt instruction: counts of bit pairs, nibbles, bytes) */
        if (i->size == 8)
            line("movq %s, %%rax", slot(a));
        else
            line("movl %s, %%eax", slot(a));
        line("movq %%rax, %%rcx");
        line("shrq %%rcx");
        line("movabsq $0x5555555555555555, %%rdx");
        line("andq %%rdx, %%rcx");
        line("subq %%rcx, %%rax");
        line("movabsq $0x3333333333333333, %%rdx");
        line("movq %%rax, %%rcx");
        line("andq %%rdx, %%rax");
        line("shrq $2, %%rcx");
        line("andq %%rdx, %%rcx");
        line("addq %%rcx, %%rax");
        line("movq %%rax, %%rcx");
        line("shrq $4, %%rcx");
        line("addq %%rcx, %%rax");
        line("movabsq $0x0f0f0f0f0f0f0f0f, %%rdx");
        line("andq %%rdx, %%rax");
        line("movabsq $0x0101010101010101, %%rdx");
        line("imulq %%rdx, %%rax");
        line("shrq $56, %%rax");
        break;
    case BI_BSWAP16:
        line("movzwl %s, %%eax", opnd(a, 2));
        line("rolw $8, %%ax");
        break;
    case BI_BSWAP32:
        line("movl %s, %%eax", slot(a));
        line("bswapl %%eax");
        break;
    case BI_BSWAP64:
        line("movq %s, %%rax", slot(a));
        line("bswapq %%rax");
        break;
    }
    store_from(d, "%rax", "%eax");
}

/* (39f) the packed loops' operations: lanes of 4 bytes (int or float by
 * the operation), b from its register or xmm1, the result in d's register
 * or xmm0 */
static void gen_vbin(struct ir *i)
{
    int d = i->dst, a = i->a, b = i->b;
    const char *op = 0;
    switch (i->sub) {
    case OP_ADD: op = "paddd"; break;
    case OP_SUB: op = "psubd"; break;
    case OP_AND: op = "pand"; break;
    case OP_OR: op = "por"; break;
    case OP_XOR: op = "pxor"; break;
    case OP_SHL: op = "pslld"; break;
    case OP_SHR: op = "psrld"; break;
    case OP_SAR: op = "psrad"; break;
    case OP_FADD: op = "addps"; break;
    case OP_FSUB: op = "subps"; break;
    case OP_FMUL: op = "mulps"; break;
    case OP_FDIV: op = "divps"; break;
    default: error_at(i->tok, "internal error: a vector operation cc does not have");
    }
    int R = vreg(d) >= 0 && vreg(b) != vreg(d) ? vreg(d) : 0;
    int64_t c;
    if (i->sub == OP_SHL || i->sub == OP_SHR || i->sub == OP_SAR) {
        const32(b, &c);
        vec_to(R, a);
        line("%s $%d, %s", op, (int)c, xname(R, false));
    } else {
        int x = vec_in(b, 1);
        vec_to(R, a);
        line("%s %s, %s", op, xname(x, false), xname(R, false));
    }
    vec_from(d, R);
}

/* a value in every lane */
static void gen_vsplat(struct ir *i)
{
    int d = i->dst, a = i->a, x = vreg(d) >= 0 ? vreg(d) : 0;
    if (flt(a)) {
        movf_to_xmm(xname(x, false), a);
        line("shufps $0, %s, %s", xname(x, false), xname(x, false));
    } else {
        load_to("%eax", a);
        line("movd %%eax, %s", xname(x, false));
        line("pshufd $0, %s, %s", xname(x, false), xname(x, false));
    }
    vec_from(d, x);
}

/* the sum of the 4 int lanes */
static void gen_vhadd(struct ir *i)
{
    vec_to(0, i->a);
    line("pshufd $0x4e, %%xmm0, %%xmm1");
    line("paddd %%xmm1, %%xmm0");
    line("pshufd $0xb1, %%xmm0, %%xmm1");
    line("paddd %%xmm1, %%xmm0");
    line("movd %%xmm0, %%eax");
    store_from(i->dst, "%rax", "%eax");
}

/* (39f-2) 8 words in reverse order; pairs of int16 products summed */
static void gen_vrevw(struct ir *i)
{
    int x = vreg(i->dst) >= 0 ? vreg(i->dst) : 0;
    vec_to(x, i->a);
    line("pshufd $0x1b, %s, %s", xname(x, false), xname(x, false));
    line("pshuflw $0xb1, %s, %s", xname(x, false), xname(x, false));
    line("pshufhw $0xb1, %s, %s", xname(x, false), xname(x, false));
    vec_from(i->dst, x);
}

static void gen_vmadd(struct ir *i)
{
    int d = i->dst, R = vreg(d) >= 0 && vreg(i->b) != vreg(d) ? vreg(d) : 0;
    int x = vec_in(i->b, 1);
    vec_to(R, i->a);
    line("pmaddwd %s, %s", xname(x, false), xname(R, false));
    vec_from(d, R);
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
        line("jmp %s", label_name(i->label));
        return;
    case IR_BR:
        if (fused(k)) {
            const char *cc = compare_flags(def_of(i->a));
            if (strcmp(cc, "feq") == 0) {
                line("jp %s", label_name(i->label2));
                jump_if("e", i->label, i->label2, next);
            } else if (strcmp(cc, "fne") == 0) {
                line("jp %s", label_name(i->label));
                jump_if("ne", i->label, i->label2, next);
            } else
                jump_if(cc, i->label, i->label2, next);
            return;
        }
        if (in_reg(i->a))
            line("test%c %s, %s", sfx(i->a), slot(i->a), slot(i->a));
        else
            line("cmp%c $0, %s", sfx(i->a), slot(i->a));
        if (next && next->op == IR_LABEL && next->label == i->label) {
            line("je %s", label_name(i->label2));
            return;
        }
        line("jne %s", label_name(i->label));
        if (!(next && next->op == IR_LABEL && next->label == i->label2))
            line("jmp %s", label_name(i->label2));
        return;
    case IR_IMM:
        if (vec(i->dst)) {
            /* (32h) a vector's zero */
            int x = vreg(i->dst) >= 0 ? vreg(i->dst) : 0;
            if (cls(i->dst) == V_V32)
                line("vpxor %s, %s, %s", xname(x, false), xname(x, false), xname(x, false));
            else
                line("pxor %s, %s", xname(x, false), xname(x, false));
            vec_from(i->dst, x);
            return;
        }
        if (wide(i->dst)) {
            if (i->imm >= INT32_MIN && i->imm <= INT32_MAX)
                line("movq $%lld, %s", (long long)i->imm, slot(i->dst));
            else {
                line("movabsq $%lld, %%rax", (long long)i->imm);
                line("movq %%rax, %s", slot(i->dst));
            }
        } else
            line("movl $%d, %s", (int)(int32_t)i->imm, slot(i->dst));
        return;
    case IR_FIMM: {
        if (in_reg(i->dst)) {
            /* (39a) a register's constant: zero by xorps (+0.0 only), else
             * its bits through rax */
            uint64_t bits;
            if (cls(i->dst) == V_F32) {
                float f = (float)i->fimm;
                uint32_t b32;
                memcpy(&b32, &f, 4);
                bits = b32;
            } else
                memcpy(&bits, &i->fimm, 8);
            if (bits == 0)
                line("xorps %s, %s", slot(i->dst), slot(i->dst));
            else if (cls(i->dst) == V_F32) {
                line("movl $%u, %%eax", (unsigned)bits);
                line("movd %%eax, %s", slot(i->dst));
            } else {
                line("movabsq $%llu, %%rax", (unsigned long long)bits);
                line("movq %%rax, %s", slot(i->dst));
            }
            return;
        }
        if (cls(i->dst) == V_F32) {
            float f = (float)i->fimm;
            uint32_t bits;
            memcpy(&bits, &f, 4);
            line("movl $%u, %s", bits, slot(i->dst));
        } else if (cls(i->dst) == V_F64) {
            uint64_t bits;
            memcpy(&bits, &i->fimm, 8);
            line("movabsq $%llu, %%rax", (unsigned long long)bits);
            line("movq %%rax, %s", slot(i->dst));
        } else {
            unsigned char b[16];
            long_double_bytes(i->fimm, b);
            uint64_t lo = 0, hi = 0;
            for (int k = 0; k < 8; k++) {
                lo |= (uint64_t)b[k] << (8 * k);
                hi |= (uint64_t)b[8 + k] << (8 * k);
            }
            line("movabsq $%llu, %%rax", (unsigned long long)lo);
            line("movq %%rax, %s", slot(i->dst));
            line("movq $%llu, %s", (unsigned long long)hi, slot_at(i->dst, 8));
        }
        return;
    }
    case IR_LOCAL:
        line("leaq %d(%%rbp), %%rax", i->obj->offset);
        line("movq %%rax, %s", slot(i->dst));
        return;
    case IR_GLOBAL:
        global_address("%rax", i->sym, i->obj, i->imm);
        line("movq %%rax, %s", slot(i->dst));
        return;
    case IR_LOAD: {
        if (vec(i->dst)) {
            int x = vreg(i->dst) >= 0 ? vreg(i->dst) : 0;
            line("%s %s, %s", cls(i->dst) == V_V32 ? "vmovdqu" : "movdqu", address(i->a, 0, k),
                 xname(x, cls(i->dst) == V_V32));
            vec_from(i->dst, x);
            return;
        }
        if (cls(i->dst) == V_LD) {
            const char *m0 = address(i->a, 0, k);
            line("movq %s, %%rax", m0);
            line("movq %%rax, %s", slot(i->dst));
            line("movq %s, %%rax", address(i->a, 8, k));
            line("movq %%rax, %s", slot_at(i->dst, 8));
            return;
        }
        if (flt(i->dst) && in_reg(i->dst)) {
            /* (39a) straight into its xmm register */
            line("mov%s %s, %s", fp_suffix(i->dst), address(i->a, 0, k), slot(i->dst));
            return;
        }
        const char *m = address(i->a, 0, k);
        const char *R = work_reg(i->dst, 0);
        int dd = i->dst;
        switch (i->size) {
        case 1: line(i->is_signed ? "movsb%c %s, %s" : "movzb%c %s, %s", wide(dd) ? 'q' : 'l', m, R); break;
        case 2: line(i->is_signed ? "movsw%c %s, %s" : "movzw%c %s, %s", wide(dd) ? 'q' : 'l', m, R); break;
        case 4:
            if (wide(dd) && i->is_signed)
                line("movslq %s, %s", m, R);
            else
                line("movl %s, %s", m, in_reg(dd) ? opnd(dd, 4) : "%eax");
            break;
        default: line("movq %s, %s", m, R);
        }
        finish(dd, R);
        return;
    }
    case IR_STORE: {
        if (vec(i->b)) {
            int x = vec_in(i->b, 0);
            line("%s %s, %s", cls(i->b) == V_V32 ? "vmovdqu" : "movdqu", xname(x, cls(i->b) == V_V32),
                 address(i->a, 0, k));
            return;
        }
        if (flt(i->b) && in_reg(i->b) && (i->size == 4 || i->size == 8)) {
            line("mov%s %s, %s", i->size == 4 ? "ss" : "sd", slot(i->b), address(i->a, 0, k));
            return;
        }
        const char *m = address(i->a, 0, k);
        int64_t c;
        if (i->size <= 8 && int_cls(i->b) && const32(i->b, &c)) {
            static const char sz[] = { 0, 'b', 'w', 0, 'l', 0, 0, 0, 'q' };
            line("mov%c $%lld, %s", sz[i->size], (long long)(i->size == 1 ? (int8_t)c : i->size == 2 ? (int16_t)c : c), m);
            return;
        }
        if (i->size <= 8 && in_reg(i->b)) {
            static const char sz[] = { 0, 'b', 'w', 0, 'l', 0, 0, 0, 'q' };
            line("mov%c %s, %s", sz[i->size], opnd(i->b, i->size), m);
            return;
        }
        switch (i->size) {
        case 1: line("movl %s, %%eax", slot(i->b)); line("movb %%al, %s", m); break;
        case 2: line("movl %s, %%eax", slot(i->b)); line("movw %%ax, %s", m); break;
        case 4: line("movl %s, %%eax", slot(i->b)); line("movl %%eax, %s", m); break;
        case 8: line("movq %s, %%rax", slot(i->b)); line("movq %%rax, %s", m); break;
        default:
            /* a long double: its ten bytes, padding too */
            line("movq %s, %%rax", slot(i->b));
            line("movq %%rax, %s", m);
            line("movq %s, %%rax", slot_at(i->b, 8));
            line("movq %%rax, %s", address(i->a, 8, k));
        }
        return;
    }
    case IR_COPY:
        if (vec(i->dst)) {
            vec_from(i->dst, vec_in(i->a, 0));
            return;
        }
        if (cls(i->dst) == V_LD) {
            line("movq %s, %%rax", slot(i->a));
            line("movq %%rax, %s", slot(i->dst));
            line("movq %s, %%rax", slot_at(i->a, 8));
            line("movq %%rax, %s", slot_at(i->dst, 8));
            return;
        }
        if (flt(i->dst) && flt(i->a) && (in_reg(i->dst) || in_reg(i->a))) {
            if (in_reg(i->dst))
                movf_to_xmm(slot(i->dst), i->a);
            else
                movf_from_xmm(i->dst, slot(i->a));
            return;
        }
        if (in_reg(i->dst) || in_reg(i->a)) {
            if (!(in_reg(i->dst) && in_reg(i->a) && ra.reg[i->dst] == ra.reg[i->a]))
                line("movq %s, %s", opnd(i->a, 8), opnd(i->dst, 8));
            return;
        }
        line("movq %s, %%rax", opnd(i->a, 8));
        line("movq %%rax, %s", opnd(i->dst, 8));
        return;
    case IR_BIN:
        gen_bin(i);
        return;
    case IR_UN:
        if (i->sub == OP_FNEG) {
            if (cls(i->dst) == V_LD) {
                fld(i->a);
                line("fchs");
                fstp(i->dst);
            } else if (cls(i->dst) == V_F32) {
                load_to("%eax", i->a);
                line("xorl $0x80000000, %%eax");
                store_from(i->dst, "%rax", "%eax");
            } else {
                load_to("%rax", i->a);
                line("btcq $63, %%rax");
                store_from(i->dst, "%rax", "%eax");
            }
            return;
        }
        load_to(ax(i->dst), i->a);
        line("%s%c %s", i->sub == OP_NEG ? "neg" : "not", sfx(i->dst), ax(i->dst));
        store_from(i->dst, "%rax", "%eax");
        return;
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
        load_address("%r10", i->a);
        load_address("%r11", i->b);
        if (i->imm <= 128)
            copy_bytes("%r10", 0, "%r11", 0, (int)i->imm);
        else {
            line("movq %%r10, %%rdi");
            line("movq %%r11, %%rsi");
            line("movq $%lld, %%rcx", (long long)i->imm);
            line("rep movsb");
        }
        return;
    case IR_MEMZERO:
        if (i->imm <= 128) {
            /* (39b) stores of zero: rep stosb takes dozens of cycles to start */
            load_address("%r10", i->a);
            int n = (int)i->imm, at = 0;
            for (; n - at >= 8; at += 8)
                line("movq $0, %d(%%r10)", at);
            for (; n - at >= 4; at += 4)
                line("movl $0, %d(%%r10)", at);
            for (; at < n; at++)
                line("movb $0, %d(%%r10)", at);
            return;
        }
        load_address("%rdi", i->a);
        line("xorl %%eax, %%eax");
        line("movq $%lld, %%rcx", (long long)i->imm);
        line("rep stosb");
        return;
    case IR_VA_START:
        gen_va_start(i);
        return;
    case IR_VA_ARG:
        gen_va_arg(i);
        return;
    case IR_ALLOCA:
        line("movq %s, %%rax", slot(i->a));
        line("addq $15, %%rax");
        line("andq $-16, %%rax");
        line("subq %%rax, %%rsp");
        if (i->imm > 16)
            line("andq $%lld, %%rsp", -(long long)i->imm);
        line("movq %%rsp, %s", slot(i->dst));
        return;
    case IR_GET_SP:
        line("movq %%rsp, %s", slot(i->dst));
        return;
    case IR_SET_SP:
        line("movq %s, %%rsp", slot(i->a));
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
        /* (62b) the general-dynamic sequence, as clang writes it for -fPIC
         * (lld relaxes exactly this one; our ld keeps it): rax = the address */
        line("data16 leaq %s@tlsgd(%%rip), %%rdi", i->sym);
        line("data16 data16 rex64 call __tls_get_addr@PLT");
        store_from(i->dst, "%rax", "%eax");
        return;
    case IR_TRAP:
        line("ud2");
        return;
    case IR_VSPLAT:
        gen_vsplat(i);
        return;
    case IR_VHADD:
        gen_vhadd(i);
        return;
    case IR_VMADD:
        gen_vmadd(i);
        return;
    case IR_VREVW:
        gen_vrevw(i);
        return;
    }
}

static void gen_prologue(struct obj *fn)
{
    line("pushq %%rbp");
    line("movq %%rsp, %%rbp");
    if (frame_size)
        line("subq $%d, %%rsp", frame_size);
    for (int k = 0; k < SAVED_REGS; k++)        /* (61a) the registers values use, kept for the caller */
        if (reg_used[k])
            line("movq %s, %d(%%rbp)", vreg_names[k][3], saves + 8 * k);
    if (ms_saves) {
        static const char *ms_regs[] = { "%rcx", "%rdx", "%r8", "%r9" };
        line("movq %%rsi, %d(%%rbp)", ms_saves);
        line("movq %%rdi, %d(%%rbp)", ms_saves + 8);
        int k = 0;
        for (struct obj *p = fn->params; p; p = p->next, k++)
            line("movq %s, %d(%%rbp)", ms_regs[k], p->offset);
        return;
    }
    int gp = 0, sse = 0;
    if (sret_slot) {
        line("movq %%rdi, %d(%%rbp)", sret_slot);
        gp = 1;
    }
    if (fn->type->is_variadic) {
        for (int k = 0; k < 6; k++)
            line("movq %s, %d(%%rbp)", gp_regs[k], va_area + 8 * k);
        for (int k = 0; k < 8 && !opt_general_regs; k++)
            line("movaps %%xmm%d, %d(%%rbp)", k, va_area + 48 + 16 * k);
    }
    for (struct obj *p = fn->params; p; p = p->next) {
        if (p->offset > 0)
            continue;                           /* (on the stack) */
        struct type *ty = unqual(p->type);
        if (is_memory_value(ty)) {
            int classes[2], n;
            struct_in_registers(ty, classes, &n);
            for (int j = 0; j < n; j++) {
                if (classes[j] == CLASS_INT)
                    line("movq %s, %d(%%rbp)", gp_regs[gp++], p->offset + 8 * j);
                else
                    line("movsd %%xmm%d, %d(%%rbp)", sse++, p->offset + 8 * j);
            }
            continue;
        }
        if (is_flonum(ty)) {
            line("mov%s %%xmm%d, %d(%%rbp)", ty->kind == TY_FLOAT ? "ss" : "sd", sse++, p->offset);
            continue;
        }
        if (ty->is_vector) {                    /* (32h) */
            line("%s %s, %d(%%rbp)", ty->size == 32 ? "vmovdqu" : "movdqu", xname(sse++, ty->size == 32), p->offset);
            continue;
        }
        switch (ty->size) {
        case 1: line("movb %s, %d(%%rbp)", (const char *[]){ "%dil", "%sil", "%dl", "%cl", "%r8b", "%r9b" }[gp++], p->offset); break;
        case 2: line("movw %s, %d(%%rbp)", (const char *[]){ "%di", "%si", "%dx", "%cx", "%r8w", "%r9w" }[gp++], p->offset); break;
        case 4: line("movl %s, %d(%%rbp)", gp_regs32[gp++], p->offset); break;
        default: line("movq %s, %d(%%rbp)", gp_regs[gp++], p->offset);
        }
    }
}

/* --- inline assembly (62c) ------------------------------------------------ */

/* No value is in a register over an asm statement (the allocator's
 * barrier), so its operands may take any register but rsp and rbp; they
 * are loaded from their slots right before it and outputs stored right
 * after. Callee-saved registers it uses are marked for the prologue. */
static const char *const gp_names[16][4] = {
    { "%al", "%ax", "%eax", "%rax" }, { "%cl", "%cx", "%ecx", "%rcx" }, { "%dl", "%dx", "%edx", "%rdx" },
    { "%bl", "%bx", "%ebx", "%rbx" }, { "%spl", "%sp", "%esp", "%rsp" }, { "%bpl", "%bp", "%ebp", "%rbp" },
    { "%sil", "%si", "%esi", "%rsi" }, { "%dil", "%di", "%edi", "%rdi" }, { "%r8b", "%r8w", "%r8d", "%r8" },
    { "%r9b", "%r9w", "%r9d", "%r9" }, { "%r10b", "%r10w", "%r10d", "%r10" }, { "%r11b", "%r11w", "%r11d", "%r11" },
    { "%r12b", "%r12w", "%r12d", "%r12" }, { "%r13b", "%r13w", "%r13d", "%r13" },
    { "%r14b", "%r14w", "%r14d", "%r14" }, { "%r15b", "%r15w", "%r15d", "%r15" },
};

static int gp_by_name(const char *name)
{
    if (*name == '%')
        name++;
    for (int r = 0; r < 16; r++)
        for (int w = 0; w < 4; w++)
            if (strcmp(gp_names[r][w] + 1, name) == 0)
                return r;
    return -1;
}

struct asm_place {
    int *reg;                                   /* per operand: its register (or address register) */
    int temp;                                   /* for storing outputs */
};
static struct asm_place *asm_places;            /* per instruction */

static bool is_mem_constraint(const char *c)
{
    return strchr(c, 'm') && !strchr(c, 'r');
}

static void place_asm(int k)
{
    struct asm_stmt *a = F->code[k].asm_info;
    struct asm_place *p = &asm_places[k];
    for (int j = 0; j < a->nops; j++)
        if (strchr(a->ops[j].constraint, 'x'))
            error_at(F->code[k].tok, "asm: \"x\" operands need one output at most, no memory operand and no "
                     "\"memory\" clobber");
    p->reg = calloc((size_t)a->nops + 1, sizeof(int));
    bool taken[16] = { 0 };
    taken[4] = taken[5] = true;                 /* rsp, rbp */
    for (int j = 0; j < a->nclobbers; j++) {
        int r = gp_by_name(a->clobbers[j]);
        if (r >= 0)
            taken[r] = true;
    }
    /* pinned first: constraint letters and register variables */
    for (int j = 0; j < a->nops; j++) {
        struct asm_operand *op = &a->ops[j];
        p->reg[j] = -1;
        if (op->has_imm)
            continue;
        struct node *e = op->expr;
        int r = -1;
        if (e->kind == ND_VAR && e->var->asm_reg)
            r = gp_by_name(e->var->asm_reg);
        else
            for (const char *c = op->constraint; *c && r < 0; c++)
                switch (*c) {
                case 'a': r = 0; break;
                case 'b': r = 3; break;
                case 'c': r = 1; break;
                case 'd': r = 2; break;
                case 'S': r = 6; break;
                case 'D': r = 7; break;
                }
        if (r >= 0) {
            p->reg[j] = r;
            taken[r] = true;
        }
    }
    static const int pool[] = { 0, 1, 2, 6, 7, 8, 9, 10, 11, 3, 12, 13, 14, 15 };
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
    /* a register for the outputs' addresses: any but the outputs' own */
    p->temp = -1;
    for (size_t q = 0; q < sizeof(pool) / sizeof(pool[0]) && p->temp < 0; q++) {
        bool out_reg = false;
        for (int j = 0; j < a->noutputs; j++)
            if (p->reg[j] == pool[q] && !is_mem_constraint(a->ops[j].constraint))
                out_reg = true;
        if (!out_reg)
            p->temp = pool[q];
    }
    /* callee-saved registers it uses or overwrites: kept for the caller */
    for (int r = 0; r < 16; r++)
        if (taken[r] || r == p->temp) {
            int v = r == 3 ? 0 : r >= 12 ? r - 11 : -1;
            if (v >= 0)
                reg_used[v] = true;
        }
}

static int width_index(int size)
{
    return size <= 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
}

/* (32h) Inline assembly given registers as an instruction's
 * (asm_in_registers): an input where its value is, else in scratch (xmm1-
 * xmm5, rax, rcx, rdx, r10); the output in its value's register, else in
 * xmm6 or r11 and then its slot; xmm0 and xmm7 are the template's own (an
 * implicit operand, a temporary: named as clobbered). The allocator never gives the output the
 * register of an input (an input's interval reaches this instruction), so
 * a template may write its output before it reads every input. */
static void gen_asm_registers(struct ir *i)
{
    struct asm_stmt *a = i->asm_info;
    struct ir_call *c = i->call;
    static const int value_hw[VALUE_REGS] = { 3, 12, 13, 14, 15, 8, 9, 6, 7 };
    static const int scratch_gp[] = { 0, 1, 2, 10 };
    int *place = calloc((size_t)a->nops + 1, sizeof(int));
    int nx = 1, ng = 0;
    for (int j = 0; j < a->nops; j++) {
        struct asm_operand *op = &a->ops[j];
        if (op->has_imm)
            continue;
        bool x = strcmp(op->constraint, "x") == 0;
        int v = op->is_output ? i->dst : c->args[j].value;
        if (op->is_output)
            place[j] = x ? (vec(v) && vreg(v) >= 0 ? vreg(v) : 6) : in_reg(v) && !flt(v) ? value_hw[ra.reg[v]] : 11;
        else if (x && vec(v)) {
            if (vreg(v) < 0 && nx > 5)
                error_at(i->tok, "asm: too many \"x\" operands");
            place[j] = vec_in(v, vreg(v) >= 0 ? 0 : nx++);
        } else if (x) {
            if (cls(v) != V_F32 && cls(v) != V_F64)
                error_at(i->tok, "asm: an \"x\" operand is a vector, float or double");
            if (nx > 5)
                error_at(i->tok, "asm: too many \"x\" operands");
            line("mov%s %s, %s", fp_suffix(v), slot(v), xname(nx, false));
            place[j] = nx++;
        } else if (in_reg(v) && !flt(v))
            place[j] = value_hw[ra.reg[v]];
        else {
            if (ng == 4)
                error_at(i->tok, "asm: too many \"r\" operands");
            place[j] = scratch_gp[ng++];
            load_to(gp_names[place[j]][value_size(cls(v)) == 8 ? 3 : 2], v);    /* (39a: a float's bits too) */
        }
    }
    struct buffer b = { 0 };
    int alternative = -1;                       /* in {first|second}: which part (-1: outside) */
    for (const char *t = a->text; *t; t++) {
        /* (GCC's {a|b}: alternatives; here the first in SSE code, the
         * second, the VEX form, in an AVX function) */
        if (*t == '{' && alternative < 0) {
            alternative = 0;
            continue;
        }
        if (*t == '|' && alternative >= 0) {
            alternative = 1;
            continue;
        }
        if (*t == '}' && alternative >= 0) {
            alternative = -1;
            continue;
        }
        if (alternative >= 0 && alternative != (avx_fn ? 1 : 0))
            continue;
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
            buf_printf(&b, mod == 'c' ? "%lld" : "$%lld", (long long)op->imm);
            continue;
        }
        int size = c->args[j].type->size;
        if (strcmp(op->constraint, "x") == 0) {
            bool wide = mod == 't' || (mod != 'x' && size == 32);
            buf_printf(&b, "%s", xname(place[j], wide));
            continue;
        }
        int w = mod == 'b' ? 0 : mod == 'w' ? 1 : mod == 'k' ? 2 : mod == 'q' ? 3 : width_index(size > 8 ? 8 : size);
        buf_printf(&b, "%s", gp_names[place[j]][w]);
    }
    fprintf(out, "\t%s\n", b.data ? b.data : "");
    free(b.data);
    if (a->noutputs) {
        int d = i->dst;
        if (strcmp(a->ops[0].constraint, "x") == 0) {
            if (vec(d))
                vec_from(d, place[0]);
            else
                line("mov%s %s, %s", fp_suffix(d), xname(place[0], false), slot(d));
        } else if (!in_reg(d) || flt(d))
            store_from(d, gp_names[11][3], gp_names[11][2]);
    }
    free(place);
}

static void gen_asm(int k)
{
    struct ir *i = &F->code[k];
    struct asm_stmt *a = i->asm_info;
    if (a->is_basic) {
        fprintf(out, "\t%s\n", a->text);
        return;
    }
    if (asm_in_registers(a)) {
        gen_asm_registers(i);
        return;
    }
    struct asm_place *p = &asm_places[k];
    struct ir_call *c = i->call;
    /* operands into their registers */
    for (int j = 0; j < a->nops; j++) {
        struct asm_operand *op = &a->ops[j];
        int v = c->args[j].value, r = p->reg[j], size = c->args[j].type->size;
        if (op->has_imm)
            continue;
        if (is_mem_constraint(op->constraint))
            line("movq %s, %s", opnd(v, 8), gp_names[r][3]);
        else if (op->is_output && op->is_inout) {
            line("movq %s, %s", opnd(v, 8), gp_names[r][3]);
            if (size == 1)
                line("movzbl (%s), %s", gp_names[r][3], gp_names[r][2]);
            else if (size == 2)
                line("movzwl (%s), %s", gp_names[r][3], gp_names[r][2]);
            else
                line("mov%c (%s), %s", size == 8 ? 'q' : 'l', gp_names[r][3], gp_names[r][size == 8 ? 3 : 2]);
        } else if (!op->is_output)
            line("mov%c %s, %s", value_size(cls(v)) == 8 ? 'q' : 'l', opnd(v, value_size(cls(v)) == 8 ? 8 : 4),
                 gp_names[r][value_size(cls(v)) == 8 ? 3 : 2]);
    }
    /* the template */
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
            buf_printf(&b, mod == 'c' ? "%lld" : "$%lld", (long long)op->imm);
            continue;
        }
        int r = p->reg[j];
        if (is_mem_constraint(op->constraint)) {
            buf_printf(&b, "(%s)", gp_names[r][3]);
            continue;
        }
        int size = c->args[j].type->size;
        int w = mod == 'b' ? 0 : mod == 'w' ? 1 : mod == 'k' ? 2 : mod == 'q' ? 3 : width_index(size > 8 ? 8 : size);
        buf_printf(&b, "%s", gp_names[r][w]);
    }
    fprintf(out, "\t%s\n", b.data ? b.data : "");
    free(b.data);
    /* outputs back */
    for (int j = 0; j < a->noutputs; j++) {
        struct asm_operand *op = &a->ops[j];
        if (is_mem_constraint(op->constraint))
            continue;
        int v = c->args[j].value, r = p->reg[j], size = c->args[j].type->size, t = p->temp;
        line("movq %s, %s", opnd(v, 8), gp_names[t][3]);
        line("mov%c %s, (%s)", size == 1 ? 'b' : size == 2 ? 'w' : size == 4 ? 'l' : 'q', gp_names[r][width_index(size)],
             gp_names[t][3]);
    }
}

static void emit_function(struct obj *fn)
{
    F = ir_function(fn);
    if (getenv("CC_DUMP_IR"))
        ir_dump(stderr, F);
    fn_number++;
    avx_fn = fn->is_avx;
    for (int v = 1; v < F->nvalues; v++)
        avx_fn |= F->values[v].cls == V_V32;
    slot_of = calloc((size_t)F->nvalues + 1, sizeof(int));
    memset(reg_used, 0, sizeof(reg_used));
    analyze();
    layout_frame(fn);
    asm_places = calloc((size_t)F->ncode + 1, sizeof(*asm_places));
    for (int k = 0; k < F->ncode; k++)
        if (F->code[k].op == IR_ASM && !F->code[k].asm_info->is_basic && !asm_in_registers(F->code[k].asm_info))
            place_asm(k);
    fprintf(out, "\t.text\n");
    if (!fn->is_static)
        fprintf(out, "\t.globl %s\n", fn->name);
    if (fn->is_weak)
        fprintf(out, "\t.weak %s\n", fn->name);
    if (fn->section)
        fprintf(out, "\t.section %s,\"ax\",@progbits\n", fn->section);
    fprintf(out, "\t.p2align 4\n");
    fprintf(out, "\t.type %s, @function\n", fn->name);
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
    free(lea_at);
    free(memop_of);
    free(disp_ok);
    disp_ok = 0;
    free(last_use);
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

/* The exact value of a double in the x87's 80-bit format (16 bytes). */
static void long_double_bytes(double v, unsigned char b[16])
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    int sign = (int)(bits >> 63), exp = (int)((bits >> 52) & 0x7FF);
    uint64_t frac = bits & 0xFFFFFFFFFFFFFull;
    memset(b, 0, 16);
    int e;
    uint64_t mant;
    if (exp == 0 && frac == 0) {
        e = 0;
        mant = 0;
    } else if (exp == 0x7FF) {
        e = 0x7FFF;
        mant = (1ull << 63) | (frac << 11);
    } else if (exp == 0) {
        int shift = 0;
        while (!(frac & (1ull << 52))) {
            frac <<= 1;
            shift++;
        }
        e = 1 - 1023 - shift + 16383;
        mant = frac << 11;
    } else {
        e = exp - 1023 + 16383;
        mant = (1ull << 63) | (frac << 11);
    }
    for (int k = 0; k < 8; k++)
        b[k] = (unsigned char)(mant >> (8 * k));
    b[8] = (unsigned char)e;
    b[9] = (unsigned char)((e >> 8) | (sign << 7));
}

void emit_x86_64(FILE *o, struct obj *program)
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
    emit_data(out, program, false);
    if (need_two64 || need_two63f) {
        fprintf(out, "\t.section .rodata\n\t.p2align 2\n");
        if (need_two64)
            fprintf(out, ".L.two64:\n\t.long 0x5f800000\n");
        if (need_two63f)
            fprintf(out, ".L.two63:\n\t.long 0x5f000000\n");
    }
    fprintf(out, "\t.section .note.GNU-stack,\"\",@progbits\n");
}
