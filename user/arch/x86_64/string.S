/*
 * The C library's memory and string functions on x86-64 (32e): each in an
 * AVX2 version (32 bytes at a time) and an SSE2 one (16, every x86-64
 * processor). The entry chooses by __ritteros_simd (string.c): 2 AVX2, 1
 * SSE2, 0 not asked yet (the first call asks CPUID and XGETBV, also in
 * static programs, which run no constructors). strcpy, stpcpy and strnlen
 * are C on top of these (string.c, cstd.c).
 *
 * Reading past the end: a string's end is found reading aligned blocks
 * only, which never reach into a page the string does not; functions
 * reading two strings at once go a byte at a time where either is near a
 * page's end; ones with a length read only inside it (overlapping loads at
 * the ends instead of byte loops). memcpy is memmove (forward, or backward
 * when the destination starts inside the source).
 */

    .text
    .hidden __ritteros_simd
    .hidden __ritteros_simd_detect

/* NAME: the entry, choosing NAME_avx2 or NAME_sse2 (arguments kept) */
.macro ENTRY name
    .global \name
    .type   \name, @function
\name:
    cmpb    $2, __ritteros_simd(%rip)
    je      \name\()_avx2
    cmpb    $1, __ritteros_simd(%rip)
    je      \name\()_sse2
    pushq   %rdi
    pushq   %rsi
    pushq   %rdx                            /* (the stack 16-aligned for the call) */
    call    __ritteros_simd_detect
    popq    %rdx
    popq    %rsi
    popq    %rdi
    jmp     \name
    .size   \name, . - \name
.endm

    ENTRY memcpy
    ENTRY memmove
    ENTRY memset
    ENTRY memcmp
    ENTRY memchr
    ENTRY memrchr
    ENTRY strlen
    ENTRY strchr
    ENTRY strrchr
    ENTRY strcmp
    ENTRY strncmp

/* --- AVX2 ---------------------------------------------------------------- */

    .p2align 4
memcpy_avx2:
memmove_avx2:
    movq    %rdi, %rax
    cmpq    $32, %rdx
    jb      .Lmove_avx2_small
    cmpq    $64, %rdx
    jbe     .Lmove_avx2_two
    vmovdqu (%rsi), %ymm4                   /* the first and last 32 bytes, stored last */
    vmovdqu -32(%rsi,%rdx), %ymm5
    leaq    (%rdi,%rdx), %r9                /* the destination's end */
    movq    %rdi, %rcx
    subq    %rsi, %rcx
    cmpq    %rdx, %rcx
    jb      .Lmove_avx2_back                /* the destination starts inside the source */
    leaq    32(%rdi), %r8
    andq    $-32, %r8                       /* aligned stores from here */
    movq    %r8, %r11
    subq    %rdi, %r11
    addq    %rsi, %r11                      /* its source */
    leaq    -32(%r9), %r10                  /* up to the last 32 */
    leaq    -128(%r10), %rcx
1:  cmpq    %rcx, %r8
    jae     2f
    vmovdqu (%r11), %ymm0
    vmovdqu 32(%r11), %ymm1
    vmovdqu 64(%r11), %ymm2
    vmovdqu 96(%r11), %ymm3
    vmovdqa %ymm0, (%r8)
    vmovdqa %ymm1, 32(%r8)
    vmovdqa %ymm2, 64(%r8)
    vmovdqa %ymm3, 96(%r8)
    subq    $-128, %r8
    subq    $-128, %r11
    jmp     1b
2:  cmpq    %r10, %r8
    jae     3f
    vmovdqu (%r11), %ymm0
    vmovdqa %ymm0, (%r8)
    addq    $32, %r8
    addq    $32, %r11
    jmp     2b
3:  vmovdqu %ymm5, -32(%r9)
    vmovdqu %ymm4, (%rdi)
    vzeroupper
    ret
.Lmove_avx2_back:
    movq    %r9, %r8
    andq    $-32, %r8                       /* aligned stores downward from here */
    leaq    32(%rdi), %r10
    leaq    128(%r10), %rsi                 /* (the source's start is not needed now) */
0:  cmpq    %rsi, %r8                       /* 128 at a time while all above the first 32 */
    jbe     1f
    subq    $128, %r8
    movq    %r8, %r11
    subq    %rcx, %r11
    vmovdqu 96(%r11), %ymm0
    vmovdqu 64(%r11), %ymm1
    vmovdqu 32(%r11), %ymm2
    vmovdqu (%r11), %ymm3
    vmovdqa %ymm0, 96(%r8)
    vmovdqa %ymm1, 64(%r8)
    vmovdqa %ymm2, 32(%r8)
    vmovdqa %ymm3, (%r8)
    jmp     0b
1:  cmpq    %r10, %r8
    jbe     2f
    subq    $32, %r8
    movq    %r8, %r11
    subq    %rcx, %r11                      /* its source: the destination less d - s */
    vmovdqu (%r11), %ymm0
    vmovdqa %ymm0, (%r8)
    jmp     1b
