/*
 * sys/auxv.h (43a-3): getauxval: a value of the vector the kernel gives
 * a new program (AT_PAGESZ, AT_PHDR, AT_PHNUM, AT_ENTRY, AT_BASE...: the
 * ELF standard's numbers); 0 and ENOENT for one it did not give.
 */
#ifndef RITTEROS_SYS_AUXV_H
#define RITTEROS_SYS_AUXV_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AT_NULL
#define AT_NULL         0
#define AT_IGNORE       1
#define AT_EXECFD       2
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_BASE         7
#define AT_FLAGS        8
#define AT_ENTRY        9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_PLATFORM     15
#define AT_HWCAP        16
#define AT_CLKTCK       17
#define AT_SECURE       23
#define AT_RANDOM       25
#define AT_HWCAP2       26
#define AT_EXECFN       31
#endif

/* (43b-6) AT_HWCAP: x86 CPUID 1's EDX (AT_HWCAP2 0); AArch64 Linux's
 * HWCAP_* bits from the ID registers (AT_HWCAP2 0 so far). */
#if defined(__aarch64__) && !defined(HWCAP_FP)
#define HWCAP_FP        (1UL << 0)
#define HWCAP_ASIMD     (1UL << 1)
#define HWCAP_EVTSTRM   (1UL << 2)
#define HWCAP_AES       (1UL << 3)
#define HWCAP_PMULL     (1UL << 4)
#define HWCAP_SHA1      (1UL << 5)
#define HWCAP_SHA2      (1UL << 6)
#define HWCAP_CRC32     (1UL << 7)
#define HWCAP_ATOMICS   (1UL << 8)
#define HWCAP_FPHP      (1UL << 9)
#define HWCAP_ASIMDHP   (1UL << 10)
#define HWCAP_CPUID     (1UL << 11)
#define HWCAP_ASIMDRDM  (1UL << 12)
#define HWCAP_JSCVT     (1UL << 13)
#define HWCAP_FCMA      (1UL << 14)
#define HWCAP_LRCPC     (1UL << 15)
#define HWCAP_DCPOP     (1UL << 16)
#define HWCAP_SHA3      (1UL << 17)
#define HWCAP_SM3       (1UL << 18)
#define HWCAP_SM4       (1UL << 19)
#define HWCAP_ASIMDDP   (1UL << 20)
#define HWCAP_SHA512    (1UL << 21)
#define HWCAP_SVE       (1UL << 22)
#define HWCAP_ASIMDFHM  (1UL << 23)
#define HWCAP_ILRCPC    (1UL << 26)
#endif

unsigned long getauxval(unsigned long type);

#ifdef __cplusplus
}
#endif

#endif
