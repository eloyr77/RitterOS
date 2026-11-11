/*
 * power (37a, roadmap item 37): what the machine's firmware offers for
 * power management, read from its ACPI tables without running their code:
 * the FADT's profile and flags (low-power S0 idle, the buttons), the
 * sleep states the firmware names (\_S0_-\_S5_), the FACS (the waking
 * vector), the power devices (battery, AC adapter, lid, buttons, the
 * embedded controller, the low-power S0 device, processors) and where the
 * power methods are defined. The kernel's report (REPORT_POWER), as its
 * rescue console shows it. Later steps add the battery and sleeping.
 *
 *   power tables DIR    (37b-1, root) every ACPI table saved as a file in
 *                       DIR, named as Linux's acpidump -b names them
 *                       (dsdt.dat, facs.dat, apic.dat, ssdt1.dat, ...)
 *   power names         (37b-2, root) every object of the AML namespace the
 *                       kernel's interpreter made from the DSDT and SSDTs,
 *                       one a line: path, type, value
 *   power eval NAME     (37b-3, root) the object NAME evaluated (a method run
 *                       without arguments, a field read), its value printed
 *   power sleep SECONDS (37g-4, root) the machine asleep (ACPI S3) for
 *                       SECONDS, woken by the RTC's alarm
 *   power sleep         (37g-7, root) asleep until the power button (or the
 *                       firmware's own wake: a laptop's lid opened) wakes it
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("power [tables DIR | names | eval NAME | sleep [SECONDS]]",
             "what the firmware offers for power management: sleep states, devices");

static char text[8 * 1024];

/* (37b-1) Every table to DIR/NAME.dat: the signature in small letters, a
 * number from the second of a kind on (ssdt1, ssdt2, ...). */
static int tables(const char *dir)
{
    size_t size = 64 * 1024;
    char *buffer = malloc(size);
    char seen[64][5];
    int kinds = 0, saved = 0;
    for (int i = 0; buffer; i++) {
        int64_t n = ritteros_power_table(i, buffer, size);
        if (n < 0 && errno == ENOENT)
            break;
        if (n < 0) {
            printf("power: tables: %s\n", strerror(errno));
            free(buffer);
            return 1;
        }
        if ((size_t)n > size) {                 /* (too small: once more, large enough) */
            char *more = realloc(buffer, (size_t)n);
            if (!more)
                break;
            buffer = more, size = (size_t)n;
            i--;
            continue;
        }
        char sig[5];
        for (int k = 0; k < 4; k++)
            sig[k] = (char)tolower((unsigned char)buffer[k]);
        sig[4] = 0;
        int same = 0;                           /* (how many of this kind came before) */
        for (int k = 0; k < kinds; k++)
            same += strcmp(seen[k], sig) == 0;
        if (kinds < 64)
            memcpy(seen[kinds++], sig, 5);
        int again = 0;
        for (int k = 0; k < kinds - 1; k++)
            again += strcmp(seen[k], sig) == 0;
        char path[512];
        if (!strcmp(sig, "ssdt") || again)
            snprintf(path, sizeof(path), "%s/%s%d.dat", dir, sig, same + 1);
        else
            snprintf(path, sizeof(path), "%s/%s.dat", dir, sig);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || write(fd, buffer, (size_t)n) != n || close(fd) < 0) {
            printf("power: %s: %s\n", path, strerror(errno));
            free(buffer);
            return 1;
        }
        printf("power: %s: %lld bytes\n", path, (long long)n);
        saved++;
    }
    free(buffer);
    printf("power: %d table(s) saved in %s\n", saved, dir);
    return 0;
}

/* (37b-2) The namespace: asked with a buffer of 256 KiB, again with one
 * large enough if the text is longer. */
static int names(void)
{
    size_t size = 256 * 1024;
    for (int tries = 0; tries < 4; tries++) {
        char *buffer = malloc(size);
        if (!buffer) {
            printf("power: names: out of memory\n");
            return 1;
        }
        int64_t n = ritteros_power_names(buffer, size);
        if (n < 0) {
            printf("power: names: %s\n", strerror(errno));
            free(buffer);
            return 1;
        }
        if ((size_t)n < size) {
            write(STDOUT_FILENO, buffer, (size_t)n);
            free(buffer);
            return 0;
        }
        free(buffer);
        size = (size_t)n + 4096;
    }
    printf("power: names: the namespace keeps growing\n");
    return 1;
}

/* (37b-3) One object's value. */
static int eval(const char *name)
{
    size_t size = 64 * 1024;
    for (int tries = 0; tries < 4; tries++) {
        char *buffer = malloc(size);
        if (!buffer) {
            printf("power: eval: out of memory\n");
            return 1;
        }
        int64_t n = ritteros_power_eval(name, buffer, size);
        if (n < 0) {
            printf("power: eval: %s: %s\n", name,
                   errno == ENOENT ? "no such object"
                   : errno == EIO ? "the evaluation failed (the kernel's log says why)"
                   : strerror(errno));
            free(buffer);
            return 1;
        }
        if ((size_t)n < size) {
            write(STDOUT_FILENO, buffer, (size_t)n);
            free(buffer);
            return 0;
        }
        free(buffer);
        size = (size_t)n + 4096;
    }
    printf("power: eval: %s: the value keeps growing\n", name);
    return 1;
}

/* (37g-4) Asleep for seconds, woken by the RTC's alarm; (37g-7) without
 * (text 0) until woken. */
static int sleep_seconds(const char *text)
{
    char *end;
    unsigned long s = text ? strtoul(text, &end, 10) : 0;
    if (text && (*end || !*text || s < 1 || s > 86399)) {
        printf("power: sleep: SECONDS is 1 to 86399 (a day), not '%s'\n", text);
        return 2;
    }
    if (text)
        printf("power: sleeping %lu s (the RTC's alarm wakes the machine)\n", s);
    else
        printf("power: sleeping until woken (the power button)\n");
    if (ritteros_power_sleep((unsigned)s) < 0) {
        printf("power: sleep: %s\n", errno == EPERM ? "only root may" :
               errno == EIO ? "the machine did not sleep (the kernel's log says why)" :
               strerror(errno));
        return 1;
    }
    printf("power: awake again\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "tables"))
        return tables(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "names"))
        return names();
    if (argc == 3 && !strcmp(argv[1], "eval"))
        return eval(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "sleep"))
        return sleep_seconds(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "sleep"))
        return sleep_seconds(0);
    if (argc != 1) {
        printf("usage: power [tables DIR | names | eval NAME | sleep [SECONDS]]\n");
        return 2;
    }
    int64_t n = ritteros_report(REPORT_POWER, text, sizeof(text));
    if (n < 0)
        return 1;
    write(STDOUT_FILENO, text, n < (int64_t)sizeof(text) ? (size_t)n : sizeof(text) - 1);
    return 0;
}
