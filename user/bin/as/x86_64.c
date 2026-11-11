/*
 * The x86-64 part of as (60e): AT&T syntax (source first, sizes in the
 * mnemonic's suffix), encoded as the processor manuals give the forms:
 * prefixes, REX, the opcode, ModRM, SIB, displacement, immediate. Where an
 * instruction has several encodings the one LLVM's assembler picks is used
 * (the accumulator forms, 0x83 for small immediates, ...), so objects can
 * be compared byte for byte with clang's.
 */
#include "as.h"

enum { K_X86_ABS = 0, K_X86_PC32 = 1, K_X86_PLT32 = 2, K_X86_GOTPCREL = 3, K_X86_GOTPCREL_REX = 4, K_X86_TLSGD = 5 };
enum { M_NONE, M_PLT, M_GOTPCREL, M_TLSGD };

#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_PLT32 4
#define R_X86_64_GOTPCREL 9
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_16 12
#define R_X86_64_8 14
#define R_X86_64_TLSGD 19
#define R_X86_64_GOTPCRELX 41
#define R_X86_64_REX_GOTPCRELX 42

enum op_kind { OP_NONE, OP_REG, OP_IMM, OP_MEM };
enum reg_class { RC_GPR, RC_XMM, RC_ST, RC_CR, RC_SEG };

struct operand {
    enum op_kind kind;
    enum reg_class rc;
    int reg;                                    /* 0-15 */
    int size;                                   /* a register's: 1, 2, 4, 8 (xmm 16, ymm 32) */
    bool high8;                                 /* ah, ch, dh, bh */
    bool rex8;                                  /* spl, bpl, sil, dil */
    struct as_expr expr;                        /* the immediate or displacement */
    bool has_disp;
    int base, index, scale;                     /* -1: none */
    bool rip;
    bool indirect;                              /* "*" in jmp and call */
    int modifier;
    int segment;                                /* (62d) %fs: 0x64, %gs: 0x65, else 0 */
};

/* --- registers ----------------------------------------------------------- */

static const char *gpr64[] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                               "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
static const char *gpr32[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                               "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d" };
static const char *gpr16[] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                               "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w" };
static const char *gpr8[] = { "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
                              "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b" };
static const char *high8[] = { "ah", "ch", "dh", "bh" };

static bool parse_register(const char **pp, struct operand *o)
{
    const char *p = *pp;
    if (*p != '%')
        return false;
    p++;
    const char *start = p;
    while ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
        p++;
    size_t n = (size_t)(p - start);
    char name[16];
    if (n >= sizeof(name))
        return false;
    memcpy(name, start, n);
    name[n] = 0;
    memset(o, 0, sizeof(*o));
    o->kind = OP_REG;
    o->base = o->index = -1;
    for (int i = 0; i < 16; i++) {
        if (strcmp(name, gpr64[i]) == 0) { o->reg = i; o->size = 8; goto found; }
        if (strcmp(name, gpr32[i]) == 0) { o->reg = i; o->size = 4; goto found; }
        if (strcmp(name, gpr16[i]) == 0) { o->reg = i; o->size = 2; goto found; }
        if (strcmp(name, gpr8[i]) == 0) { o->reg = i; o->size = 1; o->rex8 = i >= 4 && i < 8; goto found; }
    }
    for (int i = 0; i < 4; i++)
        if (strcmp(name, high8[i]) == 0) {
            o->reg = i + 4;
            o->size = 1;
            o->high8 = true;
            goto found;
        }
    if (strncmp(name, "cr", 2) == 0 && name[2] >= '0' && name[2] <= '9') {
        /* (62d) a control register: only in mov to or from a 64-bit one */
        o->rc = RC_CR;
        o->reg = atoi(name + 2);
        o->size = 8;
        if (o->reg > 15)
            return false;
        goto found;
    }
    static const char *segments[] = { "es", "cs", "ss", "ds", "fs", "gs" };
    for (int i = 0; i < 6; i++)
        if (strcmp(name, segments[i]) == 0) {
            /* (62e) a segment register: only in mov */
            o->rc = RC_SEG;
            o->reg = i;
            o->size = 2;
            goto found;
        }
    if ((strncmp(name, "xmm", 3) == 0 || strncmp(name, "ymm", 3) == 0) && name[3]) {
        o->rc = RC_XMM;                         /* (32c: ymm, the same registers at 256 bits) */
        o->reg = atoi(name + 3);
        o->size = name[0] == 'y' ? 32 : 16;
        if (o->reg > 15)
            return false;
        goto found;
    }
    if (strcmp(name, "st") == 0) {
        o->rc = RC_ST;
        o->reg = 0;
        if (*p == '(') {
            o->reg = p[1] - '0';
            if (p[2] != ')')
                return false;
            p += 3;
        }
        goto found;
    }
    if (strcmp(name, "rip") == 0) {
        o->reg = 16;
        o->size = 8;
        goto found;
    }
    return false;
found:
    *pp = p;
    return true;
}

/* --- operands ------------------------------------------------------------ */

static void parse_modifier(const char **pp, struct operand *o)
{
    const char *p = *pp;
    if (*p != '@')
        return;
    p++;
    if (strncmp(p, "PLT", 3) == 0) {
        o->modifier = M_PLT;
        p += 3;
    } else if (strncmp(p, "GOTPCREL", 8) == 0) {
        o->modifier = M_GOTPCREL;
        p += 8;
    } else if (strncmp(p, "tlsgd", 5) == 0) {
        o->modifier = M_TLSGD;                  /* (62b) the general-dynamic GOT pair */
        p += 5;
    } else
        as_error("unknown symbol modifier '@%.10s'", p);
    *pp = p;
}

static void parse_operand(const char *text, struct operand *o)
{
    const char *p = skip_space(text);
    memset(o, 0, sizeof(*o));
    o->base = o->index = -1;
    bool indirect = false;
    if (*p == '*') {
        indirect = true;
        p = skip_space(p + 1);
    }
    int segment = 0;
    if (strncmp(p, "%fs:", 4) == 0 || strncmp(p, "%gs:", 4) == 0) {
        segment = p[1] == 'f' ? 0x64 : 0x65;   /* (the thread pointer's segment) */
        p = skip_space(p + 4);
    }
    if (*p == '%' && !segment) {
        if (!parse_register(&p, o))
            as_error("unknown register in '%s'", text);
        o->indirect = indirect;
        if (*skip_space(p))
            as_error("junk after a register: '%s'", text);
        return;
    }
    if (*p == '$') {
        p++;
        o->kind = OP_IMM;
        if (!parse_expr(&p, &o->expr))
            as_error("bad immediate '%s'", text);
        if (*skip_space(p))
            as_error("junk after an immediate: '%s'", text);
        return;
    }
    o->kind = OP_MEM;
    o->indirect = indirect;
    o->segment = segment;
    o->scale = 1;
    if (*p != '(') {
        if (!parse_expr(&p, &o->expr))
            as_error("bad operand '%s'", text);
        o->has_disp = true;
        parse_modifier(&p, o);
        p = skip_space(p);
    }
    if (*p == '(') {
        p = skip_space(p + 1);
        struct operand r;
        if (*p == '%') {
            if (!parse_register(&p, &r) || r.rc != RC_GPR || r.size != 8)
                as_error("bad base register in '%s'", text);
            if (r.reg == 16)
                o->rip = true;
            else
                o->base = r.reg;
            p = skip_space(p);
        }
        if (*p == ',') {
            p = skip_space(p + 1);
            if (!parse_register(&p, &r) || r.rc != RC_GPR || r.size != 8 || r.reg == 16)
                as_error("bad index register in '%s'", text);
            o->index = r.reg;
            p = skip_space(p);
            if (*p == ',') {
                p = skip_space(p + 1);
                o->scale = (int)strtol(p, (char **)&p, 10);
                if (o->scale != 1 && o->scale != 2 && o->scale != 4 && o->scale != 8)
                    as_error("bad scale in '%s'", text);
                p = skip_space(p);
            }
        }
        if (*p != ')')
            as_error("expected ')' in '%s'", text);
        p++;
    }
    if (*skip_space(p))
        as_error("junk after an operand: '%s'", text);
}

/* the operands: split at commas outside parentheses */
static int split_operands(char *s, struct operand *ops)
{
    int n = 0, depth = 0;
    char *start = s;
    if (!*skip_space(s))
        return 0;
    for (char *p = s;; p++) {
        if (*p == '(')
            depth++;
        else if (*p == ')')
            depth--;
        if (!*p || (*p == ',' && depth == 0)) {
            char end = *p;
            *p = 0;
            if (n == 4)
                as_error("too many operands");
            parse_operand(start, &ops[n++]);
            if (!end)
                break;
            start = p + 1;
        }
    }
    return n;
}

/* --- encoding ------------------------------------------------------------ */

static bool fits8(int64_t v)
{
    return v >= -128 && v <= 127;
}

static bool constant(struct as_expr *e, int64_t *v)
{
    return expr_constant(e, v);
}

/* REX for a ModRM instruction: reg field r, r/m operand rm. */
static int rex_for(bool w, int r, struct operand *rm, bool force)
{
    int rex = 0;
    if (w)
        rex |= 8;
    if (r >= 8)
        rex |= 4;
    if (rm->kind == OP_MEM) {
        if (rm->index >= 8)
            rex |= 2;
        if (rm->base >= 8)
            rex |= 1;
    } else if (rm->reg >= 8)
        rex |= 1;
    if (rm->kind == OP_REG && rm->rex8)
        force = true;
    if (rex || force)
        return 0x40 | rex;
    return 0;
}

/* ModRM (and SIB and displacement) for reg field r and operand rm; imm_after
 * is the number of immediate bytes that follow (rip-relative counts from
 * the instruction's end). kind: the fixup for a symbol displacement. */
static void modrm(int r, struct operand *rm, int imm_after, int kind)
{
    int reg = r & 7;
    if (rm->kind == OP_REG) {
        emit_byte(0xC0 | (reg << 3) | (rm->reg & 7));
        return;
    }
    int64_t disp = 0;
    bool sym = rm->has_disp && !constant(&rm->expr, &disp);
    if (rm->rip) {
        emit_byte(0x05 | (reg << 3));
        if (sym || rm->has_disp) {
            int k = kind;
            if (rm->modifier == M_GOTPCREL)
                k = kind;
            if (sym)
                emit_fixup(&rm->expr, k, 4, true, 4 + imm_after);
        }
        emit_le(sym ? 0 : (uint64_t)disp, 4);
        return;
    }
    if (rm->base < 0) {
        /* an absolute address: SIB with no base */
        emit_byte(0x04 | (reg << 3));
        int index = rm->index >= 0 ? rm->index & 7 : 4;
        int ss = rm->scale == 8 ? 3 : rm->scale == 4 ? 2 : rm->scale == 2 ? 1 : 0;
        emit_byte((ss << 6) | (index << 3) | 5);
        if (sym)
            emit_fixup(&rm->expr, K_X86_ABS, 4, false, 0);
        emit_le(sym ? 0 : (uint64_t)disp, 4);
        return;
    }
    int mod;
    if (sym)
        mod = 2;
    else if (disp == 0 && (rm->base & 7) != 5)
        mod = 0;
    else if (fits8(disp))
        mod = 1;
    else
        mod = 2;
    bool sib = rm->index >= 0 || (rm->base & 7) == 4;
    if (sib) {
        emit_byte((mod << 6) | (reg << 3) | 4);
        int index = rm->index >= 0 ? rm->index & 7 : 4;
        int ss = rm->scale == 8 ? 3 : rm->scale == 4 ? 2 : rm->scale == 2 ? 1 : 0;
        emit_byte((ss << 6) | (index << 3) | (rm->base & 7));
    } else
        emit_byte((mod << 6) | (reg << 3) | (rm->base & 7));
    if (mod == 1)
        emit_byte((int)disp);
    else if (mod == 2) {
        if (sym)
            emit_fixup(&rm->expr, K_X86_ABS, 4, false, 0);
        emit_le(sym ? 0 : (uint64_t)disp, 4);
    }
}

static void immediate(struct as_expr *e, int size)
{
    int64_t v;
    if (constant(e, &v)) {
        emit_le((uint64_t)v, size);
        return;
    }
    emit_fixup(e, K_X86_ABS, size, false, 0);
    emit_le(0, size);
}

/* One instruction: [mandatory prefix] [66] [REX] opcode ModRM ... imm */
struct enc {
    int prefix;                                 /* 0, 0x66, 0xF2, 0xF3 (mandatory, before REX) */
    bool size16;                                /* the operand-size prefix */
    bool w;
    int nop;
    int op[3];
    int r;                                      /* the reg field (a register or an opcode extension) */
    struct operand *rm;
    int imm_size;
    struct as_expr *imm;
    bool force_rex;
    int fixup_kind;                             /* for rip-relative symbols */
};

