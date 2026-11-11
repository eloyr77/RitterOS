/*
 * The AArch64 part of as (60e): every instruction is one 32-bit word,
 * built from the Arm Architecture Reference Manual's encodings. Aliases
 * are resolved the way LLVM's assembler does (mov to movz, movn, orr or
 * add; cmp to subs; cset to csinc; lsl #n to ubfm; ldr with an unaligned
 * or negative offset to ldur), so objects compare byte for byte with
 * clang's.
 */
#include "as.h"

enum {
    K_A_ABS = 0, K_A_CALL26, K_A_JUMP26, K_A_COND19, K_A_TBZ14, K_A_ADRP, K_A_ADRP_GOT, K_A_ADD_LO12,
    K_A_LDST8, K_A_LDST16, K_A_LDST32, K_A_LDST64, K_A_LDST128, K_A_GOT_LO12, K_A_PREL32,
    /* (62b) a TLS descriptor's page, its function's load, its address, its call */
    K_A_TLSDESC, K_A_TLSDESC_LO12, K_A_TLSDESC_LD64, K_A_TLSDESC_ADD, K_A_TLSDESC_CALL,
};

#define R_AARCH64_ABS64 257
#define R_AARCH64_ABS32 258
#define R_AARCH64_ABS16 259
#define R_AARCH64_PREL32 261
#define R_AARCH64_ADR_PREL_PG_HI21 275
#define R_AARCH64_ADD_ABS_LO12_NC 277
#define R_AARCH64_LDST8_ABS_LO12_NC 278
#define R_AARCH64_TSTBR14 279
#define R_AARCH64_CONDBR19 280
#define R_AARCH64_JUMP26 282
#define R_AARCH64_CALL26 283
#define R_AARCH64_LDST16_ABS_LO12_NC 284
#define R_AARCH64_LDST32_ABS_LO12_NC 285
#define R_AARCH64_LDST64_ABS_LO12_NC 286
#define R_AARCH64_LDST128_ABS_LO12_NC 299
#define R_AARCH64_ADR_GOT_PAGE 311
#define R_AARCH64_LD64_GOT_LO12_NC 312
#define R_AARCH64_TLSDESC_ADR_PAGE21 562
#define R_AARCH64_TLSDESC_LD64_LO12 563
#define R_AARCH64_TLSDESC_ADD_LO12 564
#define R_AARCH64_TLSDESC_CALL 569

enum { A_NONE, A_REG, A_FPREG, A_IMM, A_MEM, A_SHIFT, A_COND, A_SYM, A_VREG, A_VLIST };

struct aop {
    int kind;
    int reg;                                    /* 0-31 (31: sp or zr, see is_sp) */
    bool w;                                     /* a w register (32 bits) */
    bool is_sp;
    char fp;                                    /* b h s d q */
    int64_t imm;
    struct as_expr expr;                        /* an immediate with a symbol, or a branch target */
    bool has_sym;
    int modifier;                               /* K_A_... of :lo12: and the like */
    /* memory */
    int base;
    bool base_sp;
    int mode;                                   /* 0 [xn, #imm], 1 [xn, #imm]!, 2 post-index (imm in the next operand) */
    int index;                                  /* -1: none */
    bool index_w;
    bool index_signed;                          /* sxtw (else uxtw for a w index) */
    int ext_shift;
    /* shifts */
    int shift_type;                             /* 0 lsl 1 lsr 2 asr 3 ror */
    int shift_amount;
    /* (32f) vector registers: v<reg>.<arrangement> (q: 128 bits; esz: b h s d
     * as 0-3), v<reg>.<esz>[lane], or a list {v<reg>.<arr>, ...} of nregs */
    bool q;
    int esz, lane, nregs;
};

static uint32_t word;

static void emit32(uint32_t w)
{
    emit_le(w, 4);
}

/* --- operands ------------------------------------------------------------ */

static const char *conds[16] = { "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                                 "hi", "ls", "ge", "lt", "gt", "le", "al", "nv" };

static int cond_of(const char *s)
{
    for (int i = 0; i < 16; i++)
        if (strcmp(s, conds[i]) == 0)
            return i;
    if (strcmp(s, "cs") == 0)
        return 2;
    if (strcmp(s, "cc") == 0)
        return 3;
    return -1;
}

static bool reg_name(const char *s, struct aop *o)
{
    memset(o, 0, sizeof(*o));
    o->index = -1;
    if (strcmp(s, "sp") == 0 || strcmp(s, "wsp") == 0) {
        o->kind = A_REG;
        o->reg = 31;
        o->is_sp = true;
        o->w = s[0] == 'w';
        return true;
    }
    if (strcmp(s, "xzr") == 0 || strcmp(s, "wzr") == 0) {
        o->kind = A_REG;
        o->reg = 31;
        o->w = s[0] == 'w';
        return true;
    }
    if (strcmp(s, "lr") == 0) {
        o->kind = A_REG;
        o->reg = 30;
        return true;
    }
    if (strcmp(s, "fp") == 0) {
        o->kind = A_REG;
        o->reg = 29;
        return true;
    }
    if ((s[0] == 'x' || s[0] == 'w') && s[1] >= '0' && s[1] <= '9') {
        char *end;
        long n = strtol(s + 1, &end, 10);
        if (*end || n > 30)
            return false;
        o->kind = A_REG;
        o->reg = (int)n;
        o->w = s[0] == 'w';
        return true;
    }
    if (strchr("bhsdqv", s[0]) && s[1] >= '0' && s[1] <= '9') {
        char *end;
        long n = strtol(s + 1, &end, 10);
        if (*end || n > 31)
            return false;
        o->kind = A_FPREG;
        o->reg = (int)n;
        o->fp = s[0];
        return true;
    }
    return false;
}

static int modifier_of(const char **pp)
{
    const char *p = *pp;
    static const struct {
        const char *name;
        int kind;
    } mods[] = {
        { ":lo12:", K_A_ADD_LO12 }, { ":got:", K_A_ADRP_GOT }, { ":got_lo12:", K_A_GOT_LO12 },
        { ":tlsdesc:", K_A_TLSDESC }, { ":tlsdesc_lo12:", K_A_TLSDESC_LO12 },
    };
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++)
        if (strncmp(p, mods[i].name, strlen(mods[i].name)) == 0) {
            *pp = p + strlen(mods[i].name);
            return mods[i].kind;
        }
    return 0;
}

static void parse_imm(const char *text, struct aop *o)
{
    const char *p = skip_space(text);
    if (*p == '#')
        p++;
    o->kind = A_IMM;
    o->modifier = modifier_of(&p);
    if (!parse_expr(&p, &o->expr))
        as_error("bad immediate '%s'", text);
    if (*skip_space(p))
        as_error("junk after '%s'", text);
    o->has_sym = !expr_constant(&o->expr, &o->imm);
}

/* (32f) v12.16b, v3.s[1] (also v3.4s[1]): false if not one */
static bool vreg_name(const char *s, struct aop *o)
{
    if ((s[0] != 'v' && s[0] != 'V') || s[1] < '0' || s[1] > '9')
        return false;
    char *end;
    long n = strtol(s + 1, &end, 10);
    if (n > 31 || *end != '.')
        return false;
    const char *a = end + 1;
    long count = 0;
    if (*a >= '0' && *a <= '9')
        count = strtol(a, (char **)&a, 10);
    static const char sizes[] = "bhsd";
    const char *z = *a ? strchr(sizes, *a | 0x20) : 0;
    if (!z)
        return false;
    memset(o, 0, sizeof(*o));
    o->kind = A_VREG;
    o->reg = (int)n;
    o->index = -1;
    o->esz = (int)(z - sizes);
    o->lane = -1;
    o->nregs = 1;
    a++;
    if (count) {
        int bits = (int)count * (8 << o->esz);
        if (bits != 64 && bits != 128)
            return false;
        o->q = bits == 128;
    }
    if (*a == '[') {
        o->lane = (int)strtol(a + 1, (char **)&a, 10);
        if (*a != ']')
            return false;
        a++;
    } else if (!count)
        return false;
    return *skip_space(a) == 0;
}

/* {v0.16b, v1.16b} or {v0.16b-v3.16b}: consecutive registers (past v31 to v0) */
static void parse_vlist(char *p, struct aop *o, const char *text)
{
    char *close = strchr(p, '}');
    if (!close || *skip_space(close + 1))
        as_error("bad register list '%s'", text);
    *close = 0;
    struct aop first = { 0 }, r;
    int count = 0, last = -1;
    char *q = p + 1;
    bool range = strchr(q, '-') != 0;
    while (*skip_space(q)) {
        char *sep = q + strcspn(q, ",-");
        char was = *sep;
        *sep = 0;
        char *name = (char *)skip_space(q);
        char *t = name + strlen(name);
        while (t > name && (t[-1] == ' ' || t[-1] == '\t'))
            *--t = 0;
        if (!vreg_name(name, &r) || r.lane >= 0)
            as_error("bad register list '%s'", text);
        if (!count)
            first = r;
        else if (r.q != first.q || r.esz != first.esz || (!range && r.reg != (last + 1) % 32))
            as_error("the registers of a list are consecutive, of one arrangement: '%s'", text);
        if (range && count)
            count = (r.reg - first.reg + 32) % 32 + 1;
        else
            count++;
        last = r.reg;
        if (!was)
            break;
        q = sep + 1;
    }
    if (count < 1 || count > 4)
        as_error("a list of 1 to 4 registers: '%s'", text);
    *o = first;
    o->kind = A_VLIST;
    o->nregs = count;
}

