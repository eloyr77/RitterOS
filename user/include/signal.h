/*
 * Signals. A signal ends the process it is sent to, unless the process
 * ignores it (SIG_IGN) or handles it (since 22r): a handler is a function
 * the kernel calls on the program's way back from the kernel (a system
 * call, an interrupt, a fault), on its own stack; when it returns, the
 * program goes on where it was. A waiting system call such as read that a
 * signal interrupts returns -1 with errno EINTR (it is not restarted), so
 * a shell notices Ctrl-C (SIGINT) at its prompt. Faults (SIGSEGV, SIGBUS,
 * SIGILL, SIGFPE) go to a handler too; one that returns runs the faulting
 * instruction again, so such a handler usually ends the program or jumps
 * away with longjmp. SIGKILL can be neither handled nor ignored.
 * (43b-5) Linux's numbers 1-31. Ignored unless handled: SIGCHLD (sent to a
 * parent when a child ends), SIGURG, SIGWINCH, SIGCONT, and the stop
 * signals (SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU: nothing can be stopped yet,
 * no job control); such a signal never interrupts a waiting call. kill's
 * pid 0 is the caller's group, -1 every process it may signal, -N group
 * N; signal 0 only asks (ESRCH, EPERM).
 */
#ifndef RITTEROS_SIGNAL_H
#define RITTEROS_SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include "../../common/syscalls.h"  /* (46b) not ritteros.h: POSIX names only */

typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

/* What a handler may change and the program read safely (22r). */
typedef int sig_atomic_t;

int kill(pid_t pid, int signal);
/* (22r) The handler (or SIG_DFL, SIG_IGN) for a signal, as glibc's and the
 * BSDs' signal: it stays set, and its signal waits while it runs. The old
 * one, or SIG_ERR. */
sighandler_t signal(int signal, sighandler_t action);
/* (22r) Send a signal to this program: its handler has run when raise
 * returns. */
int raise(int signal);

/* (22r) Sets of signals (bit n: signal n), and sigaction: a handler with
 * the signals it holds back while it runs (sa_mask, besides its own unless
 * SA_NODEFER) and SA_RESETHAND (back to SIG_DFL once called). SA_RESTART is
 * taken and does nothing (interrupted calls are never restarted here).
 * (43b-5) SA_SIGINFO: sa_sigaction(signal, info, context) is called (the
 * same place as sa_handler: every handler is given both), info as Linux's
 * siginfo_t (si_signo, si_code, si_pid, si_uid, si_status, si_addr), the
 * context a ucontext_t (sys/ucontext.h: the registers, which it may
 * change for the way back); SA_ONSTACK: on the thread's alternate stack
 * (sigaltstack). (44a) SA_NOCLDSTOP: no SIGCHLD when a child stops or
 * continues (job control), only when it ends; SA_NOCLDWAIT is
 * not done (ignoring SIGCHLD frees ended children: use that). */
typedef unsigned long sigset_t;
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESETHAND 0x80000000
#define SA_NODEFER   0x40000000
#define SA_RESTART   0x10000000
#define SA_ONESHOT   SA_RESETHAND
#define SA_NOMASK    SA_NODEFER

union sigval {
    int sival_int;
    void *sival_ptr;
};

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;                /* SI_USER, SI_KERNEL, SI_TKILL, CLD_EXITED, SEGV_MAPERR... */
    int __pad0;
    union {
        struct {
            pid_t si_pid;       /* the sender (kill, pthread_kill); SIGCHLD's child */
            uid_t si_uid;
            int si_status;      /* SIGCHLD: the exit code, or the signal that ended it */
        } __proc;
        void *si_addr;          /* a fault's address (SIGSEGV, SIGBUS; SIGILL, SIGFPE the
                                   instruction's) */
        int __pad[28];
    } __fields;
} siginfo_t;
#define si_pid      __fields.__proc.si_pid
#define si_uid      __fields.__proc.si_uid
#define si_status   __fields.__proc.si_status
#define si_addr     __fields.si_addr

struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } __handler;
    sigset_t sa_mask;
    int sa_flags;
};
#define sa_handler      __handler.sa_handler
#define sa_sigaction    __handler.sa_sigaction
int sigaction(int signal, const struct sigaction *action, struct sigaction *old);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signal);
int sigdelset(sigset_t *set, int signal);
int sigismember(const sigset_t *set, int signal);

/* (52f) The calling thread's blocked signals: SIG_BLOCK adds set, SIG_UNBLOCK
 * takes it away, SIG_SETMASK makes it the set (set 0: only ask; old, if
 * not 0, gets the set before). A blocked signal waits until a thread lets
 * it through, then is handled or ends the program; it does not interrupt
 * a blocking thread's waiting call. SIGKILL is never blocked. New threads
 * start with their creator's set, and so do programs started by spawn
 * (52i, as posix_spawn); signals waiting are not passed on.
 * 0, or -1 with errno set (EINVAL: how). */
#define SIG_BLOCK       SIGMASK_BLOCK
#define SIG_UNBLOCK     SIGMASK_UNBLOCK
#define SIG_SETMASK     SIGMASK_SET
int sigprocmask(int how, const sigset_t *set, sigset_t *old);

/* (52i) The signals waiting that the calling thread blocks; 0. */
int sigpending(sigset_t *set);
/* (52i) Block mask instead until a signal it lets through comes (ignored
 * ones do not count): its handler runs, then the set from before is back
 * and sigsuspend returns -1 with errno EINTR. Or the program ends. */
int sigsuspend(const sigset_t *mask);

/* (43a-3) killpg: a signal to a process group; psignal: "s: SIGNAL'S
 * WORDS" on the standard error; GNU's sigisemptyset, sigandset (both
 * sets' signals), sigorset (either set's). */
int killpg(pid_t group, int signal);

/* (43b-5) The thread's alternate stack for handlers with SA_ONSTACK (a
 * handler for a stack overflow needs one): ss_sp and ss_size, or
 * SS_DISABLE; the old one's ss_flags SS_ONSTACK while it runs there.
 * ENOMEM below MINSIGSTKSZ. Threads start without one; exec forgets it. */
typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;
#define SS_ONSTACK      SIGSTACK_ONSTACK
#define SS_DISABLE      SIGSTACK_DISABLE
#define MINSIGSTKSZ     SIGSTACK_MIN
#define SIGSTKSZ        16384
int sigaltstack(const stack_t *stack, stack_t *old);
#define NSIG            SIGNALS
#define _NSIG           SIGNALS
#define SIGPOLL         SIGIO
#define SIGIOT          SIGABRT
#define SIGCLD          SIGCHLD
void psignal(int signal, const char *s);
int sigisemptyset(const sigset_t *set);
int sigandset(sigset_t *dst, const sigset_t *a, const sigset_t *b);
int sigorset(sigset_t *dst, const sigset_t *a, const sigset_t *b);

#ifdef __cplusplus
}
#endif

#endif
