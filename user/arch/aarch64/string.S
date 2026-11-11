/*
 * The C library's memory and string functions on AArch64 (32g), in NEON
 * (every AArch64 processor has it): 16 bytes at a time, 32 or 64 in the
 * long loops. strcpy, stpcpy and strnlen are C on top of these (string.c,
 * cstd.c). Only v0-v7 are used (the AAPCS64 keeps v8-v15 for the caller).
 *
 * A match in a block becomes a number with 4 bits per byte (cmeq, then
 * shrn #4 of its halfwords, then fmov): its lowest set bit (rbit, clz)
 * over 4 is the first byte, its highest the last. Reading past the end as
 * on x86 (user/arch/x86_64/string.S): a string's end is found in aligned
 * blocks only, two strings go a byte at a time near a page's end, and a
 * length's functions read only inside it. memcpy is memmove.
 */

    .text

.macro FUNCTION name
    .global \name
    .type   \name, %function
    .p2align 4
\name:
.endm

/* x\to: the 4-bits-per-byte number of v\v's bytes (0x00 or 0xff each) */
.macro SYNDROME v, to
    shrn    v\v\().8b, v\v\().8h, #4
    fmov    x\to, d\v
.endm

    .global memcpy
    .type   memcpy, %function
    FUNCTION memmove
