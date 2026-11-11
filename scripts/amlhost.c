/*
 * (37b-2) The kernel's AML interpreter (kernel/aml.c, aml_run.c) on the
 * Mac: loads ACPI tables (a DSDT first, then SSDTs) with operation regions
 * simulated in memory as acpiexec has them, and prints the namespace as
 * `power names` does, one object a line; scripts/amlcompare.py compares
 * it with acpiexec's. The counters go to stderr. (37b-3) -e FILE: after
 * loading, every path in FILE (one a line) is evaluated instead, its value
 * printed as `power eval` prints it; -a FILE the same after doing what
 * acpiexec does as it starts (_REG, its device searches and tests), for
 * scripts/amleval.py to compare with acpiexec. AMLTRACE=1 lists every
 * region access on stderr.
 *
 *   clang -DAML_HOST -fsanitize=address,undefined -I kernel -o amlhost \
 *       scripts/amlhost.c kernel/aml.c kernel/aml_run.c
 *   amlhost [-i] [-e FILE | -a FILE] dsdt.dat ssdt1.dat ...   (-i: with -a, the
 *                                    boot's _INI as acpiexec without -di)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "aml.h"

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* AMLTRACE=1: every region access on stderr, as acpiexec -vr lists them */
static bool tracing;

/* The spaces acpiexec has handlers for (aeinstall.c): others fail there
 * (-a only: the kernel's simulation takes every space). */
static bool as_acpiexec;

static bool handled(uint8_t space)
{
    return !as_acpiexec || space <= 11 || space == 0x7F || space == 0x80 || space == 0xE4;
}

static bool traced_read(uint8_t space, uint64_t address, int width, uint64_t *value)
{
    if (!handled(space))
        return false;
    bool ok = aml_simulated_read(space, address, width, value);
    if (tracing)
        fprintf(stderr, "trace: space %u read  0x%llX width %d = 0x%llX\n", space,
                (unsigned long long)address, width, (unsigned long long)*value);
    return ok;
}

static bool traced_write(uint8_t space, uint64_t address, int width, uint64_t value)
{
    if (!handled(space))
        return false;
    if (tracing)
        fprintf(stderr, "trace: space %u write 0x%llX width %d = 0x%llX\n", space,
                (unsigned long long)address, width, (unsigned long long)value);
    return aml_simulated_write(space, address, width, value);
}

static void print(const char *line)
{
    puts(line);
}

static void evaluate(const char *path)
{
    if (tracing)
        fprintf(stderr, "trace: evaluate %s\n", path);
    struct aml_node *n = aml_find(path);
    if (!n) {
        printf("%s: not found\n", path);
        return;
    }
    char first[160];
    aml_path(n, first, sizeof(first) - 2);
    strcat(first, ": ");
    struct aml_value v = {0};
    if (aml_evaluate(n, 0, 0, &v))
        aml_print_value(&v, first, print);
    else
        printf("%sfailed\n", first);
    aml_release(&v);
}

int main(int argc, char **argv)
{
    const char *evaluations = 0;
    bool initialize = false;            /* -i before -a: the boot's _INI too */
    if (argc > 1 && !strcmp(argv[1], "-i")) {
        initialize = true;
        argv++, argc--;
    }
    if (argc > 2 && (!strcmp(argv[1], "-e") || !strcmp(argv[1], "-a"))) {
        as_acpiexec = argv[1][1] == 'a';
        evaluations = argv[2];
        argv += 2, argc -= 2;
    }
    static const struct aml_os os = {
        .region_read = traced_read,
        .region_write = traced_write,
        .now_ns = now_ns,
    };
    tracing = getenv("AMLTRACE") != 0;
    aml_init(&os);
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            perror(argv[i]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        /* kept: the tables' methods point into them */
        unsigned char *table = malloc(size > 0 ? (size_t)size : 1);
        if (!table || fread(table, 1, (size_t)size, f) != (size_t)size) {
            fprintf(stderr, "%s: not read\n", argv[i]);
            return 1;
        }
        fclose(f);
        int made = aml_load(table, (uint32_t)size);
        fprintf(stderr, "%s: %d objects\n", argv[i], made);
    }
    aml_loaded();
    if (evaluations && as_acpiexec) {
        /* _REG as acpiexec runs it as it starts (aemain.c, aeinstall.c):
         * the embedded controller's space at every EC device, PCI
         * configuration at every PCI Express root bridge, its other
         * spaces at the root, then PCI configuration at the root (one of
         * ACPICA's default spaces); each search for devices runs every
         * device's _HID */
        struct aml_node *devices[16];
        int n = aml_find_devices("PNP0C09", devices, 16);
        for (int i = 0; i < n; i++)
            aml_run_regs(devices[i], 3);
        n = aml_find_devices("PNP0A08", devices, 16);
        for (int i = 0; i < n; i++)
            aml_run_regs(devices[i], 2);
        aml_find_devices("ACPI0013", devices, 16);    /* (its GED search: _HIDs run again) */
        static const uint8_t spaces[] = { 4, 5, 6, 7, 8, 9, 10, 11, 0x7F, 0x80, 0xE4 };
        for (size_t i = 0; i < sizeof(spaces); i++)
            aml_run_regs(aml_find("\\"), spaces[i]);
        if (initialize) {               /* AcpiInitializeObjects: the boot's own */
            struct aml_init c;
            aml_initialize(&c);
            fprintf(stderr, "amlhost: _INI %u, _STA %u, %u absent, %u failed\n", c.ini, c.sta,
                    c.absent, c.failed);
        } else {
            aml_run_regs(aml_find("\\"), 2);
        }
        /* then its own tests (aetests.c, AeMiscellaneousTests): "Windows
         * 2006" taken out of _OSI, every device's _STA run */
        aml_osi_drop("Windows 2006");
        aml_find_devices(0, devices, 16);
    }
    if (evaluations) {
        FILE *f = fopen(evaluations, "r");
        char line[256];
        while (f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = 0;
            if (line[0])
                evaluate(line);
        }
        if (f)
            fclose(f);
    } else {
        aml_print_names(print);
    }
    struct aml_stats s;
    aml_stats(&s);
    fprintf(stderr, "amlhost: %u objects, %u methods run, %u errors, %u tables, stack %u bytes\n",
            s.nodes, s.methods_run, s.errors, s.loads, s.stack_deepest);
    return 0;
}