static void encode(struct enc *e)
{
    if (e->rm && e->rm->kind == OP_MEM && e->rm->segment)
        emit_byte(e->rm->segment);
    if (e->size16)
        emit_byte(0x66);
    if (e->prefix)
        emit_byte(e->prefix);
    int rex = e->rm ? rex_for(e->w, e->r, e->rm, e->force_rex) : (e->w ? 0x48 : 0);
    if (rex)
        emit_byte(rex);
    for (int i = 0; i < e->nop; i++)
        emit_byte(e->op[i]);
    if (e->rm) {
        int kind = e->fixup_kind ? e->fixup_kind : K_X86_PC32;
        if (e->rm->kind == OP_MEM && e->rm->modifier == M_GOTPCREL)
            kind = rex ? K_X86_GOTPCREL_REX : K_X86_GOTPCREL;
        if (e->rm->kind == OP_MEM && e->rm->modifier == M_TLSGD)
            kind = K_X86_TLSGD;
        modrm(e->r, e->rm, e->imm_size, kind);
    }
    if (e->imm_size)
        immediate(e->imm, e->imm_size);
}

static void check_high8(struct operand *a, struct operand *b, bool w)
{
    bool high = (a && a->kind == OP_REG && a->high8) || (b && b->kind == OP_REG && b->high8);
    bool needs = w || (a && ((a->kind == OP_REG && (a->reg >= 8 || a->rex8)) ||
                             (a->kind == OP_MEM && (a->base >= 8 || a->index >= 8)))) ||
                 (b && ((b->kind == OP_REG && (b->reg >= 8 || b->rex8)) ||
                        (b->kind == OP_MEM && (b->base >= 8 || b->index >= 8))));
    if (high && needs)
        as_error("%%ah, %%bh, %%ch and %%dh cannot be used with a REX prefix");
}

/* --- mnemonics ----------------------------------------------------------- */

static int suffix_size(char c)
{
    switch (c) {
    case 'b': return 1;
    case 'w': return 2;
    case 'l': return 4;
    case 'q': return 8;
    }
    return 0;
}

/* name + optional size suffix: the operand size, or 0 if none given */
static bool match(const char *mnemonic, const char *base, int *size, bool suffix_required)
{
    size_t n = strlen(base);
    if (strncmp(mnemonic, base, n) != 0)
        return false;
    if (mnemonic[n] == 0) {
        *size = 0;
        return !suffix_required;
    }
    if (mnemonic[n + 1] == 0 && suffix_size(mnemonic[n])) {
        *size = suffix_size(mnemonic[n]);
        return true;
    }
    return false;
}

static int operand_size(int suffix, struct operand *ops, int n)
{
    if (suffix)
        return suffix;
    for (int i = 0; i < n; i++)
        if (ops[i].kind == OP_REG && ops[i].rc == RC_GPR)
            return ops[i].size;
    as_error("the operand size is not known: add a suffix");
}

static const char *cc_names[16][3] = {
    { "o" }, { "no" }, { "b", "c", "nae" }, { "ae", "nb", "nc" }, { "e", "z" }, { "ne", "nz" },
    { "be", "na" }, { "a", "nbe" }, { "s" }, { "ns" }, { "p", "pe" }, { "np", "po" },
    { "l", "nge" }, { "ge", "nl" }, { "le", "ng" }, { "g", "nle" },
};

static int condition(const char *s)
{
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 3; j++)
            if (cc_names[i][j] && strcmp(s, cc_names[i][j]) == 0)
                return i;
    return -1;
}

static void need(int n, int want, const char *m)
{
    if (n != want)
        as_error("'%s' takes %d operand(s)", m, want);
}

/* add, or, adc, sbb, and, sub, xor, cmp */
static void alu(int ext, int size, struct operand *src, struct operand *dst)
{
    struct enc e = { 0 };
    e.size16 = size == 2;
    e.w = size == 8;
    if (src->kind == OP_IMM) {
        int64_t v;
        bool known = constant(&src->expr, &v);
        e.rm = dst;
        e.r = ext;
        if (size == 1) {
            if (dst->kind == OP_REG && dst->reg == 0 && !dst->high8) {
                e.rm = 0;
                e.nop = 1;
                e.op[0] = 0x04 + 8 * ext;
                e.imm_size = 1;
                e.imm = &src->expr;
                encode(&e);
                return;
            }
            e.nop = 1;
            e.op[0] = 0x80;
            e.imm_size = 1;
        } else if (known && fits8(v)) {
            e.nop = 1;
            e.op[0] = 0x83;
            e.imm_size = 1;
        } else if (dst->kind == OP_REG && dst->reg == 0) {
            e.rm = 0;
            e.nop = 1;
            e.op[0] = 0x05 + 8 * ext;
            e.imm_size = size == 2 ? 2 : 4;
            e.imm = &src->expr;
            encode(&e);
            return;
        } else {
            e.nop = 1;
            e.op[0] = 0x81;
            e.imm_size = size == 2 ? 2 : 4;
        }
        e.imm = &src->expr;
        encode(&e);
        return;
    }
    if (src->kind == OP_REG) {
        check_high8(src, dst, e.w);
        e.nop = 1;
        e.op[0] = 8 * ext + (size == 1 ? 0 : 1);
        e.r = src->reg;
        e.rm = dst;
        e.force_rex = src->rex8;
        encode(&e);
        return;
    }
    if (dst->kind != OP_REG)
        as_error("two memory operands");
    check_high8(src, dst, e.w);
    e.nop = 1;
    e.op[0] = 8 * ext + (size == 1 ? 2 : 3);
    e.r = dst->reg;
    e.rm = src;
    e.force_rex = dst->rex8;
    encode(&e);
}

static void mov(int size, struct operand *src, struct operand *dst)
{
    struct enc e = { 0 };
    e.size16 = size == 2;
    e.w = size == 8;
    if (src->kind == OP_IMM) {
        int64_t v;
        bool known = constant(&src->expr, &v);
        if (dst->kind == OP_REG && size != 8) {
            /* B0+r / B8+r */
            int rex = dst->reg >= 8 ? 0x41 : dst->rex8 ? 0x40 : 0;
            if (size == 2)
                emit_byte(0x66);
            if (rex)
                emit_byte(rex);
            emit_byte((size == 1 ? 0xB0 : 0xB8) + (dst->reg & 7));
            immediate(&src->expr, size);
            return;
        }
        if (dst->kind == OP_REG && size == 8 && known && !(v >= INT32_MIN && v <= INT32_MAX)) {
            emit_byte(0x48 | (dst->reg >= 8 ? 1 : 0));
            emit_byte(0xB8 + (dst->reg & 7));
            emit_le((uint64_t)v, 8);
            return;
        }
        e.nop = 1;
        e.op[0] = size == 1 ? 0xC6 : 0xC7;
        e.r = 0;
        e.rm = dst;
        e.imm_size = size == 1 ? 1 : size == 2 ? 2 : 4;
        e.imm = &src->expr;
        encode(&e);
        return;
    }
    if (src->kind == OP_REG) {
        check_high8(src, dst, e.w);
        e.nop = 1;
        e.op[0] = size == 1 ? 0x88 : 0x89;
        e.r = src->reg;
        e.rm = dst;
        e.force_rex = src->rex8;
        encode(&e);
        return;
    }
    if (dst->kind != OP_REG)
        as_error("two memory operands");
    check_high8(src, dst, e.w);
    e.nop = 1;
    e.op[0] = size == 1 ? 0x8A : 0x8B;
    e.r = dst->reg;
    e.rm = src;
    e.force_rex = dst->rex8;
    encode(&e);
}

/* r <- r/m with a two-byte opcode (0F xx) */
static void reg_rm(int prefix, bool w, int op1, int op2, struct operand *rm, struct operand *reg, int size16)
{
    struct enc e = { 0 };
    e.prefix = prefix;
    e.size16 = size16;
    e.w = w;
    e.nop = op2 >= 0 ? 2 : 1;
    e.op[0] = op1;
    e.op[1] = op2;
    e.r = reg->reg;
    e.rm = rm;
    e.force_rex = rm->kind == OP_REG && rm->rex8;
    encode(&e);
}

static void x87_mem(int op, int ext, struct operand *m)
{
    struct enc e = { 0 };
    e.nop = 1;
    e.op[0] = op;
    e.r = ext;
    e.rm = m;
    encode(&e);
}

static bool simd_instruction(const char *m, struct operand *ops, int n);

