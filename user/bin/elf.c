/*
 * elf [start | stop | restart | remove NAME | add NAME COMMAND...]: the
 * elves, programs RitterOS keeps running (elvesd starts them at the boot
 * and again whenever they end). Each has a file in /system/config/elves/:
 *   command = PROGRAM WORDS...
 *   start = on | off
 * elf alone lists them: on or off, running (its process) or not. The rest
 * is root's: start and stop switch an elf on or off (now, and for the boots
 * to come; elvesd acts within 2 seconds), restart ends its process (elvesd
 * starts it again), add makes an elf (on), remove deletes its file (elvesd
 * stops it). elf starts elvesd when an elf is switched on and elvesd is
 * not running.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("elf [start|stop|restart|remove NAME | add NAME COMMAND...]",
             "the elves: programs kept running (/system/config/elves/; changes: root)");

#define MAX_PROCESSES 32

static char report[32 * 1024];
static char text[4096];
static struct process { int pid, parent; char name[32]; } processes[MAX_PROCESSES];
static int process_count;
static char bin_dir[160];                       /* where elf (and elvesd) live */

static int fail(const char *what, const char *why)
{
    dprintf(STDERR_FILENO, "elf: %s: %s\n", what, why);
    return 1;
}

static void elf_path(const char *name, char *path, size_t size)
{
    snprintf(path, size, "%s/%s", ELVES_DIR, name);
}

/* The next word of *p (at most size - 1 bytes) into out; false if none. */
static bool word(char **p, char *out, size_t size)
{
    while (**p == ' ')
        (*p)++;
    size_t n = 0;
    while (**p && **p != ' ') {
        if (n + 1 < size)
            out[n++] = **p;
        (*p)++;
    }
    out[n] = 0;
    return n > 0;
}

/* The running processes, from the kernel's report (as ps shows it:
 * pid, parent, group, state, name). */
static void read_processes(void)
{
    process_count = 0;
    if (ritteros_report(REPORT_PROCESSES, report, sizeof(report)) < 0)
        return;
    for (char *line = report; *line && process_count < MAX_PROCESSES;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        struct process p;
        char pid[16], parent[16], group[16], state[16];
        char *at = line;
        if (word(&at, pid, sizeof(pid)) && word(&at, parent, sizeof(parent)) &&
            word(&at, group, sizeof(group)) && word(&at, state, sizeof(state)) &&
            word(&at, p.name, sizeof(p.name)) && pid[0] >= '0' && pid[0] <= '9' &&
            strcmp(state, "running") == 0) {
            p.pid = atoi(pid);
            p.parent = atoi(parent);
            processes[process_count++] = p;
        }
        line = end ? end + 1 : line + strlen(line);
    }
}

/* The program's name in an elf's command (the process's name). */
static void program_of(const char *command, char *out, size_t size)
{
    while (*command == ' ' || *command == '\t')
        command++;
    size_t n = strcspn(command, " \t");
    const char *start = command;
    for (size_t i = 0; i < n; i++)
        if (command[i] == '/')
            start = command + i + 1;
    snprintf(out, size, "%.*s", (int)(command + n - start), start);
}

/* The processes of elvesd's running the elf's program: their number, the
 * first in *pid. */
static int elf_processes(const char *name, int *pid)
{
    char path[320], command[256], program[64];
    elf_path(name, path, sizeof(path));
    if (config_get(path, "command", command, sizeof(command)) <= 0)
        return 0;
    program_of(command, program, sizeof(program));
    int parent = elvesd_pid(), n = 0;
    for (int i = 0; i < process_count && parent; i++)
        if (processes[i].parent == parent && strcmp(processes[i].name, program) == 0) {
            if (n++ == 0)
                *pid = processes[i].pid;
        }
    return n;
}

static int list(void)
{
    read_processes();
    int daemon = elvesd_pid();
    if (daemon)
        printf("elvesd: running (process %d)\n", daemon);
    else
        printf("elvesd: not running\n");
    DIR *dir = opendir(ELVES_DIR);
    struct dirent *entry;
    int count = 0;
    while (dir && (entry = readdir(dir))) {
        if (entry->d_type != DT_REG || entry->d_name[0] == '.')
            continue;
        char path[320], start[16], command[256], state[48];
        elf_path(entry->d_name, path, sizeof(path));
        bool on = config_get(path, "start", start, sizeof(start)) > 0 && strcmp(start, "on") == 0;
        if (config_get(path, "command", command, sizeof(command)) <= 0)
            snprintf(command, sizeof(command), "(no command)");
        int pid = 0, n = elf_processes(entry->d_name, &pid);
        if (n == 1)
            snprintf(state, sizeof(state), "running (process %d)", pid);
        else if (n > 1)
            snprintf(state, sizeof(state), "running (%d processes)", n);
        else
            snprintf(state, sizeof(state), "not running");
        printf("%-16s %-3s  %-22s  %s\n", entry->d_name, on ? "on" : "off", state, command);
        count++;
    }
    if (dir)
        closedir(dir);
    if (!count)
        printf("no elves (root adds one: elf add NAME COMMAND...)\n");
    return 0;
}

