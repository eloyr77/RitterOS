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
 */
#ifndef RITTEROS_SIGNAL_H
#define RITTEROS_SIGNAL_H

#include <sys/types.h>
#include "ritteros.h"

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
 * There is no SA_SIGINFO. */
typedef unsigned long sigset_t;
#define SA_RESETHAND 0x80000000
#define SA_NODEFER   0x40000000
#define SA_RESTART   0x10000000
struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
};
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

#endif
