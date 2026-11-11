/*
 * netnamesd (50f): the elf that keeps the network cards' names. Every
 * second it compares the cards the kernel has (its network report) with
 * /system/config/network/names and adds each card that is not there yet,
 * by its MAC address, under the name the kernel gave it, with a comment
 * saying what it is and when it was found. The kernel names the cards from
 * that file, so a card keeps its name (and its settings file) at every
 * start and plug-in. A card new to the machine also gets its settings file
 * (50h, the user's: address = dhcp, as net up makes it; never one that is
 * there) and, if the elf dhclient has no file yet, that one too: so it gets
 * its address without net up. (36d-3) On a machine net up never set up
 * (no file for the elf guardd: login runs net up only when a card is there
 * at its start, and a card whose firmware starts on a thread of its own,
 * like an Intel Wi-Fi card, comes seconds later), the new card also gets
 * the other network elves' files net up would make: dnsd, sshd, and the
 * firewall's guardd last (its first start lets in what has a file then).
 * It writes nothing else and prints only its errors.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "netnames.h"
#include "ritteros.h"

PROGRAM_HELP("netnamesd", "an elf: records new network cards in /system/config/network/names");

static char report[65536];             /* (38a: room for 64 cards) */
static bool complained;             /* an error is printed once, not every second */

static void complain(const char *path)
{
    if (!complained)
        fprintf(stderr, "netnamesd: %s: %s\n", path, strerror(errno));
    complained = true;
}

/* Add one card to the file (made with its header the first time). */
static bool record(const char *name, const uint8_t mac[6], const char *about)
{
    struct stat st;
    bool fresh = stat(NETNAMES_FILE, &st) != 0;
    if (stat(NETWORK_CONFIG, &st) != 0 && mkdir(NETWORK_CONFIG, 0755) < 0) {
        complain(NETWORK_CONFIG);               /* (a machine net up never set up) */
        return false;
    }
    int fd = open(NETNAMES_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        complain(NETNAMES_FILE);
        return false;
    }
    char day[16] = "";
    time_t now = time(0);
    struct tm tm;
    if (now > 0 && localtime_r(&now, &tm))
        strftime(day, sizeof(day), "%Y-%m-%d", &tm);
    char mac_text[18], text[1024];
    netnames_mac_text(mac, mac_text);
    int n = snprintf(text, sizeof(text), "%s# %s: %s%s%s\n%s = %s\n", fresh ? NETNAMES_HEADER : "",
                     name, about, day[0] ? ", found " : "", day, name, mac_text);
    bool ok = n > 0 && (size_t)n < sizeof(text) && write(fd, text, (size_t)n) == n;
    if (!ok)
        complain(NETNAMES_FILE);
    close(fd);
    return ok;
}

/* (50h) A file made only if it is not there (O_EXCL); false on an error. */
static bool make_missing(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return errno == EEXIST;
    size_t n = strlen(text);
    bool ok = write(fd, text, n) == (ssize_t)n;
    close(fd);
    if (!ok)
        complain(path);
    return ok;
}

/* An elf's file as net up makes it, made only if missing. */
static void elf_missing(const char *name, const char *what)
{
    char path[64], text[160];
    snprintf(path, sizeof(path), "%s/%s", ELVES_DIR, name);
    snprintf(text, sizeof(text), "# %s, an elf (netnamesd made this file)\ncommand = %s\n"
             "start = on\n", what, name);
    make_missing(path, text);
}

/* A new card's settings file and the elf that gets it its address; on a
 * machine net up never set up, the other network elves too. */
static void settings_for(const char *name)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", NETWORK_CONFIG, name);
    struct stat st;
    if (stat(path, &st) == 0)
        return;                                 /* a card known before: its own settings */
    if (!make_missing(path, NETNAMES_SETTINGS))
        return;
    snprintf(path, sizeof(path), "%s/guardd", ELVES_DIR);
    bool set_up = stat(path, &st) == 0;
    elf_missing("dhclient", "The DHCP client");
    if (set_up)
        return;
    elf_missing("dnsd", "Names for the programs");
    elf_missing("sshd", "The SSH server");
    elf_missing("guardd", "The firewall");
}

/* One look: every card the kernel has that is plugged in and not in the
 * file is added (with its settings when it has none). */
static void look(void)
{
    int64_t n = ritteros_report(REPORT_NET, report, sizeof(report) - 1);
    if (n <= 0)
        return;
    static struct netname known[NETNAMES_MAX];
    int count = netnames_read(known, NETNAMES_MAX);
    for (const char *line = report; line < report + n;) {
        const char *end = memchr(line, '\n', (size_t)(report + n - line));
        if (!end)
            end = report + n;
        char name[8], about[96];
        uint8_t mac[6];
        bool unplugged;
        if (netnames_card_line(line, (size_t)(end - line), name, mac, &unplugged, about,
                               sizeof(about)) && !unplugged) {
            bool there = false;
            for (int i = 0; i < count && !there; i++)
                there = !memcmp(known[i].mac, mac, 6);
            if (!there && record(name, mac, about)) {
                settings_for(name);
                if (count < NETNAMES_MAX) {
                    memcpy(known[count].mac, mac, 6);   /* (a card listed twice: once) */
                    count++;
                }
            }
        }
        line = end + 1;
    }
}

int main(void)
{
    for (;;) {
        look();
        sleep(1);
    }
}
