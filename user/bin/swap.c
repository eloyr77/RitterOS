/*
 * swap (35a, roadmap item 35): the swap file /system/swap, where the kernel
 * keeps programs' pages while memory is short (35b), encrypted with a key
 * made each time swap is turned on. It is a fixed file of RitterFS: its
 * blocks stay where they are and only the kernel writes them.
 *
 *   swap            whether swap is on, the file's size and what it holds
 *   swap make SIZE  make /system/swap (SIZE in MiB or GiB: 512M, 4G) and
 *                   turn swap on
 *   swap on         turn swap on, and on at every boot (/system/config/swap:
 *                   on = yes)
 *   swap off        turn it off, also for later boots (on = no)
 *
 * Changing it is root's. Nothing is written until the user asks: making the
 * file writes zeros once (RitterFS allocates its blocks then), and the kernel
 * makes it fixed when swap goes on.
 */
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"
#include "../../common/settings_defaults.h"

PROGRAM_HELP("swap [make SIZE | on | off]",
             "show swap; make the swap file /system/swap, turn swap on or off");

#define FILE_NAME     "/system/swap"
#define SETTINGS_NAME "/system/config/swap"
#define CHUNK         (1024 * 1024)

/* "512M", "4G": bytes, a whole number of MiB (0 if it is not). */
static unsigned long long parse_size(const char *s)
{
    char *end;
    unsigned long long n = strtoull(s, &end, 10);
    if (end == s || n == 0 || end[1])
        return 0;
    if (*end == 'M' && n <= 1024ULL * 1024)
        return n << 20;
    if (*end == 'G' && n <= 1024)
        return n << 30;
    return 0;
}

/* Later boots: on = yes or no. (37e-1b) Only the `on` line changes (or is
 * added): the file's other lines, its comments, stay; a missing file
 * starts as its default text. */
static int remember(int on)
{
    static char old[4096], content[4096 + 64];
    ssize_t got = 0;
    int fd = open(SETTINGS_NAME, O_RDONLY);
    if (fd >= 0) {
        got = read(fd, old, sizeof(old) - 1);
        close(fd);
    } else {
        got = snprintf(old, sizeof(old), "%s", SETTINGS_SWAP_TEXT);   /* its default text */
    }
    old[got > 0 ? got : 0] = 0;
    const char *wanted = on ? "on = yes\n" : "on = no\n";
    size_t n = 0;
    bool replaced = false;
    for (char *line = old; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) + 1 : strlen(line);
        const char *k = line + strspn(line, " \t");
        bool is_on = !strncmp(k, "on", 2) && k[2] && strchr(" \t=", k[2]);
        if (is_on && !replaced)
            n += (size_t)snprintf(content + n, sizeof(content) - n, "%s", wanted);
        else if (!is_on) {
            memcpy(content + n, line, len);
            n += len;
            if (!end)
                content[n++] = '\n';
        }
        replaced |= is_on;
        line += len;
    }
    if (!replaced)
        n += (size_t)snprintf(content + n, sizeof(content) - n, "%s", wanted);
    fd = open(SETTINGS_NAME, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, content, n) != (ssize_t)n || close(fd) < 0) {
        fprintf(stderr, "swap: %s: %s\n", SETTINGS_NAME, strerror(errno));
        return 1;
    }
    return 0;
}

static int show(void)
{
    struct sys_swap s;
    struct stat st;
    if (ritteros_swap(SWAP_STATUS, &s) < 0) {
        fprintf(stderr, "swap: %s\n", strerror(errno));
        return 1;
    }
    if (s.on)
        printf("swap: on, %s: %llu MiB (%llu pages), %llu in use; since the boot %llu pages "
               "written, %llu read\n", FILE_NAME, (unsigned long long)(s.pages >> 8),
               (unsigned long long)s.pages, (unsigned long long)s.used,
               (unsigned long long)s.written, (unsigned long long)s.read);
    else if (stat(FILE_NAME, &st) == 0)
        printf("swap: off; %s: %llu MiB\n", FILE_NAME, (unsigned long long)(st.st_size >> 20));
    else
        printf("swap: off; no %s (swap make SIZE makes one)\n", FILE_NAME);
    return 0;
}

static int turn_on(void)
{
    if (ritteros_swap(SWAP_ON, 0) < 0) {
        fprintf(stderr, "swap: on: %s%s\n", strerror(errno),
                errno == ENOENT ? " (no " FILE_NAME ": swap make SIZE makes one)"
                : errno == EINVAL ? " (" FILE_NAME " has holes, a part block or blocks a "
                                    "snapshot shares)" : "");
        return 1;
    }
    return remember(1);
}

static int make(const char *size_text)
{
    unsigned long long size = parse_size(size_text);
    if (!size) {
        fprintf(stderr, "swap: make %s: not a size in MiB or GiB (512M, 4G)\n", size_text);
        return 2;
    }
    int fd = open(FILE_NAME, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        fprintf(stderr, "swap: %s: %s%s\n", FILE_NAME, strerror(errno),
                errno == EEXIST ? " (swap off, then rm it, to make another)" : "");
        return 1;
    }
    char *zeros = calloc(1, CHUNK);
    unsigned long long done = 0;
    while (zeros && done < size) {
        ssize_t n = write(fd, zeros, CHUNK);
        if (n != CHUNK)
            break;
        done += CHUNK;
    }
    int saved = errno;
    free(zeros);
    if (close(fd) < 0 && done == size) {
        saved = errno;
        done = 0;
    }
    if (done != size) {
        unlink(FILE_NAME);
        fprintf(stderr, "swap: %s: %s\n", FILE_NAME, strerror(saved));
        return 1;
    }
    printf("swap: %s made: %llu MiB\n", FILE_NAME, size >> 20);
    if (turn_on()) {
        unlink(FILE_NAME);
        return 1;
    }
    return show();
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return show();
    if (!(argc == 3 && !strcmp(argv[1], "make")) &&
        !(argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off")))) {
        fprintf(stderr, "usage: swap [make SIZE | on | off]\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "swap: changing swap is root's\n");
        return 1;
    }
    if (!strcmp(argv[1], "make"))
        return make(argv[2]);
    if (!strcmp(argv[1], "on"))
        return turn_on() || show();
    if (ritteros_swap(SWAP_OFF, 0) < 0) {
        fprintf(stderr, "swap: off: %s\n", strerror(errno));
        return 1;
    }
    return remember(0) || show();
}