2:  vmovdqu %ymm4, (%rdi)
    vmovdqu %ymm5, -32(%r9)
    vzeroupper
    ret
.Lmove_avx2_two:
    vmovdqu (%rsi), %ymm0
    vmovdqu -32(%rsi,%rdx), %ymm1
    vmovdqu %ymm0, (%rdi)
    vmovdqu %ymm1, -32(%rdi,%rdx)
    vzeroupper
    ret
.Lmove_avx2_small:                          /* (every load before the stores) */
    cmpq    $16, %rdx
    jb      1f
    vmovdqu (%rsi), %xmm0
    vmovdqu -16(%rsi,%rdx), %xmm1
    vmovdqu %xmm0, (%rdi)
    vmovdqu %xmm1, -16(%rdi,%rdx)
    ret
1:  cmpq    $8, %rdx
    jb      2f
    movq    (%rsi), %rcx
    movq    -8(%rsi,%rdx), %r8
    movq    %rcx, (%rdi)
    movq    %r8, -8(%rdi,%rdx)
    ret
2:  cmpq    $4, %rdx
    jb      3f
    movl    (%rsi), %ecx
    movl    -4(%rsi,%rdx), %r8d
    movl    %ecx, (%rdi)
    movl    %r8d, -4(%rdi,%rdx)
    ret
3:  cmpq    $2, %rdx
    jb      4f
    movzwl  (%rsi), %ecx
    movzwl  -2(%rsi,%rdx), %r8d
    movw    %cx, (%rdi)
    movw    %r8w, -2(%rdi,%rdx)
    ret
4:  testq   %rdx, %rdx
    jz      5f
    movzbl  (%rsi), %ecx
    movb    %cl, (%rdi)
5:  ret

    .p2align 4
memset_avx2:
    movq    %rdi, %rax
    vmovd   %esi, %xmm0
    vpbroadcastb %xmm0, %ymm0
    cmpq    $32, %rdx
    jb      .Lset_avx2_small
    leaq    (%rdi,%rdx), %rcx
    vmovdqu %ymm0, (%rdi)
    vmovdqu %ymm0, -32(%rcx)
    cmpq    $64, %rdx
    jbe     3f
    leaq    32(%rdi), %r8
    andq    $-32, %r8
    leaq    -32(%rcx), %r10
    leaq    -128(%r10), %r9
1:  cmpq    %r9, %r8
    jae     2f
    vmovdqa %ymm0, (%r8)
    vmovdqa %ymm0, 32(%r8)
    vmovdqa %ymm0, 64(%r8)
    vmovdqa %ymm0, 96(%r8)
    subq    $-128, %r8
    jmp     1b
2:  cmpq    %r10, %r8
    jae     3f
    vmovdqa %ymm0, (%r8)
    addq    $32, %r8
    jmp     2b
3:  vzeroupper
    ret
.Lset_avx2_small:
    vmovq   %xmm0, %rcx
    cmpq    $16, %rdx
    jb      1f
    vmovdqu %xmm0, (%rdi)
    vmovdqu %xmm0, -16(%rdi,%rdx)
    vzeroupper
    ret
1:  vzeroupper
    jmp     .Lset_scalar

/* below 16 bytes, the byte in every byte of rcx */
.Lset_scalar:
    cmpq    $8, %rdx
    jb      1f
    movq    %rcx, (%rdi)
    movq    %rcx, -8(%rdi,%rdx)
    ret
1:  cmpq    $4, %rdx
    jb      2f
    movl    %ecx, (%rdi)
    movl    %ecx, -4(%rdi,%rdx)
    ret
2:  cmpq    $2, %rdx
    jb      3f
    movw    %cx, (%rdi)
    movw    %cx, -2(%rdi,%rdx)
    ret
3:  testq   %rdx, %rdx
    jz      4f
    movb    %cl, (%rdi)
4:  ret

    .p2align 4
memcmp_avx2:
    cmpq    $32, %rdx
    jb      .Lcmp_avx2_small
1:  vmovdqu (%rdi), %ymm1
    vpcmpeqb (%rsi), %ymm1, %ymm1
    vpmovmskb %ymm1, %ecx
    cmpl    $-1, %ecx
    jne     .Lcmp_avx2_diff
    addq    $32, %rdi
    addq    $32, %rsi
    subq    $32, %rdx
    cmpq    $32, %rdx
    jae     1b
    testq   %rdx, %rdx
    jz      .Lcmp_avx2_same
    leaq    -32(%rdi,%rdx), %rdi            /* the last 32, again in part */
    leaq    -32(%rsi,%rdx), %rsi
    vmovdqu (%rdi), %ymm1
    vpcmpeqb (%rsi), %ymm1, %ymm1
    vpmovmskb %ymm1, %ecx
    cmpl    $-1, %ecx
    jne     .Lcmp_avx2_diff
.Lcmp_avx2_same:
    xorl    %eax, %eax
    vzeroupper
    ret
