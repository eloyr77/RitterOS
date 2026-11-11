/*
 * uname [-amnrsv] (68i, POSIX): the system's names, in this order: its
 * name (-s, RitterOS; the one shown without options), the machine's name
 * (-n), the release (-r, year.month), the commit it was built from (-v)
 * and the machine (-m); -a all of them.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include "ritteros.h"

PROGRAM_HELP("uname [-amnrsv]", "the system's name, release, commit and machine");

enum { SYSNAME = 1, NODENAME = 2, RELEASE = 4, VERSION = 8, MACHINE = 16 };

int main(int argc, char **argv)
{
    int want = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) {
            fprintf(stderr, "uname: unexpected '%s'\nusage: uname [-amnrsv]\n", a);
            return 2;
        }
        for (const char *c = a + 1; *c; c++) {
            const char *letters = "snrvma", *at = strchr(letters, *c);
            if (!at) {
                fprintf(stderr, "uname: unknown option -%c\nusage: uname [-amnrsv]\n", *c);
                return 2;
            }
            want |= *c == 'a' ? SYSNAME | NODENAME | RELEASE | VERSION | MACHINE
                              : 1 << (at - letters);
        }
    }
    if (!want)
        want = SYSNAME;
    struct utsname u;
    if (uname(&u) < 0) {
        perror("uname");
        return 1;
    }
    const char *fields[] = { u.sysname, u.nodename, u.release, u.version, u.machine };
    bool first = true;
    for (int k = 0; k < 5; k++) {
        if (!(want & 1 << k))
            continue;
        printf("%s%s", first ? "" : " ", fields[k]);
        first = false;
    }
    printf("\n");
    return 0;
}
