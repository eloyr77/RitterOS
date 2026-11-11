/*
 * (67a) A kernel module's relocations on x86-64 (kernel/module.h). Modules
 * are compiled as the kernel (-mcmodel=kernel, no PIC), so their code
 * reaches the kernel and itself with 32-bit offsets (PC32; a call's PLT32
 * is the same: there is no PLT in the kernel) and absolute addresses in
 * the top 2 GiB (32S); tables of pointers are 64-bit (R_X86_64_64).
 */
#include "module.h"
#include "elf.h"
#include "string.h"

static int put32(uint8_t *where, int64_t v, bool is_signed)
{
    if (is_signed ? v != (int32_t)v : (uint64_t)v > 0xFFFFFFFFu)
        return -2;
    uint32_t w = (uint32_t)v;
    memcpy(where, &w, 4);
    return 0;
}

int arch_module_relocate(uint32_t type, uint8_t *where, uint64_t p, uint64_t s, int64_t a)
{
    uint64_t v;
    switch (type) {
    case R_X86_64_64:
        v = s + (uint64_t)a;
        memcpy(where, &v, 8);
        return 0;
    case R_X86_64_PC64:
        v = s + (uint64_t)a - p;
        memcpy(where, &v, 8);
        return 0;
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
        return put32(where, (int64_t)(s + (uint64_t)a - p), true);
    case R_X86_64_32S:
        return put32(where, (int64_t)(s + (uint64_t)a), true);
    case R_X86_64_32:
        return put32(where, (int64_t)(s + (uint64_t)a), false);
    default:
        return -1;
    }
}
