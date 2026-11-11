/*
 * (37g-5a) AtomBIOS: AMD's video BIOS keeps, besides its data tables, the
 * chip's procedures as command tables in a byte code of its own (setting
 * clocks, an encoder, a transmitter, the panel's power...), which the
 * driver runs. This is amdgpu's interpreter for it, part of the amdgpu
 * module (amdgpu.c includes this file); the same file builds on the Mac
 * for scripts/atomhost.c, which compares it with Linux's (the oracle).
 *
 * The image: a PCI ROM (0x55 0xAA) whose ATOM header (its place at 0x48;
 * "ATOM" at +4) names the master command table (+0x1E) and the master
 * data table (+0x20): a 4-byte header, then a 16-bit place for each
 * table by index (0: none). A command table: its size (16 bits), format
 * and content revisions, the bytes of work space (WS) and of parameter
 * space (PS; its low 7 bits) it uses, then its code from +6.
 *
 * The code: an opcode byte, then its operands. Most opcodes are an
 * operation (move, and, or, shifts, mul, div, add, sub, compare, test,
 * clear, mask, xor, shl, shr) times the kind of its destination: a
 * register (REG), a parameter (PS), the work space (WS), the scratch
 * "frame buffer" (FB), a PLL register, an MC register; an attribute byte
 * then gives the source's kind (bits 2-0: also ID, data in the ROM, and
 * IMM, the code's own bytes) and which part of 32 bits each side uses
 * (bits 5-3 the source's, 7-6 the destination's: a dword, a word at bit
 * 0, 8 or 16, a byte at bit 0, 8, 16 or 24). WS indexes from 0x40 are the
 * interpreter's own registers: the quotient and remainder of mul/div, the
 * data block, the shift, its masks, the FB window, the I/O attributes,
 * the register block. Registers are dword indexes plus the register
 * block; set_port chooses how they are reached: directly (MM) or through
 * an indirect I/O program (IIO, listed in a data table: its own small
 * code of reads, writes and bit moves). Jumps are relative to the table's
 * start, conditional on the last compare or test; switch compares with
 * case values (0x63, the value, the target; 0x5A5A ends); call_table runs
 * another table with the caller's parameters from its PS on; delay waits.
 * repeat, savereg and restorereg are not implemented, as in Linux.
 *
 * Every ROM read is checked against the image, every PS and WS index
 * against its space, and the same jump taken again and again for over
 * ATOM_LOOP_MS ends the table (Linux's rule: its clock is read where
 * Linux reads jiffies, at every jump taken), as do ATOM_STEPS
 * instructions: a wrong table stops with a reason, never the machine.
 *
 * Facts from Linux's atom.c, atom.h and atombios.h (AMD's, MIT licence:
 * the format, the opcode numbers, the operand encodings); the code is
 * ours.
 */
#include "amdgpu_atom.h"

#ifdef ATOM_STEP
void ATOM_STEP(uint32_t place, uint8_t opcode);
#endif

#define ROM_HEADER_PTR  0x48
#define ROM_CMD_PTR     0x1E
#define ROM_DATA_PTR    0x20
#define DATA_IIO        0x32            /* the master data table's IIO entry */
#define CT_WS           4
#define CT_PS           5
#define CT_CODE         6

enum { A_REG, A_PS, A_WS, A_FB, A_ID, A_IMM, A_PLL, A_MC };

#define WS_QUOTIENT     0x40
#define WS_REMAINDER    0x41
#define WS_DATAPTR      0x42
#define WS_SHIFT        0x43
#define WS_OR_MASK      0x44
#define WS_AND_MASK     0x45
#define WS_FB_WINDOW    0x46
#define WS_ATTRIBUTES   0x47
#define WS_REGPTR       0x48

#define IO_MM           0
#define IO_PCI          1
#define IO_SYSIO        2
#define IO_IIO          0x80

#define CASE_MAGIC      0x63
#define CASE_END        0x5A5A

/* Each part of 32 bits (the alignment codes 0-7): its mask and shift. */
static const uint32_t part_mask[8] = { 0xFFFFFFFF, 0xFFFF, 0xFFFF00, 0xFFFF0000,
                                       0xFF, 0xFF00, 0xFF0000, 0xFF000000 };
static const int part_shift[8] = { 0, 0, 8, 16, 0, 8, 16, 24 };
/* A destination's part (bits 7-6) by the source's (bits 5-3): a dword
 * source has only the dword; a word one a word; a byte one a byte. */
