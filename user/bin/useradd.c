/*
 * useradd NAME: a new user (root only): the next free user ID from 1000 on,
 * a group of its own with the same number, and the home directory
 * /home/NAME, which it owns. passwd NAME then sets its password.
 */
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("useradd NAME", "add a user with a home directory (root only)");

int main(int argc, char **argv)
{
    if (argc != 2) {
        dprintf(STDERR_FILENO, "usage: useradd NAME\n");
        return 2;
    }
    const char *name = argv[1];
    if (!user_name_valid(name)) {
        dprintf(STDERR_FILENO, "useradd: %s: names are 1-%d of a-z 0-9 _ - (not starting "
                "with a digit or -)\n", name, USER_NAME_MAX);
        return 1;
    }
    if (getuid() != 0) {
        dprintf(STDERR_FILENO, "useradd: only root may add users\n");
        return 1;
    }
    if (getpwnam(name)) {
        dprintf(STDERR_FILENO, "useradd: %s: the user exists\n", name);
        return 1;
    }
    uid_t uid = 1000;
    while (getpwuid(uid))
        uid++;
    char line[160], home[128];
    snprintf(home, sizeof(home), "/home/%s", name);
    snprintf(line, sizeof(line), "%s:%u:%u:%s", name, uid, uid, home);
    struct stat st;
    if (stat("/home", &st) < 0 && mkdir("/home", 0755) < 0) {
        dprintf(STDERR_FILENO, "useradd: /home: %s\n", strerror(errno));
        return 1;
    }
    if (user_file_set(USERS_FILE, name, line, 0644) < 0) {
        dprintf(STDERR_FILENO, "useradd: %s: %s\n", USERS_FILE, strerror(errno));
        return 1;
    }
    if ((stat(home, &st) < 0 && mkdir(home, 0755) < 0) || chown(home, uid, uid) < 0) {
        dprintf(STDERR_FILENO, "useradd: %s: %s\n", home, strerror(errno));
        return 1;
    }
    printf("useradd: %s: user ID %u, home %s; set a password with: passwd %s\n", name, uid,
           home, name);
    return 0;
}
