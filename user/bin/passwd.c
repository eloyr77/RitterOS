/*
 * passwd [-S] [NAME]: set a user's password (default: your own). (69f, the
 * user's) A setuid program, as su and sudo (root's, mode 4755: it runs as
 * root whoever starts it, getuid() telling who that was): root sets
 * anyone's; any other user only their own, after giving the current one.
 * The password is not shown while typed. `-S` (as sudo's): the lines from
 * standard input without prompts and without the terminal's echo touched
 * (the settings app's Account and Users pages). Every change, and every
 * refusal for a wrong password, goes to the log "login". (70b) The keyring
 * follows your own new password: its key is sealed again under it, by
 * keyringd when it runs, else in the file itself (as you, not root).
 * Root, who is not asked the present password, cannot: the keyring keeps
 * the old one until `keyring password`.
 */
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"
#include "keyring_db.h"
#include "../lib/keyring_wire.h"

PROGRAM_HELP("passwd [-S] [NAME]", "set a user's password: anyone's for root, else your own");

static bool quiet;                      /* -S */

/* A line typed without echo, without its line break; false at the end.
 * (41h-7) Read a byte at a time: from a pipe, the next line stays for the
 * next question (as sudo, 68d). */
static bool ask(const char *prompt, char *out, size_t size)
{
    if (!quiet) {
        printf("%s", prompt);
        ritteros_echo(false);
    }
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
    if (!quiet) {
        ritteros_echo(true);
        printf("\n");
    }
    if (n && out[n - 1] == '\r')
        n--;
    out[n] = 0;
    return any;
}

/* One line in the log "login", its control characters as '?' (as sudo's). */
static void log_line(const char *text)
{
    char line[200];
    snprintf(line, sizeof(line), "%s\n", text);
    for (char *c = line; c[0] && c[1]; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7F)
            *c = '?';
    ritteros_log_write("login", line);
}

/* (70b) pw's keyring sealed again from old to new: through keyringd, or
 * (none runs) in ~/.config/keyring.db by a child running as the user (so
 * the new file is the user's). Silent when there is no keyring. */
static void keyring_follow(const struct passwd *pw, const char *old, const char *new)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/keyring.db", pw->pw_dir);
    int fd = kw_connect((unsigned)pw->pw_uid);
    if (fd >= 0) {
        static struct kw_message m;
        struct kw_field f[2] = { { old, (uint32_t)strlen(old) }, { new, (uint32_t)strlen(new) } };
        bool asked = kw_ask(fd, KW_PASSWORD, f, 2, &m);
        close(fd);
        if (asked && m.code == KW_OK)
            printf("passwd: the keyring follows the new password\n");
        else if (asked && m.code == KW_WRONG)
            printf("passwd: the keyring did not open with the old password; it keeps its own\n");
        else
            printf("passwd: the keyring keeps the old password (keyringd: not saved)\n");
        return;
    }
    struct stat st;
    if (stat(path, &st) < 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0)
            _exit(3);
        struct keyring k;
        const char *why = "";
        if (keyring_load(path, old, &k, &why) != 1)
            _exit(2);
        int r = keyring_rewrap(&k, new) && keyring_save(path, &k) == 0 ? 0 : 1;
        keyring_free(&k);
        _exit(r);
    }
    int status = -1;
    while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    int code = pid > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (code == 0)
        printf("passwd: the keyring follows the new password\n");
    else if (code == 2)
        printf("passwd: the keyring did not open with the old password; it keeps its own\n");
    else
        printf("passwd: the keyring keeps the old password (not saved)\n");
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "-S")) {
        quiet = true;
        argc--, argv++;
    }
    if (argc > 2) {
        dprintf(STDERR_FILENO, "usage: passwd [-S] [NAME]\n");
        return 2;
    }
    uid_t caller = getuid();
    struct passwd *me = getpwuid(caller);
    char who[USER_NAME_MAX + 1], line[200];
    snprintf(who, sizeof(who), "%s", me ? me->pw_name : "?");
    struct passwd *pw = argc == 2 ? getpwnam(argv[1]) : me;
    if (!pw) {
        dprintf(STDERR_FILENO, "passwd: %s: no such user\n", argc == 2 ? argv[1] : "you");
        return 1;
    }
    char name[USER_NAME_MAX + 1];
    snprintf(name, sizeof(name), "%s", pw->pw_name);
    /* (70b) Who, kept: the lookups below may reuse pw's memory. */
    struct passwd target = *pw;
    char target_dir[160];
    snprintf(target_dir, sizeof(target_dir), "%s", pw->pw_dir);
    target.pw_dir = target_dir;
    target.pw_name = name;
    if (geteuid() != 0) {
        dprintf(STDERR_FILENO, "passwd: it does not run as root (it must be root's, with mode 4755)\n");
        return 1;
    }
    if (caller != 0 && pw->pw_uid != caller) {
        dprintf(STDERR_FILENO, "passwd: only root may set another user's password\n");
        return 1;
    }
    char current[128], first[128], second[128];
    if (caller != 0) {                          /* your own: the current one first */
        if (!ask("Current password: ", current, sizeof(current)))
            return 1;
        bool ok = password_check(name, current) == 1;
        if (!ok) {
            memset(current, 0, sizeof(current));
            snprintf(line, sizeof(line), "passwd: %s refused (a wrong password)", who);
            log_line(line);
            sleep(2);                           /* (guessing stays slow, as sudo) */
            dprintf(STDERR_FILENO, "passwd: wrong password; nothing changed\n");
            return 1;
        }
    }
    if (!ask("New password: ", first, sizeof(first)) ||
        !ask("Again: ", second, sizeof(second)))
        return 1;
    if (strcmp(first, second) != 0) {
        dprintf(STDERR_FILENO, "passwd: the two differ; nothing changed\n");
        return 1;
    }
    if (first[0] == 0) {
        dprintf(STDERR_FILENO, "passwd: an empty password is not allowed\n");
        return 1;
    }
    int r = password_set(name, first);
    memset(second, 0, sizeof(second));
    if (r < 0) {
        memset(first, 0, sizeof(first));
        memset(current, 0, sizeof(current));
        dprintf(STDERR_FILENO, "passwd: %s: %s\n", PASSWORDS_FILE, strerror(errno));
        return 1;
    }
    snprintf(line, sizeof(line), "passwd: %s set the password for %s", who, name);
    log_line(line);
    printf("passwd: password for %s set\n", name);
    if (caller != 0) {
        keyring_follow(&target, current, first);
    } else {
        char path[512];
        struct stat st;
        snprintf(path, sizeof(path), "%s/.config/keyring.db", target_dir);
        if (stat(path, &st) == 0)
            printf("passwd: %s's keyring keeps its old password (keyring password changes it)\n",
                   name);
    }
    memset(first, 0, sizeof(first));
    memset(current, 0, sizeof(current));
    return 0;
}
