/*
 * kill [-SIGNAL] PID...: ask processes to end. SIGNAL is TERM (the default),
 * KILL, INT, HUP, USR1, USR2 (66c-1: a program's own requests, windowd's
 * frame report; SIG may precede the name) or a number 1-31; the process
 * numbers are those `ps` lists. Root may signal any process, the others only
 * their own (the kernel checks). A program ends at the signal unless it
 * ignores it or has a handler for it (signal handlers since 22r).
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("kill [-SIGNAL] PID...",
             "ask processes to end (SIGNAL: TERM, KILL, INT, HUP, USR1, USR2 or a number)");

static const struct { const char *name; int number; } names[] = {
    { "HUP", SIGHUP }, { "INT", SIGINT }, { "KILL", SIGKILL }, { "TERM", SIGTERM },
    { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 },
};

/* A whole decimal number (digits only), or -1. */
static long number(const char *s)
{
    if (!*s)
        return -1;
    long n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > 1000000)
            return -1;
        n = n * 10 + (*s - '0');
    }
    return n;
}

/* The signal a -SIGNAL argument names, or -1. */
static int signal_named(const char *s)
{
    if (strncmp(s, "SIG", 3) == 0)
        s += 3;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(s, names[i].name) == 0)
            return names[i].number;
    long n = number(s);
    return n >= 1 && n <= 31 ? (int)n : -1;
}

int main(int argc, char **argv)
{
    int sig = SIGTERM, first = 1;
    if (argc > 1 && argv[1][0] == '-') {
        sig = signal_named(argv[1] + 1);
        if (sig < 0) {
            dprintf(STDERR_FILENO, "kill: %s: not a signal (TERM, KILL, INT, HUP, USR1, USR2 or 1-31)\n", argv[1] + 1);
            return 2;
        }
        first = 2;
    }
    if (first >= argc) {
        dprintf(STDERR_FILENO, "usage: kill [-SIGNAL] PID...\n");
        return 2;
    }
    int failed = 0;
    for (int i = first; i < argc; i++) {
        long pid = number(argv[i]);
        if (pid <= 0) {
            dprintf(STDERR_FILENO, "kill: %s: not a process number\n", argv[i]);
            failed = 1;
        } else if (kill((pid_t)pid, sig) < 0) {
            dprintf(STDERR_FILENO, "kill: %ld: %s\n", pid, strerror(errno));
            failed = 1;
        }
    }
    return failed;
}