static void parse_aop(char *text, struct aop *o)
{
    char *p = (char *)skip_space(text);
    char *e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    memset(o, 0, sizeof(*o));
    o->index = -1;
    if (*p == '{') {
        parse_vlist(p, o, text);
        return;
    }
    if (vreg_name(p, o))
        return;
    if (*p == '[') {
        o->kind = A_MEM;
        char *close = strchr(p, ']');
        if (!close)
            as_error("expected ']' in '%s'", text);
        if (close[1] == '!')
            o->mode = 1;
        *close = 0;
        char *inner = p + 1;
        char *comma = strchr(inner, ',');
        if (comma)
            *comma = 0;
        struct aop r;
        if (!reg_name((char *)skip_space(inner), &r) || r.kind != A_REG || r.w)
            as_error("bad base register in '%s'", text);
        o->base = r.reg;
        o->base_sp = r.is_sp;
        if (comma) {
            char *rest = (char *)skip_space(comma + 1);
            struct aop idx;
            char *c2 = strchr(rest, ',');
            if (c2)
                *c2 = 0;
            char *t = rest + strlen(rest);
            while (t > rest && (t[-1] == ' ' || t[-1] == '\t'))
                *--t = 0;
            if (reg_name(rest, &idx) && idx.kind == A_REG) {
                o->index = idx.reg;
                o->index_w = idx.w;
                if (c2) {
                    const char *s = skip_space(c2 + 1);
                    if (strncmp(s, "lsl", 3) == 0 || strncmp(s, "sxtw", 4) == 0 || strncmp(s, "uxtw", 4) == 0) {
                        o->index_signed = s[0] == 's';
                        const char *h = strchr(s, '#');
                        o->ext_shift = h ? atoi(h + 1) : 0;
                    }
                }
            } else {
                if (c2)
                    as_error("bad memory operand '%s'", text);
                struct aop im;
                parse_imm(rest, &im);
                o->imm = im.imm;
                o->expr = im.expr;
                o->has_sym = im.has_sym;
                o->modifier = im.modifier;
            }
        }
        return;
    }
    if (strncmp(p, "lsl ", 4) == 0 || strncmp(p, "lsr ", 4) == 0 || strncmp(p, "asr ", 4) == 0 ||
        strncmp(p, "ror ", 4) == 0) {
        o->kind = A_SHIFT;
        o->shift_type = p[0] == 'l' ? (p[2] == 'l' ? 0 : 1) : p[0] == 'a' ? 2 : 3;
        const char *h = skip_space(p + 3);
        if (*h == '#')
            h++;
        o->shift_amount = (int)strtol(h, 0, 0);
        return;
    }
    if (reg_name(p, o))
        return;
    if (*p == '#' || *p == ':' || (*p >= '0' && *p <= '9') || *p == '-') {
        parse_imm(p, o);
        return;
    }
    /* a label */
    o->kind = A_SYM;
    const char *q = p;
    if (!parse_expr(&q, &o->expr) || *skip_space(q))
        as_error("bad operand '%s'", text);
}

/* A condition operand (cset, csel, ...): a name like eq, read as a label
 * by parse_aop (a variable may be called vs). */
static int cond_operand(struct aop *o)
{
    int c = o->kind == A_SYM && o->expr.sym && !o->expr.addend ? cond_of(o->expr.sym->name) : -1;
    if (c < 0)
        as_error("expected a condition");
    return c;
}

static int split(char *s, struct aop *ops)
{
    int n = 0, depth = 0;
    char *start = s;
    if (!*skip_space(s))
        return 0;
    for (char *p = s;; p++) {
        if (*p == '[' || *p == '{')
            depth++;
        else if (*p == ']' || *p == '}')
            depth--;
        if (!*p || (*p == ',' && depth == 0)) {
            char end = *p;
            *p = 0;
            if (n == 5)
                as_error("too many operands");
            parse_aop(start, &ops[n++]);
            if (!end)
                break;
            start = p + 1;
        }
    }
    return n;
}

/* --- immediates ---------------------------------------------------------- */

/* The logical immediate's N:immr:imms for value v of width bits, or -1. */
static int bitmask_imm(uint64_t v, int width)
{
    if (width == 32)
        v = (v & 0xFFFFFFFFull) | (v << 32);
    if (v == 0 || v == ~0ull)
        return -1;
    /* the smallest element size it repeats with */
    int e = 64;
    while (e > 2) {
        int half = e / 2;
        uint64_t mask = (1ull << half) - 1;
        if ((v & mask) != ((v >> half) & mask))
            break;
        e = half;
    }
    uint64_t emask = e == 64 ? ~0ull : (1ull << e) - 1;
    uint64_t elem = v & emask;
    /* a rotated run of ones: rotate it right until it starts at bit 0 */
    int ones = __builtin_popcountll(elem);
    for (int r = 0; r < e; r++) {
        uint64_t rot = e == 64 ? (elem >> r) | (elem << ((64 - r) & 63)) : ((elem >> r) | (elem << (e - r))) & emask;
        if (r == 0)
            rot = elem;
        uint64_t run = ones == 64 ? ~0ull : (1ull << ones) - 1;
        if (rot == run) {
            /* elem = ROR(run, immr), so immr = (e - r) % e */
            int immr = (e - r) % e;
            int imms = ((~(e * 2 - 1)) & 0x3F) | (ones - 1);
            int n = e == 64 ? 1 : 0;
            if (width == 32 && n)
                return -1;
            return (n << 12) | (immr << 6) | (imms & 0x3F);
        }
    }
    return -1;
}

static void check_range(int64_t v, int64_t lo, int64_t hi, const char *what)
{
    if (v < lo || v > hi)
        as_error("%s out of range (%lld)", what, (long long)v);
}

/* --- instructions -------------------------------------------------------- */

static uint32_t sf(struct aop *o)
{
    return o->w ? 0 : 1u << 31;
}

static void need_regs(int n, int want, const char *m)
{
    if (n != want)
        as_error("'%s' takes %d operands", m, want);
}

/* add/sub (and their flag-setting forms) with any second operand */
static void add_sub(bool sub, bool flags, struct aop *d, struct aop *n, struct aop *m, struct aop *shift)
{
    uint32_t base = sf(d) | (sub ? 1u << 30 : 0) | (flags ? 1u << 29 : 0);
    if (m->kind == A_IMM) {
        int64_t v = m->imm;
        int sh = 0;
        if (m->has_sym || m->modifier) {
            if (m->modifier != K_A_ADD_LO12 && m->modifier != K_A_TLSDESC_LO12)
                as_error("only :lo12: symbols in add");
            emit_fixup(&m->expr, m->modifier == K_A_TLSDESC_LO12 ? K_A_TLSDESC_ADD : K_A_ADD_LO12, 4, false, 0);
            v = 0;
        } else if (shift && shift->kind == A_SHIFT && shift->shift_type == 0 && shift->shift_amount == 12) {
            sh = 1;
        } else if (v < 0 && v > -4096) {
            /* add #-n is sub #n */
            v = -v;
            base ^= 1u << 30;
        } else if (v > 4095 && (v & 0xFFF) == 0 && v <= 0xFFF000) {
            v >>= 12;
            sh = 1;
        }
        check_range(v, 0, 4095, "immediate");
        emit32(base | (0x22u << 23) | ((uint32_t)sh << 22) | ((uint32_t)v << 10) | ((uint32_t)n->reg << 5) | (uint32_t)d->reg);
        return;
    }
    if (m->kind != A_REG)
        as_error("bad operand of add/sub");
    if (d->is_sp || n->is_sp) {
        /* the extended-register form: UXTX (or UXTW for w registers) */
        int option = d->w ? 2 : 3;
        int amount = shift && shift->kind == A_SHIFT ? shift->shift_amount : 0;
        emit32(base | (0x59u << 21) | ((uint32_t)m->reg << 16) | ((uint32_t)option << 13) | ((uint32_t)amount << 10) |
               ((uint32_t)n->reg << 5) | (uint32_t)d->reg);
        return;
    }
    int type = 0, amount = 0;
    if (shift && shift->kind == A_SHIFT) {
        type = shift->shift_type;
        amount = shift->shift_amount;
    }
    emit32(base | (0x0Bu << 24) | ((uint32_t)type << 22) | ((uint32_t)m->reg << 16) | ((uint32_t)amount << 10) |
           ((uint32_t)n->reg << 5) | (uint32_t)d->reg);
}

/* and, orr, eor, ands (opc 0-3); invert: bic, orn, eon, bics */
static void logical(int opc, bool invert, struct aop *d, struct aop *n, struct aop *m, struct aop *shift)
{
    if (m->kind == A_IMM) {
        int enc = bitmask_imm((uint64_t)m->imm, d->w ? 32 : 64);
        if (invert || enc < 0)
            as_error("immediate 0x%llx cannot be encoded", (unsigned long long)m->imm);
        emit32(sf(d) | ((uint32_t)opc << 29) | (0x24u << 23) | ((uint32_t)enc << 10) | ((uint32_t)n->reg << 5) | (uint32_t)d->reg);
        return;
    }
    int type = 0, amount = 0;
    if (shift && shift->kind == A_SHIFT) {
        type = shift->shift_type;
        amount = shift->shift_amount;
    }
    emit32(sf(d) | ((uint32_t)opc << 29) | (0x0Au << 24) | ((uint32_t)type << 22) | (invert ? 1u << 21 : 0) |
           ((uint32_t)m->reg << 16) | ((uint32_t)amount << 10) | ((uint32_t)n->reg << 5) | (uint32_t)d->reg);
}

static void bitfield(int opc, struct aop *d, struct aop *n, int immr, int imms)
{
    uint32_t nbit = d->w ? 0 : 1u << 22;
    emit32(sf(d) | ((uint32_t)opc << 29) | (0x26u << 23) | nbit | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) |
           ((uint32_t)n->reg << 5) | (uint32_t)d->reg);
}

static void move_wide(int opc, struct aop *d, int64_t imm, int shift)
{
    if (shift % 16 || shift > (d->w ? 16 : 48))
        as_error("bad shift of a move");
    check_range(imm, 0, 65535, "immediate");
    emit32(sf(d) | ((uint32_t)opc << 29) | (0x25u << 23) | ((uint32_t)(shift / 16) << 21) | ((uint32_t)imm << 5) |
           (uint32_t)d->reg);
}

static void mov_imm(struct aop *d, int64_t v)
{
    uint64_t u = d->w ? (uint64_t)v & 0xFFFFFFFFull : (uint64_t)v;
    int width = d->w ? 32 : 64;
    for (int s = 0; s < width; s += 16)
        if ((u & ~(0xFFFFull << s)) == 0) {
            move_wide(2, d, (int64_t)((u >> s) & 0xFFFF), s);
            return;
        }
    uint64_t n = d->w ? ~u & 0xFFFFFFFFull : ~u;
    for (int s = 0; s < width; s += 16)
        if ((n & ~(0xFFFFull << s)) == 0) {
            move_wide(0, d, (int64_t)((n >> s) & 0xFFFF), s);
            return;
        }
    int enc = bitmask_imm(u, width);
    if (enc < 0)
        as_error("mov of 0x%llx cannot be encoded", (unsigned long long)u);
    struct aop zr = { .kind = A_REG, .reg = 31, .w = d->w };
    struct aop m = { .kind = A_IMM, .imm = (int64_t)u };
    logical(1, false, d, &zr, &m, 0);
}

/* loads and stores: the unsigned-offset form with the scaled immediate */
struct ldst {
    const char *name;
    uint32_t unsigned_base;                     /* the [xn, #uimm] form */
    int scale;
};

