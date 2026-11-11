/*
 * Register allocation (61a): see regalloc.c.
 */
#ifndef REGALLOC_H
#define REGALLOC_H

#include "ir.h"

struct regalloc {
    int *reg;                                   /* per value: a register (0..n-1) or -1: the stack */
    int *start, *end;                           /* where it is live (instruction indexes), -1: never */
};

/* ngpr registers for integer values, nfpr for the classes in fpr_classes
 * (the first nfpr_kept never touched by CLOBBERS_LOW_FPRS instructions)
 * (a bit per enum ir_class: float and double on AArch64, (32h) vectors on
 * x86-64; 0 registers: none);
 * values marked in exclude (or a null pointer) need no place at all.
 * (61c) Integer registers from nsaved on need no saving and are given out
 * first; clobbers (or a null pointer) has, per instruction, a bit for each
 * of them that it overwrites: a value is never kept in one over such an
 * instruction (one using the value counts too). */
void regalloc_run(struct ir_func *f, int ngpr, int nfpr, int nfpr_kept, unsigned fpr_classes, const bool *exclude,
                  const unsigned *clobbers, int nsaved, struct regalloc *out);

/* (32h) a clobbers bit: the instruction overwrites every register of the
 * nfpr kind (x86-64's calls: no xmm register is kept) */
#define CLOBBERS_FPRS (1u << 30)

/* (40d) a clobbers bit: the instruction uses the second kind's registers
 * from nfpr_kept on (x86-64's xmm2-xmm7: a call's arguments, inline
 * assembly's operands): a value in one of them never lives over it, nor
 * is used or made there */
#define CLOBBERS_LOW_FPRS (1u << 29)

#endif
