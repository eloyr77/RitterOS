/* Waiting for child processes, and what their status means. */
#ifndef RITTEROS_SYS_WAIT_H
#define RITTEROS_SYS_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#define WNOHANG                 1       /* return 0 at once if none has ended */
#define WUNTRACED               2       /* (44a) also a child that stopped (job control) */
#define WCONTINUED              8       /* (44a) also a stopped child that SIGCONT made go on */

/* What a status says (44a: Linux's encoding): ended by exit (WEXITSTATUS:
 * its code), by a signal (WTERMSIG), stopped (WSTOPSIG: by which signal),
 * or continued. */
#define WIFEXITED(status)       (((status) & 0x7F) == 0)
#define WEXITSTATUS(status)     (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status)     (((status) & 0x7F) != 0 && ((status) & 0x7F) != 0x7F)
#define WTERMSIG(status)        ((status) & 0x7F)
#define WIFSTOPPED(status)      (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status)        WEXITSTATUS(status)
#define WIFCONTINUED(status)    ((status) == 0xFFFF)

/* Wait for any child / for child pid (-1: any) to end; its ID, or -1 with
 * errno set (ECHILD: no such child). flags: 0 or WNOHANG, (44a) WUNTRACED
 * and WCONTINUED: also a child's stop and its continuing, each reported
 * once. */
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int flags);

/* (43a-3) BSD's waits with the child's use (a struct rusage of
 * sys/resource.h; a child's use is not kept: zeros). */
struct rusage;
pid_t wait3(int *status, int options, struct rusage *usage);
pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage);

#ifdef __cplusplus
}
#endif

#endif