static const uint8_t dst_part[8][4] = {
    { 0, 0, 0, 0 }, { 1, 2, 3, 0 }, { 1, 2, 3, 0 }, { 1, 2, 3, 0 },
    { 4, 5, 6, 7 }, { 4, 5, 6, 7 }, { 4, 5, 6, 7 }, { 4, 5, 6, 7 },
};
/* clear and the shifts name only a part: its usual destination part. */
static const uint8_t default_dst[8] = { 0, 0, 1, 2, 0, 1, 2, 3 };

struct run {
    struct atom *a;
    uint32_t *ps;                       /* this table's parameters */
    uint32_t ps_room;                   /* dwords of them */
    uint32_t ws[64];                    /* its work space (at most 256 bytes) */
    uint32_t ws_room;
    uint16_t start;                     /* the table's place: jumps are from here */
    uint32_t last_jump;                 /* where the last jump taken went, and when */
    uint64_t jump_ms;
    int depth;
    bool stop;
};

static void stop(struct run *r, const char *why, uint32_t at)
{
    if (r->stop)
        return;
    r->stop = true;
    struct atom *a = r->a;
    /* (no printf in a module: the reason and its place, by hand) */
    size_t n = 0;
    while (why[n] && n < sizeof(a->why) - 12) {
        a->why[n] = why[n];
        n++;
    }
    static const char hex[] = "0123456789abcdef";
    const char at_[] = " at 0x";
    for (size_t i = 0; at_[i] && n < sizeof(a->why) - 6; i++)
        a->why[n++] = at_[i];
    for (int s = 12; s >= 0; s -= 4)
        a->why[n++] = hex[at >> s & 0xF];
    a->why[n] = 0;
}

static uint8_t code8(struct run *r, uint32_t at)
{
    if (at >= r->a->size) {
        stop(r, "a read past the image", at);
        return 0;
    }
    return r->a->rom[at];
}

static uint16_t code16(struct run *r, uint32_t at)
{
    return (uint16_t)(code8(r, at) | code8(r, at + 1) << 8);
}

static uint32_t code32(struct run *r, uint32_t at)
{
    return (uint32_t)code16(r, at) | (uint32_t)code16(r, at + 2) << 16;
}

static uint64_t now_ms(struct atom *a)
{
    return a->m.now_ms ? a->m.now_ms(a->m.ctx) : 0;
}

static uint16_t rom16(const struct atom *a, uint32_t at)
{
    return at + 1 < a->size ? (uint16_t)(a->rom[at] | a->rom[at + 1] << 8) : 0;
}

/* An indirect I/O program (its place base): reads, writes and bit moves
 * on one 32-bit temporary, with the register index and data it serves.
 * (Counts and places from 32 as Linux's on the processor: modulo 32.) */
static uint32_t iio_run(struct run *r, uint32_t base, uint32_t index, uint32_t data)
{
    struct atom *a = r->a;
    uint32_t temp = 0xCDCDCDCD;
    for (int steps = 0; steps < 256 && !r->stop; steps++) {
        uint8_t op = code8(r, base);
        uint32_t mask = 0, from = 0, to = 0;
        if (op >= 4 && op <= 8) {
            mask = 0xFFFFFFFFu >> ((32 - code8(r, base + 1)) & 31);
            from = code8(r, base + 2) & 31u;
            to = op >= 6 ? code8(r, base + 3) & 31u : 0;
        }
        switch (op) {
        case 0:                         /* nop */
            base++;
            break;
        case 2:                         /* read */
            temp = a->m.reg_read(a->m.ctx, code16(r, base + 1));
            a->reads++;
            base += 3;
            break;
        case 3:                         /* write */
            a->m.reg_write(a->m.ctx, code16(r, base + 1), temp);
            a->writes++;
            base += 3;
            break;
        case 4:                         /* clear bits: count, at */
            temp &= ~(mask << from);
            base += 3;
            break;
        case 5:                         /* set bits */
            temp |= mask << from;
            base += 3;
            break;
        case 6:                         /* move index bits: count, from, to */
            temp = (temp & ~(mask << to)) | ((index >> from) & mask) << to;
            base += 4;
            break;
        case 7:                         /* move attribute bits */
            temp = (temp & ~(mask << to)) | ((uint32_t)(a->io_attr >> from) & mask) << to;
            base += 4;
            break;
        case 8:                         /* move data bits */
            temp = (temp & ~(mask << to)) | ((data >> from) & mask) << to;
            base += 4;
            break;
        case 9:                         /* end */
            return temp;
        default:
            stop(r, "an unknown indirect I/O step", base);
            return 0;
        }
    }
    stop(r, "an indirect I/O program without an end", base);
    return 0;
}