.Lcmp_avx2_diff:
    notl    %ecx
    bsfl    %ecx, %ecx
    movzbl  (%rdi,%rcx), %eax
    movzbl  (%rsi,%rcx), %edx
    subl    %edx, %eax
    vzeroupper
    ret
.Lcmp_avx2_small:
    cmpq    $16, %rdx
    jb      .Lcmp_bytes
    vmovdqu (%rdi), %xmm1
    vpcmpeqb (%rsi), %xmm1, %xmm1
    vpmovmskb %xmm1, %ecx
    cmpl    $0xFFFF, %ecx
    jne     1f
    leaq    -16(%rdi,%rdx), %rdi
    leaq    -16(%rsi,%rdx), %rsi
    vmovdqu (%rdi), %xmm1
    vpcmpeqb (%rsi), %xmm1, %xmm1
    vpmovmskb %xmm1, %ecx
    cmpl    $0xFFFF, %ecx
    je      .Lcmp_avx2_same
1:  notl    %ecx
    bsfl    %ecx, %ecx
    movzbl  (%rdi,%rcx), %eax
    movzbl  (%rsi,%rcx), %edx
    subl    %edx, %eax
    ret

/* below 16 bytes: one at a time */
.Lcmp_bytes:
    testq   %rdx, %rdx
    jz      2f
1:  movzbl  (%rdi), %eax
    movzbl  (%rsi), %ecx
    subl    %ecx, %eax
    jnz     3f
    incq    %rdi
    incq    %rsi
    decq    %rdx
    jnz     1b
2:  xorl    %eax, %eax
3:  ret

    .p2align 4
memchr_avx2:
    testq   %rdx, %rdx
    jz      .Lchr_avx2_null
    vmovd   %esi, %xmm0
    vpbroadcastb %xmm0, %ymm0
    movq    %rdi, %r8
    addq    %rdx, %r8                       /* the end */
    jnc     1f
    movq    $-1, %r8                        /* (past the address space: as far as it goes) */
1:  movq    %rdi, %rax
    andq    $-32, %rax
    vpcmpeqb (%rax), %ymm0, %ymm1
    vpmovmskb %ymm1, %edx
    movl    %edi, %ecx
    andl    $31, %ecx
    shrl    %cl, %edx                       /* (bit k: byte s + k) */
    testl   %edx, %edx
    jz      2f
    bsfl    %edx, %edx
    leaq    (%rdi,%rdx), %rax
    jmp     .Lchr_avx2_check
2:  addq    $32, %rax
    cmpq    %r8, %rax
    jae     .Lchr_avx2_null
    testl   $127, %eax
    jz      3f
    vpcmpeqb (%rax), %ymm0, %ymm1
    vpmovmskb %ymm1, %edx
    testl   %edx, %edx
    jz      2b
    jmp     .Lchr_avx2_at
3:  vpcmpeqb (%rax), %ymm0, %ymm1           /* 128 bytes at a time (in one page) */
    vpcmpeqb 32(%rax), %ymm0, %ymm2
    vpcmpeqb 64(%rax), %ymm0, %ymm3
    vpcmpeqb 96(%rax), %ymm0, %ymm4
    vpor    %ymm2, %ymm1, %ymm1
    vpor    %ymm4, %ymm3, %ymm3
    vpor    %ymm3, %ymm1, %ymm1
    vpmovmskb %ymm1, %edx
    testl   %edx, %edx
    jnz     4f
    subq    $-128, %rax
    cmpq    %r8, %rax
    jb      3b
    jmp     .Lchr_avx2_null
4:  vpcmpeqb (%rax), %ymm0, %ymm1           /* which 32 */
    vpmovmskb %ymm1, %edx
    testl   %edx, %edx
    jnz     .Lchr_avx2_at
    addq    $32, %rax
    jmp     4b
.Lchr_avx2_at:
    bsfl    %edx, %edx
    addq    %rdx, %rax
.Lchr_avx2_check:
    cmpq    %r8, %rax
    jae     .Lchr_avx2_null
    vzeroupper
    ret
.Lchr_avx2_null:
    xorl    %eax, %eax
    vzeroupper
    ret

    .p2align 4
memrchr_avx2:
    testq   %rdx, %rdx
    jz      2f
    vmovd   %esi, %xmm0
    vpbroadcastb %xmm0, %ymm0
    leaq    -1(%rdi,%rdx), %r8              /* the last byte */
    movq    %r8, %rax
    andq    $-32, %rax
    movl    %r8d, %ecx
    andl    $31, %ecx
    movl    $2, %r9d
    shlq    %cl, %r9
    decq    %r9                             /* the bytes up to the last */
    vpcmpeqb (%rax), %ymm0, %ymm1
    vpmovmskb %ymm1, %edx
    andl    %r9d, %edx
1:  testl   %edx, %edx
    jnz     3f
    cmpq    %rdi, %rax
    jbe     2f
    subq    $32, %rax
    vpcmpeqb (%rax), %ymm0, %ymm1
    vpmovmskb %ymm1, %edx
    jmp     1b
2:  xorl    %eax, %eax
    vzeroupper
    ret
