/*
 * Test: a session without a terminal (49k). Run by root at the console.
 *   noterminal        starts itself again in a session of its own whose input
 *                     is a pipe (as a command run over SSH without a terminal
 *                     would make one), which then tries what only the
 *                     console's user may: take the console's foreground, switch
 *                     its echo off, and (as user 1000) take the screen and the
 *                     keyboard and open the sound device. Each is "refused".
 *   noterminal sudo   runs sudo as user 1000 with a line break in the command's
 *                     name: the log "login" must get one line, the break a '?'.
 *   noterminal shared (65e) starts a session of its own that keeps the console
 *                     (its input), whose leader starts a member in a group of
 *                     its own: the member may not take the console's
 *                     foreground, which belongs to the console shell's session
 *                     (as sshd's commands on pipes may not).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"

#if defined(__x86_64__)
#define SYSTEM "/system/x86_64"
#else
#define SYSTEM "/system/aarch64"
#endif

static void report(const char *what, int r)
{
    if (r < 0)
        printf("noterminal: %s: refused (%s)\n", what, strerror(errno));
    else
        printf("noterminal: %s: allowed\n", what);
}

static int child(void)
{
    report("console foreground", tcsetpgrp(1, getpgrp()));
    report("console echo off", ritteros_echo(false));
    ritteros_echo(true);
    if (setuid(1000) < 0) {
        printf("noterminal: setuid: %s\n", strerror(errno));
        return 1;
    }
    int r = ritteros_display_acquire();
    report("screen and keyboard as user 1000", r);
    if (r == 0)
        ritteros_display_release();
    int fd = open("/dev/pcm0", O_WRONLY);
    report("sound device as user 1000", fd);
    if (fd >= 0)
        close(fd);
    return 0;
}

static int wait_for(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WEXITSTATUS(status);
}

/* (65e) A member of a session that shares the console. */
static int member(void)
{
    report("console foreground from another session sharing the console",
           tcsetpgrp(1, getpgrp()));
    return 0;
}

static int leader(char *self)
{
    char *av[] = { self, "member", 0 };
    int fds[3] = { 0, 1, 2 };
    pid_t pid = ritteros_spawn(self, av, SPAWN_NEW_GROUP, fds);
    if (pid < 0) {
        printf("noterminal: spawn member: %s\n", strerror(errno));
        return 1;
    }
    return wait_for(pid);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "child"))
        return child();
    if (argc == 2 && !strcmp(argv[1], "member"))
        return member();
    if (argc == 2 && !strcmp(argv[1], "leader"))
        return leader(argv[0]);
    if (argc == 2 && !strcmp(argv[1], "shared")) {
        char *av[] = { argv[0], "leader", 0 };
        int fds[3] = { 0, 1, 2 };                   /* the console: kept as its terminal */
        pid_t pid = ritteros_spawn(argv[0], av, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
        if (pid < 0) {
            printf("noterminal: spawn leader: %s\n", strerror(errno));
            return 1;
        }
        wait_for(pid);
        printf("noterminal: shared done\n");
        return 0;
    }
    int p[2];
    if (ritteros_pipe(p) < 0) {
        printf("noterminal: pipe: %s\n", strerror(errno));
        return 1;
    }
    close(p[1]);                                    /* (its input: at its end at once) */
    int fds[3] = { p[0], 1, 2 };
    if (argc == 2 && !strcmp(argv[1], "sudo")) {
        char *av[] = { "sudo", "x\nsudo: FORGED", 0 };
        pid_t pid = ritteros_spawn_as(SYSTEM "/bin/sudo", av, SPAWN_NEW_GROUP, fds, 1000, 1000);
        if (pid < 0) {
            printf("noterminal: sudo: %s\n", strerror(errno));
            return 1;
        }
        printf("noterminal: sudo ended with %d\n", wait_for(pid));
        return 0;
    }
    char *av[] = { argv[0], "child", 0 };
    pid_t pid = ritteros_spawn(argv[0], av, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
    if (pid < 0) {
        printf("noterminal: spawn: %s\n", strerror(errno));
        return 1;
    }
    wait_for(pid);
    printf("noterminal: done\n");
    return 0;
}
