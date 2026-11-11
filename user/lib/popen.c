/*
 * popen and pclose (58c): a command run by sh -c with a pipe to its
 * standard input ("w") or from its standard output ("r"), as a stream.
 * The child starts with posix_spawn; the pipe's ends are FD_CLOEXEC, so a
 * command started later never holds another one's pipe open (POSIX: popen
 * streams of the caller are not passed on).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pathsearch.h"

#define OPEN_MAX_PIPES 32

static struct {
    FILE *f;
    pid_t pid;
} pipes[OPEN_MAX_PIPES];
static pthread_mutex_t pipes_lock = PTHREAD_MUTEX_INITIALIZER;

FILE *popen(const char *command, const char *mode)
{
    bool reading = mode && mode[0] == 'r' && !mode[1], writing = mode && mode[0] == 'w' && !mode[1];
    if (!reading && !writing) {
        errno = EINVAL;
        return 0;
    }
    int p[2];
    if (pipe(p) < 0)
        return 0;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    int mine = reading ? p[0] : p[1], theirs = reading ? p[1] : p[0];
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, theirs, reading ? 1 : 0);
    char *argv[] = { "sh", "-c", (char *)command, 0 };
    pid_t pid;
    int e = posix_spawn(&pid, SYSTEM_DIR "/bin/sh", &actions, 0, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(theirs);
    if (e) {
        close(mine);
        errno = e;
        return 0;
    }
    FILE *f = fdopen(mine, reading ? "r" : "w");
    if (!f) {
        close(mine);
        waitpid(pid, 0, 0);
        return 0;
    }
    pthread_mutex_lock(&pipes_lock);
    int slot = 0;
    while (slot < OPEN_MAX_PIPES && pipes[slot].f)
        slot++;
    if (slot < OPEN_MAX_PIPES) {
        pipes[slot].f = f;
        pipes[slot].pid = pid;
    }
    pthread_mutex_unlock(&pipes_lock);
    if (slot == OPEN_MAX_PIPES) {
        fclose(f);
        waitpid(pid, 0, 0);
        errno = EMFILE;
        return 0;
    }
    return f;
}

int pclose(FILE *f)
{
    pid_t pid = -1;
    pthread_mutex_lock(&pipes_lock);
    for (int i = 0; i < OPEN_MAX_PIPES; i++)
        if (pipes[i].f == f) {
            pid = pipes[i].pid;
            pipes[i].f = 0;
        }
    pthread_mutex_unlock(&pipes_lock);
    if (pid < 0) {
        errno = EINVAL;                             /* (not a popen stream) */
        return -1;
    }
    fclose(f);
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return status;
}