3:  bsrl    %edx, %edx
    addq    %rdx, %rax
    cmpq    %rdi, %rax
    jb      2b
    vzeroupper
    ret

    .p2align 4
strlen_avx2:
    vpxor   %xmm0, %xmm0, %xmm0
    movq    %rdi, %rax
    andq    $-32, %rax
    vpcmpeqb (%rax), %ymm0, %ymm1
    vpmovmskb %ymm1, %edx
    movl    %edi, %ecx
    andl    $31, %ecx
    shrl    %cl, %edx
    testl   %edx, %edx
    jz      1f
    bsfl    %edx, %eax
    vzeroupper
    ret
1:  addq    $32, %rax                       /* single blocks to a 128-byte boundary */
    testl   $127, %eax
    jz      2f
    vpcmpeqb (%rax), %ymm0, %ymm1
    vpmovmskb %ymm1, %edx
    testl   %edx, %edx
    jz      1b
    jmp     4f
2:  vmovdqa (%rax), %ymm1                   /* then 128 at a time */
    vpminub 32(%rax), %ymm1, %ymm1
    vmovdqa 64(%rax), %ymm2
    vpminub 96(%rax), %ymm2, %ymm2
    vpminub %ymm2, %ymm1, %ymm1
    vpcmpeqb %ymm0, %ymm1, %ymm1
    vpmovmskb %ymm1, %edx
    testl   %edx, %edx
    jnz     3f
    subq    $-128, %rax
    jmp     2b
3:  vpcmpeqb (%rax), %ymm0, %ymm1           /* which 32 */
    vpmovmskb %ymm1, %edx
    testl   %edx, %edx
    jnz     4f
    addq    $32, %rax
    jmp     3b
4:  bsfl    %edx, %edx
    addq    %rdx, %rax
    subq    %rdi, %rax
    vzeroupper
    ret

    .p2align 4
strchr_avx2:
    vmovd   %esi, %xmm0
    vpbroadcastb %xmm0, %ymm0
    vpxor   %xmm1, %xmm1, %xmm1
    movq    %rdi, %rax
    andq    $-32, %rax
    vmovdqa (%rax), %ymm2
    vpcmpeqb %ymm0, %ymm2, %ymm3
    vpcmpeqb %ymm1, %ymm2, %ymm2
    vpor    %ymm3, %ymm2, %ymm2
    vpmovmskb %ymm2, %edx
    movl    %edi, %ecx
    andl    $31, %ecx
    shrl    %cl, %edx
    testl   %edx, %edx
    jz      1f
    bsfl    %edx, %edx
    leaq    (%rdi,%rdx), %rax
    jmp     2f
1:  addq    $32, %rax
    vmovdqa (%rax), %ymm2
    vpcmpeqb %ymm0, %ymm2, %ymm3
    vpcmpeqb %ymm1, %ymm2, %ymm2
    vpor    %ymm3, %ymm2, %ymm2
    vpmovmskb %ymm2, %edx
    testl   %edx, %edx
    jz      1b
    bsfl    %edx, %edx
    addq    %rdx, %rax
2:  cmpb    %sil, (%rax)                    /* c, or the end */
    je      3f
    xorl    %eax, %eax
3:  vzeroupper
    ret

    .p2align 4
strrchr_avx2:
    vmovd   %esi, %xmm0
    vpbroadcastb %xmm0, %ymm0
    vpxor   %xmm1, %xmm1, %xmm1
    xorl    %r8d, %r8d                      /* the last c so far: none */
    movq    %rdi, %rax
    andq    $-32, %rax
    vmovdqa (%rax), %ymm2
    vpcmpeqb %ymm0, %ymm2, %ymm3
    vpcmpeqb %ymm1, %ymm2, %ymm2
    vpmovmskb %ymm3, %r9d                   /* c */
    vpmovmskb %ymm2, %edx                   /* the end */
    movl    %edi, %ecx
    andl    $31, %ecx
    shrl    %cl, %r9d
    shrl    %cl, %edx
    movq    %rdi, %r10                      /* where bit 0 is */
1:  testl   %edx, %edx
    jnz     3f
    testl   %r9d, %r9d
    jz      2f
    bsrl    %r9d, %r9d
    leaq    (%r10,%r9), %r8
2:  addq    $32, %rax
    movq    %rax, %r10
    vmovdqa (%rax), %ymm2
    vpcmpeqb %ymm0, %ymm2, %ymm3
    vpcmpeqb %ymm1, %ymm2, %ymm2
    vpmovmskb %ymm3, %r9d
    vpmovmskb %ymm2, %edx
    jmp     1b
3:  leal    -1(%rdx), %ecx
    xorl    %edx, %ecx                      /* the bytes up to the end */
    andl    %ecx, %r9d
    jz      4f
    bsrl    %r9d, %r9d
    leaq    (%r10,%r9), %r8
4:  movq    %r8, %rax
    vzeroupper
    ret

    .p2align 4
strcmp_avx2:
    vpxor   %xmm0, %xmm0, %xmm0