static void x86_instruction(char *m, char *args)
{
    struct operand ops[4];
    char *copy = as_strdup(args, strlen(args));
    int size;
    /* prefixes */
    if (strcmp(m, "lock") == 0 || strcmp(m, "rep") == 0 || strcmp(m, "repe") == 0 || strcmp(m, "repne") == 0 ||
        strcmp(m, "repz") == 0 || strcmp(m, "repnz") == 0 || strcmp(m, "data16") == 0 || strcmp(m, "rex64") == 0) {
        /* (data16 and rex64 (62b): the padding of the TLS sequences, 66 and 48) */
        emit_byte(strcmp(m, "lock") == 0 ? 0xF0 : strcmp(m, "data16") == 0 ? 0x66 : strcmp(m, "rex64") == 0 ? 0x48
                  : (strcmp(m, "repne") == 0 || strcmp(m, "repnz") == 0) ? 0xF2 : 0xF3);
        char *rest = (char *)skip_space(args);
        if (!*rest)
            return;
        char *p = rest;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        char *mn = as_strdup(rest, (size_t)(p - rest));
        x86_instruction(mn, (char *)skip_space(p));
        return;
    }
    /* no operands */
    static const struct {
        const char *name;
        int n;
        unsigned char bytes[4];
    } plain[] = {
        { "ret", 1, { 0xC3 } }, { "retq", 1, { 0xC3 } }, { "leave", 1, { 0xC9 } }, { "leaveq", 1, { 0xC9 } },
        { "cltd", 1, { 0x99 } }, { "cdq", 1, { 0x99 } }, { "cqto", 2, { 0x48, 0x99 } }, { "cqo", 2, { 0x48, 0x99 } },
        { "cltq", 2, { 0x48, 0x98 } }, { "cdqe", 2, { 0x48, 0x98 } }, { "cwtl", 1, { 0x98 } },
        { "ud2", 2, { 0x0F, 0x0B } }, { "mfence", 3, { 0x0F, 0xAE, 0xF0 } }, { "lfence", 3, { 0x0F, 0xAE, 0xE8 } },
        { "sfence", 3, { 0x0F, 0xAE, 0xF8 } }, { "nop", 1, { 0x90 } }, { "syscall", 2, { 0x0F, 0x05 } },
        { "hlt", 1, { 0xF4 } }, { "int3", 1, { 0xCC } }, { "pause", 2, { 0xF3, 0x90 } },
        { "movsb", 1, { 0xA4 } }, { "movsq", 2, { 0x48, 0xA5 } }, { "movsl", 1, { 0xA5 } },
        { "stosb", 1, { 0xAA } }, { "stosq", 2, { 0x48, 0xAB } }, { "stosl", 1, { 0xAB } },
        { "fchs", 2, { 0xD9, 0xE0 } }, { "fabs", 2, { 0xD9, 0xE1 } }, { "fldz", 2, { 0xD9, 0xEE } },
        { "fld1", 2, { 0xD9, 0xE8 } }, { "fninit", 2, { 0xDB, 0xE3 } }, { "fwait", 1, { 0x9B } },
        { "fnclex", 2, { 0xDB, 0xE2 } }, { "fclex", 3, { 0x9B, 0xDB, 0xE2 } },
        { "cpuid", 2, { 0x0F, 0xA2 } }, { "rdtsc", 2, { 0x0F, 0x31 } }, { "endbr64", 4, { 0xF3, 0x0F, 0x1E, 0xFA } },
        /* (62e) the kernel's */
        { "cli", 1, { 0xFA } }, { "sti", 1, { 0xFB } }, { "cld", 1, { 0xFC } }, { "std", 1, { 0xFD } },
        { "iretq", 2, { 0x48, 0xCF } }, { "lretq", 2, { 0x48, 0xCB } }, { "swapgs", 3, { 0x0F, 0x01, 0xF8 } },
        { "wbinvd", 2, { 0x0F, 0x09 } }, { "rdmsr", 2, { 0x0F, 0x32 } }, { "wrmsr", 2, { 0x0F, 0x30 } },
        { "pushfq", 1, { 0x9C } }, { "popfq", 1, { 0x9D } }, { "pushf", 1, { 0x9C } }, { "popf", 1, { 0x9D } },
        { "sysretq", 3, { 0x48, 0x0F, 0x07 } },
        /* (32a) the extended control register: AVX's switch */
        { "xgetbv", 3, { 0x0F, 0x01, 0xD0 } }, { "xsetbv", 3, { 0x0F, 0x01, 0xD1 } },
        /* (37d-2) the idle thread's sleep (rax, ecx, edx implied) */
        { "monitor", 3, { 0x0F, 0x01, 0xC8 } }, { "mwait", 3, { 0x0F, 0x01, 0xC9 } },
    };
    int n = split_operands(copy, ops);
    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++)
        if (strcmp(m, plain[i].name) == 0 && n == 0) {
            emit_bytes(plain[i].bytes, plain[i].n);
            return;
        }
    /* (62e) system instructions on memory: 0F AE /n, 0F 01 /n, and 0F 00 /n on a register too */
    {
        static const struct {
            const char *name;
            int op, ext;
            bool w, reg;
        } system[] = {
            { "fxsave", 0xAE, 0, false, false }, { "fxrstor", 0xAE, 1, false, false },
            { "fxsave64", 0xAE, 0, true, false }, { "fxrstor64", 0xAE, 1, true, false },
            { "xsave", 0xAE, 4, false, false }, { "xrstor", 0xAE, 5, false, false },
            { "xsave64", 0xAE, 4, true, false }, { "xrstor64", 0xAE, 5, true, false },
            { "sgdt", 0x01, 0, false, false }, { "sidt", 0x01, 1, false, false }, { "lgdt", 0x01, 2, false, false },
            { "lidt", 0x01, 3, false, false }, { "invlpg", 0x01, 7, false, false },
            { "sgdtq", 0x01, 0, false, false }, { "sidtq", 0x01, 1, false, false }, { "lgdtq", 0x01, 2, false, false },
            { "lidtq", 0x01, 3, false, false },
            { "sldt", 0x00, 0, false, true }, { "str", 0x00, 1, false, true }, { "lldt", 0x00, 2, false, true },
            { "ltr", 0x00, 3, false, true },
        };
        for (size_t i = 0; i < sizeof(system) / sizeof(system[0]); i++)
            if (strcmp(m, system[i].name) == 0) {
                need(n, 1, m);
                if (ops[0].kind != OP_MEM && !(system[i].reg && ops[0].kind == OP_REG))
                    as_error("'%s' takes a memory operand", m);
                struct enc e = { 0 };
                e.nop = 2;
                e.op[0] = 0x0F;
                e.op[1] = system[i].op;
                e.r = system[i].ext;
                e.rm = &ops[0];
                e.w = system[i].w || (ops[0].kind == OP_REG && ops[0].size == 8 && strcmp(m, "str") == 0);
                e.size16 = ops[0].kind == OP_REG && ops[0].size == 2 && (strcmp(m, "str") == 0 || strcmp(m, "sldt") == 0);
                encode(&e);
                return;
            }
    }
    if (strcmp(m, "int") == 0) {
        need(n, 1, m);
        if (ops[0].kind != OP_IMM)
            as_error("int takes an immediate");
        int64_t v;
        if (constant(&ops[0].expr, &v) && v == 3) {
            emit_byte(0xCC);                    /* (int3, as LLVM writes it) */
            return;
        }
        emit_byte(0xCD);
        immediate(&ops[0].expr, 1);
        return;
    }
    /* (62e) port input and output: in PORT, A and out A, PORT; PORT is %dx or a byte */
    if ((match(m, "in", &size, false) || match(m, "out", &size, false)) && n == 2) {
        bool in = m[0] == 'i';
        struct operand *a = in ? &ops[1] : &ops[0], *port = in ? &ops[0] : &ops[1];
        if (a->kind != OP_REG || a->rc != RC_GPR || a->reg != 0 || a->high8)
            as_error("'%s' moves through %%al, %%ax or %%eax", m);
        size = size ? size : a->size;
        if (size == 2)
            emit_byte(0x66);
        bool dx = port->kind == OP_REG && port->rc == RC_GPR && port->reg == 2 && port->size == 2;
        if (!dx && port->kind != OP_IMM)
            as_error("a port is %%dx or an immediate");
        emit_byte((in ? (dx ? 0xEC : 0xE4) : (dx ? 0xEE : 0xE6)) + (size == 1 ? 0 : 1));
        if (!dx)
            immediate(&port->expr, 1);
        return;
    }
    if ((match(m, "inc", &size, false) || match(m, "dec", &size, false)) && n == 1) {
        size = operand_size(size, ops, n);
        struct enc e = { 0 };
        e.size16 = size == 2;
        e.w = size == 8;
        e.nop = 1;
        e.op[0] = size == 1 ? 0xFE : 0xFF;
        e.r = m[0] == 'd';
        e.rm = &ops[0];
        e.force_rex = ops[0].kind == OP_REG && ops[0].rex8;
        encode(&e);
        return;
    }
    /* jumps and calls */
    if (strcmp(m, "jmp") == 0 || strcmp(m, "jmpq") == 0 || strcmp(m, "call") == 0 || strcmp(m, "callq") == 0) {
        need(n, 1, m);
        bool call = m[0] == 'c';
        if (ops[0].indirect) {
            struct enc e = { 0 };
            e.nop = 1;
            e.op[0] = 0xFF;
            e.r = call ? 2 : 4;
            e.rm = &ops[0];
            encode(&e);
            return;
        }
        if (ops[0].kind != OP_MEM || ops[0].base >= 0 || ops[0].index >= 0 || ops[0].rip)
            as_error("bad jump target");
        if (call) {
            emit_jump(&ops[0].expr, -2);
            return;
        }
        emit_jump(&ops[0].expr, -1);
        return;
    }
    if (m[0] == 'j' && condition(m + 1) >= 0) {
        need(n, 1, m);
        emit_jump(&ops[0].expr, condition(m + 1));
        return;
    }
    if (strncmp(m, "set", 3) == 0 && condition(m + 3) >= 0) {
        need(n, 1, m);
        reg_rm(0, false, 0x0F, 0x90 + condition(m + 3), &ops[0], &(struct operand){ .reg = 0 }, 0);
        return;
    }
    if (strncmp(m, "cmov", 4) == 0) {
        char *c = m + 4;
        size_t len = strlen(c);
        int sz = 0;
        char cc[8] = { 0 };
        if (len >= 2 && suffix_size(c[len - 1]) && condition((memcpy(cc, c, len - 1), cc)) >= 0)
            sz = suffix_size(c[len - 1]);
        else {
            memcpy(cc, c, len < 7 ? len : 7);
            cc[len < 7 ? len : 7] = 0;
        }
        if (condition(cc) < 0)
            as_error("unknown instruction '%s'", m);
        need(n, 2, m);
        sz = operand_size(sz, ops, n);
        reg_rm(0, sz == 8, 0x0F, 0x40 + condition(cc), &ops[0], &ops[1], sz == 2);
        return;
    }
    /* the ALU group */
    static const char *alu_names[] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };
    for (int k = 0; k < 8; k++)
        if (match(m, alu_names[k], &size, false)) {
            need(n, 2, m);
            alu(k, operand_size(size, ops, n), &ops[0], &ops[1]);
            return;
        }
    if (match(m, "mov", &size, false) && n == 2 && (ops[0].rc == RC_CR || ops[1].rc == RC_CR)) {
        /* (62d) mov %crN, %r64 (0F 20) and mov %r64, %crN (0F 22): ModRM reg = the control register */
        bool to_cr = ops[1].rc == RC_CR;
        struct operand *cr = to_cr ? &ops[1] : &ops[0], *g = to_cr ? &ops[0] : &ops[1];
        if (g->kind != OP_REG || g->rc != RC_GPR || g->size != 8)
            as_error("a control register moves to or from a 64-bit register");
        struct enc e = { 0 };
        e.nop = 2;
        e.op[0] = 0x0F;
        e.op[1] = to_cr ? 0x22 : 0x20;
        e.r = cr->reg;
        e.rm = g;
        encode(&e);
        return;
    }
    if (match(m, "mov", &size, false) && n == 2 && (ops[0].rc == RC_SEG || ops[1].rc == RC_SEG)) {
        /* (62e) mov %sreg, r/m (8C) and mov r/m, %sreg (8E): ModRM reg = the segment register */
        bool to_seg = ops[1].rc == RC_SEG;
        struct operand *sr = to_seg ? &ops[1] : &ops[0], *g = to_seg ? &ops[0] : &ops[1];
        struct enc e = { 0 };
        e.nop = 1;
        e.op[0] = to_seg ? 0x8E : 0x8C;
        e.r = sr->reg;
        e.rm = g;
        if (!to_seg && g->kind == OP_REG) {
            e.size16 = g->size == 2;
            e.w = g->size == 8;
        }
        encode(&e);
        return;
    }
    if (match(m, "mov", &size, false) && !(n == 2 && (ops[0].rc == RC_XMM || ops[1].rc == RC_XMM))) {
        need(n, 2, m);
        mov(operand_size(size, ops, n), &ops[0], &ops[1]);
        return;
    }
    if (strcmp(m, "movabsq") == 0 || strcmp(m, "movabs") == 0) {
        need(n, 2, m);
        int64_t v;
        if (ops[0].kind != OP_IMM || ops[1].kind != OP_REG)
            as_error("movabs takes an immediate and a register");
        emit_byte(0x48 | (ops[1].reg >= 8 ? 1 : 0));
        emit_byte(0xB8 + (ops[1].reg & 7));
        if (constant(&ops[0].expr, &v))
            emit_le((uint64_t)v, 8);
        else {
            emit_fixup(&ops[0].expr, K_X86_ABS, 8, false, 0);
            emit_le(0, 8);
        }
        return;
    }
    /* widening moves: movzbl movsbq movslq ... */
    {
        static const struct {
            const char *name;
            int op1, op2, from, to;
        } wides[] = {
            { "movzbw", 0x0F, 0xB6, 1, 2 }, { "movzbl", 0x0F, 0xB6, 1, 4 }, { "movzbq", 0x0F, 0xB6, 1, 8 },
            { "movzwl", 0x0F, 0xB7, 2, 4 }, { "movzwq", 0x0F, 0xB7, 2, 8 },
            { "movsbw", 0x0F, 0xBE, 1, 2 }, { "movsbl", 0x0F, 0xBE, 1, 4 }, { "movsbq", 0x0F, 0xBE, 1, 8 },
            { "movswl", 0x0F, 0xBF, 2, 4 }, { "movswq", 0x0F, 0xBF, 2, 8 }, { "movslq", 0x63, -1, 4, 8 },
            { "movsxd", 0x63, -1, 4, 8 },
        };
        for (size_t i = 0; i < sizeof(wides) / sizeof(wides[0]); i++)
            if (strcmp(m, wides[i].name) == 0) {
                need(n, 2, m);
                if (ops[1].kind != OP_REG)
                    as_error("'%s' needs a register destination", m);
                reg_rm(0, wides[i].to == 8, wides[i].op1, wides[i].op2, &ops[0], &ops[1], wides[i].to == 2);
                return;
            }
    }
    if (match(m, "lea", &size, false)) {
        need(n, 2, m);
        size = operand_size(size, ops, n);
        reg_rm(0, size == 8, 0x8D, -1, &ops[0], &ops[1], size == 2);
        return;
    }
    if (match(m, "imul", &size, false) && n == 3) {
        /* (61c) imul $imm, src, dst: 6B with a byte, else 69 */
        size = operand_size(size, ops, n);
        int64_t v;
        struct enc e = { 0 };
        e.size16 = size == 2;
        e.w = size == 8;
        e.nop = 1;
        e.r = ops[2].reg;
        e.rm = &ops[1];
        e.force_rex = (ops[1].kind == OP_REG && ops[1].rex8) || ops[2].rex8;
        if (constant(&ops[0].expr, &v) && fits8(v)) {
            e.op[0] = 0x6B;
            e.imm_size = 1;
        } else {
            e.op[0] = 0x69;
            e.imm_size = size == 2 ? 2 : 4;
        }
        e.imm = &ops[0].expr;
        encode(&e);
        return;
    }
    if (match(m, "imul", &size, false) && n == 2) {
        size = operand_size(size, ops, n);
        reg_rm(0, size == 8, 0x0F, 0xAF, &ops[0], &ops[1], size == 2);
        return;
    }
    /* F6/F7 group: test (imm), not, neg, mul, imul, div, idiv */
    {
        static const char *names[] = { 0, 0, "not", "neg", "mul", "imul", "div", "idiv" };
        for (int k = 2; k < 8; k++)
            if (match(m, names[k], &size, false) && n == 1) {
                size = operand_size(size, ops, n);
                struct enc e = { 0 };
                e.size16 = size == 2;
                e.w = size == 8;
                e.nop = 1;
                e.op[0] = size == 1 ? 0xF6 : 0xF7;
                e.r = k;
                e.rm = &ops[0];
                encode(&e);
                return;
            }
    }
    if (match(m, "test", &size, false)) {
        need(n, 2, m);
        size = operand_size(size, ops, n);
        struct enc e = { 0 };
        e.size16 = size == 2;
        e.w = size == 8;
        if (ops[0].kind == OP_IMM) {
            if (ops[1].kind == OP_REG && ops[1].reg == 0) {
                e.nop = 1;
                e.op[0] = size == 1 ? 0xA8 : 0xA9;
                e.imm_size = size == 1 ? 1 : size == 2 ? 2 : 4;
                e.imm = &ops[0].expr;
                encode(&e);
                return;
            }
            e.nop = 1;
            e.op[0] = size == 1 ? 0xF6 : 0xF7;
            e.r = 0;
            e.rm = &ops[1];
            e.imm_size = size == 1 ? 1 : size == 2 ? 2 : 4;
            e.imm = &ops[0].expr;
            encode(&e);
            return;
        }
        e.nop = 1;
        e.op[0] = size == 1 ? 0x84 : 0x85;
        e.r = ops[0].reg;
        e.rm = &ops[1];
        e.force_rex = ops[0].rex8;
        encode(&e);
        return;
    }
    /* shifts and rotates */
    {
        static const char *names[] = { "rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar" };
        static const int exts[] = { 0, 1, 2, 3, 4, 5, 4, 7 };
        for (int k = 0; k < 8; k++)
            if (match(m, names[k], &size, false)) {
                struct operand *target = &ops[n - 1];
                size = operand_size(size, target, 1);
                struct enc e = { 0 };
                e.size16 = size == 2;
                e.w = size == 8;
                e.nop = 1;
                e.r = exts[k];
                e.rm = target;
                int64_t v;
                if (n == 1 || (ops[0].kind == OP_IMM && constant(&ops[0].expr, &v) && v == 1)) {
                    e.op[0] = size == 1 ? 0xD0 : 0xD1;
                } else if (ops[0].kind == OP_REG && ops[0].reg == 1 && ops[0].size == 1) {
                    e.op[0] = size == 1 ? 0xD2 : 0xD3;
                } else if (ops[0].kind == OP_IMM) {
                    e.op[0] = size == 1 ? 0xC0 : 0xC1;
                    e.imm_size = 1;
                    e.imm = &ops[0].expr;
                } else
                    as_error("bad shift count");
                encode(&e);
                return;
            }
    }
    if (match(m, "push", &size, false) || match(m, "pop", &size, false)) {
        need(n, 1, m);
        bool push = m[1] == 'u';
        if (ops[0].kind == OP_REG) {
            if (ops[0].reg >= 8)
                emit_byte(0x41);
            emit_byte((push ? 0x50 : 0x58) + (ops[0].reg & 7));
            return;
        }
        if (push && ops[0].kind == OP_IMM) {
            int64_t v;
            if (constant(&ops[0].expr, &v) && fits8(v)) {
                emit_byte(0x6A);
                emit_byte((int)v);
            } else {
                emit_byte(0x68);
                immediate(&ops[0].expr, 4);
            }
            return;
        }
        struct enc e = { 0 };
        e.nop = 1;
        e.op[0] = push ? 0xFF : 0x8F;
        e.r = push ? 6 : 0;
        e.rm = &ops[0];
        encode(&e);
        return;
    }
    if (match(m, "xchg", &size, false) || match(m, "xadd", &size, false) || match(m, "cmpxchg", &size, false)) {
        need(n, 2, m);
        size = operand_size(size, ops, n);
        struct operand *reg = &ops[0], *rm = &ops[1];
        if (m[0] == 'x' && m[1] == 'c' && ops[0].kind != OP_REG) {
            reg = &ops[1];
            rm = &ops[0];
        }
        struct enc e = { 0 };
        e.size16 = size == 2;
        e.w = size == 8;
        if (m[1] == 'c' && m[0] == 'x') {
            e.nop = 1;
            e.op[0] = size == 1 ? 0x86 : 0x87;
        } else if (m[1] == 'a') {
            e.nop = 2;
            e.op[0] = 0x0F;
            e.op[1] = size == 1 ? 0xC0 : 0xC1;
        } else {
            e.nop = 2;
            e.op[0] = 0x0F;
            e.op[1] = size == 1 ? 0xB0 : 0xB1;
        }
        e.r = reg->reg;
        e.rm = rm;
        e.force_rex = reg->rex8;
        encode(&e);
        return;
    }
    if (match(m, "bsf", &size, false) || match(m, "bsr", &size, false)) {
        need(n, 2, m);
        size = operand_size(size, ops, n);
        reg_rm(0, size == 8, 0x0F, m[2] == 'f' ? 0xBC : 0xBD, &ops[0], &ops[1], size == 2);
        return;
    }
    if (match(m, "bswap", &size, false)) {
        need(n, 1, m);
        size = operand_size(size, ops, n);
        int rex = (size == 8 ? 8 : 0) | (ops[0].reg >= 8 ? 1 : 0);
        if (rex)
            emit_byte(0x40 | rex);
        emit_byte(0x0F);
        emit_byte(0xC8 + (ops[0].reg & 7));
        return;
    }
    {
        static const char *names[] = { "bt", "bts", "btr", "btc" };
        static const int exts[] = { 4, 5, 6, 7 }, regops[] = { 0xA3, 0xAB, 0xB3, 0xBB };
        for (int k = 3; k >= 0; k--)
            if (match(m, names[k], &size, false)) {
                need(n, 2, m);
                size = operand_size(size, &ops[1], 1);
                struct enc e = { 0 };
                e.size16 = size == 2;
                e.w = size == 8;
                e.nop = 2;
                e.op[0] = 0x0F;
                e.rm = &ops[1];
                if (ops[0].kind == OP_IMM) {
                    e.op[1] = 0xBA;
                    e.r = exts[k];
                    e.imm_size = 1;
                    e.imm = &ops[0].expr;
                } else {
                    e.op[1] = regops[k];
                    e.r = ops[0].reg;
                }
                encode(&e);
                return;
            }
    }
    /* SSE */
    {
        static const struct {
            const char *name;
            int prefix, op;
        } sse[] = {
            { "addsd", 0xF2, 0x58 }, { "mulsd", 0xF2, 0x59 }, { "subsd", 0xF2, 0x5C }, { "divsd", 0xF2, 0x5E },
            { "minsd", 0xF2, 0x5D }, { "maxsd", 0xF2, 0x5F }, { "sqrtsd", 0xF2, 0x51 },
            { "addss", 0xF3, 0x58 }, { "mulss", 0xF3, 0x59 }, { "subss", 0xF3, 0x5C }, { "divss", 0xF3, 0x5E },
            { "minss", 0xF3, 0x5D }, { "maxss", 0xF3, 0x5F }, { "sqrtss", 0xF3, 0x51 },
            { "ucomisd", 0x66, 0x2E }, { "ucomiss", 0, 0x2E }, { "comisd", 0x66, 0x2F }, { "comiss", 0, 0x2F },
            { "cvtss2sd", 0xF3, 0x5A }, { "cvtsd2ss", 0xF2, 0x5A }, { "xorps", 0, 0x57 }, { "xorpd", 0x66, 0x57 },
            { "andps", 0, 0x54 }, { "andpd", 0x66, 0x54 }, { "pxor", 0x66, 0xEF }, { "movaps", 0, 0x28 },
            { "movups", 0, 0x10 }, { "movapd", 0x66, 0x28 }, { "movdqa", 0x66, 0x6F }, { "movdqu", 0xF3, 0x6F },
            { "sqrtpd", 0x66, 0x51 },
        };
        for (size_t i = 0; i < sizeof(sse) / sizeof(sse[0]); i++)
            if (strcmp(m, sse[i].name) == 0) {
                need(n, 2, m);
                int op = sse[i].op;
                if (ops[1].kind == OP_MEM) {
                    /* the store forms */
                    if (op == 0x28 || op == 0x10)
                        op++;
                    else if (op == 0x6F)
                        op = 0x7F;
                    else
                        as_error("'%s' cannot store", m);
                    reg_rm(sse[i].prefix, false, 0x0F, op, &ops[1], &ops[0], 0);
                    return;
                }
                reg_rm(sse[i].prefix, false, 0x0F, op, &ops[0], &ops[1], 0);
                return;
            }
    }
    if (strcmp(m, "movsd") == 0 || strcmp(m, "movss") == 0) {
        need(n, 2, m);
        int prefix = m[4] == 'd' ? 0xF2 : 0xF3;
        if (ops[1].kind == OP_MEM)
            reg_rm(prefix, false, 0x0F, 0x11, &ops[1], &ops[0], 0);
        else
            reg_rm(prefix, false, 0x0F, 0x10, &ops[0], &ops[1], 0);
        return;
    }
    if (strcmp(m, "movq") == 0 || strcmp(m, "movd") == 0) {
        /* between a general register (or memory) and an xmm register */
        need(n, 2, m);
        bool w = m[3] == 'q';
        if (ops[1].rc == RC_XMM && ops[0].rc == RC_XMM) {
            reg_rm(0xF3, false, 0x0F, 0x7E, &ops[0], &ops[1], 0);
            return;
        }
        if (ops[1].rc == RC_XMM) {
            if (w && ops[0].kind == OP_MEM)
                reg_rm(0xF3, false, 0x0F, 0x7E, &ops[0], &ops[1], 0);
            else
                reg_rm(0x66, w, 0x0F, 0x6E, &ops[0], &ops[1], 0);
        } else {
            if (w && ops[1].kind == OP_MEM)
                reg_rm(0x66, false, 0x0F, 0xD6, &ops[1], &ops[0], 0);
            else
                reg_rm(0x66, w, 0x0F, 0x7E, &ops[1], &ops[0], 0);
        }
        return;
    }
    {
        /* conversions with an integer side: the suffix sizes that side */
        static const struct {
            const char *name;
            int prefix, op;
            bool int_src;
        } cvt[] = {
            { "cvtsi2sd", 0xF2, 0x2A, true }, { "cvtsi2ss", 0xF3, 0x2A, true },
            { "cvttsd2si", 0xF2, 0x2C, false }, { "cvttss2si", 0xF3, 0x2C, false },
            { "cvtsd2si", 0xF2, 0x2D, false }, { "cvtss2si", 0xF3, 0x2D, false },
        };
        for (size_t i = 0; i < sizeof(cvt) / sizeof(cvt[0]); i++)
            if (match(m, cvt[i].name, &size, false)) {
                need(n, 2, m);
                if (!size)
                    size = cvt[i].int_src ? (ops[0].kind == OP_REG ? ops[0].size : 4) : ops[1].size;
                reg_rm(cvt[i].prefix, size == 8, 0x0F, cvt[i].op, &ops[0], &ops[1], 0);
                return;
            }
    }
    /* x87 */
    {
        static const struct {
            const char *name;
            int op, ext;
        } fmem[] = {
            { "fldt", 0xDB, 5 }, { "fstpt", 0xDB, 7 }, { "flds", 0xD9, 0 }, { "fldl", 0xDD, 0 },
            { "fstps", 0xD9, 3 }, { "fstpl", 0xDD, 3 }, { "fsts", 0xD9, 2 }, { "fstl", 0xDD, 2 },
            { "fildl", 0xDB, 0 }, { "fildll", 0xDF, 5 }, { "fildq", 0xDF, 5 }, { "filds", 0xDF, 0 },
            { "fistpl", 0xDB, 3 }, { "fistpll", 0xDF, 7 }, { "fisttpl", 0xDB, 1 }, { "fisttpll", 0xDD, 1 },
            { "fisttpq", 0xDD, 1 }, { "fadds", 0xD8, 0 }, { "fmuls", 0xD8, 1 }, { "fsubs", 0xD8, 4 },
            { "fdivs", 0xD8, 6 }, { "faddl", 0xDC, 0 }, { "fsubl", 0xDC, 4 }, { "fmull", 0xDC, 1 },
            { "fdivl", 0xDC, 6 }, { "fnstcw", 0xD9, 7 }, { "fldcw", 0xD9, 5 }, { "fnstsw", 0xDD, 7 },
            { "fnstenv", 0xD9, 6 }, { "fldenv", 0xD9, 4 },  /* (62c: fenv.c's) */
        };
        for (size_t i = 0; i < sizeof(fmem) / sizeof(fmem[0]); i++)
            if (strcmp(m, fmem[i].name) == 0 && n == 1 && ops[0].kind == OP_MEM) {
                x87_mem(fmem[i].op, fmem[i].ext, &ops[0]);
                return;
            }
        if ((strcmp(m, "stmxcsr") == 0 || strcmp(m, "ldmxcsr") == 0) && n == 1 && ops[0].kind == OP_MEM) {
            /* (62c) 0F AE /3 and /2 */
            struct enc e = { 0 };
            e.nop = 2;
            e.op[0] = 0x0F;
            e.op[1] = 0xAE;
            e.r = m[0] == 's' ? 3 : 2;
            e.rm = &ops[0];
            encode(&e);
            return;
        }
        /* register forms: "fXXXp %st, %st(i)" and friends (GNU's AT&T
         * names: fsubp/fsubrp and fdivp/fdivrp swapped from Intel's) */
        static const struct {
            const char *name;
            int op, base;
        } freg[] = {
            { "faddp", 0xDE, 0xC0 }, { "fmulp", 0xDE, 0xC8 }, { "fsubrp", 0xDE, 0xE8 }, { "fsubp", 0xDE, 0xE0 },
            { "fdivrp", 0xDE, 0xF8 }, { "fdivp", 0xDE, 0xF0 }, { "fucomip", 0xDF, 0xE8 }, { "fcomip", 0xDF, 0xF0 },
            { "fucomi", 0xDB, 0xE8 }, { "fstp", 0xDD, 0xD8 }, { "fld", 0xD9, 0xC0 }, { "fxch", 0xD9, 0xC8 },
        };
        for (size_t i = 0; i < sizeof(freg) / sizeof(freg[0]); i++)
            if (strcmp(m, freg[i].name) == 0) {
                int st = 1;
                if (n >= 1) {
                    struct operand *o = n == 2 ? (freg[i].op == 0xDE ? &ops[1] : &ops[0]) : &ops[0];
                    if (o->rc != RC_ST)
                        as_error("'%s' takes x87 registers", m);
                    st = o->reg;
                }
                emit_byte(freg[i].op);
                emit_byte(freg[i].base + st);
                return;
            }
    }
    if (simd_instruction(m, ops, n))
        return;
    as_error("unknown instruction '%s %s'", m, args);
}