static void load_store(uint32_t ui_base, int scale, struct aop *t, struct aop *mem, struct aop *post, int lo12_kind)
{
    uint32_t rt = (uint32_t)t->reg, rn = (uint32_t)mem->base;
    /* the unscaled, pre- and post-index forms: bits 24 clear, 11:10 */
    uint32_t imm9_base = (ui_base & ~(1u << 24)) & ~(0xFFFu << 10);
    if (post) {
        if (post->kind != A_IMM)
            as_error("bad post-index");
        check_range(post->imm, -256, 255, "offset");
        emit32(imm9_base | (((uint32_t)post->imm & 0x1FF) << 12) | (1u << 10) | (rn << 5) | rt);
        return;
    }
    if (mem->index >= 0) {
        /* register offset: option UXTW (010), LSL (011) or SXTW (110) */
        uint32_t option = !mem->index_w ? 3 : mem->index_signed ? 6 : 2;
        uint32_t s = mem->ext_shift ? 1 : 0;
        emit32(imm9_base | (1u << 21) | ((uint32_t)mem->index << 16) | (option << 13) | (s << 12) | (2u << 10) | (rn << 5) | rt);
        return;
    }
    if (mem->mode == 1) {
        check_range(mem->imm, -256, 255, "offset");
        emit32(imm9_base | (((uint32_t)mem->imm & 0x1FF) << 12) | (3u << 10) | (rn << 5) | rt);
        return;
    }
    if (mem->has_sym || mem->modifier) {
        int kind = mem->modifier == K_A_GOT_LO12 ? K_A_GOT_LO12
                   : mem->modifier == K_A_TLSDESC_LO12 ? K_A_TLSDESC_LD64 : lo12_kind;
        emit_fixup(&mem->expr, kind, 4, false, 0);
        emit32(ui_base | (rn << 5) | rt);
        return;
    }
    int64_t off = mem->imm;
    if (off >= 0 && off % scale == 0 && off / scale < 4096) {
        emit32(ui_base | ((uint32_t)(off / scale) << 10) | (rn << 5) | rt);
        return;
    }
    check_range(off, -256, 255, "offset");
    emit32(imm9_base | (((uint32_t)off & 0x1FF) << 12) | (rn << 5) | rt);
}

static int lo12_kind_of(int scale)
{
    switch (scale) {
    case 1: return K_A_LDST8;
    case 2: return K_A_LDST16;
    case 4: return K_A_LDST32;
    case 8: return K_A_LDST64;
    default: return K_A_LDST128;
    }
}

static bool ldst_instruction(const char *m, struct aop *ops, int n)
{
    bool load = m[0] == 'l';
    if (!(strncmp(m, "ldr", 3) == 0 || strncmp(m, "str", 3) == 0 || strncmp(m, "ldur", 4) == 0 ||
          strncmp(m, "stur", 4) == 0))
        return false;
    const char *s = m + 3;
    if (m[2] == 'u')
        s = m + 4;                              /* ldur/stur: the same, the offset decides */
    if (n < 2 || ops[1].kind != A_MEM)
        return false;
    struct aop *t = &ops[0], *mem = &ops[1], *post = n == 3 ? &ops[2] : 0;
    uint32_t base;
    int scale;
    if (t->kind == A_FPREG) {
        if (*s)
            return false;
        switch (t->fp) {
        case 'b': base = load ? 0x3D400000 : 0x3D000000; scale = 1; break;
        case 'h': base = load ? 0x7D400000 : 0x7D000000; scale = 2; break;
        case 's': base = load ? 0xBD400000 : 0xBD000000; scale = 4; break;
        case 'd': base = load ? 0xFD400000 : 0xFD000000; scale = 8; break;
        default: base = load ? 0x3DC00000 : 0x3D800000; scale = 16; break;
        }
    } else if (!*s) {
        base = t->w ? (load ? 0xB9400000 : 0xB9000000) : (load ? 0xF9400000 : 0xF9000000);
        scale = t->w ? 4 : 8;
    } else if (strcmp(s, "b") == 0) {
        base = load ? 0x39400000 : 0x39000000;
        scale = 1;
    } else if (strcmp(s, "h") == 0) {
        base = load ? 0x79400000 : 0x79000000;
        scale = 2;
    } else if (load && strcmp(s, "sb") == 0) {
        base = t->w ? 0x39C00000 : 0x39800000;
        scale = 1;
    } else if (load && strcmp(s, "sh") == 0) {
        base = t->w ? 0x79C00000 : 0x79800000;
        scale = 2;
    } else if (load && strcmp(s, "sw") == 0) {
        base = 0xB9800000;
        scale = 4;
    } else
        return false;
    load_store(base, scale, t, mem, post, lo12_kind_of(scale));
    return true;
}

static void pair(bool load, struct aop *t1, struct aop *t2, struct aop *mem, struct aop *post)
{
    int scale = t1->kind == A_FPREG ? (t1->fp == 's' ? 4 : t1->fp == 'd' ? 8 : 16) : t1->w ? 4 : 8;
    uint32_t opc = t1->kind == A_FPREG ? (t1->fp == 's' ? 0 : t1->fp == 'd' ? 1 : 2) : t1->w ? 0 : 2;
    uint32_t v = t1->kind == A_FPREG ? 1 : 0;
    uint32_t mode;                              /* 01 post, 11 pre, 10 offset */
    int64_t off;
    if (post) {
        mode = 1;
        off = post->imm;
    } else if (mem->mode == 1) {
        mode = 3;
        off = mem->imm;
    } else {
        mode = 2;
        off = mem->imm;
    }
    if (off % scale)
        as_error("misaligned pair offset");
    off /= scale;
    check_range(off, -64, 63, "pair offset");
    emit32((opc << 30) | (0x5u << 27) | (v << 26) | (mode << 23) | (load ? 1u << 22 : 0) | (((uint32_t)off & 0x7F) << 15) |
           ((uint32_t)t2->reg << 10) | ((uint32_t)mem->base << 5) | (uint32_t)t1->reg);
}

static int fp_type(char c)
{
    return c == 's' ? 0 : c == 'd' ? 1 : c == 'h' ? 3 : -1;
}

static void branch(uint32_t op, int kind, struct aop *target)
{
    if (target->kind != A_SYM && target->kind != A_IMM)
        as_error("bad branch target");
    emit_fixup(&target->expr, kind, 4, true, 0);
    emit32(op);
}

static void neon_instruction(const char *m, struct aop *ops, int n);

