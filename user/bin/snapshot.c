/*
 * snapshot [create NAME | delete NAME]: RitterFS snapshots. Without
 * arguments it lists them with the generation (commit) each one shows;
 * create keeps the current state under NAME, readable at /.snapshots/NAME;
 * delete removes one and reports how many blocks became free again.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("snapshot [create NAME | delete NAME]",
             "list, take or delete file system snapshots (/.snapshots/NAME)");

#define MAX_LIST 64

static int list(void)
{
    struct sys_snapshot s[MAX_LIST];
    int64_t n = ritteros_snapshot(SNAPSHOT_LIST, 0, s, sizeof(s));
    if (n < 0) {
        dprintf(STDERR_FILENO, "snapshot: %s\n", strerror(errno));
        return 1;
    }
    if (n == 0)
        printf("no snapshots\n");
    for (int64_t i = 0; i < n && i < MAX_LIST; i++)
        printf("%-24s generation %lu\n", s[i].name, (unsigned long)s[i].generation);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return list();
    if (argc != 3 || (strcmp(argv[1], "create") != 0 && strcmp(argv[1], "delete") != 0)) {
        dprintf(STDERR_FILENO, "usage: snapshot [create NAME | delete NAME]\n");
        return 2;
    }
    bool create = strcmp(argv[1], "create") == 0;
    int64_t r = ritteros_snapshot(create ? SNAPSHOT_CREATE : SNAPSHOT_DELETE, argv[2], 0, 0);
    if (r < 0) {
        dprintf(STDERR_FILENO, "snapshot: %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    if (create)
        printf("snapshot '%s' taken at generation %ld\n", argv[2], (long)r);
    else
        printf("snapshot '%s' deleted, %ld block(s) free again\n", argv[2], (long)r);
    return 0;
}