static uint32_t reg_get(struct run *r, uint32_t index, uint32_t at)
{
    struct atom *a = r->a;
    index += a->reg_block;
    if (a->io_mode == IO_MM) {
        a->reads++;
        return a->m.reg_read(a->m.ctx, index);
    }
    if ((a->io_mode & IO_IIO) && a->iio[a->io_mode & 0x7F])
        return iio_run(r, a->iio[a->io_mode & 0x7F], index, 0);
    stop(r, a->io_mode & IO_IIO ? "an indirect I/O program that is not there"
                                : "PCI or system I/O registers (not done)", at);
    return 0;
}

static void reg_put(struct run *r, uint32_t index, uint32_t value, uint32_t at)
{
    struct atom *a = r->a;
    index += a->reg_block;
    if (a->io_mode == IO_MM) {
        a->writes++;
        /* (index 0 is the MM index register: Linux writes it a byte address) */
        a->m.reg_write(a->m.ctx, index, index == 0 ? value << 2 : value);
        return;
    }
    if ((a->io_mode & IO_IIO) && a->iio[a->io_mode & 0x7F]) {
        iio_run(r, a->iio[a->io_mode & 0x7F], index, value);
        return;
    }
    stop(r, a->io_mode & IO_IIO ? "an indirect I/O program that is not there"
                                : "PCI or system I/O registers (not done)", at);
}

static uint32_t *ps_at(struct run *r, uint32_t index, uint32_t at)
{
    if (index >= r->ps_room) {
        stop(r, "a parameter past the parameter space", at);
        return 0;
    }
    return &r->ps[index];
}

static uint32_t *ws_at(struct run *r, uint32_t index, uint32_t at)
{
    if (index >= r->ws_room) {
        stop(r, "a work space index past its size", at);
        return 0;
    }
    return &r->ws[index];
}

/* An operand of kind arg with part align, read at *at (moved past it):
 * its whole 32 bits in *whole, the part shifted down returned. */
static uint32_t get(struct run *r, int arg, int align, uint32_t *at, uint32_t *whole)
{
    struct atom *a = r->a;
    uint32_t v = 0xCDCDCDCD, idx, place = *at, *p;
    switch (arg) {
    case A_REG:
        idx = code16(r, *at);
        *at += 2;
        v = reg_get(r, idx, place);
        break;
    case A_PS:
        idx = code8(r, (*at)++);
        p = ps_at(r, idx, place);
        v = p ? *p : 0;
        break;
    case A_WS:
        idx = code8(r, (*at)++);
        switch (idx) {
        case WS_QUOTIENT: v = a->divmul[0]; break;
        case WS_REMAINDER: v = a->divmul[1]; break;
        case WS_DATAPTR: v = a->data_block; break;
        case WS_SHIFT: v = a->shift; break;
        case WS_OR_MASK: v = 1u << (a->shift & 31); break;
        case WS_AND_MASK: v = ~(1u << (a->shift & 31)); break;
        case WS_FB_WINDOW: v = a->fb_base; break;
        case WS_ATTRIBUTES: v = a->io_attr; break;
        case WS_REGPTR: v = a->reg_block; break;
        default:
            p = ws_at(r, idx, place);
            v = p ? *p : 0;
        }
        break;
    case A_ID:
        idx = code16(r, *at);
        *at += 2;
        v = code32(r, idx + a->data_block);
        break;
    case A_FB:
        idx = code8(r, (*at)++);
        if (!a->scratch || a->fb_base + idx * 4 + 4 > a->scratch_bytes)
            v = 0;                      /* (past the scratch: 0, as Linux) */
        else
            v = a->scratch[a->fb_base / 4 + idx];
        break;
    case A_IMM:
        if (align == 0) {
            v = code32(r, *at);
            *at += 4;
        } else if (align <= 3) {
            v = code16(r, *at);
            *at += 2;
        } else {
            v = code8(r, (*at)++);
        }
        if (whole)
            *whole = v;
        return v;                       /* (an immediate is its part already) */
    case A_PLL:
    case A_MC:
        (*at)++;
        stop(r, arg == A_PLL ? "a PLL register (not on these chips)"
                             : "an MC register (not on these chips)", place);
        return 0;
    }
    if (whole)
        *whole = v;
    return (v & part_mask[align]) >> part_shift[align];
}