memcpy:
    cmp     x2, #16
    b.lo    .Lmove_small
    add     x9, x1, x2                      /* the source's end */
    add     x10, x0, x2                     /* the destination's */
    cmp     x2, #32
    b.ls    .Lmove_two
    cmp     x2, #64
    b.ls    .Lmove_four
    ldr     q4, [x1]                        /* the first and last 16, stored last */
    ldur    q5, [x9, #-16]
    sub     x11, x0, x1
    cmp     x11, x2
    b.lo    .Lmove_back                     /* the destination starts inside the source */
    add     x12, x0, #16
    and     x12, x12, #-16                  /* aligned stores from here */
    sub     x13, x12, x0
    add     x14, x1, x13                    /* their source */
    sub     x15, x10, #16                   /* up to the last 16 */
    sub     x13, x15, #64
1:  cmp     x12, x13
    b.hs    2f
    ldp     q0, q1, [x14]
    ldp     q2, q3, [x14, #32]
    stp     q0, q1, [x12]
    stp     q2, q3, [x12, #32]
    add     x12, x12, #64
    add     x14, x14, #64
    b       1b
2:  cmp     x12, x15
    b.hs    3f
    ldr     q0, [x14], #16
    str     q0, [x12], #16
    b       2b
3:  stur    q5, [x10, #-16]
    str     q4, [x0]
    ret
.Lmove_back:
    and     x12, x10, #-16                  /* aligned stores downward from here */
    add     x15, x0, #16
    add     x13, x15, #64
1:  cmp     x12, x13
    b.ls    2f
    sub     x12, x12, #64
    sub     x14, x12, x11                   /* the source: the destination less d - s */
    ldp     q2, q3, [x14, #32]
    ldp     q0, q1, [x14]
    stp     q2, q3, [x12, #32]
    stp     q0, q1, [x12]
    b       1b
2:  cmp     x12, x15
    b.ls    3f
    sub     x12, x12, #16
    sub     x14, x12, x11
    ldr     q0, [x14]
    str     q0, [x12]
    b       2b
3:  str     q4, [x0]
    stur    q5, [x10, #-16]
    ret
.Lmove_four:                                /* 33 to 64: the first and last 32 */
    ldp     q0, q1, [x1]
    ldp     q2, q3, [x9, #-32]
    stp     q0, q1, [x0]
    stp     q2, q3, [x10, #-32]
    ret
.Lmove_two:
    ldr     q0, [x1]
    ldur    q1, [x9, #-16]
    str     q0, [x0]
    stur    q1, [x10, #-16]
    ret
.Lmove_small:                               /* (every load before the stores) */
    add     x9, x1, x2
    add     x10, x0, x2
    cmp     x2, #8
    b.lo    1f
    ldr     x3, [x1]
    ldur    x4, [x9, #-8]
    str     x3, [x0]
    stur    x4, [x10, #-8]
    ret
1:  cmp     x2, #4
    b.lo    2f
    ldr     w3, [x1]
    ldur    w4, [x9, #-4]
    str     w3, [x0]
    stur    w4, [x10, #-4]
    ret
2:  cmp     x2, #2
    b.lo    3f
    ldrh    w3, [x1]
    ldurh   w4, [x9, #-2]
    strh    w3, [x0]
    sturh   w4, [x10, #-2]
    ret
3:  cbz     x2, 4f
    ldrb    w3, [x1]
    strb    w3, [x0]
4:  ret
    .size   memmove, . - memmove
    .size   memcpy, . - memcpy

    FUNCTION memset
    dup     v0.16b, w1
    add     x4, x0, x2                      /* the end */
    cmp     x2, #16
    b.lo    .Lset_small
    str     q0, [x0]
    stur    q0, [x4, #-16]
    cmp     x2, #32
    b.ls    3f
    cmp     x2, #64
    b.hi    0f
    str     q0, [x0, #16]                   /* 33 to 64: the middle from both ends */
    stur    q0, [x4, #-32]
    ret
0:  add     x5, x0, #16
    and     x5, x5, #-16
    sub     x6, x4, #16
    sub     x7, x6, #32
1:  cmp     x5, x7
    b.hs    2f
    stp     q0, q0, [x5], #32
    b       1b
2:  cmp     x5, x6
    b.hs    3f
    str     q0, [x5], #16
    b       2b
3:  ret
.Lset_small:
    fmov    x5, d0
    cmp     x2, #8
    b.lo    1f
    str     x5, [x0]
    stur    x5, [x4, #-8]
    ret
1:  cmp     x2, #4
    b.lo    2f
    str     w5, [x0]
    stur    w5, [x4, #-4]
    ret
2:  cmp     x2, #2
    b.lo    3f
    strh    w5, [x0]
    sturh   w5, [x4, #-2]
    ret
3:  cbz     x2, 4f
    strb    w5, [x0]
4:  ret
    .size   memset, . - memset

    FUNCTION memcmp
    cmp     x2, #16
    b.lo    .Lcmp_bytes
1:  ldr     q0, [x0]
    ldr     q1, [x1]
    cmeq    v2.16b, v0.16b, v1.16b
    uminv   b3, v2.16b
    fmov    w3, s3
    cmp     w3, #0xff
    b.ne    .Lcmp_diff
    add     x0, x0, #16
    add     x1, x1, #16
    sub     x2, x2, #16
    cmp     x2, #16
    b.hs    1b
    cbz     x2, .Lcmp_same
    add     x0, x0, x2                      /* the last 16, again in part */
    sub     x0, x0, #16
    add     x1, x1, x2
    sub     x1, x1, #16
    ldr     q0, [x0]
    ldr     q1, [x1]
    cmeq    v2.16b, v0.16b, v1.16b
    uminv   b3, v2.16b
    fmov    w3, s3
    cmp     w3, #0xff
    b.ne    .Lcmp_diff
.Lcmp_same:
    mov     w0, #0
    ret
.Lcmp_diff:
    not     v2.16b, v2.16b
    SYNDROME 2, 3
    rbit    x3, x3
    clz     x3, x3
    lsr     x3, x3, #2
    ldrb    w4, [x0, x3]
    ldrb    w5, [x1, x3]
    sub     w0, w4, w5
    ret
.Lcmp_bytes:
    cbz     x2, .Lcmp_same
1:  ldrb    w4, [x0], #1
    ldrb    w5, [x1], #1
    subs    w4, w4, w5
    b.ne    2f
    subs    x2, x2, #1
    b.ne    1b
    mov     w0, #0
    ret
2:  mov     w0, w4
    ret
    .size   memcmp, . - memcmp

    FUNCTION memchr
    cbz     x2, .Lchr_null
    dup     v2.16b, w1
    adds    x5, x0, x2                      /* the end */
    csinv   x5, x5, xzr, cc                 /* (past the address space: as far as it goes) */
    and     x1, x0, #-16
    ldr     q0, [x1]
    cmeq    v0.16b, v0.16b, v2.16b
    SYNDROME 0, 3
    and     x4, x0, #15
    lsl     x4, x4, #2
    lsr     x3, x3, x4                      /* (4 bits per byte from s) */
    cbz     x3, 1f
    rbit    x3, x3
    clz     x3, x3
    add     x0, x0, x3, lsr #2
    cmp     x0, x5
    b.hs    .Lchr_null
    ret
1:  add     x1, x1, #16
    cmp     x1, x5
    b.hs    .Lchr_null
    ldr     q0, [x1]
    cmeq    v0.16b, v0.16b, v2.16b
    umaxv   b1, v0.16b
    fmov    w3, s1
    cbz     w3, 1b
    SYNDROME 0, 3
    rbit    x3, x3
    clz     x3, x3
    add     x0, x1, x3, lsr #2
    cmp     x0, x5
    b.hs    .Lchr_null
    ret
.Lchr_null:
    mov     x0, #0
    ret
    .size   memchr, . - memchr

    FUNCTION memrchr
    cbz     x2, 3f
    dup     v2.16b, w1
    add     x6, x0, x2
    sub     x6, x6, #1                      /* the last byte */
    and     x1, x6, #-16
    and     x4, x6, #15
    mov     x7, #15
    sub     x4, x7, x4
    lsl     x4, x4, #2
    mov     x8, #-1
    lsr     x8, x8, x4                      /* the bytes up to the last */
    ldr     q0, [x1]
    cmeq    v0.16b, v0.16b, v2.16b
    SYNDROME 0, 3
    and     x3, x3, x8
1:  cbnz    x3, 2f
    cmp     x1, x0
    b.ls    3f
    sub     x1, x1, #16
    ldr     q0, [x1]
    cmeq    v0.16b, v0.16b, v2.16b
    SYNDROME 0, 3
    b       1b
2:  clz     x3, x3
    mov     x7, #63
    sub     x3, x7, x3
    add     x1, x1, x3, lsr #2
    cmp     x1, x0
    b.lo    3f
    mov     x0, x1
    ret
3:  mov     x0, #0
    ret
    .size   memrchr, . - memrchr

    FUNCTION strlen
    and     x1, x0, #-16
    ldr     q0, [x1]
    cmeq    v0.16b, v0.16b, #0
    SYNDROME 0, 2
    and     x3, x0, #15
    lsl     x3, x3, #2
    lsr     x2, x2, x3
    cbz     x2, 1f
    rbit    x2, x2
    clz     x2, x2
    lsr     x0, x2, #2
    ret
1:  add     x1, x1, #16                     /* one more 16 to a 32-byte boundary */
    tbz     x1, #4, 2f
    ldr     q0, [x1]
    cmeq    v0.16b, v0.16b, #0
    umaxv   b1, v0.16b
    fmov    w2, s1
    cbnz    w2, 4f
    add     x1, x1, #16
2:  ldp     q0, q1, [x1]                    /* then 32 at a time (in one page) */
    cmeq    v0.16b, v0.16b, #0
    cmeq    v1.16b, v1.16b, #0
    orr     v2.16b, v0.16b, v1.16b
    umaxv   b2, v2.16b
    fmov    w2, s2
    cbnz    w2, 3f
    add     x1, x1, #32
    b       2b
3:  umaxv   b2, v0.16b                      /* which 16 */
    fmov    w2, s2
    cbnz    w2, 4f
    add     x1, x1, #16
    mov     v0.16b, v1.16b
4:
    SYNDROME 0, 2
    rbit    x2, x2
    clz     x2, x2
    add     x1, x1, x2, lsr #2
    sub     x0, x1, x0
    ret
    .size   strlen, . - strlen

    FUNCTION strchr
    and     w1, w1, #0xff
    dup     v2.16b, w1
    and     x2, x0, #-16
    ldr     q0, [x2]
    cmeq    v1.16b, v0.16b, v2.16b
    cmeq    v0.16b, v0.16b, #0
    orr     v0.16b, v0.16b, v1.16b
    SYNDROME 0, 3
    and     x4, x0, #15
    lsl     x4, x4, #2
    lsr     x3, x3, x4
    cbz     x3, 1f
    rbit    x3, x3
    clz     x3, x3
    add     x0, x0, x3, lsr #2
    b       2f
1:  add     x2, x2, #16
    ldr     q0, [x2]
    cmeq    v1.16b, v0.16b, v2.16b
    cmeq    v0.16b, v0.16b, #0
    orr     v0.16b, v0.16b, v1.16b
    umaxv   b3, v0.16b
    fmov    w3, s3
    cbz     w3, 1b
    SYNDROME 0, 3
    rbit    x3, x3
    clz     x3, x3
    add     x0, x2, x3, lsr #2
2:  ldrb    w4, [x0]                        /* c, or the end */
    cmp     w4, w1
    b.eq    3f
    mov     x0, #0
3:  ret
    .size   strchr, . - strchr

    FUNCTION strrchr
    dup     v2.16b, w1
    mov     x5, #0                          /* the last c so far: none */
    and     x2, x0, #-16
    ldr     q0, [x2]
    cmeq    v1.16b, v0.16b, v2.16b
    cmeq    v0.16b, v0.16b, #0
    SYNDROME 1, 3                           /* c */
    SYNDROME 0, 4                           /* the end */
    and     x6, x0, #15
    lsl     x6, x6, #2
    lsr     x3, x3, x6
    lsr     x4, x4, x6
    mov     x6, x0                          /* where bit 0 is */
1:  cbnz    x4, 3f
    cbz     x3, 2f
    clz     x3, x3
    mov     x7, #63
    sub     x3, x7, x3
    add     x5, x6, x3, lsr #2
2:  add     x2, x2, #16
    mov     x6, x2
    ldr     q0, [x2]
    cmeq    v1.16b, v0.16b, v2.16b
    cmeq    v0.16b, v0.16b, #0
    SYNDROME 1, 3
    SYNDROME 0, 4
    b       1b
3:  sub     x7, x4, #1
    eor     x7, x7, x4                      /* the bytes up to the end */
    and     x3, x3, x7
    cbz     x3, 4f
    clz     x3, x3
    mov     x7, #63
    sub     x3, x7, x3
    add     x5, x6, x3, lsr #2
4:  mov     x0, x5
    ret
    .size   strrchr, . - strrchr

    FUNCTION strcmp
1:  and     x4, x0, #4095
    cmp     x4, #4080
    b.hi    3f
    and     x4, x1, #4095
    cmp     x4, #4080
    b.hi    3f
    ldr     q0, [x0]
    ldr     q1, [x1]
    cmeq    v2.16b, v0.16b, v1.16b          /* the same */
    cmeq    v3.16b, v0.16b, #0              /* a's end */
    bic     v2.16b, v2.16b, v3.16b          /* the same and not the end */
    uminv   b3, v2.16b
    fmov    w4, s3
    cmp     w4, #0xff
    b.ne    2f
    add     x0, x0, #16
    add     x1, x1, #16
    b       1b
2:  not     v2.16b, v2.16b
    SYNDROME 2, 4
    rbit    x4, x4
    clz     x4, x4
    lsr     x4, x4, #2
    ldrb    w2, [x0, x4]
    ldrb    w3, [x1, x4]
    sub     w0, w2, w3
    ret
3:  mov     x5, #16                         /* near a page's end: 16 one at a time */
4:  ldrb    w2, [x0], #1
    ldrb    w3, [x1], #1
    subs    w6, w2, w3
    b.ne    5f
    cbz     w2, 5f
    subs    x5, x5, #1
    b.ne    4b
    b       1b
5:  mov     w0, w6
    ret
    .size   strcmp, . - strcmp

    FUNCTION strncmp
1:  cbz     x2, 6f
    cmp     x2, #16
    b.lo    3f
    and     x4, x0, #4095
    cmp     x4, #4080
    b.hi    3f
    and     x4, x1, #4095
    cmp     x4, #4080
    b.hi    3f
    ldr     q0, [x0]
    ldr     q1, [x1]
    cmeq    v2.16b, v0.16b, v1.16b
    cmeq    v3.16b, v0.16b, #0
    bic     v2.16b, v2.16b, v3.16b
    uminv   b3, v2.16b
    fmov    w4, s3
    cmp     w4, #0xff
    b.ne    2f
    add     x0, x0, #16
    add     x1, x1, #16
    sub     x2, x2, #16
    b       1b
2:  not     v2.16b, v2.16b
    SYNDROME 2, 4
    rbit    x4, x4
    clz     x4, x4
    lsr     x4, x4, #2
    ldrb    w5, [x0, x4]
    ldrb    w3, [x1, x4]
    sub     w0, w5, w3
    ret
3:  mov     x5, #16                         /* the last ones, or near a page's end */
4:  ldrb    w6, [x0], #1
    ldrb    w3, [x1], #1
    subs    w7, w6, w3
    b.ne    5f
    cbz     w6, 5f
    subs    x2, x2, #1
    b.eq    6f
    subs    x5, x5, #1
    b.ne    4b
    b       1b
5:  mov     w0, w7
    ret
6:  mov     w0, #0
    ret
    .size   strncmp, . - strncmp
