/*
 * What each architecture provides to the shared kernel code.
 *
 * The implementation lives in arch/<arch>/, which is on the include path; its
 * arch.h includes this file and adds the small inline helpers listed at the
 * end. Shared code includes "arch.h" and nothing else from arch/.
 */
#ifndef RITTEROS_ARCH_API_H
#define RITTEROS_ARCH_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Set up the CPU's own tables (x86: GDT, TSS, IDT) so that exceptions are
 * reported. Called early, before paging. */
void arch_cpu_init(void);

/* Raise a breakpoint exception and come back, proving that the exception
 * path works in both directions; logs what happens. */
void arch_exception_selftest(void);

/* Stop the CPU for good: interrupts off, wait forever. */
__attribute__((noreturn)) void arch_halt(void);

/* Busy-wait for the given number of milliseconds. */
void arch_delay_ms(uint32_t ms);

/* A fine-grained clock for measuring: a counter that runs from the first
 * instruction (x86: the time stamp counter; ARM: CNTVCT_EL0), and its
 * frequency in Hz, 0 until known (x86: measured with the PIT when the
 * interrupts are set up). */
uint64_t arch_counter(void);
uint64_t arch_counter_hz(void);

/* (64t) The processor's clock while it runs: its real cycles and cycles at
 * the counter's rate (arch_counter_hz), both counted only while it is not
 * halted (x86: APERF and MPERF). False where it has no such counters. */
bool arch_clock_counts(uint64_t *actual, uint64_t *base);

/* (64t) The processor's clock setting, /system/config/cpu's `clock`
 * (kernel/cpuclock.c reads it): applied where the processor can, with what
 * was done described in out (one line); false where it cannot. */
enum arch_clock {
    ARCH_CLOCK_PERFORMANCE,     /* it chooses, favouring speed (the default) */
    ARCH_CLOCK_BALANCED,        /* it chooses, balancing speed and power */
    ARCH_CLOCK_POWERSAVE,       /* it chooses, favouring low power */
    ARCH_CLOCK_TOP,             /* its highest clock whenever it runs */
    ARCH_CLOCK_FIRMWARE,        /* the firmware's own settings, untouched */
};
bool arch_clock_set(enum arch_clock mode, char *out, size_t size);

/* (65b) The processor's other cores. arch_cpus_list fills list with the
 * cores the firmware names (ACPI's MADT), the boot core marked, and returns
 * how many there are (at most max); a core that cannot be started has the
 * reason in `refused`. arch_cpu_start starts c (c->hw_id) on a stack ending
 * at c->stack_top: the core sets itself up (c becomes its block; 65c: its
 * interrupts and tick too) and calls cpu_online(c) (kernel/cpus.h), which
 * makes its code its idle thread; true once it reported in, false after a
 * limit. arch_wake_core interrupts core c so that it looks at the run
 * queue (65c); arch_stop_other_cores halts every core but this one (panic,
 * power off, reboot), also those with their interrupts off where it can. */
struct cpu;
struct cpu_entry {
    uint64_t hw_id;                     /* x86: Local APIC ID; ARM: MPIDR's affinity */
    bool boot;                          /* the core running now */
    const char *refused;                /* why it cannot be started, or 0 */
};
int arch_cpus_list(struct cpu_entry *list, int max);
bool arch_cpu_start(struct cpu *c);
bool arch_wake_core(struct cpu *c);
void arch_stop_other_cores(void);

/* Set up the interrupt controller and a timer that interrupts hz times a
 * second, calling timer_tick() (kernel/timer.h) each time; logs what it
 * found. Interrupts stay masked until arch_interrupts_enable(). Returns false
 * if the machine lacks what is needed. Uses ACPI (acpi_init must have run). */
bool arch_interrupts_init(uint32_t hz);

/* A device interrupt handler; runs with interrupts masked. */
typedef void (*irq_handler_fn)(void *context);

/* Reserve an interrupt for a PCI device's message-signaled interrupts
 * (MSI-X): the device raises it by writing *data to the physical address
 * *address, and each time handler(context) runs. Returns false if none is
 * left or the machine cannot receive them. */
bool arch_msi_allocate(irq_handler_fn handler, void *context, uint64_t *address, uint32_t *data);

/* (65d) Route the message interrupt (address, data from arch_msi_allocate)
 * to core c: x86 puts the core's APIC ID into *address (the device must be
 * given the new message), ARM routes the SPI (data) in the GIC, leaving the
 * message as it is. False if it cannot. */
struct cpu;
bool arch_msi_route(uint64_t *address, uint32_t data, const struct cpu *c);

/* Deliver the platform's interrupt line gsi (an ACPI Global System
 * Interrupt: the touchpad's GPIO controller's, 15u) to handler(context),
 * level- or edge-triggered, active low or high; *vector for the log.
 * False where lines are not routed this way (ARM so far) or none is left. */
