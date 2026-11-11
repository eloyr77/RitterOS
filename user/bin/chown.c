/*
 * chown UID[:GID] PATH...: give files to another user (and group), by
 * number. Only root may.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("chown UID[:GID] PATH...", "give files to another user (root only)");

/* A decimal number up to the end or ':'; false if there is none. */
static bool number(const char **s, unsigned *out)
{
    unsigned long v = 0;
    const char *start = *s;
    while (**s >= '0' && **s <= '9' && v <= 0xFFFFFFFFUL)
        v = v * 10 + (unsigned long)(*(*s)++ - '0');
    *out = (unsigned)v;
    return *s != start && v <= 0xFFFFFFFFUL;
}

int main(int argc, char **argv)
{
    unsigned uid = 0, gid = 0;
    const char *s = argc > 1 ? argv[1] : "";
    bool valid = number(&s, &uid);
    gid = uid;
    if (valid && *s == ':') {
        s++;
        valid = number(&s, &gid);
    }
    if (argc < 3 || !valid || *s) {
        dprintf(STDERR_FILENO, "usage: chown UID[:GID] PATH...\n");
        return 2;
    }
    int status = 0;
    for (int i = 2; i < argc; i++)
        if (chown(argv[i], uid, gid) < 0) {
            dprintf(STDERR_FILENO, "chown: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    return status;
}