/* The file's text with its start line set (or added). */
static int set_start(const char *name, bool on)
{
    char path[320];
    elf_path(name, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fail(name, errno == ENOENT ? "no such elf" : strerror(errno));
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n < 0)
        return fail(path, strerror(errno));
    text[n] = 0;
    static char out[sizeof(text) + 32];
    size_t used = 0;
    bool done = false;
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        bool is_start = strncmp(p, "start", 5) == 0 && strchr(" \t=", p[5]) && p[5];
        if (is_start && !done) {
            used += (size_t)snprintf(out + used, sizeof(out) - used, "start = %s\n", on ? "on" : "off");
            done = true;
        } else if (!is_start) {
            used += (size_t)snprintf(out + used, sizeof(out) - used, "%.*s\n", (int)length, line);
        }
        line = end ? end + 1 : line + length;
    }
    if (!done)
        used += (size_t)snprintf(out + used, sizeof(out) - used, "start = %s\n", on ? "on" : "off");
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, out, used) != (ssize_t)used || close(fd) < 0)
        return fail(path, strerror(errno));
    return 0;
}

/* elvesd, if it is not running (it then finds the elves that are on). */
static void wake_elvesd(void)
{
    read_processes();
    if (elvesd_pid())
        return;
    char path[200];
    snprintf(path, sizeof(path), "%s/elvesd", bin_dir);
    char *argv[] = { path, 0 };
    int fds[3] = { -1, -1, -1 };
    pid_t pid = ritteros_spawn(path, argv, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
    if (pid < 0)
        fail(path, strerror(errno));
    else
        printf("elf: elvesd started (process %d)\n", pid);
}

static bool name_valid(const char *name)
{
    if (!*name || strlen(name) > 63 || name[0] == '.')
        return false;
    for (const char *c = name; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
              *c == '-' || *c == '_' || *c == '.'))
            return false;
    return true;
}

int main(int argc, char **argv)
{
    /* elvesd lives next to elf: /system/<arch>/bin. */
    const char *slash = strrchr(argv[0], '/');
    if (slash)
        snprintf(bin_dir, sizeof(bin_dir), "%.*s", (int)(slash - argv[0]), argv[0]);
    else
        snprintf(bin_dir, sizeof(bin_dir), "/system/%s/bin",
#if defined(__x86_64__)
                 "x86_64"
#else
                 "aarch64"
#endif
        );
    if (argc == 1)
        return list();
    const char *what = argv[1];
    bool add = strcmp(what, "add") == 0;
    if ((add && argc < 4) || (!add && argc != 3) ||
        (!add && strcmp(what, "start") && strcmp(what, "stop") && strcmp(what, "restart") &&
         strcmp(what, "remove"))) {
        dprintf(STDERR_FILENO, "usage: elf [start|stop|restart|remove NAME | add NAME COMMAND...]\n");
        return 2;
    }
    const char *name = argv[2];
    if (!name_valid(name))
        return fail(name, "not a name for an elf (letters, digits, - _ .)");
    if (getuid() != 0)
        return fail(what, "only root may change the elves");
    char path[320];
    elf_path(name, path, sizeof(path));
    struct stat st;
    if (add) {
        if (stat(path, &st) == 0)
            return fail(name, "there is such an elf already");
        if (stat(ELVES_DIR, &st) < 0 && mkdir(ELVES_DIR, 0755) < 0)
            return fail(ELVES_DIR, strerror(errno));
        int used = snprintf(text, sizeof(text), "# %s, an elf: elvesd keeps it running (see the manual)\ncommand =", name);
        for (int i = 3; i < argc; i++)
            used += snprintf(text + used, sizeof(text) - (size_t)used, " %s", argv[i]);
        used += snprintf(text + used, sizeof(text) - (size_t)used, "\nstart = on\n");
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || write(fd, text, (size_t)used) != used || close(fd) < 0)
            return fail(path, strerror(errno));
        printf("elf: %s added, on (elvesd starts it within 2 s)\n", name);
        wake_elvesd();
        return 0;
    }
    if (stat(path, &st) < 0)
        return fail(name, "no such elf");
    if (strcmp(what, "remove") == 0) {
        if (unlink(path) < 0)
            return fail(path, strerror(errno));
        printf("elf: %s removed (elvesd stops it within 2 s)\n", name);
        return 0;
    }
    if (strcmp(what, "restart") == 0) {
        read_processes();
        int pid = 0, n = elf_processes(name, &pid);
        if (n == 0)
            return fail(name, "not running");
        if (n > 1)
            return fail(name, "its program runs for several elves: stop and start it instead");
        if (kill(pid, SIGTERM) < 0)
            return fail(name, strerror(errno));
        printf("elf: %s: process %d ended (elvesd starts it again)\n", name, pid);
        return 0;
    }
    bool on = strcmp(what, "start") == 0;
    if (set_start(name, on))
        return 1;
    printf("elf: %s %s (elvesd %s it within 2 s)\n", name, on ? "on" : "off", on ? "starts" : "stops");
    if (on)
        wake_elvesd();
    return 0;
}