1:  movl    %edi, %eax
    andl    $4095, %eax
    cmpl    $4064, %eax
    ja      3f
    movl    %esi, %eax
    andl    $4095, %eax
    cmpl    $4064, %eax
    ja      3f
    vmovdqu (%rdi), %ymm1
    vpcmpeqb (%rsi), %ymm1, %ymm2           /* the same */
    vpminub %ymm1, %ymm2, %ymm2             /* 0 where they differ or a's ends */
    vpcmpeqb %ymm0, %ymm2, %ymm2
    vpmovmskb %ymm2, %ecx
    testl   %ecx, %ecx
    jnz     2f
    addq    $32, %rdi
    addq    $32, %rsi
    jmp     1b
2:  bsfl    %ecx, %ecx
    movzbl  (%rdi,%rcx), %eax
    movzbl  (%rsi,%rcx), %edx
    subl    %edx, %eax
    vzeroupper
    ret
3:  movl    $32, %ecx                       /* near a page's end: 32 one at a time */
4:  movzbl  (%rdi), %eax
    movzbl  (%rsi), %edx
    subl    %edx, %eax
    jnz     5f
    testl   %edx, %edx
    jz      5f
    incq    %rdi
    incq    %rsi
    decl    %ecx
    jnz     4b
    jmp     1b
5:  vzeroupper
    ret

    .p2align 4
strncmp_avx2:
    vpxor   %xmm0, %xmm0, %xmm0
1:  testq   %rdx, %rdx
    jz      6f
    cmpq    $32, %rdx
    jb      3f
    movl    %edi, %eax
    andl    $4095, %eax
    cmpl    $4064, %eax
    ja      3f
    movl    %esi, %eax
    andl    $4095, %eax
    cmpl    $4064, %eax
    ja      3f
    vmovdqu (%rdi), %ymm1
    vpcmpeqb (%rsi), %ymm1, %ymm2
    vpminub %ymm1, %ymm2, %ymm2
    vpcmpeqb %ymm0, %ymm2, %ymm2
    vpmovmskb %ymm2, %ecx
    testl   %ecx, %ecx
    jnz     2f
    addq    $32, %rdi
    addq    $32, %rsi
    subq    $32, %rdx
    jmp     1b
2:  bsfl    %ecx, %ecx
    movzbl  (%rdi,%rcx), %eax
    movzbl  (%rsi,%rcx), %edx
    subl    %edx, %eax
    vzeroupper
    ret
3:  movl    $32, %ecx                       /* the last ones, or near a page's end */
4:  movzbl  (%rdi), %eax
    movzbl  (%rsi), %r8d
    subl    %r8d, %eax
    jnz     5f
    testl   %r8d, %r8d
    jz      5f
    incq    %rdi
    incq    %rsi
    decq    %rdx
    jz      6f
    decl    %ecx
    jnz     4b
    jmp     1b
5:  vzeroupper
    ret
6:  xorl    %eax, %eax
    vzeroupper
    ret

/* --- SSE2 ---------------------------------------------------------------- */

/* xmm0: the byte in esi in all 16 */
.macro BROADCAST_SSE2
    movd    %esi, %xmm0
    punpcklbw %xmm0, %xmm0
    punpcklwd %xmm0, %xmm0
    pshufd  $0, %xmm0, %xmm0
.endm

    .p2align 4
memcpy_sse2:
memmove_sse2:
    movq    %rdi, %rax
    cmpq    $16, %rdx
    jb      .Lmove_sse2_small
    cmpq    $32, %rdx
    jbe     .Lmove_sse2_two
    movdqu  (%rsi), %xmm4
    movdqu  -16(%rsi,%rdx), %xmm5
    leaq    (%rdi,%rdx), %r9
    movq    %rdi, %rcx
    subq    %rsi, %rcx
    cmpq    %rdx, %rcx
    jb      .Lmove_sse2_back
    leaq    16(%rdi), %r8
    andq    $-16, %r8
    movq    %r8, %r11
    subq    %rdi, %r11
    addq    %rsi, %r11
    leaq    -16(%r9), %r10
    leaq    -64(%r10), %rcx
1:  cmpq    %rcx, %r8
    jae     2f
    movdqu  (%r11), %xmm0
    movdqu  16(%r11), %xmm1
    movdqu  32(%r11), %xmm2
    movdqu  48(%r11), %xmm3
    movdqa  %xmm0, (%r8)
    movdqa  %xmm1, 16(%r8)
    movdqa  %xmm2, 32(%r8)
    movdqa  %xmm3, 48(%r8)
    addq    $64, %r8
    addq    $64, %r11
    jmp     1b
2:  cmpq    %r10, %r8
    jae     3f
    movdqu  (%r11), %xmm0
    movdqa  %xmm0, (%r8)
    addq    $16, %r8
    addq    $16, %r11
    jmp     2b
3:  movdqu  %xmm5, -16(%r9)
    movdqu  %xmm4, (%rdi)
    ret