bool arch_route_gsi(uint32_t gsi, bool level, bool active_low, irq_handler_fn handler,
                    void *context, unsigned *vector);

/* (37d) power's line about the processor's own power features: how it
 * can idle (x86: MWAIT, whether interrupts break it; whether the APIC
 * timer keeps running in deep states, ARAT) and the package's power over
 * one second from its energy counter (RAPL), where it has one. Sleeps 1 s. */
void arch_processor_power_print(void);

/* (37d-2) The idle thread's sleep: arch_idle_setup takes the processors'
 * sleep states (the firmware's _CST, the first processor's; 0 states:
 * C1 alone) and logs what it will use; arch_idle sleeps in the deepest
 * that fits this core's recent idle periods, called with interrupts off,
 * returning with them on (as arch_wait_for_interrupt), counting per state
 * in struct cpu; arch_idle_print is power's line about them. */
struct acpi_cstate;
void arch_idle_setup(const struct acpi_cstate *states, int count);
void arch_idle(void);
void arch_idle_print(void);

/* (37c-2a) The FADT's SCI, ISA interrupt irq, to handler(context): x86 as
 * the MADT's override says, else level-triggered and active low (ACPI's
 * SCI); *gsi, *level, *active_low and *vector for the log. False where
 * there is none (ARM: hardware-reduced ACPI) or no vector is left. */
bool arch_route_sci(uint32_t irq, irq_handler_fn handler, void *context, uint32_t *gsi,
                    bool *level, bool *active_low, unsigned *vector);

/* (37c-2a) Mask the line gsi (routed by the two above): an interrupt that
 * keeps coming with nothing to handle, (37c-2b) a level one until the
 * thread has handled it, then unmasked. From its handler too. */
void arch_gsi_mask(uint32_t gsi);
void arch_gsi_unmask(uint32_t gsi);

/* Receive on the serial port that serial_putc writes to: every byte that
 * arrives is passed to receive(byte), from the interrupt handler. Logs the
 * port and its interrupt. Returns false if that is not possible. Needs
 * arch_interrupts_init. */
bool arch_serial_input_init(void (*receive)(uint8_t byte));

/* Keyboards built into the machine rather than on USB (x86: a PS/2
 * keyboard on the i8042; ARM: none). Called after USB, whose firmware
 * legacy mode may imitate them until then. Logs what it found. */
void arch_input_init(void);

/* Switch the machine off, or restart it (x86: ACPI, ARM: PSCI). Log the
 * method; return only if it did not work. */
void arch_power_off(void);

/* (37b-4) An I/O port read or written, width 8, 16 or 32 bits (the AML
 * interpreter's SystemIO): false where there are none (ARM). */
bool arch_port_access(uint16_t port, int width, uint32_t *value, bool write);
void arch_reboot(void);

/* (37g-1) Sleep in ACPI S3 (kernel/sleep.c does the firmware's and the
 * devices' part around it): every core's place kept, the RTC's alarm set
 * seconds ahead, the sleep types written; back here once awake with every
 * core taking its place again: 0, how long it slept and how many cores
 * came back; -1 with why (nothing lost: it did not sleep). */
struct arch_sleep_result {
    const char *why;
    unsigned slept_s;
    int cores, cores_back;
    uint64_t resume_ms;
    int asked_on;                       /* (37g-5d) the core the sleep was asked on */
};
int arch_sleep(uint8_t type_a, uint8_t type_b, unsigned seconds, struct arch_sleep_result *r);

/*
 * User mode. Each program runs on a thread of its own: arch_enter_user
 * leaves the kernel for good, entering user mode at entry with the stack
 * pointer user_stack. From then on the thread's system calls, faults and
 * interrupts are handled on its kernel stack, which ends at
 * kernel_stack_top: system calls go to process_syscall, faults to
 * process_fault (kernel/process.h). The program's address space must be
 * active. arch_set_kernel_stack tells the CPU the kernel stack of the thread
 * that runs next (x86: TSS.RSP0 and the SYSCALL stack; ARM: nothing, the
 * entry code keeps SP_EL1 right).
 */
void arch_user_init(void);
__attribute__((noreturn)) void arch_enter_user(uint64_t entry, uint64_t user_stack,
                                               uint64_t kernel_stack_top);
/* (56a) fork's child: leave the kernel for good into a program's saved
 * registers (frame: as the architecture's system call code has them, at
 * most ARCH_FRAME_MAX bytes; it may be on this thread's own kernel stack),
 * the thread's kernel stack ending at kernel_stack_top. Interrupts off. */
