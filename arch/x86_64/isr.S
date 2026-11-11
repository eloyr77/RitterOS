/*
 * Exception and interrupt entry stubs.
 *
 * When an exception occurs the CPU aligns RSP to 16 bytes, pushes
 *     SS, RSP, RFLAGS, CS, RIP      and, for some vectors, an error code,
 * and jumps to the handler from the IDT. To give the C handler one uniform
 * layout (struct interrupt_frame), each stub pushes a dummy error code when
 * the CPU does not supply one, then the vector number. The common path saves
 * all general-purpose registers, calls exception_handler(frame), restores
 * them and returns with IRETQ.
 */

/* Exception without a CPU-pushed error code. */
.macro ISR_NOERR vector
isr\vector:
    pushq   $0
    pushq   $\vector
    jmp     isr_common
.endm

/* Exception for which the CPU pushes an error code. */
.macro ISR_ERR vector
isr\vector:
    pushq   $\vector
    jmp     isr_common
.endm

    .section .text
ISR_NOERR 0     /* #DE divide error */
ISR_NOERR 1     /* #DB debug */
ISR_NOERR 2     /*     NMI */
ISR_NOERR 3     /* #BP breakpoint */
ISR_NOERR 4     /* #OF overflow */
ISR_NOERR 5     /* #BR bound range */
ISR_NOERR 6     /* #UD invalid opcode */
ISR_NOERR 7     /* #NM device not available */
ISR_ERR   8     /* #DF double fault (error code always 0) */
ISR_NOERR 9     /*     coprocessor segment overrun (legacy) */
ISR_ERR   10    /* #TS invalid TSS */
ISR_ERR   11    /* #NP segment not present */
ISR_ERR   12    /* #SS stack-segment fault */
ISR_ERR   13    /* #GP general protection */
ISR_ERR   14    /* #PF page fault */
ISR_NOERR 15    /*     reserved */
ISR_NOERR 16    /* #MF x87 floating point */
ISR_ERR   17    /* #AC alignment check */
ISR_NOERR 18    /* #MC machine check */
ISR_NOERR 19    /* #XM SIMD floating point */
ISR_NOERR 20    /* #VE virtualization */
ISR_ERR   21    /* #CP control protection */
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28    /* #HV hypervisor injection */
ISR_ERR   29    /* #VC VMM communication */
ISR_ERR   30    /* #SX security */
ISR_NOERR 31

/*
 * Interrupts, vectors 32-255 (arch/x86_64/apic.c): one 16-byte stub each,
 * so that the stub of vector v is at irq_stubs + 16 * (v - 32).
 */
    .balign 16
    .global irq_stubs
irq_stubs:
    .set vector, 32
    .rept 224
    .balign 16
    pushq   $0
    pushq   $vector
    jmp     isr_common
    .set vector, vector + 1
    .endr

    /* (65a) From ring 3 (the saved CS's low bits), the kernel's GS base
     * first: user.S explains. The CPU's frame is above the vector and the
     * error code: CS at 24(%rsp). */
isr_common:
    testb   $3, 24(%rsp)
    jz      isr_save
    swapgs
    .global isr_save                /* also used by syscall_entry (user.S), swapped */
isr_save:
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    /* 22 quadwords were pushed since the CPU aligned RSP, so RSP is still
     * 16-byte aligned here, as the C calling convention requires. */
    cld
    movq    %rsp, %rdi          /* argument: struct interrupt_frame * */
    call    exception_handler

    .global isr_return              /* also used by user_resume (user.S) */
isr_return:
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax
    testb   $3, 24(%rsp)        /* (65a) back to ring 3: the program's GS base */
    jz      1f
    swapgs
1:  addq    $16, %rsp           /* drop vector number and error code */
    iretq

/* Addresses of the 32 stubs, used by idt_init() to fill the IDT. */
    .section .rodata
    .balign 8
    .global isr_stub_table
isr_stub_table:
.irp v, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .quad isr\v
.endr
