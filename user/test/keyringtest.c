/*
 * keyringtest (70c, item 48): libkeyring as another program than the
 * keyring command calls it (keyring_shell_test): a program of its own, so
 * keyringd tells the two apart.
 *   keyringtest get SERVICE ACCOUNT      the secret, or the call's errno
 *   keyringtest set SERVICE ACCOUNT SECRET
 *   keyringtest remove SERVICE ACCOUNT
 *   keyringtest list                     each entry, then the count
 *   keyringtest peer                     the program at keyringd's end
 *                                        (SYS_PEER_PROGRAM, as a client)
 * Each prints one line per result: "keyringtest: ..." and an errno by name.
 * (70d) -s first: keyring_system(), the system keyring.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "keyring.h"
#include "ritteros.h"

static const char *name_of(int e)
{
    switch (e) {
    case ENOENT: return "ENOENT";
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case ECONNREFUSED: return "ECONNREFUSED";
    case EADDRINUSE: return "EADDRINUSE";
    case EINVAL: return "EINVAL";
    default: return strerror(e);
    }
}

static int failed(const char *what)
{
    printf("keyringtest: %s: %s\n", what, name_of(errno));
    return 1;
}

static void each(const char *service, const char *account, void *arg)
{
    (void)arg;
    printf("keyringtest: entry %s %s\n", service, account);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "-s")) {
        keyring_system();
        argc--, argv++;
    }
    if (argc == 4 && !strcmp(argv[1], "get")) {
        char secret[KEYRING_SECRET_MAX];
        ssize_t n = keyring_get(argv[2], argv[3], secret, sizeof(secret));
        if (n < 0)
            return failed("get");
        printf("keyringtest: secret %.*s\n", (int)n, secret);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "set")) {
        if (keyring_set(argv[2], argv[3], argv[4], strlen(argv[4])) < 0)
            return failed("set");
        printf("keyringtest: set\n");
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "remove")) {
        if (keyring_remove(argv[2], argv[3]) < 0)
            return failed("remove");
        printf("keyringtest: removed\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "list")) {
        int n = keyring_list(each, 0);
        if (n < 0)
            return failed("list");
        printf("keyringtest: %d entries\n", n);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "peer")) {
        char name[40], program[PROGRAM_PATH_MAX], small[8];
        snprintf(name, sizeof(name), "/run/keyring/%u", (unsigned)getuid());
        int fd = ritteros_local_connect(name);
        if (fd < 0)
            return failed("connect");
        int n = ritteros_peer_program(fd, program, sizeof(program));
        if (n < 0)
            return failed("peer");
        printf("keyringtest: peer %s (%d)\n", program, n);
        if (ritteros_peer_program(fd, small, sizeof(small)) < 0)
            printf("keyringtest: peer into 8 bytes: %s\n", errno == ENAMETOOLONG ? "ENAMETOOLONG"
                                                                                : name_of(errno));
        close(fd);
        return 0;
    }
    printf("usage: keyringtest get|set|remove|list|peer ...\n");
    return 2;
}
