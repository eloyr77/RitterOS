/*
 * su [USER]: become USER (root when none is named) for a shell: the user's
 * password is asked (not when root asks), then their shell runs in the
 * current folder until it ends (exit), and the one before is back. su is a
 * setuid program (50b-50c): owned by root with mode 4755 it runs as root
 * whoever starts it, getuid() telling who that was. Every attempt goes to
 * the log "login".
 */
#include <errno.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("su [USER]", "become another user (root if none) with their password");

#if defined(__x86_64__)
#define SHELL "/system/x86_64/bin/sh"
#else
#define SHELL "/system/aarch64/bin/sh"
#endif

/* A typed line, not shown on the screen; false at the end of input. */
static bool ask_hidden(const char *prompt, char *out, size_t size)
{
    printf("%s", prompt);
    ritteros_echo(false);
    ssize_t n = read(STDIN_FILENO, out, size - 1);
    ritteros_echo(true);
    printf("\n");                                   /* the Enter that was not shown */
    if (n <= 0)
        return false;
    out[n] = 0;
    if (out[n - 1] == '\n')
        out[n - 1] = 0;
    return true;
}

/* One line in the log "login". What a caller typed is shown with its
 * control characters as '?' (49k): a line break in a command's name would
 * otherwise write a second line of the caller's choosing into root's log. */
static void log_line(const char *text)
{
    char line[160];
    snprintf(line, sizeof(line), "%s\n", text);
    for (char *c = line; c[0] && c[1]; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7F)
            *c = '?';
    ritteros_log_write("login", line);
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        dprintf(STDERR_FILENO, "usage: su [USER]\n");
        return 2;
    }
    const char *name = argc == 2 ? argv[1] : "root";
    if (geteuid() != 0) {
        dprintf(STDERR_FILENO, "su: it does not run as root (it must be root's, with mode 4755)\n");
        return 1;
    }
    struct passwd *target = getpwnam(name);
    if (!target) {
        dprintf(STDERR_FILENO, "su: no user %s\n", name);
        return 1;
    }
    char tname[32];
    snprintf(tname, sizeof(tname), "%s", target->pw_name);
    uid_t tuid = target->pw_uid;
    gid_t tgid = target->pw_gid;
    uid_t caller = getuid();
    struct passwd *me = getpwuid(caller);
    char who[32], line[128];
    snprintf(who, sizeof(who), "%s", me ? me->pw_name : "?");
    if (caller != 0) {                              /* root is never asked */
        char password[128];
        if (!ask_hidden("Password: ", password, sizeof(password)))
            return 1;
        bool ok = password_check(tname, password) == 1;
        memset(password, 0, sizeof(password));
        if (!ok) {
            snprintf(line, sizeof(line), "su: %s to %s refused (a wrong password)", who, tname);
            log_line(line);
            sleep(2);                               /* (guessing stays slow, 49k) */
            dprintf(STDERR_FILENO, "su: wrong password\n");
            return 1;
        }
    }
    snprintf(line, sizeof(line), "su: %s became %s", who, tname);
    log_line(line);
    char *args[] = { "sh", 0 };
    int fds[3] = { -1, -1, -1 };
    /* A session of its own, in the foreground: the shell hands the terminal
     * to its jobs and takes it back (as login starts shells). */
    pid_t pid = ritteros_spawn_as(SHELL, args,
                                  SPAWN_NEW_GROUP | SPAWN_NEW_SESSION | SPAWN_FOREGROUND, fds,
                                  tuid, tgid);
    if (pid < 0) {
        dprintf(STDERR_FILENO, "su: %s: %s\n", SHELL, strerror(errno));
        return 1;
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WEXITSTATUS(status);
}