static void skip(struct run *r, int arg, int align, uint32_t *at)
{
    (void)r;
    switch (arg) {
    case A_REG: case A_ID: *at += 2; break;
    case A_PS: case A_WS: case A_FB: case A_PLL: case A_MC: *at += 1; break;
    case A_IMM: *at += align == 0 ? 4 : align <= 3 ? 2 : 1; break;
    }
}

static int dalign(uint8_t attr)
{
    return dst_part[attr >> 3 & 7][attr >> 6 & 3];
}

/* The destination (kind arg, its part from attr) written at at: value
 * put into its part, the other bits as whole had them. */
static void put(struct run *r, int arg, uint8_t attr, uint32_t at, uint32_t value, uint32_t whole)
{
    struct atom *a = r->a;
    int align = dalign(attr);
    uint32_t v = ((value << part_shift[align]) & part_mask[align]) | (whole & ~part_mask[align]);
    uint32_t idx, *p;
    switch (arg) {
    case A_REG:
        reg_put(r, code16(r, at), v, at);
        break;
    case A_PS:
        p = ps_at(r, code8(r, at), at);
        if (p)
            *p = v;
        break;
    case A_WS:
        idx = code8(r, at);
        switch (idx) {
        case WS_QUOTIENT: a->divmul[0] = v; break;
        case WS_REMAINDER: a->divmul[1] = v; break;
        case WS_DATAPTR: a->data_block = (uint16_t)v; break;
        case WS_SHIFT: a->shift = (uint8_t)v; break;
        case WS_OR_MASK: case WS_AND_MASK: break;
        case WS_FB_WINDOW: a->fb_base = v; break;
        case WS_ATTRIBUTES: a->io_attr = (uint16_t)v; break;
        case WS_REGPTR: a->reg_block = (uint16_t)v; break;
        default:
            p = ws_at(r, idx, at);
            if (p)
                *p = v;
        }
        break;
    case A_FB:
        idx = code8(r, at);
        if (a->scratch && a->fb_base + idx * 4 + 4 <= a->scratch_bytes)
            a->scratch[a->fb_base / 4 + idx] = v;
        break;
    case A_PLL:
    case A_MC:
        stop(r, "a PLL or MC register (not on these chips)", at);
        break;
    }
}

/* The destination read (its part) with its whole value; *at past it. */
static uint32_t get_dst(struct run *r, int arg, uint8_t attr, uint32_t *at, uint32_t *whole)
{
    return get(r, arg, dalign(attr), at, whole);
}

static uint32_t get_src(struct run *r, uint8_t attr, uint32_t *at)
{
    return get(r, attr & 7, attr >> 3 & 7, at, 0);
}

/* An immediate of part align, without an attribute (mask's mask, the
 * shifts' count). */
static uint32_t direct(struct run *r, int align, uint32_t *at)
{
    return get(r, A_IMM, align, at, 0);
}

static int run_table(struct atom *a, int index, uint32_t *ps, uint32_t ps_room, int depth);

enum op { OP_NONE, OP_MOVE, OP_AND, OP_OR, OP_SHIFT_LEFT, OP_SHIFT_RIGHT, OP_MUL, OP_DIV, OP_ADD,
          OP_SUB, OP_SETPORT, OP_SETREGBLOCK, OP_SETFBBASE, OP_COMPARE, OP_SWITCH, OP_JUMP,
          OP_TEST, OP_DELAY_MS, OP_DELAY_US, OP_CALL, OP_REPEAT, OP_CLEAR, OP_NOP, OP_EOT,
          OP_MASK, OP_POSTCARD, OP_BEEP, OP_SAVEREG, OP_RESTOREREG, OP_SETDATABLOCK, OP_XOR,
          OP_SHL, OP_SHR, OP_DEBUG, OP_PROCESSDS, OP_MUL32, OP_DIV32 };

/* The opcodes, 1-126: what each does and its destination's kind (or, for
 * jumps, the condition; for set_port, the port). */
