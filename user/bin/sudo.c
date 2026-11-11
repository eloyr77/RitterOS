/*
 * sudo COMMAND [ARGUMENTS...]: run one command as root. Allowed for the users
 * named in /system/config/sudo (`users = anna, bob`; without the file only
 * root), each with their own password, (68e) asked again only after 5
 * minutes in the same session (the kernel's stamp on the session's leader,
 * SYS_SESSION_STAMP; `sudo -k` forgets it); (41h-7) never for the users
 * also in the file's `nopassword` list (a deploying program, an agent). The command is a
 * program (a bare name is looked for in the system's bin, then apps
 * folder) and runs in the current folder with the terminal; sudo ends with
 * its exit code. A setuid program (50b-50c): owned by root with mode 4755 it
 * runs as root whoever starts it, getuid() telling who that was. Every
 * attempt goes to the log "login".
 */
#include <errno.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("sudo COMMAND [ARGUMENTS...] | -k",
             "run a command as root, with your own password (users in /system/config/sudo)");

#define SUDO_FILE "/system/config/sudo"
#define REMEMBERED 300                          /* (68e) seconds a correct password counts */

#if defined(__x86_64__)
#define SYSTEM "/system/x86_64"
#else
#define SYSTEM "/system/aarch64"
#endif

/* The password: one line, read a byte at a time (68d: from a pipe, what
 * follows the line stays for the command); false if none came. */
static bool ask_hidden(const char *prompt, char *out, size_t size)
{
    printf("%s", prompt);
    ritteros_echo(false);
    size_t n = 0;
    bool any = false;
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        any = true;
        if (c == '\n')
            break;
        if (n < size - 1)
            out[n++] = c;
    }
    ritteros_echo(true);
    printf("\n");
    if (n && out[n - 1] == '\r')
        n--;
    out[n] = 0;
    return any;
}

/* One line in the log "login". What a caller typed is shown with its
 * control characters as '?' (49k): a line break in a command's name would
 * otherwise write a second line of the caller's choosing into root's log. */
static void log_line(const char *text)
{
    char line[200];
    snprintf(line, sizeof(line), "%s\n", text);
    for (char *c = line; c[0] && c[1]; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7F)
            *c = '?';
    ritteros_log_write("login", line);
}

/* Is name in the file's list `key` (`users`; 41h-7 `nopassword`: names
 * apart by commas or spaces)? */
static bool listed(const char *key, const char *name)
{
    char list[512];
    if (config_get(SUDO_FILE, key, list, sizeof(list)) <= 0)
        return false;
    size_t n = strlen(name);
    for (const char *c = list; *c;) {
        while (*c == ',' || *c == ' ' || *c == '\t')
            c++;
        const char *start = c;
        while (*c && *c != ',' && *c != ' ' && *c != '\t')
            c++;
        if ((size_t)(c - start) == n && !strncmp(start, name, n))
            return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: sudo COMMAND [ARGUMENTS...] | -k\n");
        return 2;
    }
    if (argc == 2 && !strcmp(argv[1], "-k")) {      /* (68e) forget: the next one asks */
        ritteros_session_stamp(STAMP_FORGET);
        return 0;
    }
    if (geteuid() != 0) {
        printf("sudo: it does not run as root (it must be root's, with mode 4755)\n");
        return 1;
    }
    uid_t caller = getuid();
    struct passwd *me = getpwuid(caller);
    char who[32], line[200];
    snprintf(who, sizeof(who), "%s", me ? me->pw_name : "?");
    if (caller != 0) {
        if (!me || !listed("users", who)) {
            snprintf(line, sizeof(line), "sudo: %s may not run commands as root (%s refused)",
                     who, argv[1]);
            log_line(line);
            printf("sudo: %s may not run commands as root (not in %s)\n", who, SUDO_FILE);
            return 1;
        }
        int age = ritteros_session_stamp(STAMP_AGE);     /* (68e) a password moments ago */
        if ((age >= 0 && age < REMEMBERED) || listed("nopassword", who))   /* (41h-7) */
            goto allowed_now;
        char prompt[64], password[128];
        snprintf(prompt, sizeof(prompt), "Password for %s: ", who);
        if (!ask_hidden(prompt, password, sizeof(password))) {
            printf("sudo: no password given\n");   /* (68d: not silently) */
            return 1;
        }
        bool ok = password_check(who, password) == 1;
        memset(password, 0, sizeof(password));
        if (!ok) {
            snprintf(line, sizeof(line), "sudo: %s refused (a wrong password) for %s", who, argv[1]);
            log_line(line);
            sleep(2);                               /* (guessing stays slow, 49k) */
            printf("sudo: wrong password\n");
            return 1;
        }
        ritteros_session_stamp(STAMP_SET);          /* (68e) for the next 5 minutes */
    }
allowed_now:;
    char path[300];
    struct stat st;
    if (strchr(argv[1], '/')) {
        snprintf(path, sizeof(path), "%s", argv[1]);
    } else {
        snprintf(path, sizeof(path), "%s/bin/%s", SYSTEM, argv[1]);
        if (stat(path, &st) < 0)
            snprintf(path, sizeof(path), "%s/apps/%s.program/%s", SYSTEM, argv[1], argv[1]);   /* (42b) */
    }
    snprintf(line, sizeof(line), "sudo: %s ran %s as root", who, argv[1]);
    log_line(line);
    int fds[3] = { -1, -1, -1 };
    /* In sudo's own group: it has the terminal, and Ctrl-C reaches both. */
    pid_t pid = ritteros_spawn_as(path, argv + 1, 0, fds, 0, 0);
    if (pid < 0) {
        printf("sudo: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WEXITSTATUS(status);
}
