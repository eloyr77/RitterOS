/* Waiting for child processes, and what their status means. */
#ifndef RITTEROS_SYS_WAIT_H
#define RITTEROS_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG                 1       /* return 0 at once if none has ended */

#define WIFEXITED(status)       (((status) & 0x7F) == 0)
#define WEXITSTATUS(status)     (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status)     (((status) & 0x7F) != 0)
#define WTERMSIG(status)        ((status) & 0x7F)

/* Wait for any child / for child pid (-1: any) to end; its ID, or -1 with
 * errno set (ECHILD: no such child). flags: 0 or WNOHANG. */
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int flags);

#endif
