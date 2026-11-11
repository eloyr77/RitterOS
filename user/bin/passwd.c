/*
 * passwd [NAME]: set a user's password (default: your own). Root only so
 * far: the passwords file belongs to root, and programs cannot yet run
 * with more rights than their user. The password is not shown while typed.
 */
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("passwd [NAME]", "set a user's password (root only so far)");

/* A line typed without echo, without its line break; false at the end.
 * (41h-7) Read a byte at a time: from a pipe, the next line stays for the
 * next question (as sudo, 68d). */
static bool ask(const char *prompt, char *out, size_t size)
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

int main(int argc, char **argv)
{
    if (argc > 2) {
        dprintf(STDERR_FILENO, "usage: passwd [NAME]\n");
        return 2;
    }
    struct passwd *pw = argc == 2 ? getpwnam(argv[1]) : getpwuid(getuid());
    if (!pw) {
        dprintf(STDERR_FILENO, "passwd: %s: no such user\n", argc == 2 ? argv[1] : "you");
        return 1;
    }
    char name[USER_NAME_MAX + 1];
    snprintf(name, sizeof(name), "%s", pw->pw_name);
    if (getuid() != 0) {
        dprintf(STDERR_FILENO, "passwd: only root may set passwords so far\n");
        return 1;
    }
    char first[128], second[128];
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
    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));
    if (r < 0) {
        dprintf(STDERR_FILENO, "passwd: %s: %s\n", PASSWORDS_FILE, strerror(errno));
        return 1;
    }
    printf("passwd: password for %s set\n", name);
    return 0;
}