static void a64_instruction(char *m, char *args)
{
    if (strcmp(m, ".tlsdesccall") == 0) {
        /* (62b) marks the blr that follows: a relocation, no bytes */
        struct as_expr e;
        const char *p = args;
        if (!parse_expr(&p, &e) || !e.sym)
            as_error("bad .tlsdesccall");
        emit_fixup(&e, K_A_TLSDESC_CALL, 4, false, 0);
        return;
    }
    if (strcmp(m, "tlbi") == 0 || strcmp(m, "ic") == 0 || strcmp(m, "dc") == 0) {
        /* (62e) aliases of SYS #op1, Cn, Cm, #op2 {, Xt}: the operation's name, then perhaps a register */
        static const struct {
            const char *m, *op;
            int op1, crn, crm, op2;
            bool reg;
        } ops[] = {
            { "tlbi", "vmalle1", 0, 8, 7, 0, false }, { "tlbi", "vmalle1is", 0, 8, 3, 0, false },
            { "tlbi", "vae1", 0, 8, 7, 1, true }, { "tlbi", "vae1is", 0, 8, 3, 1, true },
            { "tlbi", "aside1", 0, 8, 7, 2, true }, { "tlbi", "vaae1", 0, 8, 7, 3, true },
            { "tlbi", "vale1", 0, 8, 7, 5, true }, { "tlbi", "vaale1", 0, 8, 7, 7, true },
            { "ic", "iallu", 0, 7, 5, 0, false }, { "ic", "ialluis", 0, 7, 1, 0, false },
            { "ic", "ivau", 3, 7, 5, 1, true },
            { "dc", "ivac", 0, 7, 6, 1, true }, { "dc", "isw", 0, 7, 6, 2, true }, { "dc", "csw", 0, 7, 10, 2, true },
            { "dc", "cisw", 0, 7, 14, 2, true }, { "dc", "zva", 3, 7, 4, 1, true }, { "dc", "cvac", 3, 7, 10, 1, true },
            { "dc", "cvau", 3, 7, 11, 1, true }, { "dc", "civac", 3, 7, 14, 1, true },
        };
        const char *p = skip_space(args), *start = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))
            p++;
        size_t len = (size_t)(p - start);
        for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
            if (strcmp(m, ops[i].m) == 0 && strlen(ops[i].op) == len && strncmp(start, ops[i].op, len) == 0) {
                int rt = 31;
                p = skip_space(p);
                if (ops[i].reg) {
                    if (*p != ',')
                        as_error("'%s %s' needs a register", m, ops[i].op);
                    p = skip_space(p + 1);
                    if (*p != 'x' && *p != 'X')
                        as_error("'%s %s' needs an x register", m, ops[i].op);
                    rt = (int)strtol(p + 1, (char **)&p, 10);
                    if (rt > 30)
                        as_error("bad register in '%s'", args);
                    p = skip_space(p);
                }
                if (*p)
                    as_error("junk after '%s %s'", m, ops[i].op);
                emit32(0xD5080000 | (uint32_t)ops[i].op1 << 16 | (uint32_t)ops[i].crn << 12 |
                       (uint32_t)ops[i].crm << 8 | (uint32_t)ops[i].op2 << 5 | (uint32_t)rt);
                return;
            }
        as_error("unknown operation '%s %s'", m, args);
    }
    struct aop ops[5];
    char *copy = as_strdup(args, strlen(args));
    int n = split(copy, ops);
    (void)word;
    /* (32f) Advanced SIMD: any vector operand (or movi, also on a d register) */
    {
        bool vector = strcmp(m, "movi") == 0 || strcmp(m, "mvni") == 0;
        for (int i = 0; i < n; i++)
            vector |= ops[i].kind == A_VREG || ops[i].kind == A_VLIST;
        if (vector) {
            neon_instruction(m, ops, n);
            return;
        }
    }
    /* without operands */
    static const struct {
        const char *name;
        uint32_t w;
    } plain[] = {
        { "ret", 0xD65F03C0 }, { "nop", 0xD503201F }, { "clrex", 0xD503305F }, { "eret", 0xD69F03E0 },
        { "wfi", 0xD503207F }, { "isb", 0xD5033FDF },
        /* (62e) the kernel's hints */
        { "wfe", 0xD503205F }, { "sev", 0xD503209F }, { "sevl", 0xD50320BF }, { "yield", 0xD503203F },
    };
    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++)
        if (strcmp(m, plain[i].name) == 0 && n == 0) {
            emit32(plain[i].w);
            return;
        }
    if (strcmp(m, "ret") == 0 && n == 1) {
        emit32(0xD65F0000 | ((uint32_t)ops[0].reg << 5));
        return;
    }
    if (strcmp(m, "dmb") == 0 || strcmp(m, "dsb") == 0) {
        need_regs(n, 1, m);
        char *opt = (char *)skip_space(args);
        static const struct {
            const char *name;
            int crm;
        } opts[] = { { "sy", 15 }, { "ish", 11 }, { "ishld", 9 }, { "ishst", 10 }, { "ld", 13 }, { "st", 14 } };
        int crm = -1;
        for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); i++)
            if (strcmp(opt, opts[i].name) == 0)
                crm = opts[i].crm;
        if (crm < 0)
            as_error("unknown barrier option '%s'", opt);
        emit32((m[1] == 'm' ? 0xD50330BF : 0xD503309F) | ((uint32_t)crm << 8));
        return;
    }
    if (strcmp(m, "udf") == 0) {
        need_regs(n, 1, m);
        emit32((uint32_t)(ops[0].imm & 0xFFFF));
        return;
    }
    if (strcmp(m, "brk") == 0 || strcmp(m, "svc") == 0 || strcmp(m, "hvc") == 0 || strcmp(m, "smc") == 0) {
        need_regs(n, 1, m);
        uint32_t base = m[0] == 'b' ? 0xD4200000 : strcmp(m, "svc") == 0 ? 0xD4000001
                      : m[0] == 'h' ? 0xD4000002 : 0xD4000003;
        emit32(base | ((uint32_t)(ops[0].imm & 0xFFFF) << 5));
        return;
    }
    /* branches */
    if (strcmp(m, "b") == 0) {
        need_regs(n, 1, m);
        branch(0x14000000, K_A_JUMP26, &ops[0]);
        return;
    }
    if (strcmp(m, "bl") == 0) {
        need_regs(n, 1, m);
        branch(0x94000000, K_A_CALL26, &ops[0]);
        return;
    }
    if (strncmp(m, "b.", 2) == 0) {
        int c = cond_of(m + 2);
        if (c < 0)
            as_error("unknown condition '%s'", m);
        need_regs(n, 1, m);
        branch(0x54000000 | (uint32_t)c, K_A_COND19, &ops[0]);
        return;
    }
    if (strcmp(m, "cbz") == 0 || strcmp(m, "cbnz") == 0) {
        need_regs(n, 2, m);
        branch((m[2] == 'n' ? 0x35000000 : 0x34000000) | sf(&ops[0]) | (uint32_t)ops[0].reg, K_A_COND19, &ops[1]);
        return;
    }
    if (strcmp(m, "tbz") == 0 || strcmp(m, "tbnz") == 0) {
        need_regs(n, 3, m);
        uint32_t bit = (uint32_t)ops[1].imm;
        branch((m[2] == 'n' ? 0x37000000 : 0x36000000) | ((bit >> 5) << 31) | ((bit & 31) << 19) | (uint32_t)ops[0].reg,
               K_A_TBZ14, &ops[2]);
        return;
    }
    if (strcmp(m, "br") == 0 || strcmp(m, "blr") == 0) {
        need_regs(n, 1, m);
        emit32((m[1] == 'r' ? 0xD61F0000 : 0xD63F0000) | ((uint32_t)ops[0].reg << 5));
        return;
    }
    if (strcmp(m, "adrp") == 0 || strcmp(m, "adr") == 0) {
        need_regs(n, 2, m);
        if (m[3] == 'p') {
            int kind = ops[1].modifier == K_A_ADRP_GOT ? K_A_ADRP_GOT
                       : ops[1].modifier == K_A_TLSDESC ? K_A_TLSDESC : K_A_ADRP;
            emit_fixup(&ops[1].expr, kind, 4, false, 0);
            emit32(0x90000000 | (uint32_t)ops[0].reg);
        } else {
            emit_fixup(&ops[1].expr, K_A_PREL32, 4, true, 0);
            emit32(0x10000000 | (uint32_t)ops[0].reg);
        }
        return;
    }
    /* arithmetic */
    if (strcmp(m, "add") == 0 || strcmp(m, "sub") == 0 || strcmp(m, "adds") == 0 || strcmp(m, "subs") == 0) {
        if (n < 3)
            as_error("'%s' takes 3 operands", m);
        add_sub(m[0] == 's', m[3] == 's', &ops[0], &ops[1], &ops[2], n > 3 ? &ops[3] : 0);
        return;
    }
    if (strcmp(m, "cmp") == 0 || strcmp(m, "cmn") == 0) {
        if (n < 2)
            as_error("'%s' takes 2 operands", m);
        struct aop zr = { .kind = A_REG, .reg = 31, .w = ops[0].w };
        add_sub(m[2] == 'p', true, &zr, &ops[0], &ops[1], n > 2 ? &ops[2] : 0);
        return;
    }
    if (strcmp(m, "neg") == 0 || strcmp(m, "negs") == 0) {
        struct aop zr = { .kind = A_REG, .reg = 31, .w = ops[0].w };
        add_sub(true, m[3] == 's', &ops[0], &zr, &ops[1], n > 2 ? &ops[2] : 0);
        return;
    }
    if (strcmp(m, "and") == 0 || strcmp(m, "orr") == 0 || strcmp(m, "eor") == 0 || strcmp(m, "ands") == 0 ||
        strcmp(m, "bic") == 0 || strcmp(m, "orn") == 0 || strcmp(m, "eon") == 0 || strcmp(m, "bics") == 0) {
        if (n < 3)
            as_error("'%s' takes 3 operands", m);
        int opc = m[0] == 'a' ? (m[3] == 's' ? 3 : 0) : m[0] == 'o' ? 1 : m[0] == 'e' ? 2 : (m[3] == 's' ? 3 : 0);
        logical(opc, m[0] == 'b' || strcmp(m, "orn") == 0 || strcmp(m, "eon") == 0, &ops[0], &ops[1], &ops[2],
                n > 3 ? &ops[3] : 0);
        return;
    }
    if (strcmp(m, "tst") == 0) {
        struct aop zr = { .kind = A_REG, .reg = 31, .w = ops[0].w };
        logical(3, false, &zr, &ops[0], &ops[1], n > 2 ? &ops[2] : 0);
        return;
    }
    if (strcmp(m, "mvn") == 0) {
        struct aop zr = { .kind = A_REG, .reg = 31, .w = ops[0].w };
        logical(1, true, &ops[0], &zr, &ops[1], n > 2 ? &ops[2] : 0);
        return;
    }
    if (strcmp(m, "mov") == 0) {
        need_regs(n, 2, m);
        if (ops[1].kind == A_IMM) {
            mov_imm(&ops[0], ops[1].imm);
            return;
        }
        if (ops[0].is_sp || ops[1].is_sp) {
            struct aop zero = { .kind = A_IMM, .imm = 0 };
            add_sub(false, false, &ops[0], &ops[1], &zero, 0);
            return;
        }
        struct aop zr = { .kind = A_REG, .reg = 31, .w = ops[0].w };
        logical(1, false, &ops[0], &zr, &ops[1], 0);
        return;
    }
    if (strcmp(m, "movz") == 0 || strcmp(m, "movn") == 0 || strcmp(m, "movk") == 0) {
        int shift = n > 2 && ops[2].kind == A_SHIFT ? ops[2].shift_amount : 0;
        move_wide(m[3] == 'n' ? 0 : m[3] == 'z' ? 2 : 3, &ops[0], ops[1].imm, shift);
        return;
    }
    if (strcmp(m, "mul") == 0 || strcmp(m, "madd") == 0 || strcmp(m, "msub") == 0 || strcmp(m, "mneg") == 0) {
        int ra = strcmp(m, "mul") == 0 || strcmp(m, "mneg") == 0 ? 31 : ops[3].reg;
        bool o0 = strcmp(m, "msub") == 0 || strcmp(m, "mneg") == 0;
        emit32(sf(&ops[0]) | (0xD8u << 21) | ((uint32_t)ops[2].reg << 16) | (o0 ? 1u << 15 : 0) | ((uint32_t)ra << 10) |
               ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "umulh") == 0 || strcmp(m, "smulh") == 0) {
        /* (62a) the high half of a 64 x 64 product */
        need_regs(n, 3, m);
        emit32((m[0] == 'u' ? 0x9BC07C00u : 0x9B407C00u) | ((uint32_t)ops[2].reg << 16) | ((uint32_t)ops[1].reg << 5) |
               (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "sdiv") == 0 || strcmp(m, "udiv") == 0) {
        need_regs(n, 3, m);
        emit32(sf(&ops[0]) | (m[0] == 's' ? 0x1AC00C00u : 0x1AC00800u) | ((uint32_t)ops[2].reg << 16) |
               ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "lsl") == 0 || strcmp(m, "lsr") == 0 || strcmp(m, "asr") == 0 || strcmp(m, "ror") == 0) {
        need_regs(n, 3, m);
        int width = ops[0].w ? 32 : 64;
        if (ops[2].kind == A_IMM) {
            int s = (int)ops[2].imm;
            check_range(s, 0, width - 1, "shift");
            if (m[0] == 'l' && m[2] == 'l')
                bitfield(2, &ops[0], &ops[1], (width - s) % width, width - 1 - s);
            else if (m[0] == 'l')
                bitfield(2, &ops[0], &ops[1], s, width - 1);
            else if (m[0] == 'a')
                bitfield(0, &ops[0], &ops[1], s, width - 1);
            else {
                /* ror #s is extr rd, rn, rn, #s */
                emit32(sf(&ops[0]) | 0x13800000u | (ops[0].w ? 0 : 1u << 22) | ((uint32_t)ops[1].reg << 16) |
                       ((uint32_t)s << 10) | ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
            }
            return;
        }
        int op2 = m[0] == 'l' ? (m[2] == 'l' ? 0 : 1) : m[0] == 'a' ? 2 : 3;
        emit32(sf(&ops[0]) | 0x1AC02000u | ((uint32_t)ops[2].reg << 16) | ((uint32_t)op2 << 10) |
               ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "sxtb") == 0 || strcmp(m, "sxth") == 0 || strcmp(m, "sxtw") == 0 || strcmp(m, "uxtb") == 0 ||
        strcmp(m, "uxth") == 0) {
        need_regs(n, 2, m);
        int imms = m[3] == 'b' ? 7 : m[3] == 'h' ? 15 : 31;
        bitfield(m[0] == 's' ? 0 : 2, &ops[0], &ops[1], 0, imms);
        return;
    }
    if (strcmp(m, "sbfm") == 0 || strcmp(m, "ubfm") == 0 || strcmp(m, "bfm") == 0) {
        need_regs(n, 4, m);
        bitfield(m[0] == 's' ? 0 : m[0] == 'u' ? 2 : 1, &ops[0], &ops[1], (int)ops[2].imm, (int)ops[3].imm);
        return;
    }
    if (strcmp(m, "ubfx") == 0 || strcmp(m, "sbfx") == 0) {
        need_regs(n, 4, m);
        int lsb = (int)ops[2].imm, width = (int)ops[3].imm;
        bitfield(m[0] == 's' ? 0 : 2, &ops[0], &ops[1], lsb, lsb + width - 1);
        return;
    }
    if (strcmp(m, "cset") == 0 || strcmp(m, "csetm") == 0) {
        need_regs(n, 2, m);
        uint32_t c = (uint32_t)cond_operand(&ops[1]) ^ 1;  /* (the inverted condition) */
        emit32(sf(&ops[0]) | (m[4] == 'm' ? 0x5A800000u : 0x1A800400u) | (31u << 16) | (c << 12) | (31u << 5) |
               (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "csel") == 0 || strcmp(m, "csinc") == 0 || strcmp(m, "csinv") == 0 || strcmp(m, "csneg") == 0) {
        need_regs(n, 4, m);
        uint32_t base = strcmp(m, "csel") == 0 ? 0x1A800000u : strcmp(m, "csinc") == 0 ? 0x1A800400u
                        : strcmp(m, "csinv") == 0 ? 0x5A800000u : 0x5A800400u;
        emit32(sf(&ops[0]) | base | ((uint32_t)ops[2].reg << 16) | ((uint32_t)cond_operand(&ops[3]) << 12) | ((uint32_t)ops[1].reg << 5) |
               (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "clz") == 0 || strcmp(m, "rbit") == 0 || strcmp(m, "rev") == 0 || strcmp(m, "rev16") == 0 ||
        strcmp(m, "rev32") == 0 || strcmp(m, "cls") == 0) {
        need_regs(n, 2, m);
        uint32_t base;
        if (strcmp(m, "clz") == 0)
            base = 0x5AC01000;
        else if (strcmp(m, "cls") == 0)
            base = 0x5AC01400;
        else if (strcmp(m, "rbit") == 0)
            base = 0x5AC00000;
        else if (strcmp(m, "rev16") == 0)
            base = 0x5AC00400;
        else if (strcmp(m, "rev32") == 0)
            base = 0x5AC00800;
        else
            base = ops[0].w ? 0x5AC00800 : 0x5AC00C00;
        emit32(sf(&ops[0]) | base | ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
        return;
    }
    /* memory */
    if (ldst_instruction(m, ops, n))
        return;
    if (strcmp(m, "ldp") == 0 || strcmp(m, "stp") == 0) {
        if (n < 3 || ops[2].kind != A_MEM)
            as_error("'%s' takes two registers and an address", m);
        pair(m[0] == 'l', &ops[0], &ops[1], &ops[2], n == 4 ? &ops[3] : 0);
        return;
    }
    {
        static const struct {
            const char *name;
            uint32_t base;
            bool status;                        /* a status register first (stlxr) */
        } excl[] = {
            { "ldaxr", 0x885FFC00, false }, { "ldaxrb", 0x085FFC00, false }, { "ldaxrh", 0x485FFC00, false },
            { "ldxr", 0x885F7C00, false }, { "ldar", 0x88DFFC00, false }, { "ldarb", 0x08DFFC00, false },
            { "ldarh", 0x48DFFC00, false }, { "stlr", 0x889FFC00, false }, { "stlrb", 0x089FFC00, false },
            { "stlrh", 0x489FFC00, false }, { "stlxr", 0x8800FC00, true }, { "stlxrb", 0x0800FC00, true },
            { "stlxrh", 0x4800FC00, true }, { "stxr", 0x88007C00, true },
        };
        for (size_t i = 0; i < sizeof(excl) / sizeof(excl[0]); i++)
            if (strcmp(m, excl[i].name) == 0) {
                struct aop *t = excl[i].status ? &ops[1] : &ops[0];
                struct aop *mem = excl[i].status ? &ops[2] : &ops[1];
                uint32_t size_bit = 0;
                size_t len = strlen(m);
                bool sized = m[len - 1] == 'b' || m[len - 1] == 'h';
                if (!sized && !t->w)
                    size_bit = 1u << 30;
                uint32_t rs = excl[i].status ? (uint32_t)ops[0].reg << 16 : 0;
                emit32(excl[i].base | size_bit | rs | ((uint32_t)mem->base << 5) | (uint32_t)t->reg);
                return;
            }
    }
    /* floating point */
    {
        static const struct {
            const char *name;
            uint32_t base;
        } fp3[] = {
            { "fadd", 0x1E202800 }, { "fsub", 0x1E203800 }, { "fmul", 0x1E200800 }, { "fdiv", 0x1E201800 },
            { "fmax", 0x1E204800 }, { "fmin", 0x1E205800 },
        };
        for (size_t i = 0; i < sizeof(fp3) / sizeof(fp3[0]); i++)
            if (strcmp(m, fp3[i].name) == 0) {
                need_regs(n, 3, m);
                emit32(fp3[i].base | ((uint32_t)fp_type(ops[0].fp) << 22) | ((uint32_t)ops[2].reg << 16) |
                       ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
                return;
            }
    }
    if (strcmp(m, "fneg") == 0 || strcmp(m, "fabs") == 0 || strcmp(m, "fsqrt") == 0) {
        need_regs(n, 2, m);
        uint32_t base = m[1] == 'n' ? 0x1E214000 : m[1] == 'a' ? 0x1E20C000 : 0x1E21C000;
        emit32(base | ((uint32_t)fp_type(ops[0].fp) << 22) | ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "fcmp") == 0 || strcmp(m, "fcmpe") == 0) {
        need_regs(n, 2, m);
        uint32_t e = m[4] == 'e' ? 0x10 : 0;
        if (ops[1].kind == A_IMM)
            emit32(0x1E202008 | e | ((uint32_t)fp_type(ops[0].fp) << 22) | ((uint32_t)ops[0].reg << 5));
        else
            emit32(0x1E202000 | e | ((uint32_t)fp_type(ops[0].fp) << 22) | ((uint32_t)ops[1].reg << 16) |
                   ((uint32_t)ops[0].reg << 5));
        return;
    }
    if (strcmp(m, "fcvt") == 0) {
        need_regs(n, 2, m);
        int from = fp_type(ops[1].fp), to = fp_type(ops[0].fp);
        int opc = to == 0 ? 0 : to == 1 ? 1 : 3;
        emit32(0x1E224000 | ((uint32_t)from << 22) | ((uint32_t)opc << 15) | ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "scvtf") == 0 || strcmp(m, "ucvtf") == 0) {
        need_regs(n, 2, m);
        emit32((m[0] == 's' ? 0x1E220000 : 0x1E230000) | sf(&ops[1]) | ((uint32_t)fp_type(ops[0].fp) << 22) |
               ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "fcvtzs") == 0 || strcmp(m, "fcvtzu") == 0) {
        need_regs(n, 2, m);
        emit32((m[5] == 's' ? 0x1E380000 : 0x1E390000) | sf(&ops[0]) | ((uint32_t)fp_type(ops[1].fp) << 22) |
               ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "fmov") == 0) {
        need_regs(n, 2, m);
        if (ops[0].kind == A_FPREG && ops[1].kind == A_FPREG) {
            emit32(0x1E204000 | ((uint32_t)fp_type(ops[0].fp) << 22) | ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
            return;
        }
        if (ops[0].kind == A_FPREG && ops[1].kind == A_REG) {
            uint32_t base = ops[0].fp == 'd' ? 0x9E670000 : 0x1E270000;
            emit32(base | ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
            return;
        }
        if (ops[0].kind == A_REG && ops[1].kind == A_FPREG) {
            uint32_t base = ops[1].fp == 'd' ? 0x9E660000 : 0x1E260000;
            emit32(base | ((uint32_t)ops[1].reg << 5) | (uint32_t)ops[0].reg);
            return;
        }
        as_error("unsupported fmov");
    }
    if (strcmp(m, "mrs") == 0 || strcmp(m, "msr") == 0) {
        /* the system registers a program may use */
        static const struct {
            const char *name;
            uint32_t enc;
        } sysregs[] = {
            { "tpidr_el0", 0x5E82 }, { "fpcr", 0x5A20 }, { "fpsr", 0x5A21 }, { "nzcv", 0x5A10 },
            { "cntvct_el0", 0x5F02 }, { "cntfrq_el0", 0x5F00 },
#define SYSREG(op0, op1, crn, crm, op2) (((op0) - 2) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))
            /* (62d) the kernel's: EL1 control, exceptions, the timer, GICv3's CPU interface */
            { "ttbr0_el1", SYSREG(3, 0, 2, 0, 0) }, { "ttbr1_el1", SYSREG(3, 0, 2, 0, 1) },
            { "tcr_el1", SYSREG(3, 0, 2, 0, 2) }, { "mair_el1", SYSREG(3, 0, 10, 2, 0) },
            { "sctlr_el1", SYSREG(3, 0, 1, 0, 0) }, { "cpacr_el1", SYSREG(3, 0, 1, 0, 2) },
            { "vbar_el1", SYSREG(3, 0, 12, 0, 0) }, { "elr_el1", SYSREG(3, 0, 4, 0, 1) },
            { "spsr_el1", SYSREG(3, 0, 4, 0, 0) }, { "esr_el1", SYSREG(3, 0, 5, 2, 0) },
            { "far_el1", SYSREG(3, 0, 6, 0, 0) }, { "sp_el0", SYSREG(3, 0, 4, 1, 0) },
            { "sp_el1", SYSREG(3, 4, 4, 1, 0) }, { "spsel", SYSREG(3, 0, 4, 2, 0) },
            { "currentel", SYSREG(3, 0, 4, 2, 2) }, { "daif", SYSREG(3, 3, 4, 2, 1) },
            { "mpidr_el1", SYSREG(3, 0, 0, 0, 5) }, { "id_aa64mmfr0_el1", SYSREG(3, 0, 0, 7, 0) },
            { "ctr_el0", SYSREG(3, 3, 0, 0, 1) }, { "cntv_ctl_el0", SYSREG(3, 3, 14, 3, 1) },
            { "cntv_cval_el0", SYSREG(3, 3, 14, 3, 2) }, { "icc_sre_el1", SYSREG(3, 0, 12, 12, 5) },
            { "icc_pmr_el1", SYSREG(3, 0, 4, 6, 0) }, { "icc_igrpen1_el1", SYSREG(3, 0, 12, 12, 7) },
            { "icc_iar1_el1", SYSREG(3, 0, 12, 12, 0) }, { "icc_eoir1_el1", SYSREG(3, 0, 12, 12, 1) },
            { "icc_bpr1_el1", SYSREG(3, 0, 12, 12, 3) },
            /* (65a) each core's block (kernel/percpu.h) */
            { "tpidr_el1", SYSREG(3, 0, 13, 0, 4) },
        };
        char *names = as_strdup(args, strlen(args));
        char *comma = strchr(names, ',');
        if (!comma)
            as_error("'%s' takes two operands", m);
        *comma = 0;
        char *sysname = m[1] == 'r' ? (char *)skip_space(comma + 1) : (char *)skip_space(names);
        char *regname = m[1] == 'r' ? (char *)skip_space(names) : (char *)skip_space(comma + 1);
        char *t = sysname + strlen(sysname);
        while (t > sysname && (t[-1] == ' ' || t[-1] == '\t'))
            *--t = 0;
        t = regname + strlen(regname);
        while (t > regname && (t[-1] == ' ' || t[-1] == '\t'))
            *--t = 0;
        for (char *c = sysname; *c; c++)
            if (*c >= 'A' && *c <= 'Z')
                *c += 32;
        if (m[1] == 's' && *regname == '#' &&
            (strcmp(sysname, "daifset") == 0 || strcmp(sysname, "daifclr") == 0 || strcmp(sysname, "spsel") == 0)) {
            /* (62d) MSR (immediate): a PSTATE field */
            int imm = (int)strtol(regname + 1, 0, 0);     /* (#0xf as #15) */
            uint32_t op = strcmp(sysname, "daifset") == 0 ? 0xD50340DFu
                          : strcmp(sysname, "daifclr") == 0 ? 0xD50340FFu : 0xD50040BFu;
            emit32(op | (uint32_t)(imm & 15) << 8);
            return;
        }
        struct aop r;
        if (!reg_name(regname, &r))
            as_error("bad register in '%s'", m);
        uint32_t enc = 0;
        for (size_t i = 0; i < sizeof(sysregs) / sizeof(sysregs[0]); i++)
            if (strcmp(sysname, sysregs[i].name) == 0)
                enc = sysregs[i].enc;
        if (!enc && sysname[0] == 's') {
            /* (the generic name s<op0>_<op1>_c<n>_c<m>_<op2>) */
            int f[5], k = 0;
            const char *c = sysname + 1;
            while (k < 5 && *c >= '0' && *c <= '9') {
                f[k++] = atoi(c);
                while (*c >= '0' && *c <= '9')
                    c++;
                if (*c == '_')
                    c++;
                if (*c == 'c')
                    c++;
            }
            if (k == 5 && !*c)
                enc = (uint32_t)SYSREG(f[0], f[1], f[2], f[3], f[4]);
        }
#undef SYSREG
        if (!enc)
            as_error("unknown system register '%s'", sysname);
        emit32((m[1] == 'r' ? 0xD5300000u : 0xD5100000u) | (enc << 5) | (uint32_t)r.reg);
        return;
    }
    as_error("unknown instruction '%s %s'", m, args);
}

/* --- Advanced SIMD (NEON, 32f) ------------------------------------------- */

/*
 * The integer vector instructions in their encoding groups (the Arm ARM's
 * C4.1.x): three registers of one arrangement, two-register forms, across
 * lanes, shifts by an immediate, copies (dup, ins, umov, smov), modified
 * immediates (movi, mvni), ext, tbl/tbx, zip/uzp/trn, ld1-ld4/st1-st4 of
 * whole registers and ld1r, plus fmov of a vector's upper half and the mov
 * aliases. Each checked against clang's bytes (neonas_host_test).
 */

static uint32_t vq(const struct aop *o)
{
    return o->q ? 1u << 30 : 0;
}

static void need_vec(const struct aop *o, const char *m)
{
    if (o->kind != A_VREG || o->lane >= 0)
        as_error("'%s' takes vector registers with an arrangement", m);
}

static void same_arrangement(const struct aop *a, const struct aop *b, const char *m)
{
    if (a->q != b->q || a->esz != b->esz)
        as_error("'%s': the registers' arrangements differ", m);
}

/* lane index and element size as imm5 (dup, ins, umov) */
static uint32_t imm5_of(int esz, int lane)
{
    if (lane < 0 || lane >= (16 >> esz))
        as_error("lane %d out of range", lane);
    return (uint32_t)((lane << (esz + 1)) | (1 << esz));
}

static void neon_instruction(const char *m, struct aop *ops, int n)
{
    /* three registers, one arrangement: Q U size Rm opcode */
    static const struct {
        const char *name;
        int u, opcode;
        int sizes;                              /* allowed: bit 0 8b/16b ... bit 3 2d */
    } three[] = {
        { "add", 0, 0x10, 15 }, { "sub", 1, 0x10, 15 }, { "cmeq", 1, 0x11, 15 }, { "cmtst", 0, 0x11, 15 },
        { "cmhi", 1, 0x06, 15 }, { "cmhs", 1, 0x07, 15 }, { "cmgt", 0, 0x06, 15 }, { "cmge", 0, 0x07, 15 },
        { "umax", 1, 0x0C, 7 }, { "umin", 1, 0x0D, 7 }, { "smax", 0, 0x0C, 7 }, { "smin", 0, 0x0D, 7 },
        { "uqadd", 1, 0x01, 15 }, { "sqadd", 0, 0x01, 15 }, { "uqsub", 1, 0x05, 15 }, { "sqsub", 0, 0x05, 15 },
        { "addp", 0, 0x17, 15 }, { "umaxp", 1, 0x14, 7 }, { "uminp", 1, 0x15, 7 }, { "smaxp", 0, 0x14, 7 },
        { "sminp", 0, 0x15, 7 }, { "mul", 0, 0x13, 7 }, { "uhadd", 1, 0x00, 7 }, { "urhadd", 1, 0x02, 7 },
        { "ushl", 1, 0x08, 15 }, { "sshl", 0, 0x08, 15 },
    };
    /* the logical ones: the size field is part of the operation (8b/16b only) */
    static const struct {
        const char *name;
        int u, size;
    } logic[] = {
        { "and", 0, 0 }, { "bic", 0, 1 }, { "orr", 0, 2 }, { "orn", 0, 3 }, { "eor", 1, 0 }, { "bsl", 1, 1 },
        { "bit", 1, 2 }, { "bif", 1, 3 },
    };
    /* two registers: Q U size opcode (zero: a third operand #0) */
    static const struct {
        const char *name;
        int u, opcode, sizes;
        bool zero;
        int fixed_size;                         /* -1: from the arrangement */
    } two[] = {
        { "rev64", 0, 0x00, 7, false, -1 }, { "rev32", 1, 0x00, 3, false, -1 }, { "rev16", 0, 0x01, 1, false, -1 },
        { "cnt", 0, 0x05, 1, false, -1 }, { "not", 1, 0x05, 1, false, 0 }, { "mvn", 1, 0x05, 1, false, 0 },
        { "rbit", 1, 0x05, 1, false, 1 }, { "cls", 0, 0x04, 7, false, -1 }, { "clz", 1, 0x04, 7, false, -1 },
        { "cmgt", 0, 0x08, 15, true, -1 }, { "cmge", 1, 0x08, 15, true, -1 }, { "cmeq", 0, 0x09, 15, true, -1 },
        { "cmle", 1, 0x09, 15, true, -1 }, { "cmlt", 0, 0x0A, 15, true, -1 }, { "abs", 0, 0x0B, 15, false, -1 },
        { "neg", 1, 0x0B, 15, false, -1 },
    };
    /* across lanes: U size opcode, the result a scalar (long: twice the element) */
    static const struct {
        const char *name;
        int u, opcode;
        bool longer;
    } across[] = {
        { "addv", 0, 0x1B, false }, { "umaxv", 1, 0x0A, false }, { "uminv", 1, 0x1A, false },
        { "smaxv", 0, 0x0A, false }, { "sminv", 0, 0x1A, false }, { "uaddlv", 1, 0x03, true },
        { "saddlv", 0, 0x03, true },
    };
    /* shifts by an immediate: U opcode; kind 0 right, 1 left, 2 right narrowing, 3 left lengthening */
    static const struct {
        const char *name;
        int u, opcode, kind;
    } shifts[] = {
        { "shl", 0, 0x0A, 1 }, { "sli", 1, 0x0A, 1 }, { "ushr", 1, 0x00, 0 }, { "sshr", 0, 0x00, 0 },
        { "usra", 1, 0x02, 0 }, { "ssra", 0, 0x02, 0 }, { "sri", 1, 0x08, 0 }, { "urshr", 1, 0x04, 0 },
        { "srshr", 0, 0x04, 0 }, { "shrn", 0, 0x10, 2 }, { "rshrn", 0, 0x11, 2 }, { "shrn2", 0, 0x10, 2 },
        { "rshrn2", 0, 0x11, 2 }, { "ushll", 1, 0x14, 3 }, { "sshll", 0, 0x14, 3 }, { "ushll2", 1, 0x14, 3 },
        { "sshll2", 0, 0x14, 3 },
    };
    for (size_t i = 0; i < sizeof(logic) / sizeof(logic[0]); i++)
        if (strcmp(m, logic[i].name) == 0 && n == 3 && ops[2].kind == A_VREG) {
            for (int k = 0; k < 3; k++)
                need_vec(&ops[k], m);
            same_arrangement(&ops[0], &ops[1], m);
            same_arrangement(&ops[0], &ops[2], m);
            if (ops[0].esz != 0)
                as_error("'%s' takes 8b or 16b", m);
            emit32(0x0E201C00 | vq(&ops[0]) | (uint32_t)logic[i].u << 29 | (uint32_t)logic[i].size << 22 |
                   (uint32_t)ops[2].reg << 16 | (uint32_t)ops[1].reg << 5 | (uint32_t)ops[0].reg);
            return;
        }
    for (size_t i = 0; i < sizeof(three) / sizeof(three[0]); i++)
        if (strcmp(m, three[i].name) == 0 && n == 3 && ops[2].kind == A_VREG) {
            for (int k = 0; k < 3; k++)
                need_vec(&ops[k], m);
            same_arrangement(&ops[0], &ops[1], m);
            same_arrangement(&ops[0], &ops[2], m);
            if (!(three[i].sizes >> ops[0].esz & 1) || (ops[0].esz == 3 && !ops[0].q))
                as_error("'%s' has no such arrangement", m);
            emit32(0x0E200400 | vq(&ops[0]) | (uint32_t)three[i].u << 29 | (uint32_t)ops[0].esz << 22 |
                   (uint32_t)ops[2].reg << 16 | (uint32_t)three[i].opcode << 11 | (uint32_t)ops[1].reg << 5 |
                   (uint32_t)ops[0].reg);
            return;
        }
    for (size_t i = 0; i < sizeof(two) / sizeof(two[0]); i++)
        if (strcmp(m, two[i].name) == 0 && n == (two[i].zero ? 3 : 2)) {
            need_vec(&ops[0], m);
            need_vec(&ops[1], m);
            same_arrangement(&ops[0], &ops[1], m);
            if (two[i].zero && (ops[2].kind != A_IMM || ops[2].imm != 0))
                as_error("'%s' compares with #0", m);
            if (!(two[i].sizes >> ops[0].esz & 1) || (ops[0].esz == 3 && !ops[0].q))
                as_error("'%s' has no such arrangement", m);
            int size = two[i].fixed_size >= 0 ? two[i].fixed_size : ops[0].esz;
            emit32(0x0E200800 | vq(&ops[0]) | (uint32_t)two[i].u << 29 | (uint32_t)size << 22 |
                   (uint32_t)two[i].opcode << 12 | (uint32_t)ops[1].reg << 5 | (uint32_t)ops[0].reg);
            return;
        }
    if ((strcmp(m, "xtn") == 0 || strcmp(m, "xtn2") == 0 || strcmp(m, "uqxtn") == 0 || strcmp(m, "uqxtn2") == 0 ||
         strcmp(m, "sqxtn") == 0 || strcmp(m, "sqxtn2") == 0) && n == 2) {
        /* narrowing: the destination's element half the source's (2: the upper half) */
        need_vec(&ops[0], m);
        need_vec(&ops[1], m);
        bool upper = m[strlen(m) - 1] == '2';
        if (ops[1].esz != ops[0].esz + 1 || !ops[1].q || ops[0].q != upper)
            as_error("'%s': bad arrangements", m);
        int u = m[0] == 'u' ? 1 : 0, opcode = m[0] == 'x' ? 0x12 : 0x14;
        emit32(0x0E200800 | vq(&ops[0]) | (uint32_t)u << 29 | (uint32_t)ops[0].esz << 22 | (uint32_t)opcode << 12 |
               (uint32_t)ops[1].reg << 5 | (uint32_t)ops[0].reg);
        return;
    }
    for (size_t i = 0; i < sizeof(across) / sizeof(across[0]); i++)
        if (strcmp(m, across[i].name) == 0 && n == 2) {
            need_vec(&ops[1], m);
            int want = ops[1].esz + (across[i].longer ? 1 : 0);
            if (ops[0].kind != A_FPREG || ops[0].fp != "bhsd"[want] || ops[1].esz == 3 ||
                (ops[1].esz == 2 && !ops[1].q))
                as_error("'%s': bad registers", m);
            emit32(0x0E300800 | vq(&ops[1]) | (uint32_t)across[i].u << 29 | (uint32_t)ops[1].esz << 22 |
                   (uint32_t)across[i].opcode << 12 | (uint32_t)ops[1].reg << 5 | (uint32_t)ops[0].reg);
            return;
        }
    /* uxtl/sxtl: ushll/sshll by #0 */
    if ((strcmp(m, "uxtl") == 0 || strcmp(m, "sxtl") == 0 || strcmp(m, "uxtl2") == 0 || strcmp(m, "sxtl2") == 0) &&
        n == 2) {
        char name[8];
        snprintf(name, sizeof(name), "%cshll%s", m[0], m[4] == '2' ? "2" : "");
        struct aop all[3] = { ops[0], ops[1] };
        all[2].kind = A_IMM;
        all[2].imm = 0;
        neon_instruction(name, all, 3);
        return;
    }
    for (size_t i = 0; i < sizeof(shifts) / sizeof(shifts[0]); i++)
        if (strcmp(m, shifts[i].name) == 0 && n == 3 && ops[2].kind == A_IMM) {
            need_vec(&ops[0], m);
            need_vec(&ops[1], m);
            int kind = shifts[i].kind, esz;
            bool upper = m[strlen(m) - 1] == '2';
            if (kind == 2) {                    /* shrn: Vd.Tb, Vn.Ta (Ta twice Tb) */
                esz = ops[0].esz;
                if (ops[1].esz != esz + 1 || !ops[1].q || ops[0].q != upper)
                    as_error("'%s': bad arrangements", m);
            } else if (kind == 3) {             /* ushll: Vd.Ta, Vn.Tb */
                esz = ops[1].esz;
                if (ops[0].esz != esz + 1 || !ops[0].q || ops[1].q != upper)
                    as_error("'%s': bad arrangements", m);
            } else {
                same_arrangement(&ops[0], &ops[1], m);
                esz = ops[0].esz;
                if (esz == 3 && !ops[0].q)
                    as_error("'%s' has no 1d form", m);
            }
            int bits = 8 << esz;
            int64_t sh = ops[2].imm;
            int immhb;
            if (kind == 0 || kind == 2) {
                check_range(sh, 1, bits, "shift");
                immhb = 2 * bits - (int)sh;
            } else {
                check_range(sh, 0, bits - 1, "shift");
                immhb = bits + (int)sh;
            }
            bool qbit = kind == 2 || kind == 3 ? upper : ops[0].q;
            emit32(0x0F000400 | (qbit ? 1u << 30 : 0) | (uint32_t)shifts[i].u << 29 | (uint32_t)immhb << 16 |
                   (uint32_t)shifts[i].opcode << 11 | (uint32_t)ops[1].reg << 5 | (uint32_t)ops[0].reg);
            return;
        }
    if (strcmp(m, "dup") == 0 && n == 2) {
        need_vec(&ops[0], m);
        if (ops[0].esz == 3 && !ops[0].q)
            as_error("'dup' has no 1d form");
        if (ops[1].kind == A_REG) {             /* from a general register */
            if (ops[1].w != (ops[0].esz < 3))
                as_error("'dup': a w register for b, h, s, an x register for d");
            emit32(0x0E000C00 | vq(&ops[0]) | imm5_of(ops[0].esz, 0) << 16 | (uint32_t)ops[1].reg << 5 |
                   (uint32_t)ops[0].reg);
            return;
        }
        if (ops[1].kind != A_VREG || ops[1].lane < 0 || ops[1].esz != ops[0].esz)
            as_error("'dup': bad source");
        emit32(0x0E000400 | vq(&ops[0]) | imm5_of(ops[1].esz, ops[1].lane) << 16 | (uint32_t)ops[1].reg << 5 |
               (uint32_t)ops[0].reg);
        return;
    }
    if ((strcmp(m, "ins") == 0 || strcmp(m, "mov") == 0) && n == 2 && ops[0].kind == A_VREG && ops[0].lane >= 0) {
        if (ops[1].kind == A_REG) {             /* a lane from a general register */
            if (ops[1].w != (ops[0].esz < 3))
                as_error("'%s': a w register for b, h, s, an x register for d", m);
            emit32(0x4E001C00 | imm5_of(ops[0].esz, ops[0].lane) << 16 | (uint32_t)ops[1].reg << 5 |
                   (uint32_t)ops[0].reg);
            return;
        }
        if (ops[1].kind != A_VREG || ops[1].lane < 0 || ops[1].esz != ops[0].esz)
            as_error("'%s': bad source", m);
        imm5_of(ops[1].esz, ops[1].lane);
        emit32(0x6E000400 | imm5_of(ops[0].esz, ops[0].lane) << 16 | (uint32_t)(ops[1].lane << ops[1].esz) << 11 |
               (uint32_t)ops[1].reg << 5 | (uint32_t)ops[0].reg);
        return;
    }
    if ((strcmp(m, "umov") == 0 || strcmp(m, "smov") == 0 || strcmp(m, "mov") == 0) && n == 2 &&
        ops[0].kind == A_REG && ops[1].kind == A_VREG && ops[1].lane >= 0) {
        bool s = m[0] == 's';
        int esz = ops[1].esz;
        bool x = !ops[0].w;
        if (m[0] == 'm' && esz < 2)
            as_error("'mov' moves an s or d lane: umov for b and h");
        if (s ? (esz == 3 || (esz == 2 && !x)) : (x != (esz == 3)))
            as_error("'%s': bad registers", m);
        emit32((s ? 0x0E002C00u : 0x0E003C00u) | (x ? 1u << 30 : 0) | imm5_of(esz, ops[1].lane) << 16 |
               (uint32_t)ops[1].reg << 5 | (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "mov") == 0 && n == 2 && ops[1].kind == A_VREG) {
        /* mov Vd.T, Vn.T: orr Vd, Vn, Vn */
        need_vec(&ops[0], m);
        need_vec(&ops[1], m);
        same_arrangement(&ops[0], &ops[1], m);
        if (ops[0].esz != 0)
            as_error("'mov' of whole registers takes 8b or 16b");
        emit32(0x0EA01C00 | vq(&ops[0]) | (uint32_t)ops[1].reg << 16 | (uint32_t)ops[1].reg << 5 |
               (uint32_t)ops[0].reg);
        return;
    }
    if (strcmp(m, "fmov") == 0 && n == 2) {
        /* the upper half of a vector: fmov x0, v1.d[1] / fmov v0.d[1], x1 */
        struct aop *v = ops[0].kind == A_VREG ? &ops[0] : &ops[1], *r = v == &ops[0] ? &ops[1] : &ops[0];
        if (v->kind != A_VREG || v->esz != 3 || v->lane != 1 || r->kind != A_REG || r->w)
            as_error("'fmov' of a vector moves v<n>.d[1] and an x register");
        emit32((v == &ops[0] ? 0x9EAF0000u : 0x9EAE0000u) | (uint32_t)(v == &ops[0] ? r->reg : v->reg) << 5 |
               (uint32_t)(v == &ops[0] ? v->reg : r->reg));
        return;
    }
    if ((strcmp(m, "movi") == 0 || strcmp(m, "mvni") == 0) && (n == 2 || n == 3)) {
        bool invert = m[1] == 'v';
        if (ops[1].kind != A_IMM || ops[1].has_sym)
            as_error("'%s' takes an immediate", m);
        int shift = 0;
        if (n == 3) {
            if (ops[2].kind != A_SHIFT || ops[2].shift_type != 0)
                as_error("'%s': lsl only", m);
            shift = ops[2].shift_amount;
        }
        uint64_t v = (uint64_t)ops[1].imm;
        int op = invert ? 1 : 0, cmode;
        uint32_t q = 0;
        int rd;
        if (ops[0].kind == A_FPREG && ops[0].fp == 'd' && !invert) {
            /* movi d0, #imm: each byte 0 or 0xff */
            op = 1, cmode = 0xE, rd = ops[0].reg;
            goto bytes;
        }
        need_vec(&ops[0], m);
        q = vq(&ops[0]);
        rd = ops[0].reg;
        if (ops[0].esz == 3) {
            if (invert || !ops[0].q)
                as_error("'%s': bad arrangement", m);
            op = 1, cmode = 0xE;
        bytes:
            {
                uint32_t abc = 0;
                for (int k = 0; k < 8; k++) {
                    unsigned b = (unsigned)(v >> (8 * k)) & 0xFF;
                    if (b != 0 && b != 0xFF)
                        as_error("'%s': every byte 0 or 0xff", m);
                    abc |= (b ? 1u : 0) << k;
                }
                if (shift)
                    as_error("'%s': no shift here", m);
                emit32(0x0F000400 | q | (uint32_t)op << 29 | (abc >> 5) << 16 | (uint32_t)cmode << 12 |
                       (abc & 31) << 5 | (uint32_t)rd);
                return;
            }
        }
        if (v > 0xFF)
            as_error("'%s': an 8-bit immediate (with lsl)", m);
        if (ops[0].esz == 0) {
            if (invert || shift)
                as_error("'%s': bad form", m);
            cmode = 0xE;
        } else if (ops[0].esz == 1) {
            if (shift != 0 && shift != 8)
                as_error("'%s': lsl #0 or #8", m);
            cmode = 0x8 | (shift / 8) << 1;
        } else {
            if (shift % 8 || shift > 24)
                as_error("'%s': lsl #0, #8, #16 or #24", m);
            cmode = (shift / 8) << 1;
        }
        emit32(0x0F000400 | q | (uint32_t)op << 29 | (uint32_t)(v >> 5) << 16 | (uint32_t)cmode << 12 |
               (uint32_t)(v & 31) << 5 | (uint32_t)rd);
        return;
    }
    if (strcmp(m, "ext") == 0 && n == 4) {
        for (int k = 0; k < 3; k++)
            need_vec(&ops[k], m);
        same_arrangement(&ops[0], &ops[1], m);
        same_arrangement(&ops[0], &ops[2], m);
        if (ops[0].esz != 0 || ops[3].kind != A_IMM)
            as_error("'ext' takes 8b or 16b and an index");
        check_range(ops[3].imm, 0, ops[0].q ? 15 : 7, "index");
        emit32(0x2E000000 | vq(&ops[0]) | (uint32_t)ops[2].reg << 16 | (uint32_t)ops[3].imm << 11 |
               (uint32_t)ops[1].reg << 5 | (uint32_t)ops[0].reg);
        return;
    }
    if ((strcmp(m, "tbl") == 0 || strcmp(m, "tbx") == 0) && n == 3) {
        need_vec(&ops[0], m);
        need_vec(&ops[2], m);
        if (ops[1].kind != A_VLIST || !ops[1].q || ops[1].esz != 0 || ops[0].esz != 0 || ops[2].esz != 0 ||
            ops[0].q != ops[2].q)
            as_error("'%s' Vd.8b/16b, {16b list}, Vm", m);
        emit32(0x0E000000 | vq(&ops[0]) | (uint32_t)ops[2].reg << 16 | (uint32_t)(ops[1].nregs - 1) << 13 |
               (m[2] == 'x' ? 1u << 12 : 0) | (uint32_t)ops[1].reg << 5 | (uint32_t)ops[0].reg);
        return;
    }
    {
        static const struct {
            const char *name;
            int opc;
        } perm[] = {
            { "uzp1", 1 }, { "trn1", 2 }, { "zip1", 3 }, { "uzp2", 5 }, { "trn2", 6 }, { "zip2", 7 },
        };
        for (size_t i = 0; i < sizeof(perm) / sizeof(perm[0]); i++)
            if (strcmp(m, perm[i].name) == 0 && n == 3) {
                for (int k = 0; k < 3; k++)
                    need_vec(&ops[k], m);
                same_arrangement(&ops[0], &ops[1], m);
                same_arrangement(&ops[0], &ops[2], m);
                if (ops[0].esz == 3 && !ops[0].q)
                    as_error("'%s' has no 1d form", m);
                emit32(0x0E000800 | vq(&ops[0]) | (uint32_t)ops[0].esz << 22 | (uint32_t)ops[2].reg << 16 |
                       (uint32_t)perm[i].opc << 12 | (uint32_t)ops[1].reg << 5 | (uint32_t)ops[0].reg);
                return;
            }
    }
    {
        /* whole registers to and from memory: ld1-ld4, st1-st4 {list}, [xn] (, #imm or xm afterwards) */
        bool load = m[0] == 'l';
        if ((strncmp(m, "ld", 2) == 0 || strncmp(m, "st", 2) == 0) && m[2] >= '1' && m[2] <= '4' && (!m[3] ||
            (m[3] == 'r' && !m[4] && load)) && (n == 2 || n == 3) && ops[0].kind == A_VLIST && ops[1].kind == A_MEM) {
            struct aop *l = &ops[0], *mem = &ops[1];
            int k = m[2] - '0';
            bool replicate = m[3] == 'r';
            if (mem->index >= 0 || mem->imm || mem->mode)
                as_error("'%s': the address is [xn]", m);
            if (l->esz == 3 && !l->q && k > 1 && !replicate)
                as_error("'%s' has no 1d form", m);
            if (replicate ? l->nregs != k : (k == 1 ? false : l->nregs != k))
                as_error("'%s': %d registers", m, k);
            uint32_t w;
            if (replicate)                      /* ld1r-ld4r: R for 2 and 4, opcode 110 or 111 */
                w = 0x0D400000 | vq(l) | (uint32_t)(k == 2 || k == 4) << 21 | (k <= 2 ? 0xC000u : 0xE000u) |
                    (uint32_t)l->esz << 10;
            else {
                static const int opcode[5][5] = { { 0 }, { 0, 0x7, 0xA, 0x6, 0x2 }, { 0, 0, 0x8 }, { 0, 0, 0, 0x4 },
                                                  { 0, 0, 0, 0, 0x0 } };
                w = 0x0C000000 | vq(l) | (uint32_t)opcode[k][l->nregs] << 12 | (uint32_t)l->esz << 10;
            }
            if (load)
                w |= 1u << 22;
            if (n == 3) {
                w |= 1u << 23;
                if (ops[2].kind == A_REG && !ops[2].w && !ops[2].is_sp && ops[2].reg != 31)
                    w |= (uint32_t)ops[2].reg << 16;
                else if (ops[2].kind == A_IMM) {
                    int64_t want = replicate ? (int64_t)k << l->esz : (int64_t)l->nregs * (l->q ? 16 : 8);
                    if (ops[2].imm != want)
                        as_error("'%s': the step after it is #%ld", m, (long)want);
                    w |= 31u << 16;
                } else
                    as_error("'%s': bad step", m);
            }
            emit32(w | (uint32_t)mem->base << 5 | (uint32_t)l->reg);
            return;
        }
    }
    as_error("unknown vector instruction '%s'", m);
}

/* --- the layout's part --------------------------------------------------- */

static int a64_jump_size(struct as_fragment *f, bool is_long)
{
    (void)f;
    (void)is_long;
    return 4;
}

static void a64_write_jump(struct as_fragment *f, unsigned char *out)
{
    (void)f;
    (void)out;
}

static uint32_t read32(unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write32(unsigned char *p, uint32_t w)
{
    p[0] = (unsigned char)w;
    p[1] = (unsigned char)(w >> 8);
    p[2] = (unsigned char)(w >> 16);
    p[3] = (unsigned char)(w >> 24);
}

static void a64_apply(struct as_fixup *x, struct as_fragment *f, unsigned char *out, int64_t value, bool resolved)
{
    (void)f;
    if (!out)
        return;
    unsigned char *p = out + x->offset;
    if (x->kind == K_A_ABS) {
        for (int i = 0; i < x->size; i++)
            p[i] = resolved ? (unsigned char)(value >> (8 * i)) : 0;
        return;
    }
    if (!resolved)
        return;                                 /* (the immediate fields stay zero; RELA has the addend) */
    uint32_t w = read32(p);
    switch (x->kind) {
    case K_A_CALL26: case K_A_JUMP26:
        if (value & 3)
            as_error("misaligned branch target");
        check_range(value, -(1 << 27), (1 << 27) - 4, "branch");
        w |= (uint32_t)(value >> 2) & 0x3FFFFFF;
        break;
    case K_A_COND19:
        if (value & 3)
            as_error("misaligned branch target");
        check_range(value, -(1 << 20), (1 << 20) - 4, "conditional branch");
        w |= ((uint32_t)(value >> 2) & 0x7FFFF) << 5;
        break;
    case K_A_TBZ14:
        check_range(value, -(1 << 15), (1 << 15) - 4, "test branch");
        w |= ((uint32_t)(value >> 2) & 0x3FFF) << 5;
        break;
    case K_A_PREL32:
        /* adr */
        check_range(value, -(1 << 20), (1 << 20) - 1, "adr");
        w |= ((uint32_t)value & 3) << 29 | ((uint32_t)(value >> 2) & 0x7FFFF) << 5;
        break;
    default:
        as_error("a reference that needs a relocation (kind %d, to %s)", x->kind, x->expr.sym ? x->expr.sym->name : "-");
    }
    write32(p, w);
}

static bool a64_relocate(struct as_fixup *x, int *type, int64_t *addend)
{
    *addend = 0;
    if (x->kind >= K_A_TLSDESC && x->kind <= K_A_TLSDESC_CALL && x->expr.sym)
        x->expr.sym->type = STT_TLS;            /* (62b: also undefined ones, as LLVM) */
    switch (x->kind) {
    case K_A_ABS:
        *type = x->size == 8 ? R_AARCH64_ABS64 : x->size == 4 ? R_AARCH64_ABS32 : R_AARCH64_ABS16;
        return true;
    case K_A_CALL26: *type = R_AARCH64_CALL26; return true;
    case K_A_JUMP26: *type = R_AARCH64_JUMP26; return true;
    case K_A_COND19: *type = R_AARCH64_CONDBR19; return true;
    case K_A_TBZ14: *type = R_AARCH64_TSTBR14; return true;
    case K_A_ADRP: *type = R_AARCH64_ADR_PREL_PG_HI21; return true;
    case K_A_ADRP_GOT: *type = R_AARCH64_ADR_GOT_PAGE; return true;
    case K_A_ADD_LO12: *type = R_AARCH64_ADD_ABS_LO12_NC; return true;
    case K_A_LDST8: *type = R_AARCH64_LDST8_ABS_LO12_NC; return true;
    case K_A_LDST16: *type = R_AARCH64_LDST16_ABS_LO12_NC; return true;
    case K_A_LDST32: *type = R_AARCH64_LDST32_ABS_LO12_NC; return true;
    case K_A_LDST64: *type = R_AARCH64_LDST64_ABS_LO12_NC; return true;
    case K_A_LDST128: *type = R_AARCH64_LDST128_ABS_LO12_NC; return true;
    case K_A_GOT_LO12: *type = R_AARCH64_LD64_GOT_LO12_NC; return true;
    case K_A_TLSDESC: *type = R_AARCH64_TLSDESC_ADR_PAGE21; return true;
    case K_A_TLSDESC_LD64: *type = R_AARCH64_TLSDESC_LD64_LO12; return true;
    case K_A_TLSDESC_ADD: *type = R_AARCH64_TLSDESC_ADD_LO12; return true;
    case K_A_TLSDESC_CALL: *type = R_AARCH64_TLSDESC_CALL; return true;
    case K_A_TLSDESC_LO12: return false;
    case K_A_PREL32: return false;
    }
    return false;
}

static void a64_nops(unsigned char *out, int64_t n)
{
    int64_t i = 0;
    for (; i + 4 <= n; i += 4)
        write32(out + i, 0xD503201F);
    for (; i < n; i++)
        out[i] = 0;
}

struct as_arch arch_aarch64 = {
    .name = "aarch64",
    .machine = 183,                             /* EM_AARCH64 */
    .instruction = a64_instruction,
    .jump_size = a64_jump_size,
    .write_jump = a64_write_jump,
    .apply = a64_apply,
    .relocate = a64_relocate,
    .nops = a64_nops,
    .comment = 0,
};