enum { J_ABOVE, J_ABOVE_EQ, J_ALWAYS, J_BELOW, J_BELOW_EQ, J_EQUAL, J_NOT_EQUAL };
static void decode(uint8_t opcode, enum op *op, int *arg)
{
    static const enum op six[] = { OP_MOVE, OP_AND, OP_OR, OP_SHIFT_LEFT, OP_SHIFT_RIGHT,
                                   OP_MUL, OP_DIV, OP_ADD, OP_SUB };
    *arg = 0;
    if (opcode >= 1 && opcode <= 54) {          /* nine operations, six kinds each */
        *op = six[(opcode - 1) / 6];
        *arg = (opcode - 1) % 6;
        if (*arg >= A_ID)
            *arg += 2;                          /* (the kinds: REG PS WS FB, then PLL MC) */
        return;
    }
    if (opcode >= 55 && opcode <= 57) { *op = OP_SETPORT; *arg = opcode - 55; return; }
    if (opcode == 58) { *op = OP_SETREGBLOCK; return; }
    if (opcode == 59) { *op = OP_SETFBBASE; return; }
    if (opcode >= 60 && opcode <= 65) {
        *op = OP_COMPARE;
        *arg = opcode - 60;
        if (*arg >= A_ID)
            *arg += 2;
        return;
    }
    if (opcode == 66) { *op = OP_SWITCH; return; }
    if (opcode >= 67 && opcode <= 73) {
        static const int conds[] = { J_ALWAYS, J_EQUAL, J_BELOW, J_ABOVE, J_BELOW_EQ,
                                     J_ABOVE_EQ, J_NOT_EQUAL };
        *op = OP_JUMP;
        *arg = conds[opcode - 67];
        return;
    }
    if (opcode >= 74 && opcode <= 79) {
        *op = OP_TEST;
        *arg = opcode - 74;
        if (*arg >= A_ID)
            *arg += 2;
        return;
    }
    switch (opcode) {
    case 80: *op = OP_DELAY_MS; return;
    case 81: *op = OP_DELAY_US; return;
    case 82: *op = OP_CALL; return;
    case 83: *op = OP_REPEAT; return;
    }
    if (opcode >= 84 && opcode <= 89) {
        *op = OP_CLEAR;
        *arg = opcode - 84;
        if (*arg >= A_ID)
            *arg += 2;
        return;
    }
    switch (opcode) {
    case 90: *op = OP_NOP; return;
    case 91: *op = OP_EOT; return;
    }
    if (opcode >= 92 && opcode <= 97) {
        *op = OP_MASK;
        *arg = opcode - 92;
        if (*arg >= A_ID)
            *arg += 2;
        return;
    }
    switch (opcode) {
    case 98: *op = OP_POSTCARD; return;
    case 99: *op = OP_BEEP; return;
    case 100: *op = OP_SAVEREG; return;
    case 101: *op = OP_RESTOREREG; return;
    case 102: *op = OP_SETDATABLOCK; return;
    }
    if (opcode >= 103 && opcode <= 120) {       /* xor, shl, shr: six kinds each */
        static const enum op three[] = { OP_XOR, OP_SHL, OP_SHR };
        *op = three[(opcode - 103) / 6];
        *arg = (opcode - 103) % 6;
        if (*arg >= A_ID)
            *arg += 2;
        return;
    }
    switch (opcode) {
    case 121: *op = OP_DEBUG; return;
    case 122: *op = OP_PROCESSDS; return;
    case 123: *op = OP_MUL32; *arg = A_PS; return;
    case 124: *op = OP_MUL32; *arg = A_WS; return;
    case 125: *op = OP_DIV32; *arg = A_PS; return;
    case 126: *op = OP_DIV32; *arg = A_WS; return;
    }
    *op = OP_NONE;
}