#define ARCH_FRAME_MAX 512
__attribute__((noreturn)) void arch_resume_user(const void *frame, uint64_t kernel_stack_top);
void arch_set_kernel_stack(uint64_t kernel_stack_top);

/*
 * Threads (kernel/sched.c). arch_thread_init prepares a new thread's kernel
 * stack (ending at stack_top) so that the first switch to it calls
 * sched_thread_start() and then fn(arg); it returns the stack pointer to
 * save. arch_context_switch saves the callee-saved registers on the current
 * stack, stores the stack pointer in *save, loads load and returns there:
 * into the other thread. Interrupts must be disabled.
 */
uint64_t arch_thread_init(uint64_t stack_top, void (*fn)(void *arg), void *arg);
void arch_context_switch(uint64_t *save, uint64_t load);

/*
 * FP/SIMD registers of programs (the kernel never uses them). The state of
 * a program's thread lives in an area of ARCH_FPU_STATE_SIZE bytes (arch.h),
 * 64-byte aligned. arch_fpu_clean fills one with the state a new program
 * starts with; arch_user_init switches FP/SIMD on for programs.
 */
void arch_fpu_save(void *area);
void arch_fpu_restore(const void *area);
void arch_fpu_clean(void *area);

/*
 * Signal handlers (22r, arch/<arch>/signal.c). arch_signal_enter turns the
 * return to a program (frame: the saved registers, as the architecture's
 * exception code has them) into a call of handler(signal): the registers,
 * FP/SIMD ones included, and blocked are kept in a context on the program's
 * own stack, and the handler returns to restorer, which calls SYS_SIGRETURN
 * with the context's address. False if the stack cannot take it.
 * arch_signal_return puts a context back into frame (its address: the
 * system call's first argument), checked so that only a program's state
 * can come back, and gives its blocked set; false if it is not one.
 * Both are called with interrupts off and switch them on to copy.
 */
bool arch_signal_enter(void *frame, uint64_t handler, int signal, uint64_t restorer,
                       uint64_t blocked);
bool arch_signal_return(void *frame, uint64_t *blocked);

/*
 * A program thread's thread pointer (22i): the register its per-thread data
 * is found from -- x86 the FS base, ARM TPIDR_EL0. The scheduler saves it
 * from the thread leaving and loads it for the one coming.
 */
uint64_t arch_thread_pointer(void);
void arch_set_thread_pointer(uint64_t tp);

/* (65c-3) What else of a program thread's registers stays in the core and
 * not in its frames, saved and loaded the same way: x86 the GS selector (a
 * program may load its own; it moved with nothing on one core, and on
 * several a thread found another core's), ARM nothing (0). */
uint64_t arch_user_segments(void);
void arch_set_user_segments(uint64_t segments);

/* Code was written to memory at addr (kernel address) and will be executed:
 * make the instruction fetches see it (ARM: clean and invalidate caches). */
void arch_code_written(const void *addr, uint64_t size);

/* Architecture-specific deliberate crashes ("crash=<kind>"): trigger the kind
 * and do not return, or return false if this architecture has no such kind.
 * arch_crash_kinds lists them, separated by spaces. */
bool arch_crash_test(const char *kind);
extern const char arch_crash_kinds[];

/*
 * Inline helpers each arch.h defines:
 *   ARCH_NAME                 "x86_64", "aarch64"
 *   ARCH_RETURN_INSTRUCTION   bytes of a "return" instruction, as an
 *                             initializer (used by the NX crash test)
 *   arch_cpu_relax()          hint inside a busy-wait loop
 *   arch_write_barrier()      earlier memory writes are visible to devices
 *                             before later ones (e.g. a doorbell register)
 *   arch_read_barrier()       later memory reads happen after earlier ones
 *                             (e.g. after seeing a device's completion flag)
 *   mmio_read8/16/32/64(addr), mmio_write8/16/32/64(addr, value)
 *                             device register access; every access to
 *                             device memory goes through these (on ARM
 *                             under a hypervisor, the instruction must be
 *                             one it can emulate)
 *   arch_interrupts_enable()  let interrupts in (x86: sti, ARM: unmask IRQ)
 *   arch_interrupts_disable() keep them out (x86: cli, ARM: mask IRQ)
 *   arch_irq_save()           disable interrupts, return the previous state
 *   arch_irq_restore(state)   go back to that state (spinlocks, kernel/sync.h)
 *   arch_wait_for_interrupt() called with interrupts disabled: enable them
 *                             and sleep until the next interrupt, as one
 *                             step, so that an interrupt arriving after the
 *                             caller's last check still wakes it. Returns
 *                             with interrupts enabled. Usage:
 *                               for (;;) {
 *                                   arch_interrupts_disable();
 *                                   if (done) break;
 *                                   arch_wait_for_interrupt();
 *                               }
 *                               arch_interrupts_enable();
 */

#endif
