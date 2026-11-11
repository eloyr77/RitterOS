/*
 * Test (38a): a program on the wire of a virtual card.
 *
 * taptest TAP ADDRESS SECONDS: holds /dev/TAP for SECONDS and is a machine
 * at ADDRESS on it (MAC 02:00:00:00:00:02): it answers ARP requests for
 * ADDRESS and ICMP echo requests to it, then prints what it read. First it
 * writes one frame for another MAC address, which the card drops.
 *
 * taptest TAP ADDRESS SECONDS TARGET (38b): the same, and it also asks ARP
 * for TARGET and sends it three echo requests, a second apart: through a
 * bridge the frames reach the other ports' machines (QEMU's network, a
 * program on another tap). Its MAC is 02:00:00:00:00:NN, NN the last byte
 * of ADDRESS, so two of them differ.
 *
 * taptest open TAP: the rules of opening it: a first open, a second one
 * while the first holds it, and (as root) one as another user (uid 4242)
 * after the first is closed.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

static uint8_t our_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t checksum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += get16(p + i);
    if (n & 1)
        sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static int open_rules(const char *tap)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/%s", tap);
    int a = open(path, O_RDWR);
    printf("taptest: first open: %s\n", a >= 0 ? "ok" : strerror(errno));
    int b = open(path, O_RDWR);
    printf("taptest: second open: %s\n", b >= 0 ? "ok" : strerror(errno));
    if (b >= 0)
        close(b);
    if (a >= 0)
        close(a);
    if (setuid(4242) < 0) {
        printf("taptest: setuid: %s\n", strerror(errno));
        return 1;
    }
    int c = open(path, O_RDWR);
    printf("taptest: open as uid 4242: %s\n", c >= 0 ? "ok" : strerror(errno));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "open"))
        return open_rules(argv[2]);
    uint32_t address, target = 0;
    if ((argc != 4 && argc != 5) || !ip_parse(argv[2], &address, 0) ||
        (argc == 5 && !ip_parse(argv[4], &target, 0))) {
        fprintf(stderr, "usage: taptest TAP ADDRESS SECONDS [TARGET] | taptest open TAP\n");
        return 2;
    }
    our_mac[5] = (uint8_t)address;
    char path[32];
    snprintf(path, sizeof(path), "/dev/%s", argv[1]);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        printf("taptest: %s: %s\n", path, strerror(errno));
        return 1;
    }
    uint8_t ip[4] = { (uint8_t)(address >> 24), (uint8_t)(address >> 16), (uint8_t)(address >> 8),
                      (uint8_t)address };
    printf("taptest: %s: %s is 02:00:00:00:00:%02x\n", argv[1], argv[2], our_mac[5]);
    uint8_t to[4] = { (uint8_t)(target >> 24), (uint8_t)(target >> 16), (uint8_t)(target >> 8),
                      (uint8_t)target };
    uint8_t to_mac[6];
    bool to_known = false;
    long asked = 0, pinged = 0, answered = 0;
    time_t next = 0;
    /* A frame for another card's MAC: the tap's filter drops it (net counts it). */
    uint8_t other[60] = { 0x02, 0x99, 0x99, 0x99, 0x99, 0x99, 0x02, 0, 0, 0, 0, 2, 0x08, 0x00 };
    if (write(fd, other, sizeof(other)) != (ssize_t)sizeof(other))
        printf("taptest: a frame for another card: %s\n", strerror(errno));
    long frames = 0, arps = 0, echoes = 0;
    time_t end = time(0) + atoi(argv[3]);
    while (time(0) < end) {
        if (target && time(0) >= next && (!to_known || pinged < 3)) {
            next = time(0) + 1;
            uint8_t q[74];
            memset(q, 0, sizeof(q));
            if (!to_known) {                        /* who has TARGET? */
                memset(q, 0xFF, 6);
                memcpy(q + 6, our_mac, 6);
                put16(q + 12, 0x0806);
                put16(q + 14, 1);
                put16(q + 16, 0x0800);
                q[18] = 6, q[19] = 4;
                put16(q + 20, 1);
                memcpy(q + 22, our_mac, 6);
                memcpy(q + 28, ip, 4);
                memcpy(q + 38, to, 4);
                if (write(fd, q, 42) == 42)
                    asked++;
            } else {                                /* an echo request, 32 bytes of data */
                memcpy(q, to_mac, 6);
                memcpy(q + 6, our_mac, 6);
                put16(q + 12, 0x0800);
                q[14] = 0x45;
                put16(q + 16, 60);
                q[22] = 64, q[23] = 1;
                memcpy(q + 26, ip, 4);
                memcpy(q + 30, to, 4);
                put16(q + 24, checksum(q + 14, 20));
                q[34] = 8;
                put16(q + 38, 0x7454);
                put16(q + 40, (uint16_t)++pinged);
                for (int i = 0; i < 32; i++)
                    q[42 + i] = (uint8_t)i;
                put16(q + 36, checksum(q + 34, 40));
                write(fd, q, 74);
            }
        }
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0)
            continue;
        uint8_t f[1514];
        ssize_t n = read(fd, f, sizeof(f));
        if (n < 14)
            continue;
        frames++;
        uint16_t type = get16(f + 12);
        if (target && type == 0x0806 && n >= 42 && get16(f + 20) == 2 && !memcmp(f + 28, to, 4) &&
            !to_known) {
            memcpy(to_mac, f + 22, 6);
            to_known = true;
            next = 0;
            printf("taptest: %s: %s is %02x:%02x:%02x:%02x:%02x:%02x\n", argv[1], argv[4],
                   to_mac[0], to_mac[1], to_mac[2], to_mac[3], to_mac[4], to_mac[5]);
        } else if (target && type == 0x0800 && n >= 42 && f[23] == 1 && !memcmp(f + 26, to, 4) &&
                   !memcmp(f + 30, ip, 4) && f[34] == 0 && get16(f + 38) == 0x7454) {
            answered++;
            printf("taptest: %s: echo seq=%u answered by %s\n", argv[1], get16(f + 40), argv[4]);
        } else if (type == 0x0806 && n >= 42 && get16(f + 20) == 1 && !memcmp(f + 38, ip, 4)) {
            /* ARP request for us: the reply goes back to the asker. */
            uint8_t r[42];
            memcpy(r, f + 6, 6);
            memcpy(r + 6, our_mac, 6);
            memcpy(r + 12, f + 12, 8);              /* type, hardware, protocol, sizes */
            put16(r + 20, 2);
            memcpy(r + 22, our_mac, 6);
            memcpy(r + 28, ip, 4);
            memcpy(r + 32, f + 22, 10);             /* the asker's MAC and address */
            if (write(fd, r, sizeof(r)) == (ssize_t)sizeof(r))
                arps++;
        } else if (type == 0x0800 && n >= 34 && (f[14] & 0x0F) == 5 && f[23] == 1 &&
                   !memcmp(f + 30, ip, 4) && f[34] == 8) {
            /* An echo request to us: the same with addresses swapped. */
            uint8_t r[1514];
            size_t total = get16(f + 16);
            if (total < 28 || 14 + total > (size_t)n)
                continue;
            memcpy(r, f + 6, 6);
            memcpy(r + 6, our_mac, 6);
            memcpy(r + 12, f + 12, 2 + total);
            memcpy(r + 26, f + 30, 4);
            memcpy(r + 30, f + 26, 4);
            put16(r + 24, 0);
            put16(r + 24, checksum(r + 14, 20));
            r[34] = 0;
            put16(r + 36, 0);
            put16(r + 36, checksum(r + 34, total - 20));
            if (write(fd, r, 14 + total) == (ssize_t)(14 + total))
                echoes++;
        }
    }
    printf("taptest: %s: %ld frame(s) read, %ld ARP answer(s), %ld echo answer(s)\n", argv[1],
           frames, arps, echoes);
    if (target)
        printf("taptest: %s: %s: %ld ARP question(s), %ld of %ld echo request(s) answered\n",
               argv[1], argv[4], asked, answered, pinged);
    close(fd);
    return 0;
}