/* One instruction at *at (moved on). */
static void step(struct run *r, uint32_t *at)
{
    struct atom *a = r->a;
    uint32_t place = *at;
    uint8_t opcode = code8(r, (*at)++);
    enum op op;
    int arg;
    decode(opcode, &op, &arg);
    a->ops++;
#ifdef ATOM_STEP
    ATOM_STEP(place, opcode);           /* (atomhost: which code ran) */
#endif
    uint8_t attr;
    uint32_t dst, src, whole = 0, dat;
    switch (op) {
    case OP_MOVE:
        attr = code8(r, (*at)++);
        dat = *at;
        if ((attr >> 3 & 7) != 0)
            get_dst(r, arg, attr, at, &whole);  /* (a part keeps the rest) */
        else {
            skip(r, arg, dalign(attr), at);
            whole = 0xCDCDCDCD;
        }
        src = get_src(r, attr, at);
        put(r, arg, attr, dat, src, whole);
        return;
    case OP_AND: case OP_OR: case OP_ADD: case OP_SUB: case OP_XOR:
        attr = code8(r, (*at)++);
        dat = *at;
        dst = get_dst(r, arg, attr, at, &whole);
        src = get_src(r, attr, at);
        dst = op == OP_AND ? dst & src : op == OP_OR ? dst | src : op == OP_ADD ? dst + src
            : op == OP_SUB ? dst - src : dst ^ src;
        put(r, arg, attr, dat, dst, whole);
        return;
    case OP_SHIFT_LEFT: case OP_SHIFT_RIGHT: {
        attr = code8(r, (*at)++) & 0x38;
        attr |= (uint8_t)(default_dst[attr >> 3] << 6);
        dat = *at;
        dst = get_dst(r, arg, attr, at, &whole);
        /* (counts from 32: as Linux's on the processor, which takes them
         * modulo 32; in C they are undefined) */
        uint32_t n = direct(r, 4, at) & 31;
        dst = op == OP_SHIFT_LEFT ? dst << n : dst >> n;
        put(r, arg, attr, dat, dst, whole);
        return;
    }
    case OP_SHL: case OP_SHR: {
        attr = code8(r, (*at)++);
        dat = *at;
        int al = dalign(attr);
        get_dst(r, arg, attr, at, &whole);
        uint32_t n = get_src(r, attr, at) & 31;       /* (a byte in Linux, then as above) */
        dst = op == OP_SHL ? whole << n : whole >> n;   /* the whole value */
        dst = (dst & part_mask[al]) >> part_shift[al];
        put(r, arg, attr, dat, dst, whole);
        return;
    }
    case OP_MUL: case OP_DIV: case OP_MUL32: case OP_DIV32:
        attr = code8(r, (*at)++);
        dst = get_dst(r, arg, attr, at, 0);
        src = get_src(r, attr, at);
        if (op == OP_MUL) {
            a->divmul[0] = dst * src;
        } else if (op == OP_MUL32) {
            uint64_t p64 = (uint64_t)dst * src;
            a->divmul[0] = (uint32_t)p64;
            a->divmul[1] = (uint32_t)(p64 >> 32);
        } else if (op == OP_DIV) {
            a->divmul[0] = src ? dst / src : 0;
            a->divmul[1] = src ? dst % src : 0;
        } else {
            uint64_t n64 = (uint64_t)a->divmul[1] << 32 | dst;
            a->divmul[0] = src ? (uint32_t)(n64 / src) : 0;
            a->divmul[1] = src ? (uint32_t)(n64 / src >> 32) : 0;
        }
        return;
    case OP_COMPARE:
        attr = code8(r, (*at)++);
        dst = get_dst(r, arg, attr, at, 0);
        src = get_src(r, attr, at);
        a->equal = dst == src;
        a->above = dst > src;
        return;
    case OP_TEST:
        attr = code8(r, (*at)++);
        dst = get_dst(r, arg, attr, at, 0);
        src = get_src(r, attr, at);
        a->equal = (dst & src) == 0;
        return;
    case OP_CLEAR:
        attr = code8(r, (*at)++) & 0x38;
        attr |= (uint8_t)(default_dst[attr >> 3] << 6);
        dat = *at;
        get_dst(r, arg, attr, at, &whole);
        put(r, arg, attr, dat, 0, whole);
        return;
    case OP_MASK: {
        attr = code8(r, (*at)++);
        dat = *at;
        dst = get_dst(r, arg, attr, at, &whole);
        uint32_t mask = direct(r, attr >> 3 & 7, at);
        src = get_src(r, attr, at);
        put(r, arg, attr, dat, (dst & mask) | src, whole);
        return;
    }
    case OP_SETPORT:
        if (arg == 0) {
            uint16_t port = code16(r, *at);
            *at += 2;
            a->io_mode = port ? IO_IIO | port : IO_MM;
        } else {
            a->io_mode = arg == 1 ? IO_PCI : IO_SYSIO;
            (*at)++;
        }
        return;
    case OP_SETREGBLOCK:
        a->reg_block = code16(r, *at);
        *at += 2;
        return;
    case OP_SETFBBASE:
        attr = code8(r, (*at)++);
        a->fb_base = get_src(r, attr, at);
        return;
    case OP_SETDATABLOCK: {
        uint8_t idx = code8(r, (*at)++);
        a->data_block = idx == 0 ? 0 : idx == 255 ? r->start
                      : rom16(a, (uint32_t)a->data_table + 4 + 2u * idx);
        return;
    }
    case OP_SWITCH: {
        attr = code8(r, (*at)++);
        src = get_src(r, attr, at);
        for (int cases = 0; cases < 256 && !r->stop; cases++) {
            if (code16(r, *at) == CASE_END) {
                *at += 2;
                return;
            }
            if (code8(r, *at) != CASE_MAGIC) {
                stop(r, "a switch without its case mark", *at);
                return;
            }
            (*at)++;
            uint32_t value = get(r, A_IMM, attr >> 3 & 7, at, 0);
            uint16_t target = code16(r, *at);
            if (value == src) {
                *at = (uint32_t)r->start + target;
                return;
            }
            *at += 2;
        }
        stop(r, "a switch without its end", *at);
        return;
    }
    case OP_JUMP: {
        uint16_t target = code16(r, *at);
        *at += 2;
        bool go = arg == J_ALWAYS ? true : arg == J_EQUAL ? a->equal
                : arg == J_NOT_EQUAL ? !a->equal : arg == J_ABOVE ? a->above
                : arg == J_ABOVE_EQ ? a->above || a->equal
                : arg == J_BELOW ? !(a->above || a->equal) : !a->above;
        if (!go)
            return;
        uint32_t to = (uint32_t)r->start + target;
        if (to == r->last_jump) {
            uint64_t t = now_ms(a);
            if (t > r->jump_ms) {
                if (t - r->jump_ms > ATOM_LOOP_MS) {
                    stop(r, "a loop that does not end", place);
                    return;
                }
            } else {
                r->jump_ms = now_ms(a);         /* (as Linux: the clock read again) */
            }
        } else {
            r->last_jump = to;
            r->jump_ms = now_ms(a);
        }
        *at = to;
        return;
    }
    case OP_DELAY_MS: case OP_DELAY_US: {
        uint32_t n = code8(r, (*at)++);
        uint32_t us = op == OP_DELAY_MS ? n * 1000 : n;
        a->delays_us += us;
        if (a->m.delay_us)
            a->m.delay_us(a->m.ctx, us);
        return;
    }
    case OP_CALL: {
        uint8_t idx = code8(r, (*at)++);
        a->calls++;
        if (!rom16(a, (uint32_t)a->cmd_table + 4 + 2u * idx))
            return;                         /* (no such table: nothing, as Linux) */
        uint32_t shift = (code8(r, r->start + CT_PS) & 0x7F) / 4;
        if (shift > r->ps_room) {
            stop(r, "a call past the parameter space", place);
            return;
        }
        if (run_table(a, idx, r->ps + shift, r->ps_room - shift, r->depth + 1) == -1)
            r->stop = true;
        return;
    }
    case OP_NOP: case OP_EOT: case OP_POSTCARD: case OP_BEEP: case OP_DEBUG:
        if (op == OP_POSTCARD || op == OP_DEBUG)
            (*at)++;
        return;
    case OP_PROCESSDS:
        *at += code16(r, *at) + 2u;
        return;
    case OP_REPEAT: case OP_SAVEREG: case OP_RESTOREREG:
        stop(r, "repeat, savereg or restorereg (not implemented, as in Linux)", place);
        return;
    case OP_NONE:
        stop(r, "an opcode that is not one", place);
        return;
    }
}

