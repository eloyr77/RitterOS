/* Test: the kernel's logs (step 45b), before logd; root only.
 *   logtest take              what the logs hold now, "NAME: line" (taken)
 *   logtest as NAME TEXT      print TEXT with this process's output logged as NAME
 *   logtest write NAME TEXT   add a line to the log NAME
 *   logtest fill NAME BYTES   add BYTES in lines of 100 to the log NAME at once
 *   logtest term              a child that ignores SIGTERM is sent one: its
 *                             sleep is interrupted, it says so and ends */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"

static uint8_t buffer[96 * 1024];

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "take") == 0) {
        int n = ritteros_log_take(buffer, sizeof(buffer));
        if (n < 0) {
            printf("logtest: take: %s\n", strerror(errno));
            return 1;
        }
        for (int at = 0; at + (int)sizeof(struct sys_log_record) <= n;) {
            struct sys_log_record r;
            memcpy(&r, buffer + at, sizeof(r));
            at += (int)sizeof(r);
            if (r.lost)
                printf("%s: (%u bytes lost)\n", r.name, r.lost);
            const char *text = (const char *)buffer + at;
            for (uint32_t i = 0; i < r.length;) {
                uint32_t end = i;
                while (end < r.length && text[end] != '\n')
                    end++;
                printf("%s: %.*s\n", r.name, (int)(end - i), text + i);
                i = end + 1;
            }
            at += (int)r.length;
        }
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "as") == 0) {
        if (ritteros_log_as(argv[2]) < 0) {
            printf("logtest: as: %s\n", strerror(errno));
            return 1;
        }
        printf("%s\n", argv[3]);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "write") == 0) {
        char line[200];
        snprintf(line, sizeof(line), "%s\n", argv[3]);
        if (ritteros_log_write(argv[2], line) < 0) {
            printf("logtest: write: %s\n", strerror(errno));
            return 1;
        }
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "fill") == 0) {
        char line[101];
        int bytes = 0;
        for (const char *c = argv[3]; *c >= '0' && *c <= '9'; c++)
            bytes = bytes * 10 + (*c - '0');
        for (int done = 0; done < bytes; done += 100) {
            snprintf(line, sizeof(line), "fill %08d %s", done,
                     "................................................................................");
            line[99] = '\n';
            line[100] = 0;
            if (ritteros_log_write(argv[2], line) < 0)
                return 1;
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "termchild") == 0) {
        signal(SIGTERM, SIG_IGN);
        printf("logtest: waiting\n");
        if (usleep(30000000) < 0 && errno == EINTR)
            printf("logtest: SIGTERM interrupted the sleep\n");
        else
            printf("logtest: slept to the end\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "term") == 0) {
        char *child[] = { argv[0], "termchild", 0 };
        int fds[3] = { -1, -1, -1 };
        pid_t pid = ritteros_spawn(argv[0], child, 0, fds);
        if (pid < 0)
            return 1;
        sleep(1);
        kill(pid, SIGTERM);
        int status;
        waitpid(pid, &status, 0);
        printf("logtest: the child ended with exit code %d\n",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 0;
    }
    printf("usage: logtest take | as NAME TEXT | write NAME TEXT | term\n");
    return 2;
}