.Lmove_sse2_back:
    movq    %r9, %r8
    andq    $-16, %r8
    leaq    16(%rdi), %r10
    leaq    64(%r10), %rsi
0:  cmpq    %rsi, %r8
    jbe     1f
    subq    $64, %r8
    movq    %r8, %r11
    subq    %rcx, %r11
    movdqu  48(%r11), %xmm0
    movdqu  32(%r11), %xmm1
    movdqu  16(%r11), %xmm2
    movdqu  (%r11), %xmm3
    movdqa  %xmm0, 48(%r8)
    movdqa  %xmm1, 32(%r8)
    movdqa  %xmm2, 16(%r8)
    movdqa  %xmm3, (%r8)
    jmp     0b
1:  cmpq    %r10, %r8
    jbe     2f
    subq    $16, %r8
    movq    %r8, %r11
    subq    %rcx, %r11
    movdqu  (%r11), %xmm0
    movdqa  %xmm0, (%r8)
    jmp     1b
2:  movdqu  %xmm4, (%rdi)
    movdqu  %xmm5, -16(%r9)
    ret
.Lmove_sse2_two:
    movdqu  (%rsi), %xmm0
    movdqu  -16(%rsi,%rdx), %xmm1
    movdqu  %xmm0, (%rdi)
    movdqu  %xmm1, -16(%rdi,%rdx)
    ret
.Lmove_sse2_small:
    cmpq    $8, %rdx
    jb      1f
    movq    (%rsi), %rcx
    movq    -8(%rsi,%rdx), %r8
    movq    %rcx, (%rdi)
    movq    %r8, -8(%rdi,%rdx)
    ret
1:  cmpq    $4, %rdx
    jb      2f
    movl    (%rsi), %ecx
    movl    -4(%rsi,%rdx), %r8d
    movl    %ecx, (%rdi)
    movl    %r8d, -4(%rdi,%rdx)
    ret
2:  cmpq    $2, %rdx
    jb      3f
    movzwl  (%rsi), %ecx
    movzwl  -2(%rsi,%rdx), %r8d
    movw    %cx, (%rdi)
    movw    %r8w, -2(%rdi,%rdx)
    ret
3:  testq   %rdx, %rdx
    jz      4f
    movzbl  (%rsi), %ecx
    movb    %cl, (%rdi)
4:  ret

    .p2align 4
memset_sse2:
    movq    %rdi, %rax
    BROADCAST_SSE2
    cmpq    $16, %rdx
    jb      1f
    leaq    (%rdi,%rdx), %rcx
    movdqu  %xmm0, (%rdi)
    movdqu  %xmm0, -16(%rcx)
    cmpq    $32, %rdx
    jbe     4f
    leaq    16(%rdi), %r8
    andq    $-16, %r8
    leaq    -16(%rcx), %r10
    leaq    -64(%r10), %r9
2:  cmpq    %r9, %r8
    jae     3f
    movdqa  %xmm0, (%r8)
    movdqa  %xmm0, 16(%r8)
    movdqa  %xmm0, 32(%r8)
    movdqa  %xmm0, 48(%r8)
    addq    $64, %r8
    jmp     2b
3:  cmpq    %r10, %r8
    jae     4f
    movdqa  %xmm0, (%r8)
    addq    $16, %r8
    jmp     3b
4:  ret
1:  movq    %xmm0, %rcx
    jmp     .Lset_scalar

    .p2align 4
memcmp_sse2:
    cmpq    $16, %rdx
    jb      .Lcmp_bytes
1:  movdqu  (%rdi), %xmm1
    movdqu  (%rsi), %xmm2
    pcmpeqb %xmm2, %xmm1
    pmovmskb %xmm1, %ecx
    cmpl    $0xFFFF, %ecx
    jne     3f
    addq    $16, %rdi
    addq    $16, %rsi
    subq    $16, %rdx
    cmpq    $16, %rdx
    jae     1b
    testq   %rdx, %rdx
    jz      2f
    leaq    -16(%rdi,%rdx), %rdi
    leaq    -16(%rsi,%rdx), %rsi
    movdqu  (%rdi), %xmm1
    movdqu  (%rsi), %xmm2
    pcmpeqb %xmm2, %xmm1
    pmovmskb %xmm1, %ecx
    cmpl    $0xFFFF, %ecx
    jne     3f
2:  xorl    %eax, %eax
    ret
3:  notl    %ecx
    bsfl    %ecx, %ecx
    movzbl  (%rdi,%rcx), %eax
    movzbl  (%rsi,%rcx), %edx
    subl    %edx, %eax
    ret

    .p2align 4
memchr_sse2:
    testq   %rdx, %rdx
    jz      .Lchr_sse2_null
    BROADCAST_SSE2
    movq    %rdi, %r8
    addq    %rdx, %r8
    jnc     1f
    movq    $-1, %r8