/* --- SSE and AVX (32c) ---------------------------------------------------- */

/*
 * One table gives both encodings of a vector instruction: the SSE one
 * (mandatory prefix, REX, 0F [38|3A] op) under its name, and the AVX one
 * (a VEX prefix carrying REX's bits, the map, the prefix, a second source
 * register and the vector length) under "v" + the name, as the Intel manual
 * pairs them. Entries marked S_VEX exist only as AVX (their name is the
 * whole mnemonic), S_SSE only as SSE. The SSE forms the old table above
 * already has are taken there first. LLVM's choices are kept: the 2-byte
 * VEX prefix where it fits, and where it does not only because the r/m
 * register is r8-r15, commutable operations (not the scalar ones: their
 * upper part comes from the first source) and moves are encoded with their
 * operands the other way round so that it does (LLVM's
 * optimizeInstFromVEX3ToVEX2).
 */

/* operand forms (AT&T order; an S_IMM entry has the immediate first) */
enum {
    F_RVM,      /* VEX: r/m, vvvv, reg; SSE: r/m, reg */
    F_RM,       /* r/m, reg */
    F_MR,       /* reg, r/m (stores, extracts) */
    F_VMI,      /* VEX: $imm, r/m, vvvv; SSE: $imm, r/m (shifts by an immediate: reg is ext) */
    F_RVMR,     /* VEX: reg4, r/m, vvvv, reg (the 4th register in the immediate's top bits) */
    F_MVR,      /* VEX: reg, vvvv, r/m (masked stores) */
};

