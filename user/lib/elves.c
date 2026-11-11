/*
 * elvesd for the programs that start it (login, elf, net): is it running
 * (the kernel's process report, as ps shows it), and starting it; logd
 * asked to write what it keeps before the machine goes off.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

static char report[32 * 1024];

int process_named(const char *name)
{
    if (ritteros_report(REPORT_PROCESSES, report, sizeof(report)) < 0)
        return 0;
    /* Lines: pid, parent, group, state, name. */
    size_t want = strlen(name);
    for (char *line = report; *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        size_t length = strlen(line);
        if (length > want && line[length - want - 1] == ' ' &&
            strcmp(line + length - want, name) == 0 && strstr(line, " running ")) {
            while (*line == ' ')
                line++;
            return atoi(line);
        }
        line = end ? end + 1 : line + length;
    }
    return 0;
}

int elvesd_pid(void)
{
    return process_named("elvesd");
}

void logs_flush(void)
{
    int pid = process_named("logd");
    if (!pid || kill(pid, SIGTERM) < 0)
        return;
    for (int i = 0; i < 30 && process_named("logd") == pid; i++)
        usleep(100000);                             /* it writes what is left, then ends */
}

int elvesd_start(const char *bin_dir)
{
    int pid = elvesd_pid();
    if (pid)
        return pid;
    char path[200];
    snprintf(path, sizeof(path), "%s/elvesd", bin_dir);
    char *argv[] = { path, 0 };
    int fds[3] = { -1, -1, -1 };
    return ritteros_spawn(path, argv, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
}