1:  movq    %rdi, %rax
    andq    $-16, %rax
    movdqa  (%rax), %xmm1
    pcmpeqb %xmm0, %xmm1
    pmovmskb %xmm1, %edx
    movl    %edi, %ecx
    andl    $15, %ecx
    shrl    %cl, %edx
    testl   %edx, %edx
    jz      2f
    bsfl    %edx, %edx
    leaq    (%rdi,%rdx), %rax
    jmp     .Lchr_sse2_check
2:  addq    $16, %rax
    cmpq    %r8, %rax
    jae     .Lchr_sse2_null
    testl   $63, %eax
    jz      3f
    movdqa  (%rax), %xmm1
    pcmpeqb %xmm0, %xmm1
    pmovmskb %xmm1, %edx
    testl   %edx, %edx
    jz      2b
    jmp     .Lchr_sse2_at
3:  movdqa  (%rax), %xmm1                   /* 64 bytes at a time */
    movdqa  16(%rax), %xmm2
    movdqa  32(%rax), %xmm3
    movdqa  48(%rax), %xmm4
    pcmpeqb %xmm0, %xmm1
    pcmpeqb %xmm0, %xmm2
    pcmpeqb %xmm0, %xmm3
    pcmpeqb %xmm0, %xmm4
    por     %xmm2, %xmm1
    por     %xmm4, %xmm3
    por     %xmm3, %xmm1
    pmovmskb %xmm1, %edx
    testl   %edx, %edx
    jnz     4f
    addq    $64, %rax
    cmpq    %r8, %rax
    jb      3b
    jmp     .Lchr_sse2_null
4:  movdqa  (%rax), %xmm1
    pcmpeqb %xmm0, %xmm1
    pmovmskb %xmm1, %edx
    testl   %edx, %edx
    jnz     .Lchr_sse2_at
    addq    $16, %rax
    jmp     4b
.Lchr_sse2_at:
    bsfl    %edx, %edx
    addq    %rdx, %rax
.Lchr_sse2_check:
    cmpq    %r8, %rax
    jae     .Lchr_sse2_null
    ret
.Lchr_sse2_null:
    xorl    %eax, %eax
    ret

    .p2align 4
memrchr_sse2:
    testq   %rdx, %rdx
    jz      2f
    BROADCAST_SSE2
    leaq    -1(%rdi,%rdx), %r8
    movq    %r8, %rax
    andq    $-16, %rax
    movl    %r8d, %ecx
    andl    $15, %ecx
    movl    $2, %r9d
    shll    %cl, %r9d
    decl    %r9d
    movdqa  (%rax), %xmm1
    pcmpeqb %xmm0, %xmm1
    pmovmskb %xmm1, %edx
    andl    %r9d, %edx
1:  testl   %edx, %edx
    jnz     3f
    cmpq    %rdi, %rax
    jbe     2f
    subq    $16, %rax
    movdqa  (%rax), %xmm1
    pcmpeqb %xmm0, %xmm1
    pmovmskb %xmm1, %edx
    jmp     1b
2:  xorl    %eax, %eax
    ret
3:  bsrl    %edx, %edx
    addq    %rdx, %rax
    cmpq    %rdi, %rax
    jb      2b
    ret

    .p2align 4
strlen_sse2:
    pxor    %xmm0, %xmm0
    movq    %rdi, %rax
    andq    $-16, %rax
    movdqa  (%rax), %xmm1
    pcmpeqb %xmm0, %xmm1
    pmovmskb %xmm1, %edx
    movl    %edi, %ecx
    andl    $15, %ecx
    shrl    %cl, %edx
    testl   %edx, %edx
    jz      1f
    bsfl    %edx, %eax
    ret
1:  addq    $16, %rax                       /* single blocks to a 64-byte boundary */
    testl   $63, %eax
    jz      2f
    movdqa  (%rax), %xmm1
    pcmpeqb %xmm0, %xmm1
    pmovmskb %xmm1, %edx
    testl   %edx, %edx
    jz      1b
    jmp     4f
2:  movdqa  (%rax), %xmm1                   /* then 64 at a time */
    pminub  16(%rax), %xmm1
    movdqa  32(%rax), %xmm2
    pminub  48(%rax), %xmm2
    pminub  %xmm2, %xmm1
    pcmpeqb %xmm0, %xmm1
    pmovmskb %xmm1, %edx
    testl   %edx, %edx
    jnz     3f
    addq    $64, %rax
    jmp     2b
3:  movdqa  (%rax), %xmm1
    pcmpeqb %xmm0, %xmm1
    pmovmskb %xmm1, %edx
    testl   %edx, %edx
    jnz     4f
    addq    $16, %rax
    jmp     3b
4:  bsfl    %edx, %edx
    addq    %rdx, %rax
    subq    %rdi, %rax
    ret

    .p2align 4
strchr_sse2:
    BROADCAST_SSE2
    pxor    %xmm1, %xmm1
    movq    %rdi, %rax
    andq    $-16, %rax
    movdqa  (%rax), %xmm2
    movdqa  %xmm2, %xmm3
    pcmpeqb %xmm0, %xmm3
    pcmpeqb %xmm1, %xmm2
    por     %xmm3, %xmm2
    pmovmskb %xmm2, %edx
    movl    %edi, %ecx
    andl    $15, %ecx
    shrl    %cl, %edx
    testl   %edx, %edx
    jz      1f
    bsfl    %edx, %edx
    leaq    (%rdi,%rdx), %rax
    jmp     2f