static int run_table(struct atom *a, int index, uint32_t *ps, uint32_t ps_room, int depth)
{
    uint16_t base = rom16(a, (uint32_t)a->cmd_table + 4 + 2u * (uint32_t)index);
    if (!base || base + CT_CODE >= a->size)
        return -2;
    struct run r = { .a = a, .ps = ps, .ps_room = ps_room, .start = base, .depth = depth };
    if (depth >= ATOM_DEPTH) {
        stop(&r, "tables called too deep", base);
        return -1;
    }
    r.ws_room = a->rom[base + CT_WS];               /* (dwords, as Linux allocates them) */
    if (r.ws_room > 64)
        r.ws_room = 64;                             /* (from 0x40 the interpreter's own) */
    uint32_t end = base + (uint32_t)rom16(a, base);
    uint32_t at = base + CT_CODE;
    for (uint64_t steps = 0; !r.stop; steps++) {
        if (at >= end || at >= a->size) {
            stop(&r, "code past its table", at);
            break;
        }
        if (a->rom[at] == 91) {                     /* EOT */
            a->ops++;
            return 0;
        }
        if (a->rom[at] == 0 || a->rom[at] >= 127)
            return 0;                               /* (no opcode: the end, as Linux) */
        step(&r, &at);
        if (a->refused) {
            stop(&r, a->refused, at);
            break;
        }
        if (steps > ATOM_STEPS) {
            stop(&r, "too many steps", at);
            break;
        }
    }
    return -1;
}