#define S_IMM       0x0001      /* an 8-bit immediate */
#define S_VEX       0x0002      /* only the AVX form */
#define S_SSE       0x0004      /* only the SSE form */
#define S_COMM      0x0008      /* commutable */
#define S_W1        0x0010      /* VEX.W (REX.W in the SSE form) */
#define S_128       0x0020      /* no 256-bit form */
#define S_256       0x0040      /* only the 256-bit form */
#define S_HALF      0x0080      /* the r/m register is an xmm one in the 256-bit form too */
#define S_GPR       0x0100      /* the r/m register is a general one */
#define S_GREG      0x0200      /* the reg operand is a general register */
#define S_MEM       0x0400      /* r/m only memory */
#define S_REG       0x0800      /* r/m only a register */
#define S_CMP       0x1000      /* a compare whose predicate decides whether it commutes */

struct simd {
    const char *name;
    unsigned char pp, map, op, form;
    unsigned short flags;
    unsigned char ext;          /* F_VMI: the reg field */
    unsigned char store;        /* a move's store opcode (reg, r/m), 0: none */
};

static const struct simd simd_table[] = {
    /* moves */
    { "movaps", 0, 1, 0x28, F_RM, 0, 0, 0x29 }, { "movups", 0, 1, 0x10, F_RM, 0, 0, 0x11 },
    { "movapd", 0x66, 1, 0x28, F_RM, 0, 0, 0x29 }, { "movupd", 0x66, 1, 0x10, F_RM, 0, 0, 0x11 },
    { "movdqa", 0x66, 1, 0x6F, F_RM, 0, 0, 0x7F }, { "movdqu", 0xF3, 1, 0x6F, F_RM, 0, 0, 0x7F },
    { "movntdq", 0x66, 1, 0xE7, F_MR, S_MEM, 0, 0 }, { "movntps", 0, 1, 0x2B, F_MR, S_MEM, 0, 0 },
    { "movntpd", 0x66, 1, 0x2B, F_MR, S_MEM, 0, 0 }, { "movntdqa", 0x66, 2, 0x2A, F_RM, S_MEM, 0, 0 },
    { "lddqu", 0xF2, 1, 0xF0, F_RM, S_MEM, 0, 0 },
    { "movshdup", 0xF3, 1, 0x16, F_RM, 0, 0, 0 }, { "movsldup", 0xF3, 1, 0x12, F_RM, 0, 0, 0 },
    { "movddup", 0xF2, 1, 0x12, F_RM, 0, 0, 0 },
    { "movhlps", 0, 1, 0x12, F_RVM, S_REG | S_128, 0, 0 }, { "movlhps", 0, 1, 0x16, F_RVM, S_REG | S_128, 0, 0 },
    { "movlps", 0, 1, 0x12, F_RVM, S_MEM | S_128, 0, 0 }, { "movlps", 0, 1, 0x13, F_MR, S_MEM | S_128, 0, 0 },
    { "movhps", 0, 1, 0x16, F_RVM, S_MEM | S_128, 0, 0 }, { "movhps", 0, 1, 0x17, F_MR, S_MEM | S_128, 0, 0 },
    { "movlpd", 0x66, 1, 0x12, F_RVM, S_MEM | S_128, 0, 0 }, { "movlpd", 0x66, 1, 0x13, F_MR, S_MEM | S_128, 0, 0 },
    { "movhpd", 0x66, 1, 0x16, F_RVM, S_MEM | S_128, 0, 0 }, { "movhpd", 0x66, 1, 0x17, F_MR, S_MEM | S_128, 0, 0 },
    /* integers */
    { "paddb", 0x66, 1, 0xFC, F_RVM, S_COMM, 0, 0 }, { "paddw", 0x66, 1, 0xFD, F_RVM, S_COMM, 0, 0 },
    { "paddd", 0x66, 1, 0xFE, F_RVM, S_COMM, 0, 0 }, { "paddq", 0x66, 1, 0xD4, F_RVM, S_COMM, 0, 0 },
    { "psubb", 0x66, 1, 0xF8, F_RVM, 0, 0, 0 }, { "psubw", 0x66, 1, 0xF9, F_RVM, 0, 0, 0 },
    { "psubd", 0x66, 1, 0xFA, F_RVM, 0, 0, 0 }, { "psubq", 0x66, 1, 0xFB, F_RVM, 0, 0, 0 },
    { "paddsb", 0x66, 1, 0xEC, F_RVM, S_COMM, 0, 0 }, { "paddsw", 0x66, 1, 0xED, F_RVM, S_COMM, 0, 0 },
    { "paddusb", 0x66, 1, 0xDC, F_RVM, S_COMM, 0, 0 }, { "paddusw", 0x66, 1, 0xDD, F_RVM, S_COMM, 0, 0 },
    { "psubsb", 0x66, 1, 0xE8, F_RVM, 0, 0, 0 }, { "psubsw", 0x66, 1, 0xE9, F_RVM, 0, 0, 0 },
    { "psubusb", 0x66, 1, 0xD8, F_RVM, 0, 0, 0 }, { "psubusw", 0x66, 1, 0xD9, F_RVM, 0, 0, 0 },
    { "pmullw", 0x66, 1, 0xD5, F_RVM, S_COMM, 0, 0 }, { "pmulhw", 0x66, 1, 0xE5, F_RVM, S_COMM, 0, 0 },
    { "pmulhuw", 0x66, 1, 0xE4, F_RVM, S_COMM, 0, 0 }, { "pmuludq", 0x66, 1, 0xF4, F_RVM, S_COMM, 0, 0 },
    { "pmaddwd", 0x66, 1, 0xF5, F_RVM, S_COMM, 0, 0 }, { "psadbw", 0x66, 1, 0xF6, F_RVM, S_COMM, 0, 0 },
    { "pavgb", 0x66, 1, 0xE0, F_RVM, S_COMM, 0, 0 }, { "pavgw", 0x66, 1, 0xE3, F_RVM, S_COMM, 0, 0 },
    { "pminub", 0x66, 1, 0xDA, F_RVM, S_COMM, 0, 0 }, { "pmaxub", 0x66, 1, 0xDE, F_RVM, S_COMM, 0, 0 },
    { "pminsw", 0x66, 1, 0xEA, F_RVM, S_COMM, 0, 0 }, { "pmaxsw", 0x66, 1, 0xEE, F_RVM, S_COMM, 0, 0 },
    { "pand", 0x66, 1, 0xDB, F_RVM, S_COMM, 0, 0 }, { "pandn", 0x66, 1, 0xDF, F_RVM, 0, 0, 0 },
    { "por", 0x66, 1, 0xEB, F_RVM, S_COMM, 0, 0 }, { "pxor", 0x66, 1, 0xEF, F_RVM, S_COMM, 0, 0 },
    { "pcmpeqb", 0x66, 1, 0x74, F_RVM, S_COMM, 0, 0 }, { "pcmpeqw", 0x66, 1, 0x75, F_RVM, S_COMM, 0, 0 },
    { "pcmpeqd", 0x66, 1, 0x76, F_RVM, S_COMM, 0, 0 }, { "pcmpgtb", 0x66, 1, 0x64, F_RVM, 0, 0, 0 },
    { "pcmpgtw", 0x66, 1, 0x65, F_RVM, 0, 0, 0 }, { "pcmpgtd", 0x66, 1, 0x66, F_RVM, 0, 0, 0 },
    { "packsswb", 0x66, 1, 0x63, F_RVM, 0, 0, 0 }, { "packuswb", 0x66, 1, 0x67, F_RVM, 0, 0, 0 },
    { "packssdw", 0x66, 1, 0x6B, F_RVM, 0, 0, 0 },
    { "punpcklbw", 0x66, 1, 0x60, F_RVM, 0, 0, 0 }, { "punpcklwd", 0x66, 1, 0x61, F_RVM, 0, 0, 0 },
    { "punpckldq", 0x66, 1, 0x62, F_RVM, 0, 0, 0 }, { "punpcklqdq", 0x66, 1, 0x6C, F_RVM, 0, 0, 0 },
    { "punpckhbw", 0x66, 1, 0x68, F_RVM, 0, 0, 0 }, { "punpckhwd", 0x66, 1, 0x69, F_RVM, 0, 0, 0 },
    { "punpckhdq", 0x66, 1, 0x6A, F_RVM, 0, 0, 0 }, { "punpckhqdq", 0x66, 1, 0x6D, F_RVM, 0, 0, 0 },
    { "psrlw", 0x66, 1, 0xD1, F_RVM, S_HALF, 0, 0 }, { "psrld", 0x66, 1, 0xD2, F_RVM, S_HALF, 0, 0 },
    { "psrlq", 0x66, 1, 0xD3, F_RVM, S_HALF, 0, 0 }, { "psraw", 0x66, 1, 0xE1, F_RVM, S_HALF, 0, 0 },
    { "psrad", 0x66, 1, 0xE2, F_RVM, S_HALF, 0, 0 }, { "psllw", 0x66, 1, 0xF1, F_RVM, S_HALF, 0, 0 },
    { "pslld", 0x66, 1, 0xF2, F_RVM, S_HALF, 0, 0 }, { "psllq", 0x66, 1, 0xF3, F_RVM, S_HALF, 0, 0 },
    { "psrlw", 0x66, 1, 0x71, F_VMI, S_IMM | S_REG, 2, 0 }, { "psraw", 0x66, 1, 0x71, F_VMI, S_IMM | S_REG, 4, 0 },
    { "psllw", 0x66, 1, 0x71, F_VMI, S_IMM | S_REG, 6, 0 }, { "psrld", 0x66, 1, 0x72, F_VMI, S_IMM | S_REG, 2, 0 },
    { "psrad", 0x66, 1, 0x72, F_VMI, S_IMM | S_REG, 4, 0 }, { "pslld", 0x66, 1, 0x72, F_VMI, S_IMM | S_REG, 6, 0 },
    { "psrlq", 0x66, 1, 0x73, F_VMI, S_IMM | S_REG, 2, 0 }, { "psrldq", 0x66, 1, 0x73, F_VMI, S_IMM | S_REG, 3, 0 },
    { "psllq", 0x66, 1, 0x73, F_VMI, S_IMM | S_REG, 6, 0 }, { "pslldq", 0x66, 1, 0x73, F_VMI, S_IMM | S_REG, 7, 0 },
    { "pmovmskb", 0x66, 1, 0xD7, F_RM, S_GREG | S_REG, 0, 0 },
    { "pshufd", 0x66, 1, 0x70, F_RM, S_IMM, 0, 0 }, { "pshufhw", 0xF3, 1, 0x70, F_RM, S_IMM, 0, 0 },
    { "pshuflw", 0xF2, 1, 0x70, F_RM, S_IMM, 0, 0 },
    { "pinsrw", 0x66, 1, 0xC4, F_RVM, S_IMM | S_GPR | S_128, 0, 0 },
    { "pextrw", 0x66, 1, 0xC5, F_RM, S_IMM | S_GREG | S_REG | S_128, 0, 0 },
    { "pextrw", 0x66, 3, 0x15, F_MR, S_IMM | S_MEM | S_128, 0, 0 },
    { "pshufb", 0x66, 2, 0x00, F_RVM, 0, 0, 0 }, { "phaddw", 0x66, 2, 0x01, F_RVM, 0, 0, 0 },
    { "phaddd", 0x66, 2, 0x02, F_RVM, 0, 0, 0 }, { "phaddsw", 0x66, 2, 0x03, F_RVM, 0, 0, 0 },
    { "pmaddubsw", 0x66, 2, 0x04, F_RVM, 0, 0, 0 }, { "phsubw", 0x66, 2, 0x05, F_RVM, 0, 0, 0 },
    { "phsubd", 0x66, 2, 0x06, F_RVM, 0, 0, 0 }, { "phsubsw", 0x66, 2, 0x07, F_RVM, 0, 0, 0 },
    { "psignb", 0x66, 2, 0x08, F_RVM, 0, 0, 0 }, { "psignw", 0x66, 2, 0x09, F_RVM, 0, 0, 0 },
    { "psignd", 0x66, 2, 0x0A, F_RVM, 0, 0, 0 }, { "pmulhrsw", 0x66, 2, 0x0B, F_RVM, 0, 0, 0 },
    { "pabsb", 0x66, 2, 0x1C, F_RM, 0, 0, 0 }, { "pabsw", 0x66, 2, 0x1D, F_RM, 0, 0, 0 },
    { "pabsd", 0x66, 2, 0x1E, F_RM, 0, 0, 0 },
    { "pmuldq", 0x66, 2, 0x28, F_RVM, 0, 0, 0 }, { "pcmpeqq", 0x66, 2, 0x29, F_RVM, 0, 0, 0 },
    { "packusdw", 0x66, 2, 0x2B, F_RVM, 0, 0, 0 }, { "pcmpgtq", 0x66, 2, 0x37, F_RVM, 0, 0, 0 },
    { "pminsb", 0x66, 2, 0x38, F_RVM, 0, 0, 0 }, { "pminsd", 0x66, 2, 0x39, F_RVM, 0, 0, 0 },
    { "pminuw", 0x66, 2, 0x3A, F_RVM, 0, 0, 0 }, { "pminud", 0x66, 2, 0x3B, F_RVM, 0, 0, 0 },
    { "pmaxsb", 0x66, 2, 0x3C, F_RVM, 0, 0, 0 }, { "pmaxsd", 0x66, 2, 0x3D, F_RVM, 0, 0, 0 },
    { "pmaxuw", 0x66, 2, 0x3E, F_RVM, 0, 0, 0 }, { "pmaxud", 0x66, 2, 0x3F, F_RVM, 0, 0, 0 },
    { "pmulld", 0x66, 2, 0x40, F_RVM, 0, 0, 0 }, { "phminposuw", 0x66, 2, 0x41, F_RM, S_128, 0, 0 },
    { "ptest", 0x66, 2, 0x17, F_RM, 0, 0, 0 },
    { "pmovsxbw", 0x66, 2, 0x20, F_RM, S_HALF, 0, 0 }, { "pmovsxbd", 0x66, 2, 0x21, F_RM, S_HALF, 0, 0 },
    { "pmovsxbq", 0x66, 2, 0x22, F_RM, S_HALF, 0, 0 }, { "pmovsxwd", 0x66, 2, 0x23, F_RM, S_HALF, 0, 0 },
    { "pmovsxwq", 0x66, 2, 0x24, F_RM, S_HALF, 0, 0 }, { "pmovsxdq", 0x66, 2, 0x25, F_RM, S_HALF, 0, 0 },
    { "pmovzxbw", 0x66, 2, 0x30, F_RM, S_HALF, 0, 0 }, { "pmovzxbd", 0x66, 2, 0x31, F_RM, S_HALF, 0, 0 },
    { "pmovzxbq", 0x66, 2, 0x32, F_RM, S_HALF, 0, 0 }, { "pmovzxwd", 0x66, 2, 0x33, F_RM, S_HALF, 0, 0 },
    { "pmovzxwq", 0x66, 2, 0x34, F_RM, S_HALF, 0, 0 }, { "pmovzxdq", 0x66, 2, 0x35, F_RM, S_HALF, 0, 0 },
    { "pblendvb", 0x66, 2, 0x10, F_RVM, S_SSE, 0, 0 }, { "blendvps", 0x66, 2, 0x14, F_RVM, S_SSE, 0, 0 },
    { "blendvpd", 0x66, 2, 0x15, F_RVM, S_SSE, 0, 0 },
    { "palignr", 0x66, 3, 0x0F, F_RVM, S_IMM, 0, 0 }, { "pblendw", 0x66, 3, 0x0E, F_RVM, S_IMM, 0, 0 },
    { "blendps", 0x66, 3, 0x0C, F_RVM, S_IMM, 0, 0 }, { "blendpd", 0x66, 3, 0x0D, F_RVM, S_IMM, 0, 0 },
    { "mpsadbw", 0x66, 3, 0x42, F_RVM, S_IMM, 0, 0 }, { "dpps", 0x66, 3, 0x40, F_RVM, S_IMM, 0, 0 },
    { "dppd", 0x66, 3, 0x41, F_RVM, S_IMM | S_128, 0, 0 },
    { "roundps", 0x66, 3, 0x08, F_RM, S_IMM, 0, 0 }, { "roundpd", 0x66, 3, 0x09, F_RM, S_IMM, 0, 0 },
    { "roundss", 0x66, 3, 0x0A, F_RVM, S_IMM | S_128, 0, 0 }, { "roundsd", 0x66, 3, 0x0B, F_RVM, S_IMM | S_128, 0, 0 },
    { "insertps", 0x66, 3, 0x21, F_RVM, S_IMM | S_128, 0, 0 },
    { "extractps", 0x66, 3, 0x17, F_MR, S_IMM | S_GPR | S_128, 0, 0 },
    { "pinsrb", 0x66, 3, 0x20, F_RVM, S_IMM | S_GPR | S_128, 0, 0 },
    { "pinsrd", 0x66, 3, 0x22, F_RVM, S_IMM | S_GPR | S_128, 0, 0 },
    { "pinsrq", 0x66, 3, 0x22, F_RVM, S_IMM | S_GPR | S_W1 | S_128, 0, 0 },
    { "pextrb", 0x66, 3, 0x14, F_MR, S_IMM | S_GPR | S_128, 0, 0 },
    { "pextrd", 0x66, 3, 0x16, F_MR, S_IMM | S_GPR | S_128, 0, 0 },
    { "pextrq", 0x66, 3, 0x16, F_MR, S_IMM | S_GPR | S_W1 | S_128, 0, 0 },
    /* floating point */
    { "addps", 0, 1, 0x58, F_RVM, S_COMM, 0, 0 }, { "addpd", 0x66, 1, 0x58, F_RVM, S_COMM, 0, 0 },
    { "addss", 0xF3, 1, 0x58, F_RVM, S_128, 0, 0 }, { "addsd", 0xF2, 1, 0x58, F_RVM, S_128, 0, 0 },
    { "mulps", 0, 1, 0x59, F_RVM, S_COMM, 0, 0 }, { "mulpd", 0x66, 1, 0x59, F_RVM, S_COMM, 0, 0 },
    { "mulss", 0xF3, 1, 0x59, F_RVM, S_128, 0, 0 }, { "mulsd", 0xF2, 1, 0x59, F_RVM, S_128, 0, 0 },
    { "subps", 0, 1, 0x5C, F_RVM, 0, 0, 0 }, { "subpd", 0x66, 1, 0x5C, F_RVM, 0, 0, 0 },
    { "subss", 0xF3, 1, 0x5C, F_RVM, S_128, 0, 0 }, { "subsd", 0xF2, 1, 0x5C, F_RVM, S_128, 0, 0 },
    { "divps", 0, 1, 0x5E, F_RVM, 0, 0, 0 }, { "divpd", 0x66, 1, 0x5E, F_RVM, 0, 0, 0 },
    { "divss", 0xF3, 1, 0x5E, F_RVM, S_128, 0, 0 }, { "divsd", 0xF2, 1, 0x5E, F_RVM, S_128, 0, 0 },
    { "minps", 0, 1, 0x5D, F_RVM, 0, 0, 0 }, { "minpd", 0x66, 1, 0x5D, F_RVM, 0, 0, 0 },
    { "minss", 0xF3, 1, 0x5D, F_RVM, S_128, 0, 0 }, { "minsd", 0xF2, 1, 0x5D, F_RVM, S_128, 0, 0 },
    { "maxps", 0, 1, 0x5F, F_RVM, 0, 0, 0 }, { "maxpd", 0x66, 1, 0x5F, F_RVM, 0, 0, 0 },
    { "maxss", 0xF3, 1, 0x5F, F_RVM, S_128, 0, 0 }, { "maxsd", 0xF2, 1, 0x5F, F_RVM, S_128, 0, 0 },
    { "sqrtps", 0, 1, 0x51, F_RM, 0, 0, 0 }, { "sqrtpd", 0x66, 1, 0x51, F_RM, 0, 0, 0 },
    { "sqrtss", 0xF3, 1, 0x51, F_RVM, S_128, 0, 0 }, { "sqrtsd", 0xF2, 1, 0x51, F_RVM, S_128, 0, 0 },
    { "rcpps", 0, 1, 0x53, F_RM, 0, 0, 0 }, { "rcpss", 0xF3, 1, 0x53, F_RVM, S_128, 0, 0 },
    { "rsqrtps", 0, 1, 0x52, F_RM, 0, 0, 0 }, { "rsqrtss", 0xF3, 1, 0x52, F_RVM, S_128, 0, 0 },
    { "andps", 0, 1, 0x54, F_RVM, S_COMM, 0, 0 }, { "andpd", 0x66, 1, 0x54, F_RVM, S_COMM, 0, 0 },
    { "andnps", 0, 1, 0x55, F_RVM, 0, 0, 0 }, { "andnpd", 0x66, 1, 0x55, F_RVM, 0, 0, 0 },
    { "orps", 0, 1, 0x56, F_RVM, S_COMM, 0, 0 }, { "orpd", 0x66, 1, 0x56, F_RVM, S_COMM, 0, 0 },
    { "xorps", 0, 1, 0x57, F_RVM, S_COMM, 0, 0 }, { "xorpd", 0x66, 1, 0x57, F_RVM, S_COMM, 0, 0 },
    { "unpcklps", 0, 1, 0x14, F_RVM, 0, 0, 0 }, { "unpcklpd", 0x66, 1, 0x14, F_RVM, 0, 0, 0 },
    { "unpckhps", 0, 1, 0x15, F_RVM, 0, 0, 0 }, { "unpckhpd", 0x66, 1, 0x15, F_RVM, 0, 0, 0 },
    { "shufps", 0, 1, 0xC6, F_RVM, S_IMM, 0, 0 }, { "shufpd", 0x66, 1, 0xC6, F_RVM, S_IMM, 0, 0 },
    { "cmpps", 0, 1, 0xC2, F_RVM, S_IMM | S_CMP, 0, 0 }, { "cmppd", 0x66, 1, 0xC2, F_RVM, S_IMM | S_CMP, 0, 0 },
    { "cmpss", 0xF3, 1, 0xC2, F_RVM, S_IMM | S_128, 0, 0 },
    { "cmpsd", 0xF2, 1, 0xC2, F_RVM, S_IMM | S_128, 0, 0 },
    { "movmskps", 0, 1, 0x50, F_RM, S_GREG | S_REG, 0, 0 }, { "movmskpd", 0x66, 1, 0x50, F_RM, S_GREG | S_REG, 0, 0 },
    { "haddps", 0xF2, 1, 0x7C, F_RVM, 0, 0, 0 }, { "haddpd", 0x66, 1, 0x7C, F_RVM, 0, 0, 0 },
    { "hsubps", 0xF2, 1, 0x7D, F_RVM, 0, 0, 0 }, { "hsubpd", 0x66, 1, 0x7D, F_RVM, 0, 0, 0 },
    { "addsubps", 0xF2, 1, 0xD0, F_RVM, 0, 0, 0 }, { "addsubpd", 0x66, 1, 0xD0, F_RVM, 0, 0, 0 },
    { "cvtdq2ps", 0, 1, 0x5B, F_RM, 0, 0, 0 }, { "cvtps2dq", 0x66, 1, 0x5B, F_RM, 0, 0, 0 },
    { "cvttps2dq", 0xF3, 1, 0x5B, F_RM, 0, 0, 0 }, { "cvtps2pd", 0, 1, 0x5A, F_RM, S_HALF, 0, 0 },
    { "cvtdq2pd", 0xF3, 1, 0xE6, F_RM, S_HALF, 0, 0 },
    { "vcvtss2sd", 0xF3, 1, 0x5A, F_RVM, S_VEX | S_128, 0, 0 }, { "vcvtsd2ss", 0xF2, 1, 0x5A, F_RVM, S_VEX | S_128, 0, 0 },
    { "ucomiss", 0, 1, 0x2E, F_RM, S_128, 0, 0 }, { "ucomisd", 0x66, 1, 0x2E, F_RM, S_128, 0, 0 },
    { "comiss", 0, 1, 0x2F, F_RM, S_128, 0, 0 }, { "comisd", 0x66, 1, 0x2F, F_RM, S_128, 0, 0 },
    /* only AVX */
    { "vbroadcastss", 0x66, 2, 0x18, F_RM, S_VEX | S_HALF, 0, 0 },
    { "vbroadcastsd", 0x66, 2, 0x19, F_RM, S_VEX | S_HALF | S_256, 0, 0 },
    { "vbroadcastf128", 0x66, 2, 0x1A, F_RM, S_VEX | S_MEM | S_256, 0, 0 },
    { "vbroadcasti128", 0x66, 2, 0x5A, F_RM, S_VEX | S_MEM | S_256, 0, 0 },
    { "vpbroadcastb", 0x66, 2, 0x78, F_RM, S_VEX | S_HALF, 0, 0 },
    { "vpbroadcastw", 0x66, 2, 0x79, F_RM, S_VEX | S_HALF, 0, 0 },
    { "vpbroadcastd", 0x66, 2, 0x58, F_RM, S_VEX | S_HALF, 0, 0 },
    { "vpbroadcastq", 0x66, 2, 0x59, F_RM, S_VEX | S_HALF, 0, 0 },
    { "vinsertf128", 0x66, 3, 0x18, F_RVM, S_VEX | S_IMM | S_HALF | S_256, 0, 0 },
    { "vinserti128", 0x66, 3, 0x38, F_RVM, S_VEX | S_IMM | S_HALF | S_256, 0, 0 },
    { "vextractf128", 0x66, 3, 0x19, F_MR, S_VEX | S_IMM | S_HALF | S_256, 0, 0 },
    { "vextracti128", 0x66, 3, 0x39, F_MR, S_VEX | S_IMM | S_HALF | S_256, 0, 0 },
    { "vperm2f128", 0x66, 3, 0x06, F_RVM, S_VEX | S_IMM | S_256, 0, 0 },
    { "vperm2i128", 0x66, 3, 0x46, F_RVM, S_VEX | S_IMM | S_256, 0, 0 },
    { "vpermq", 0x66, 3, 0x00, F_RM, S_VEX | S_IMM | S_W1 | S_256, 0, 0 },
    { "vpermpd", 0x66, 3, 0x01, F_RM, S_VEX | S_IMM | S_W1 | S_256, 0, 0 },
    { "vpermd", 0x66, 2, 0x36, F_RVM, S_VEX | S_256, 0, 0 },
    { "vpermps", 0x66, 2, 0x16, F_RVM, S_VEX | S_256, 0, 0 },
    { "vpermilps", 0x66, 2, 0x0C, F_RVM, S_VEX, 0, 0 }, { "vpermilps", 0x66, 3, 0x04, F_RM, S_VEX | S_IMM, 0, 0 },
    { "vpermilpd", 0x66, 2, 0x0D, F_RVM, S_VEX, 0, 0 }, { "vpermilpd", 0x66, 3, 0x05, F_RM, S_VEX | S_IMM, 0, 0 },
    { "vpblendd", 0x66, 3, 0x02, F_RVM, S_VEX | S_IMM, 0, 0 },
    { "vpblendvb", 0x66, 3, 0x4C, F_RVMR, S_VEX, 0, 0 }, { "vblendvps", 0x66, 3, 0x4A, F_RVMR, S_VEX, 0, 0 },
    { "vblendvpd", 0x66, 3, 0x4B, F_RVMR, S_VEX, 0, 0 },
    { "vpsllvd", 0x66, 2, 0x47, F_RVM, S_VEX, 0, 0 }, { "vpsllvq", 0x66, 2, 0x47, F_RVM, S_VEX | S_W1, 0, 0 },
    { "vpsrlvd", 0x66, 2, 0x45, F_RVM, S_VEX, 0, 0 }, { "vpsrlvq", 0x66, 2, 0x45, F_RVM, S_VEX | S_W1, 0, 0 },
    { "vpsravd", 0x66, 2, 0x46, F_RVM, S_VEX, 0, 0 },
    { "vpmaskmovd", 0x66, 2, 0x8C, F_RVM, S_VEX | S_MEM, 0, 0 },
    { "vpmaskmovd", 0x66, 2, 0x8E, F_MVR, S_VEX | S_MEM, 0, 0 },
    { "vpmaskmovq", 0x66, 2, 0x8C, F_RVM, S_VEX | S_MEM | S_W1, 0, 0 },
    { "vpmaskmovq", 0x66, 2, 0x8E, F_MVR, S_VEX | S_MEM | S_W1, 0, 0 },
    { "vmaskmovps", 0x66, 2, 0x2C, F_RVM, S_VEX | S_MEM, 0, 0 },
    { "vmaskmovps", 0x66, 2, 0x2E, F_MVR, S_VEX | S_MEM, 0, 0 },
    { "vmaskmovpd", 0x66, 2, 0x2D, F_RVM, S_VEX | S_MEM, 0, 0 },
    { "vmaskmovpd", 0x66, 2, 0x2F, F_MVR, S_VEX | S_MEM, 0, 0 },
    { "vtestps", 0x66, 2, 0x0E, F_RM, S_VEX, 0, 0 }, { "vtestpd", 0x66, 2, 0x0F, F_RM, S_VEX, 0, 0 },
    { "vcvtpd2ps", 0x66, 1, 0x5A, F_RM, S_VEX | S_REG, 0, 0 }, { "vcvtpd2dq", 0xF2, 1, 0xE6, F_RM, S_VEX | S_REG, 0, 0 },
    { "vcvttpd2dq", 0x66, 1, 0xE6, F_RM, S_VEX | S_REG, 0, 0 },
};