1:  addq    $16, %rax
    movdqa  (%rax), %xmm2
    movdqa  %xmm2, %xmm3
    pcmpeqb %xmm0, %xmm3
    pcmpeqb %xmm1, %xmm2
    por     %xmm3, %xmm2
    pmovmskb %xmm2, %edx
    testl   %edx, %edx
    jz      1b
    bsfl    %edx, %edx
    addq    %rdx, %rax
2:  cmpb    %sil, (%rax)
    je      3f
    xorl    %eax, %eax
3:  ret

    .p2align 4
strrchr_sse2:
    BROADCAST_SSE2
    pxor    %xmm1, %xmm1
    xorl    %r8d, %r8d
    movq    %rdi, %rax
    andq    $-16, %rax
    movdqa  (%rax), %xmm2
    movdqa  %xmm2, %xmm3
    pcmpeqb %xmm0, %xmm3
    pcmpeqb %xmm1, %xmm2
    pmovmskb %xmm3, %r9d
    pmovmskb %xmm2, %edx
    movl    %edi, %ecx
    andl    $15, %ecx
    shrl    %cl, %r9d
    shrl    %cl, %edx
    movq    %rdi, %r10
1:  testl   %edx, %edx
    jnz     3f
    testl   %r9d, %r9d
    jz      2f
    bsrl    %r9d, %r9d
    leaq    (%r10,%r9), %r8
2:  addq    $16, %rax
    movq    %rax, %r10
    movdqa  (%rax), %xmm2
    movdqa  %xmm2, %xmm3
    pcmpeqb %xmm0, %xmm3
    pcmpeqb %xmm1, %xmm2
    pmovmskb %xmm3, %r9d
    pmovmskb %xmm2, %edx
    jmp     1b
3:  leal    -1(%rdx), %ecx
    xorl    %edx, %ecx
    andl    %ecx, %r9d
    jz      4f
    bsrl    %r9d, %r9d
    leaq    (%r10,%r9), %r8
4:  movq    %r8, %rax
    ret

    .p2align 4
strcmp_sse2:
    pxor    %xmm0, %xmm0
1:  movl    %edi, %eax
    andl    $4095, %eax
    cmpl    $4080, %eax
    ja      3f
    movl    %esi, %eax
    andl    $4095, %eax
    cmpl    $4080, %eax
    ja      3f
    movdqu  (%rdi), %xmm1
    movdqu  (%rsi), %xmm2
    pcmpeqb %xmm1, %xmm2
    pminub  %xmm1, %xmm2
    pcmpeqb %xmm0, %xmm2
    pmovmskb %xmm2, %ecx
    testl   %ecx, %ecx
    jnz     2f
    addq    $16, %rdi
    addq    $16, %rsi
    jmp     1b
2:  bsfl    %ecx, %ecx
    movzbl  (%rdi,%rcx), %eax
    movzbl  (%rsi,%rcx), %edx
    subl    %edx, %eax
    ret
3:  movl    $16, %ecx
4:  movzbl  (%rdi), %eax
    movzbl  (%rsi), %edx
    subl    %edx, %eax
    jnz     5f
    testl   %edx, %edx
    jz      5f
    incq    %rdi
    incq    %rsi
    decl    %ecx
    jnz     4b
    jmp     1b
5:  ret

    .p2align 4
strncmp_sse2:
    pxor    %xmm0, %xmm0
1:  testq   %rdx, %rdx
    jz      6f
    cmpq    $16, %rdx
    jb      3f
    movl    %edi, %eax
    andl    $4095, %eax
    cmpl    $4080, %eax
    ja      3f
    movl    %esi, %eax
    andl    $4095, %eax
    cmpl    $4080, %eax
    ja      3f
    movdqu  (%rdi), %xmm1
    movdqu  (%rsi), %xmm2
    pcmpeqb %xmm1, %xmm2
    pminub  %xmm1, %xmm2
    pcmpeqb %xmm0, %xmm2
    pmovmskb %xmm2, %ecx
    testl   %ecx, %ecx
    jnz     2f
    addq    $16, %rdi
    addq    $16, %rsi
    subq    $16, %rdx
    jmp     1b
2:  bsfl    %ecx, %ecx
    movzbl  (%rdi,%rcx), %eax
    movzbl  (%rsi,%rcx), %edx
    subl    %edx, %eax
    ret
3:  movl    $16, %ecx
4:  movzbl  (%rdi), %eax
    movzbl  (%rsi), %r8d
    subl    %r8d, %eax
    jnz     5f
    testl   %r8d, %r8d
    jz      5f
    incq    %rdi
    incq    %rsi
    decq    %rdx
    jz      6f
    decl    %ecx
    jnz     4b
    jmp     1b
5:  ret
6:  xorl    %eax, %eax
    ret
