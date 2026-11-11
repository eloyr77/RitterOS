/*
 * Processes: programs (ELF executables from RitterFS) running in user mode,
 * each in its own address space (the lower half) and on a thread of its
 * own, scheduled like every other thread. They reach the kernel only through
 * system calls (common/syscalls.h).
 *
 * A process that ends stays as a "zombie" holding its status until its
 * parent collects it with wait; the kernel is the parent of the programs the
 * terminal starts. Children of an ended process become orphans and are freed
 * when they end. Processes belong to a group: the terminal starts each job
 * in a new group, children join their parent's group, and Ctrl-C stops the
 * foreground group (kernel/terminal.c).
 */
#ifndef RITTEROS_PROCESS_H
#define RITTEROS_PROCESS_H

#include <stdbool.h>
#include <stdint.h>

struct mmap_state;

struct vfs;
struct process;

/*
 * Load the program at path with the arguments argv[0..argc) (argv[0]: the
 * name it was started by), not yet running. A relative path is relative to
 * the parent's current directory, which the child inherits. parent 0: the
 * kernel (it waits with process_wait(0, ...)); the process then also leads
 * a new session. group 0: a new group led by the process.
 * envp: its environment (22t; envc strings "NAME=value", 0 for none).
 * Returns 0 and sets *error (ERR_*) if it cannot be loaded.
 */
struct process *process_create(struct vfs *fs, const char *path, int argc,
                               const char *const argv[], int envc, const char *const envp[],
                               struct process *parent, int group, int *error);
int process_id(const struct process *p);
int process_group(const struct process *p);

/* Start its thread. False (and the process is gone) if out of memory. */
bool process_start(struct process *p);

/* (35b) For the thread pageout: up to want pages of the next process (by
 * ID, after the last one asked) written to swap (mmap_pageout); *looked:
 * the pages looked at (0: no process). How many went. */
uint64_t process_pageout(uint64_t want, uint64_t *looked);

/* (35c-2) fn(&mm, space, ctx) for every live process (ending ones too; not
 * one in execve, whose memory is made new) with its memory, one at a time (each kept from being freed
 * meanwhile, as pageout's). For pageout, which takes no other's lock. */
void process_each_space(void (*fn)(struct mmap_state *m, uint64_t space, void *ctx), void *ctx);

/* (35b) swap off: every process's pages in swap read back
 * (mmap_swap_in_all): 0 or the first error. */
int64_t process_swap_in_all(void);

/* Wait until a child of parent (0: the kernel's) with this ID (-1: any)
 * has ended; returns its ID and status (common/syscalls.h), or -ERR_CHILD
 * if there is none, -ERR_INTR if the waiting process is being stopped.
 * no_hang: return 0 at once if such children exist but none has ended. */
int process_wait(struct process *parent, int pid, int *status, bool no_hang);

/* End a process, or every process of a group, with a signal: it stops at
 * its next return to user mode, or at once if it waits for input, a child
 * or time. Interrupt context is fine. process_signal returns -ERR_SRCH if
 * there is no such process, -ERR_PERM if it belongs to another user and
 * the sender (uid) is not root (49a: until then anyone could end root's
 * processes, login and the elves too). process_signal_group sends nothing
 * and returns false if no process of the group takes the signal (none
 * there, or all ignore it, like the shell at its prompt). */
int process_signal(int pid, int signal, uint32_t uid);
bool process_signal_group(int group, int signal);

/* A pseudo-terminal's master side closed: its processes end (SIGHUP). */
struct pty;
void process_hang_up(struct pty *t);

/* Ctrl-Alt-Delete: end the program that owns the screen (SYS_DISPLAY), so
 * the text console comes back even if it hangs. Does nothing if none owns
 * it. Interrupt context is fine (the keyboard calls it). */
void display_escape(void);

/* A short text for an ERR_* number, e.g. "No such file or directory". */
const char *process_error_text(int error);

/* One line per process (the terminal's ps command). */
void process_print_all(void);

/* --- called by the architecture code --------------------------------------- */

/* A system call of the running process; returns the result for it. */
int64_t process_syscall(uint64_t number, const uint64_t args[6]);

/* Just before returning to the program after an interrupt or a system call
 * (interrupts disabled; frame: its saved registers): end it if a signal
 * asked for that, or call its handler for a signal waiting (22r). */
void process_before_return(void *frame);
/* (22r) A fault in a program (frame as above): true if the program has a
 * handler for signal and it was called (the program goes on in it). */
bool process_fault_signal(void *frame, int signal);
/* (56a) SYS_FORK, which the architecture's system call code handles itself
 * (the child needs the caller's registers): frame is a copy of them with
 * the result register already 0, the child's. The child's ID, or -ERR_*. */
int64_t process_fork(const void *frame, uint64_t size);
/* (22r) SYS_SIGRETURN, which the architecture's system call code handles
 * itself (it restores every register, so no result is written). */
void process_sigreturn(void *frame);
/* Copies between the kernel and the calling program's memory, checked as
 * every system call's are (interrupts must be on: a page may be made). */
bool process_copy_to_user(uint64_t addr, const void *kernel, uint64_t size);
bool process_copy_from_user(void *kernel, uint64_t addr, uint64_t size);

/* The running program caused an exception (what: a description, pc: where,
 * address: the faulting address or ~0): report it, end it with signal. */
/* A program's page fault at address (22e): 0 if one of its mappings made the
 * page (anonymous memory's first touch, a copy-on-write page's first
 * write), so it goes on; else the signal that ends it (SIGSEGV, or SIGBUS
 * past a mapped file's end). Called with interrupts on. */
int process_page_fault(uint64_t address, bool write, bool exec);

__attribute__((noreturn)) void process_fault(const char *what, uint64_t pc, uint64_t address,
                                             int signal);

#endif
