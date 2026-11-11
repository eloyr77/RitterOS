/*
 * guardd: the firewall's elf (roadmap item 12). It loads the rules of
 * /system/config/guard into the kernel, which checks every packet (see
 * SYS_GUARD), and loads them again when the file changes (it looks every
 * 2 seconds). Until the first load the kernel drops what comes in. A file
 * with a mistake is not loaded: the rules loaded before stay, and guardd
 * says which line is wrong. Its first start makes the file: what comes in
 * is dropped, what goes out allowed; ping comes in, and so do the elves
 * that are there to be reached (sshd on tcp 22, dnsd on udp 53). Switched
 * off (elf stop guardd: SIGTERM, its file says start = off), it switches
 * the checks off, so nothing is held back; restarted (elf restart), it
 * leaves the rules as they are. net up makes its elf file.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("guardd", "the firewall's elf: loads /system/config/guard into the kernel");

#define LOOK_MS     2000

static struct sys_guard table;
static char text[16384], loaded_text[16384];
static bool loaded;

/* The file as it is now: its length, or -1. */
static ssize_t read_rules(char *out, size_t size)
{
    int fd = open(GUARD_FILE, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t length = 0, r;
    while (length < (ssize_t)size - 1 && (r = read(fd, out + length, size - 1 - (size_t)length)) > 0)
        length += r;
    close(fd);
    out[length] = 0;
    return length;
}

static bool elf_exists(const char *name)
{
    char path[64];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", ELVES_DIR, name);
    return stat(path, &st) == 0;
}

static void add(char *out, size_t size, const char *more)
{
    size_t n = strlen(out);
    snprintf(out + n, size - n, "%s", more);
}

/* The first start: the file, with what is there to be reached. */
static void make_rules(void)
{
    char rules[1024], let_in[128] = "icmp";
    snprintf(rules, sizeof(rules),
             "# The firewall's rules (guardd made this file; see the manual, \"guard\").\n"
             "# Read top to bottom: the first rule that matches a packet decides; the\n"
             "# replies to the machine's own connections always pass.\n"
             "in = drop\n"
             "out = allow\n"
             "allow in icmp                # ping\n");
    if (elf_exists("sshd")) {
        add(rules, sizeof(rules), "allow in tcp 22              # sshd\n");
        add(let_in, sizeof(let_in), ", tcp 22 (sshd)");
    }
    if (elf_exists("dnsd")) {
        add(rules, sizeof(rules), "allow in udp 53              # dnsd\n");
        add(let_in, sizeof(let_in), ", udp 53 (dnsd)");
    }
    int fd = open(GUARD_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, rules, strlen(rules)) != (ssize_t)strlen(rules)) {
        printf("guardd: %s: %s\n", GUARD_FILE, strerror(errno));
        if (fd >= 0)
            close(fd);
        return;
    }
    close(fd);
    printf("guardd: %s made: in = drop, out = allow; let in: %s\n", GUARD_FILE, let_in);
}

/* The file into the kernel, if it changed since the last load. */
static void load(void)
{
    ssize_t length = read_rules(text, sizeof(text));
    if (length < 0) {
        if (loaded || errno != ENOENT)
            return;                                 /* (gone: the rules stay) */
        make_rules();
        if ((length = read_rules(text, sizeof(text))) < 0)
            return;
    }
    if (loaded && strcmp(text, loaded_text) == 0)
        return;
    char why[200];
    static struct sys_guard6 table6;                /* (64e) IPv6's rules */
    if (guard_read6(GUARD_FILE, &table, &table6, why, sizeof(why)) < 0) {
        if (strcmp(text, loaded_text) != 0) {       /* (said once per version of the file) */
            static struct sys_guard now;
            uint32_t state = ritteros_guard(GUARD_STATUS, &now) == 0 ? now.state : GUARD_STATE_ON;
            printf("guardd: %s: %s; %s\n", GUARD_FILE, why,
                   state == GUARD_STATE_BOOT ? "what comes in is dropped until it is right"
                   : state == GUARD_STATE_OFF ? "nothing is checked until it is right"
                                              : "the rules loaded before stay");
        }
        snprintf(loaded_text, sizeof(loaded_text), "%s", text);
        return;
    }
    if (ritteros_guard(GUARD_LOAD, &table) < 0 || ritteros_guard6(GUARD_SET6, &table6) < 0) {
        printf("guardd: the kernel took no rules: %s\n", strerror(errno));
        return;
    }
    loaded = true;
    snprintf(loaded_text, sizeof(loaded_text), "%s", text);
    unsigned rules = table.count;                   /* the file's: IPv4's and IPv6's own */
    for (int i = 0; i < table6.count; i++)
        rules += !table6.rules[i].both;
    printf("guardd: %u rule(s) loaded (in = %s, out = %s)\n", rules,
           table.in_default == GUARD_DROP ? "drop" : "allow",
           table.out_default == GUARD_DROP ? "drop" : "allow");
}

/* Is guardd still meant to run (its elf file says start = on)? */
static bool switched_on(void)
{
    char path[64], value[16];
    snprintf(path, sizeof(path), "%s/guardd", ELVES_DIR);
    int r = config_get(path, "start", value, sizeof(value));
    return r > 0 ? strcmp(value, "off") != 0 : r == 0;
}

int main(void)
{
    signal(SIGTERM, SIG_IGN);                       /* stopping: an interrupted wait */
    for (;;) {
        load();
        if (usleep(LOOK_MS * 1000) < 0 && errno == EINTR) {
            if (!switched_on()) {
                ritteros_guard(GUARD_OFF, &table);
                printf("guardd: switched off: nothing is checked now\n");
            }
            return 0;                               /* restarted: the rules stay */
        }
    }
}