bool atom_init(struct atom *a, const uint8_t *rom, uint32_t size, const struct atom_machine *m,
               uint32_t *scratch, uint32_t scratch_bytes)
{
    for (size_t i = 0; i < sizeof(*a); i++)
        ((uint8_t *)a)[i] = 0;
    a->rom = rom, a->size = size, a->m = *m, a->scratch = scratch;
    a->scratch_bytes = scratch_bytes;
    const char *why = 0;
    uint16_t header = rom16(a, ROM_HEADER_PTR);
    if (size < 0x4A || rom[0] != 0x55 || rom[1] != 0xAA)
        why = "not a PCI ROM";
    else if (header + 0x22 > size || rom[header + 4] != 'A' || rom[header + 5] != 'T' ||
             rom[header + 6] != 'O' || rom[header + 7] != 'M')
        why = "no ATOM header";
    if (!why) {
        a->cmd_table = rom16(a, header + ROM_CMD_PTR);
        a->data_table = rom16(a, header + ROM_DATA_PTR);
        if (!a->cmd_table || !a->data_table || a->cmd_table + 4u >= size ||
            a->data_table + 4u >= size)
            why = "no master tables";
    }
    if (why) {
        size_t n = 0;
        for (; why[n] && n < sizeof(a->why) - 1; n++)
            a->why[n] = why[n];
        a->why[n] = 0;
        return false;
    }
    uint32_t master = rom16(a, a->cmd_table);       /* its size: a 4-byte header, then 2 a table */
    a->tables = (uint16_t)(master < 4 ? 0 : (master - 4) / 2 > 256 ? 256 : (master - 4) / 2);
    /* The indirect I/O programs: in the data table's IIO entry, each
     * START (1) with its number, its steps, END (9) and two bytes more. */
    static const uint8_t length[10] = { 1, 2, 3, 3, 3, 3, 4, 4, 4, 3 };
    uint32_t base = rom16(a, (uint32_t)a->data_table + DATA_IIO);
    if (base) {
        base += 4;
        for (int programs = 0; programs < 256 && base + 1 < size && rom[base] == 1; programs++) {
            a->iio[rom[base + 1]] = (uint16_t)(base + 2);
            base += 2;
            for (int steps = 0; steps < 1024 && base < size && rom[base] != 9; steps++)
                base += rom[base] < 10 ? length[rom[base]] : 1;
            base += 3;
        }
    }
    return true;
}

int atom_run(struct atom *a, int index, uint32_t *params)
{
    a->why[0] = 0;
    a->refused = 0;
    a->data_block = a->reg_block = 0;
    a->fb_base = 0;
    a->io_mode = IO_MM;
    a->divmul[0] = a->divmul[1] = 0;
    if (index < 0 || index >= a->tables)
        return -2;                      /* (past the master list; a table's calls: as Linux) */
    return run_table(a, index, params, ATOM_PARAMS, 0);
}

uint16_t atom_table(const struct atom *a, int index, uint16_t *size, uint8_t *frev,
                    uint8_t *crev, uint8_t *ws, uint8_t *ps)
{
    uint16_t base = index >= 0 && index < a->tables ?
                    rom16(a, (uint32_t)a->cmd_table + 4 + 2u * (uint32_t)index) : 0;
    if (!base || base + CT_CODE >= a->size)
        return 0;
    *size = rom16(a, base);
    *frev = a->rom[base + 2];
    *crev = a->rom[base + 3];
    *ws = a->rom[base + CT_WS];
    *ps = a->rom[base + CT_PS] & 0x7F;
    return base;
}

uint16_t atom_data_table(const struct atom *a, int index)
{
    return index >= 0 && index < 256 ? rom16(a, (uint32_t)a->data_table + 4 + 2u * (uint32_t)index)
                                     : 0;
}