/* The compare predicates' names (cmpeqps is cmpps $0): SSE has the first
 * 8, AVX all 32. */
static const char *cmp_names[32] = {
    "eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord",
    "eq_uq", "nge", "ngt", "false", "neq_oq", "ge", "gt", "true",
    "eq_os", "lt_oq", "le_oq", "unord_s", "neq_us", "nlt_uq", "nle_uq", "ord_s",
    "eq_us", "nge_uq", "ngt_uq", "false_os", "neq_os", "ge_oq", "gt_oq", "true_us",
};

static bool is_vec(const struct operand *o)
{
    return o->kind == OP_REG && o->rc == RC_XMM;
}

static bool is_gpr(const struct operand *o)
{
    return o->kind == OP_REG && o->rc == RC_GPR && o->reg < 16 && !o->high8;
}

/* VEX prefix, opcode, ModRM...: reg and vvvv are register numbers (vvvv
 * 0 when unused: stored inverted as 1111). */
static void vex_encode(int pp, int map, bool w, bool l, int reg, int vvvv, struct operand *rm, int op,
                       int imm_size, struct as_expr *imm, int is4)
{
    if (rm->kind == OP_MEM && rm->segment)
        emit_byte(rm->segment);
    bool x = rm->kind == OP_MEM && rm->index >= 8;
    bool b = rm->kind == OP_MEM ? rm->base >= 8 : rm->reg >= 8;
    int ppbits = pp == 0x66 ? 1 : pp == 0xF3 ? 2 : pp == 0xF2 ? 3 : 0;
    int tail = (~vvvv & 15) << 3 | (l ? 4 : 0) | ppbits;
    if (map == 1 && !w && !x && !b) {
        emit_byte(0xC5);
        emit_byte((reg >= 8 ? 0 : 0x80) | tail);
    } else {
        emit_byte(0xC4);
        emit_byte((reg >= 8 ? 0 : 0x80) | (x ? 0 : 0x40) | (b ? 0 : 0x20) | map);
        emit_byte((w ? 0x80 : 0) | tail);
    }
    emit_byte(op);
    modrm(reg, rm, is4 >= 0 ? 1 : imm_size, K_X86_PC32);
    if (is4 >= 0)
        emit_byte(is4 << 4);
    else if (imm_size)
        immediate(imm, imm_size);
}

