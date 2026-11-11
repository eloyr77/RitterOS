/*
 * (67a) A kernel module's relocations on AArch64 (kernel/module.h), as
 * AAELF64 defines them. Modules lie within 128 MiB of all kernel code
 * (MODULE_BASE), so a call's 26-bit branch (CALL26, JUMP26) reaches it;
 * addresses are made as ADRP (the 4 KiB page, +-4 GiB) plus the low 12
 * bits in an ADD or a load/store (scaled by its access size).
 */
#include "module.h"
#include "elf.h"
#include "string.h"

static uint32_t get(const uint8_t *where)
{
    uint32_t insn;
    memcpy(&insn, where, 4);
    return insn;
}

static void set(uint8_t *where, uint32_t insn)
{
    memcpy(where, &insn, 4);
}

/* A branch's offset: its imm field of `bits` at bit `at`, in words. */
static int branch(uint8_t *where, int64_t off, int bits, int at)
{
    if (off & 3 || off < -((int64_t)1 << (bits + 1)) || off >= (int64_t)1 << (bits + 1))
        return -2;
    uint32_t mask = ((1u << bits) - 1) << at;
    set(where, (get(where) & ~mask) | ((uint32_t)(off >> 2) << at & mask));
    return 0;
}

/* ADR/ADRP's 21-bit immediate: low 2 bits at 29, high 19 at 5. */
static int adr(uint8_t *where, int64_t imm)
{
    if (imm < -((int64_t)1 << 20) || imm >= (int64_t)1 << 20)
        return -2;
    uint32_t u = (uint32_t)imm;
    uint32_t insn = get(where) & ~(3u << 29 | 0x7FFFFu << 5);
    set(where, insn | (u & 3) << 29 | (u >> 2 & 0x7FFFF) << 5);
    return 0;
}

/* The low 12 bits of an address into an ADD or load/store, scaled. */
static int low12(uint8_t *where, uint64_t v, int shift)
{
    uint32_t imm = (uint32_t)(v & 0xFFF) >> shift;
    set(where, (get(where) & ~(0xFFFu << 10)) | imm << 10);
    return 0;
}

int arch_module_relocate(uint32_t type, uint8_t *where, uint64_t p, uint64_t s, int64_t a)
{
    uint64_t v = s + (uint64_t)a;
    int64_t off = (int64_t)(v - p);
    switch (type) {
    case R_AARCH64_ABS64:
        memcpy(where, &v, 8);
        return 0;
    case R_AARCH64_PREL64:
        memcpy(where, &off, 8);
        return 0;
    case R_AARCH64_ABS32:
        if ((int64_t)v < INT32_MIN || v > 0xFFFFFFFFu)
            return -2;
        set(where, (uint32_t)v);
        return 0;
    case R_AARCH64_PREL32:
        if (off != (int32_t)off)
            return -2;
        set(where, (uint32_t)off);
        return 0;
    case R_AARCH64_CALL26:
    case R_AARCH64_JUMP26:
        return branch(where, off, 26, 0);
    case R_AARCH64_CONDBR19:
        return branch(where, off, 19, 5);
    case R_AARCH64_TSTBR14:
        return branch(where, off, 14, 5);
    case R_AARCH64_ADR_PREL_LO21:
        return adr(where, off);
    case R_AARCH64_ADR_PREL_PG_HI21:
        return adr(where, (int64_t)((v & ~0xFFFull) - (p & ~0xFFFull)) >> 12);
    case R_AARCH64_ADD_ABS_LO12_NC:
    case R_AARCH64_LDST8_ABS_LO12_NC:
        return low12(where, v, 0);
    case R_AARCH64_LDST16_ABS_LO12_NC:
        return low12(where, v, 1);
    case R_AARCH64_LDST32_ABS_LO12_NC:
        return low12(where, v, 2);
    case R_AARCH64_LDST64_ABS_LO12_NC:
        return low12(where, v, 3);
    case R_AARCH64_LDST128_ABS_LO12_NC:
        return low12(where, v, 4);
    default:
        return -1;
    }
}
