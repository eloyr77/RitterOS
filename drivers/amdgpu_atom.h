/*
 * (37g-5a) AtomBIOS, the command tables of AMD's video BIOS, run: amdgpu's
 * interpreter (amdgpu_atom.c, part of the amdgpu module; the same file
 * builds on the Mac for scripts/atomhost.c). The machine is reached only
 * through struct atom_machine: register reads and writes (dword indexes in
 * the chip's register space) and waits.
 */
#ifndef AMDGPU_ATOM_H
#define AMDGPU_ATOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct atom_machine {
    uint32_t (*reg_read)(void *ctx, uint32_t index);
    void (*reg_write)(void *ctx, uint32_t index, uint32_t value);
    void (*delay_us)(void *ctx, uint32_t us);       /* the table's waits */
    uint64_t (*now_ms)(void *ctx);                  /* a clock (0: none) */
    void *ctx;
};

#define ATOM_PARAMS     64          /* dwords of parameter space a run may use */
#define ATOM_DEPTH      16          /* nested table calls at most */
#define ATOM_LOOP_MS    20000       /* the same jump taken again for this long: stuck */
#define ATOM_STEPS      10000000    /* instructions of one table at most */

struct atom {
    const uint8_t *rom;
    uint32_t size;
    uint16_t cmd_table, data_table; /* the master command and data tables */
    uint16_t tables;                /* (37g-5b) the master command table's entries */
    uint16_t iio[256];              /* indirect I/O programs, by number (0: none) */
    uint32_t *scratch;              /* the "frame buffer" scratch space (FB operands) */
    uint32_t scratch_bytes;
    struct atom_machine m;
    /* a run's state, as the tables see it */
    uint16_t data_block, reg_block, io_attr;
    uint32_t fb_base, divmul[2];
    uint8_t shift;
    int io_mode;
    bool equal, above;
    /* (37g-5b) set by the machine when it refuses an access: the run
     * stops there with it as the reason */
    const char *refused;
    /* why the last run stopped early ("" when it did not), and counters */
    char why[96];
    uint64_t ops, reads, writes, delays_us, calls;
};

/* The video BIOS image (a PCI ROM with an ATOM header), checked and its
 * tables found: false (why in a->why) when it is not one. scratch may be 0. */
bool atom_init(struct atom *a, const uint8_t *rom, uint32_t size, const struct atom_machine *m,
               uint32_t *scratch, uint32_t scratch_bytes);

/* Command table index run with params (ATOM_PARAMS dwords, in and out): 0,
 * or -1 when it stopped early (a->why), -2 when the BIOS has no such table. */
int atom_run(struct atom *a, int index, uint32_t *params);

/* Command table index's place and header (0 when there is none): its
 * size, format and content revisions, work and parameter space bytes. */
uint16_t atom_table(const struct atom *a, int index, uint16_t *size, uint8_t *frev,
                    uint8_t *crev, uint8_t *ws, uint8_t *ps);

/* The master data table's entry index (a data table's place, 0 if none). */
uint16_t atom_data_table(const struct atom *a, int index);

#endif
