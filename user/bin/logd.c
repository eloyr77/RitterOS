/*
 * logd: the logs' keeper, an elf. Every 10 seconds it takes what the
 * kernel kept in memory (its own messages, the elves' output, login's
 * lines; SYS_LOG) and adds it to /system/logs/NAME.log. A log that would
 * grow past 64 KiB becomes NAME.log.old (the one before it goes) and a new
 * one begins. Lines the kernel had to drop (its ring was full) are noted
 * where they were. Asked to stop (SIGTERM: elf stop logd), it writes what
 * is left first. login makes its elf's file on every system.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("logd", "the logs' keeper, an elf: writes /system/logs/NAME.log every 10 s");

#define EVERY_MS        10000
#define LOG_MAX         (64 * 1024)     /* bytes a log may hold before it becomes .old */

static uint8_t taken[128 * 1024];

/* text (n bytes) added to the log name's file. */
static void add(const char *name, const char *text, size_t n, uint32_t lost)
{
    char path[64], old[72], note[64];
    snprintf(path, sizeof(path), "%s/%s.log", LOGS_DIR, name);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size + n > LOG_MAX) {
        snprintf(old, sizeof(old), "%s.old", path);
        unlink(old);
        if (rename(path, old) < 0)
            printf("logd: %s: %s\n", path, strerror(errno));
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        printf("logd: %s: %s\n", path, strerror(errno));
        return;
    }
    if (lost) {
        snprintf(note, sizeof(note), "(%u bytes lost here: the log was full)\n", lost);
        write(fd, note, strlen(note));
    }
    while (n > 0) {
        ssize_t w = write(fd, text, n);
        if (w <= 0) {
            printf("logd: %s: %s\n", path, strerror(errno));
            break;
        }
        text += w, n -= (size_t)w;
    }
    close(fd);
}

/* What the kernel kept, into the files. */
static void save(void)
{
    for (;;) {
        int n = ritteros_log_take(taken, sizeof(taken));
        if (n <= 0)
            return;
        for (int at = 0; at + (int)sizeof(struct sys_log_record) <= n;) {
            struct sys_log_record r;
            memcpy(&r, taken + at, sizeof(r));
            at += (int)sizeof(r);
            r.name[sizeof(r.name) - 1] = 0;
            if (r.name[0] && !strchr(r.name, '/') && r.name[0] != '.')
                add(r.name, (const char *)taken + at, r.length, r.lost);
            at += (int)r.length;
        }
        if ((size_t)n < sizeof(taken) / 2)
            return;                                 /* (else there may be more) */
    }
}

int main(void)
{
    signal(SIGTERM, SIG_IGN);                       /* stopping: an interrupted wait */
    struct stat st;
    if (stat(LOGS_DIR, &st) < 0 && mkdir(LOGS_DIR, 0755) < 0) {
        printf("logd: %s: %s\n", LOGS_DIR, strerror(errno));
        return 1;
    }
    for (;;) {
        save();
        if (usleep(EVERY_MS * 1000) < 0 && errno == EINTR) {
            save();                                 /* asked to stop: what is left first */
            return 0;
        }
    }
}