/* The SSE form: [prefix] [REX] 0F [38|3A] op ModRM [imm]. */
static void sse_encode(int pp, int map, bool w, int reg, struct operand *rm, int op, int imm_size, struct as_expr *imm)
{
    struct enc e = { 0 };
    e.prefix = pp;
    e.w = w;
    e.op[0] = 0x0F;
    if (map == 1) {
        e.nop = 2;
        e.op[1] = op;
    } else {
        e.nop = 3;
        e.op[1] = map == 2 ? 0x38 : 0x3A;
        e.op[2] = op;
    }
    e.r = reg;
    e.rm = rm;
    e.imm_size = imm_size;
    e.imm = imm;
    e.force_rex = rm->kind == OP_REG && rm->rex8;
    encode(&e);
}

/* Whether the entry takes these operands, and how: the roles found. */
static bool simd_fits(const struct simd *t, bool vex, struct operand *ops, int n, struct operand **reg,
                      struct operand **vvvv, struct operand **rm, struct operand **reg4, struct operand **imm)
{
    *reg = *vvvv = *rm = *reg4 = *imm = 0;
    int k = 0;
    if (t->flags & S_IMM) {
        if (n < 1 || ops[0].kind != OP_IMM)
            return false;
        *imm = &ops[k++];
    } else if (n >= 1 && ops[0].kind == OP_IMM)
        return false;
    int left = n - k;
    struct operand *o = ops + k;
    switch (t->form) {
    case F_RVM:
        if (vex && left == 3)
            *rm = &o[0], *vvvv = &o[1], *reg = &o[2];
        else if (!vex && left == 2)
            *rm = &o[0], *reg = &o[1];
        else
            return false;
        break;
    case F_RM:
        if (left != 2)
            return false;
        *rm = &o[0], *reg = &o[1];
        break;
    case F_MR:
        if (left != 2)
            return false;
        *reg = &o[0], *rm = &o[1];
        break;
    case F_VMI:
        if (vex && left == 2)
            *rm = &o[0], *vvvv = &o[1];
        else if (!vex && left == 1)
            *rm = &o[0];
        else
            return false;
        break;
    case F_RVMR:
        if (!vex || left != 4)
            return false;
        *reg4 = &o[0], *rm = &o[1], *vvvv = &o[2], *reg = &o[3];
        break;
    case F_MVR:
        if (!vex || left != 3)
            return false;
        *reg = &o[0], *vvvv = &o[1], *rm = &o[2];
        break;
    }
    if (*reg && !((t->flags & S_GREG) ? is_gpr(*reg) : is_vec(*reg)))
        return false;
    if (*vvvv && !is_vec(*vvvv))
        return false;
    if (*reg4 && !is_vec(*reg4))
        return false;
    if ((*rm)->kind == OP_IMM)
        return false;
    if ((*rm)->kind == OP_REG) {
        if (t->flags & S_MEM)
            return false;
        if (!((t->flags & S_GPR) ? is_gpr(*rm) : is_vec(*rm)))
            return false;
    } else if (t->flags & S_REG)
        return false;
    return true;
}

