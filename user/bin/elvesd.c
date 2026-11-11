/*
 * elvesd: keeps the elves running. An elf is a program RitterOS starts at
 * every boot and starts again whenever it ends (what other systems call a
 * service or a daemon). Each has a file in /system/config/elves/, named
 * after it:
 *   command = sshd -p 22     the program and its words (a name without '/'
 *                            is in /system/<arch>/bin)
 *   start = on               on: running; off: not
 * login starts elvesd at the boot when there are elves (after net up); elf
 * starts it when one is added or switched on and elvesd is not running.
 * Every 2 seconds it reads the folder: an elf that is on and not running
 * is started (as root, in a session of its own, its output on the
 * console); one that ended is started again after a wait that doubles
 * (2 s up to 60 s) while it keeps ending within 10 seconds of its start;
 * one that is off, or whose file is gone, is stopped (SIGTERM; SIGKILL 5
 * seconds later if it still runs). elvesd writes nothing: the program elf
 * changes the files. What an elf prints also goes to its log (the kernel
 * keeps it for logd: /system/logs/NAME.log), elvesd's own lines to
 * elvesd's.
 */
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("elvesd", "keeps the elves running (login starts it): /system/config/elves/");

#define MAX_ELVES       32
#define MAX_WORDS       16
#define TICK            2               /* seconds between looks at the folder */
#define FIRST_WAIT      2               /* seconds before starting an ended elf again */
#define LONGEST_WAIT    60
#define SHORT_RUN       10              /* an elf that ended sooner waits longer */
#define KILL_AFTER      5               /* seconds from SIGTERM to SIGKILL */

struct elf {
    char name[64];
    bool used, seen;                    /* seen: its file is there (this look) */
    pid_t pid;                          /* 0: not running */
    uint64_t started, next_start, stop_asked;
    unsigned wait;
};

static struct elf elves[MAX_ELVES];
static uint64_t now;                    /* seconds since elvesd started */
static char bin_dir[160];

static struct elf *find(const char *name, bool make)
{
    struct elf *free = 0;
    for (int i = 0; i < MAX_ELVES; i++) {
        if (elves[i].used && strcmp(elves[i].name, name) == 0)
            return &elves[i];
        if (!elves[i].used && !free)
            free = &elves[i];
    }
    if (!make || !free)
        return 0;
    memset(free, 0, sizeof(*free));
    free->used = true;
    snprintf(free->name, sizeof(free->name), "%s", name);
    free->wait = FIRST_WAIT;
    return free;
}

/* Start the elf: its command's words, the program found like the shell
 * finds it. */
static void start(struct elf *e, char *command)
{
    char *argv[MAX_WORDS + 1];
    int n = 0;
    for (char *c = command; *c && n < MAX_WORDS;) {
        while (*c == ' ' || *c == '\t')
            *c++ = 0;
        if (!*c)
            break;
        argv[n++] = c;
        while (*c && *c != ' ' && *c != '\t')
            c++;
    }
    argv[n] = 0;
    e->next_start = now + e->wait;
    if (n == 0) {
        printf("elvesd: %s: no command = ... in its file\n", e->name);
        return;
    }
    char path[256];
    if (strchr(argv[0], '/'))
        snprintf(path, sizeof(path), "%s", argv[0]);
    else
        snprintf(path, sizeof(path), "%s/%s", bin_dir, argv[0]);
    int fds[3] = { -1, -1, -1 };
    ritteros_log_as(e->name);                       /* the elf's output: its log (inherited) */
    pid_t pid = ritteros_spawn(path, argv, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
    int error = errno;
    ritteros_log_as("elvesd");
    errno = error;
    if (pid < 0) {
        printf("elvesd: %s: %s: %s; again in %u s\n", e->name, path, strerror(errno), e->wait);
        e->wait = e->wait * 2 > LONGEST_WAIT ? LONGEST_WAIT : e->wait * 2;
        return;
    }
    e->pid = pid;
    e->started = now;
    e->stop_asked = 0;
    printf("elvesd: %s started (process %d)\n", e->name, pid);
}

/* Elves that ended: when to start them again. */
static void reap(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_ELVES; i++) {
            struct elf *e = &elves[i];
            if (!e->used || e->pid != pid)
                continue;
            e->pid = 0;
            if (e->stop_asked) {
                e->stop_asked = 0;
                break;                          /* stopped as asked */
            }
            if (now - e->started >= SHORT_RUN)
                e->wait = FIRST_WAIT;           /* it ran a while: no long wait */
            char how[48];
            if (WIFEXITED(status))
                snprintf(how, sizeof(how), "exit code %d", WEXITSTATUS(status));
            else
                snprintf(how, sizeof(how), "signal %d", WTERMSIG(status));
            printf("elvesd: %s ended (%s); starting it again in %u s\n", e->name, how, e->wait);
            e->next_start = now + e->wait;
            e->wait = e->wait * 2 > LONGEST_WAIT ? LONGEST_WAIT : e->wait * 2;
            break;
        }
    }
}

static void stop(struct elf *e)
{
    if (!e->pid)
        return;
    if (!e->stop_asked) {
        e->stop_asked = now;
        kill(e->pid, SIGTERM);
        printf("elvesd: %s stopped\n", e->name);
    } else if (now - e->stop_asked >= KILL_AFTER) {
        kill(e->pid, SIGKILL);
    }
}

/* One look at the folder: start, stop. */
static void look(void)
{
    for (int i = 0; i < MAX_ELVES; i++)
        elves[i].seen = false;
    DIR *dir = opendir(ELVES_DIR);
    struct dirent *entry;
    while (dir && (entry = readdir(dir))) {
        if (entry->d_type != DT_REG || entry->d_name[0] == '.')
            continue;
        char path[320], value[16], command[256];
        snprintf(path, sizeof(path), "%s/%s", ELVES_DIR, entry->d_name);
        struct elf *e = find(entry->d_name, true);
        if (!e)
            continue;                           /* more than MAX_ELVES */
        e->seen = true;
        bool on = config_get(path, "start", value, sizeof(value)) > 0 && strcmp(value, "on") == 0;
        if (!on) {
            stop(e);
            e->wait = FIRST_WAIT;
            e->next_start = 0;
        } else if (!e->pid && now >= e->next_start) {
            if (config_get(path, "command", command, sizeof(command)) <= 0)
                command[0] = 0;
            start(e, command);
        }
    }
    if (dir)
        closedir(dir);
    for (int i = 0; i < MAX_ELVES; i++) {
        struct elf *e = &elves[i];
        if (!e->used || e->seen)
            continue;
        if (e->pid)
            stop(e);                            /* its file is gone */
        else
            e->used = false;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    /* Programs live next to elvesd: /system/<arch>/bin. */
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
    signal(SIGINT, SIG_IGN);
    ritteros_log_as("elvesd");
    for (;; now += TICK) {
        reap();
        look();
        sleep(TICK);
    }
}
