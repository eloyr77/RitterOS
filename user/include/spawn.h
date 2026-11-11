/*
 * posix_spawn (55c): start a program in a new process, with file actions
 * and attributes applied on its way. The calls return 0 or an error
 * number (they do not set errno); *pid gets the child's number.
 *
 * Since 56b as POSIX describes it, by fork and exec: the child has every
 * descriptor of the caller not marked FD_CLOEXEC, then the file actions
 * done in order (on any descriptor); signals the caller ignores stay
 * ignored unless named by POSIX_SPAWN_SETSIGDEF, handled ones go back to
 * their default; it takes the caller's blocked signals, or the
 * attributes' with POSIX_SPAWN_SETSIGMASK. A failed action or exec is
 * posix_spawn's error (the child it made has ended then).
 */
#ifndef RITTEROS_SPAWN_H
#define RITTEROS_SPAWN_H

#include <signal.h>
#include <sys/types.h>

#define POSIX_SPAWN_RESETIDS    1   /* the child runs as the real user (giving up another: root only) */
#define POSIX_SPAWN_SETPGROUP   2   /* the child joins process group pgroup, 0: leads a new one */
#define POSIX_SPAWN_SETSIGDEF   4   /* the signals in sigdefault take their default action */
#define POSIX_SPAWN_SETSIGMASK  8   /* the child's blocked signals are sigmask */

struct __spawn_action;

typedef struct {
    int count, room;
    struct __spawn_action *actions;     /* (the library's: done in order) */
} posix_spawn_file_actions_t;

typedef struct {
    short flags;                        /* POSIX_SPAWN_* */
    pid_t pgroup;
    sigset_t sigmask, sigdefault;
} posix_spawnattr_t;

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]);
/* As posix_spawn, with a name without '/' looked up as execvp does (PATH,
 * else the system's bin and apps); a file without #! that is no binary
 * runs through sh. */
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attr, char *const argv[], char *const envp[]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions);
/* In the child: fd becomes PATH opened with oflag (and mode with O_CREAT). */
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions, int fd,
                                     const char *path, int oflag, mode_t mode);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int newfd);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd);

int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);
int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags);
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);  /* EINVAL: unknown flags */
int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup);
int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *mask);
int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *mask);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *signals);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *signals);

#endif