static bool simd_instruction(const char *m, struct operand *ops, int n)
{
    bool vex = m[0] == 'v';
    struct operand pred;
    /* the compares' predicate names: cmpeqps, vcmpneq_oqpd ... */
    {
        const char *p = m + (vex ? 1 : 0);
        size_t len = strlen(p);
        if (strncmp(p, "cmp", 3) == 0 && len > 5) {
            const char *type = p + len - 2;
            if (strcmp(type, "ps") == 0 || strcmp(type, "pd") == 0 || strcmp(type, "ss") == 0 ||
                strcmp(type, "sd") == 0) {
                for (int k = 0; k < (vex ? 32 : 8); k++)
                    if (strlen(cmp_names[k]) == len - 5 && strncmp(p + 3, cmp_names[k], len - 5) == 0) {
                        char base[8], text[8];
                        snprintf(base, sizeof(base), "%scmp%s", vex ? "v" : "", type);
                        snprintf(text, sizeof(text), "$%d", k);
                        parse_operand(text, &pred);
                        struct operand all[4];
                        if (n == 4)
                            as_error("too many operands");
                        all[0] = pred;
                        for (int i = 0; i < n; i++)
                            all[i + 1] = ops[i];
                        return simd_instruction(base, all, n + 1);
                    }
            }
        }
    }
    /* FMA: vf[n]m(add|sub)(132|213|231)(ps|pd|ss|sd), vfm(addsub|subadd)...(ps|pd) */
    if (strncmp(m, "vf", 2) == 0) {
        static const struct {
            const char *name;
            int op;
            bool packed_only;
        } fma[] = {
            { "fmaddsub", 0x96, true }, { "fmsubadd", 0x97, true }, { "fmadd", 0x98, false },
            { "fmsub", 0x9A, false }, { "fnmadd", 0x9C, false }, { "fnmsub", 0x9E, false },
        };
        for (size_t i = 0; i < sizeof(fma) / sizeof(fma[0]); i++) {
            size_t k = strlen(fma[i].name);
            const char *p = m + 1;
            if (strncmp(p, fma[i].name, k) != 0 || strlen(p + k) != 5)
                continue;
            const char *order = p + k, *type = p + k + 3;
            int add = strncmp(order, "132", 3) == 0 ? 0 : strncmp(order, "213", 3) == 0 ? 0x10
                    : strncmp(order, "231", 3) == 0 ? 0x20 : -1;
            bool scalar = type[0] == 's';
            if (add < 0 || (type[0] != 'p' && type[0] != 's') || (type[1] != 's' && type[1] != 'd') ||
                (scalar && fma[i].packed_only))
                continue;
            if (n != 3 || !is_vec(&ops[1]) || !is_vec(&ops[2]) || ops[0].kind == OP_IMM ||
                (ops[0].kind == OP_REG && !is_vec(&ops[0])))
                as_error("'%s' takes vector registers", m);
            bool l = ops[1].size == 32 || ops[2].size == 32 || (ops[0].kind == OP_REG && ops[0].size == 32);
            if (scalar && l)
                as_error("'%s' has no 256-bit form", m);
            vex_encode(0x66, 2, type[1] == 'd', l, ops[2].reg, ops[1].reg, &ops[0],
                       fma[i].op + add + (scalar ? 1 : 0), 0, 0, -1);
            return true;
        }
    }
    if (vex && (strcmp(m, "vzeroupper") == 0 || strcmp(m, "vzeroall") == 0)) {
        if (n)
            as_error("'%s' takes no operands", m);
        emit_byte(0xC5);
        emit_byte(m[5] == 'u' ? 0xF8 : 0xFC);
        emit_byte(0x77);
        return true;
    }
    if (strcmp(m, "vmovd") == 0 || strcmp(m, "vmovq") == 0) {
        /* between a general register (or memory) and an xmm register, as movd/movq */
        if (n != 2)
            as_error("'%s' takes two operands", m);
        bool w = m[4] == 'q';
        if (is_vec(&ops[0]) && is_vec(&ops[1])) {
            if (ops[0].reg >= 8 && ops[1].reg < 8)      /* (LLVM: the store form, the short prefix) */
                vex_encode(0x66, 1, false, false, ops[0].reg, 0, &ops[1], 0xD6, 0, 0, -1);
            else
                vex_encode(0xF3, 1, false, false, ops[1].reg, 0, &ops[0], 0x7E, 0, 0, -1);
        } else if (is_vec(&ops[1])) {
            if (w && ops[0].kind == OP_MEM)
                vex_encode(0xF3, 1, false, false, ops[1].reg, 0, &ops[0], 0x7E, 0, 0, -1);
            else
                vex_encode(0x66, 1, w, false, ops[1].reg, 0, &ops[0], 0x6E, 0, 0, -1);
        } else if (is_vec(&ops[0])) {
            if (w && ops[1].kind == OP_MEM)
                vex_encode(0x66, 1, false, false, ops[0].reg, 0, &ops[1], 0xD6, 0, 0, -1);
            else
                vex_encode(0x66, 1, w, false, ops[0].reg, 0, &ops[1], 0x7E, 0, 0, -1);
        } else
            as_error("'%s' takes an xmm register", m);
        return true;
    }
    {
        /* (32h) the conversions with an integer side: vcvtsi2ss/sd[l|q] gpr-or-memory,
         * xmm, xmm; vcvt[t]ss2si/sd2si[l|q] xmm-or-memory, gpr */
        static const struct {
            const char *name;
            int pp, op;
            bool to_int;
        } cvt[] = {
            { "vcvtsi2ss", 0xF3, 0x2A, false }, { "vcvtsi2sd", 0xF2, 0x2A, false },
            { "vcvttss2si", 0xF3, 0x2C, true }, { "vcvttsd2si", 0xF2, 0x2C, true },
            { "vcvtss2si", 0xF3, 0x2D, true }, { "vcvtsd2si", 0xF2, 0x2D, true },
        };
        for (size_t k = 0; k < sizeof(cvt) / sizeof(cvt[0]); k++) {
            size_t len = strlen(cvt[k].name);
            if (strncmp(m, cvt[k].name, len) != 0 || (m[len] && ((m[len] != 'l' && m[len] != 'q') || m[len + 1])))
                continue;
            int size = m[len] == 'q' ? 8 : m[len] == 'l' ? 4 : 0;
            if (cvt[k].to_int) {
                if (n != 2 || !is_gpr(&ops[1]) || ops[0].kind == OP_IMM || (ops[0].kind == OP_REG && !is_vec(&ops[0])))
                    as_error("bad operands for '%s'", m);
                if (!size)
                    size = ops[1].size;
                vex_encode(cvt[k].pp, 1, size == 8, false, ops[1].reg, 0, &ops[0], cvt[k].op, 0, 0, -1);
            } else {
                if (n != 3 || !is_vec(&ops[1]) || !is_vec(&ops[2]) || ops[0].kind == OP_IMM ||
                    (ops[0].kind == OP_REG && !is_gpr(&ops[0])))
                    as_error("bad operands for '%s'", m);
                if (!size)
                    size = ops[0].kind == OP_REG ? ops[0].size : 4;
                vex_encode(cvt[k].pp, 1, size == 8, false, ops[2].reg, ops[1].reg, &ops[0], cvt[k].op, 0, 0, -1);
            }
            return true;
        }
    }
    if (strcmp(m, "vmovss") == 0 || strcmp(m, "vmovsd") == 0) {
        int pp = m[5] == 's' ? 0xF3 : 0xF2;
        if (n == 2 && ops[0].kind == OP_MEM && is_vec(&ops[1]))
            vex_encode(pp, 1, false, false, ops[1].reg, 0, &ops[0], 0x10, 0, 0, -1);
        else if (n == 2 && is_vec(&ops[0]) && ops[1].kind == OP_MEM)
            vex_encode(pp, 1, false, false, ops[0].reg, 0, &ops[1], 0x11, 0, 0, -1);
        else if (n == 3 && is_vec(&ops[0]) && is_vec(&ops[1]) && is_vec(&ops[2])) {
            if (ops[0].reg >= 8 && ops[2].reg < 8)      /* (the store form, as LLVM) */
                vex_encode(pp, 1, false, false, ops[0].reg, ops[1].reg, &ops[2], 0x11, 0, 0, -1);
            else
                vex_encode(pp, 1, false, false, ops[2].reg, ops[1].reg, &ops[0], 0x10, 0, 0, -1);
        } else
            as_error("bad operands for '%s'", m);
        return true;
    }
    bool named = false;
    for (size_t i = 0; i < sizeof(simd_table) / sizeof(simd_table[0]); i++) {
        const struct simd *t = &simd_table[i];
        bool as_vex;
        if (t->flags & S_VEX) {
            if (strcmp(m, t->name) != 0)
                continue;
            as_vex = true;
        } else if (vex && !(t->flags & S_SSE) && strcmp(m + 1, t->name) == 0)
            as_vex = true;
        else if (strcmp(m, t->name) == 0)
            as_vex = false;
        else
            continue;
        named = true;
        struct operand *reg, *vvvv, *rm, *reg4, *imm;
        struct operand *o = ops;
        int count = n;
        struct operand rest[4];
        if (!as_vex && (t->flags & S_SSE) && t->map == 2 && n == 3 && is_vec(&ops[0]) && ops[0].reg == 0 &&
            ops[0].size == 16) {
            /* pblendvb %xmm0, src, dst: the mask register named */
            for (int k = 1; k < n; k++)
                rest[k - 1] = ops[k];
            o = rest, count = n - 1;
        }
        bool store = false;
        struct operand swapped;
        if (!simd_fits(t, as_vex, o, count, &reg, &vvvv, &rm, &reg4, &imm)) {
            /* a move to memory: its store form */
            if (!t->store || count != 2 || o[0].kind == OP_IMM || !is_vec(&o[0]) || o[1].kind != OP_MEM)
                continue;
            reg = &o[0], rm = &o[1], vvvv = reg4 = imm = 0;
            store = true;
        }
        bool l = false;
        struct operand *all[4] = { reg, vvvv, rm, reg4 };
        for (int k = 0; k < 4; k++)
            if (all[k] && is_vec(all[k]) && all[k]->size == 32)
                l = true;
        if (!as_vex && l)
            as_error("'%s' has no 256-bit form: v%s", m, m);
        if (l && (t->flags & S_128))
            as_error("'%s' has no 256-bit form", m);
        if (!l && (t->flags & S_256))
            as_error("'%s' takes ymm registers", m);
        bool w = (t->flags & S_W1) != 0;
        int op = store ? t->store : t->op;
        int r = t->form == F_VMI ? t->ext : reg->reg;
        int v = vvvv ? vvvv->reg : 0;
        if (as_vex && t->map == 1 && !w && rm->kind == OP_REG && rm->reg >= 8) {
            /* LLVM's swaps for the 2-byte prefix */
            bool comm = (t->flags & S_COMM) != 0;
            if (t->flags & S_CMP) {
                int64_t k;
                comm = constant(&imm->expr, &k) && ((k & 7) == 0 || (k & 7) == 3 || (k & 7) == 4 || (k & 7) == 7);
            }
            if (comm && vvvv && v < 8) {
                int keep = v;
                v = rm->reg;
                swapped = *rm;
                swapped.reg = keep;
                rm = &swapped;
            } else if (t->store && !store && r < 8) {
                swapped = *rm;
                swapped.reg = r;
                r = rm->reg;
                rm = &swapped;
                op = t->store;
            }
        }
        if (as_vex)
            vex_encode(t->pp, t->map, w, l, r, v, rm, op, imm ? 1 : 0, imm ? &imm->expr : 0,
                       reg4 ? reg4->reg : -1);
        else
            sse_encode(t->pp, t->map, w, r, rm, op, imm ? 1 : 0, imm ? &imm->expr : 0);
        return true;
    }
    if (named)
        as_error("bad operands for '%s'", m);
    return false;
}

/* --- the layout's part --------------------------------------------------- */

static int x86_jump_size(struct as_fragment *f, bool is_long)
{
    if (f->jump_op == -2)
        return 5;
    if (!is_long)
        return 2;
    return f->jump_op == -1 ? 5 : 6;
}

static void x86_write_jump(struct as_fragment *f, unsigned char *out)
{
    if (f->jump_op == -2) {
        out[0] = 0xE8;
        return;
    }
    if (!f->is_long) {
        out[0] = f->jump_op == -1 ? 0xEB : (unsigned char)(0x70 + f->jump_op);
        return;
    }
    if (f->jump_op == -1)
        out[0] = 0xE9;
    else {
        out[0] = 0x0F;
        out[1] = (unsigned char)(0x80 + f->jump_op);
    }
}

static void x86_apply(struct as_fixup *x, struct as_fragment *f, unsigned char *out, int64_t value, bool resolved)
{
    (void)f;
    if (!out)
        return;
    if (resolved && x->size == 1 && (value < -128 || value > 255))
        as_error("a value does not fit in a byte");
    if (resolved && x->size == 4 && x->pcrel && (value < INT32_MIN || value > INT32_MAX))
        as_error("a displacement does not fit in 32 bits");
    for (int i = 0; i < x->size; i++)
        out[x->offset + i] = resolved ? (unsigned char)(value >> (8 * i)) : 0;
}

static bool x86_relocate(struct as_fixup *x, int *type, int64_t *addend)
{
    switch (x->kind) {
    case K_X86_ABS:
        *type = x->size == 8 ? R_X86_64_64 : x->size == 4 ? R_X86_64_32 : x->size == 2 ? R_X86_64_16 : R_X86_64_8;
        *addend = 0;
        return true;
    case K_X86_PC32:
        *type = R_X86_64_PC32;
        break;
    case K_X86_PLT32:
        *type = R_X86_64_PLT32;
        break;
    case K_X86_GOTPCREL:
        *type = R_X86_64_GOTPCRELX;
        break;
    case K_X86_GOTPCREL_REX:
        *type = R_X86_64_REX_GOTPCRELX;
        break;
    case K_X86_TLSGD:
        *type = R_X86_64_TLSGD;
        if (x->expr.sym)
            x->expr.sym->type = STT_TLS;        /* (a per-thread symbol, also undefined here: as LLVM) */
        break;
    default:
        return false;
    }
    /* S + A - P, where P is the field: the PC is pc_base - offset further */
    *addend = -(x->pc_base - x->offset);
    return true;
}

/* LLVM's padding: long NOPs of up to 10 bytes, then the rest. */
static void x86_nops(unsigned char *out, int64_t n)
{
    static const unsigned char nops[10][10] = {
        { 0x90 },
        { 0x66, 0x90 },
        { 0x0F, 0x1F, 0x00 },
        { 0x0F, 0x1F, 0x40, 0x00 },
        { 0x0F, 0x1F, 0x44, 0x00, 0x00 },
        { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 },
        { 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 },
        { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
    };
    while (n > 0) {
        int k = n > 10 ? 10 : (int)n;
        memcpy(out, nops[k - 1], (size_t)k);
        out += k;
        n -= k;
    }
}

struct as_arch arch_x86_64 = {
    .name = "x86_64",
    .machine = 62,                              /* EM_X86_64 */
    .instruction = x86_instruction,
    .jump_size = x86_jump_size,
    .write_jump = x86_write_jump,
    .apply = x86_apply,
    .relocate = x86_relocate,
    .nops = x86_nops,
    .comment = '#',
};
