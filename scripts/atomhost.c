/*
 * (37g-5a) amdgpu's AtomBIOS interpreter (drivers/amdgpu_atom.c) on the
 * Mac, compared with Linux's (atom.c, the oracle: scripts/atomoracle/):
 * every command table of a video BIOS is run by both, each on its own
 * simulated machine, and what they did must be the same: every register
 * read and write in order (with the values), every wait, the result, the
 * parameters and the scratch space afterwards.
 *
 * The machine: a register reads what was last written to it, else a value
 * made from its index and the run's seed (so tables that poll a bit see it
 * sometimes set). The clock moves a millisecond at every look; atom.c
 * looks at jiffies, ours at its now_ms, at the same places. Round 0 runs
 * every table with its parameters 0, each further round with parameters
 * made from the seed. A run that does more than CUT things or looks at the
 * clock more than TICKS times is cut there in both (some poll for ever on
 * a machine that never answers; Linux has no other limit): the parts
 * before the cut are compared. (Between two looks a table runs forward
 * only, at most its length: TICKS of them stay below ours' ATOM_STEPS.)
 *
 * IMAGE: a VFCT table (ACPI's, as the laptops' firmware gives it; its
 * first image) or a ROM file. -f COUNT instead: COUNT images of its own
 * making from seed -s SEED on (random code of every opcode, valid
 * encodings, kept to what both interpreters define alike: see
 * make_image), a difference printed with its image's seed.
 *
 *   clang -c -w -fno-sanitize=shift -I scripts/atomoracle \
 *       -I tools/src/atom-VER -o oracle.o scripts/atomoracle/oracle.c
 *   clang -DATOM_STEP=atomhost_step -I drivers -o atomhost \
 *       scripts/atomhost.c drivers/amdgpu_atom.c oracle.o
 *   atomhost [-v] [-r ROUNDS] IMAGE
 *   atomhost [-v] [-r ROUNDS] -f COUNT [-s SEED]
 *   atomhost [-t TRACE] [-i REGISTERS] -c TABLE:P0,... [-c ...] IMAGE | -f COUNT [-s SEED]
 *
 * (37g-5c) -c: the calls given, in order, each with its parameters (the
 * first dwords; the rest 0), on one machine whose registers stay from
 * call to call, as a driver's sequence runs; -t writes what ours did
 * (each call, then its reads, writes and waits) to TRACE; -i FILE starts
 * both machines with those registers ("INDEX VALUE" lines: the state a
 * machine was found in, as `displays sleep` lists it).
 *
 * Afterwards it lists the opcodes ours ran and at how many places. Exit
 * status 1 when anything differed.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amdgpu_atom.h"

void *oracle_parse(void *bios, uint32_t *scratch, int scratch_bytes);
int oracle_run(void *ctx, int index, uint32_t *params);
void oracle_destroy(void *ctx);

#define CUT             1000000         /* events of one run at most */
#define TICKS           100000          /* looks at the clock in one run at most */
#define SCRATCH         0x10000         /* bytes of the "frame buffer" scratch */
#define ORACLE_PARAMS   4096            /* the oracle's parameter space (unchecked there) */
#define ORACLE_ROM      0x40000         /* the oracle's copy of the image (unchecked reads) */
#define SLOTS           (1 << 16)       /* registers written in one run, at most */

struct event {
    char kind;                          /* R/W register, P/p PLL, M/m MC (read/write), D wait */
    uint32_t index, value;
};

struct machine {
    struct event *events;
    size_t count;
    uint64_t ticks;
    bool cut;
    uint64_t seed;
    jmp_buf out;
    uint32_t keys[SLOTS], values[SLOTS];
    uint8_t used[SLOTS];
};

static struct machine *now;

/* What ran of ours (drivers/amdgpu_atom.c's ATOM_STEP): each opcode's
 * count, and each place in the image that held an instruction run. */
static uint64_t opcodes[256];
static uint8_t ran[0x20000];

void atomhost_step(uint32_t place, uint8_t opcode)
{
    opcodes[opcode]++;
    if (place < sizeof(ran))
        ran[place] = 1;
}

static uint64_t mix(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ x >> 33;
}

static void cut_here(struct machine *m)
{
    m->cut = true;
    longjmp(m->out, 1);
}

static void record(char kind, uint32_t index, uint32_t value)
{
    struct machine *m = now;
    if (m->count == CUT)
        cut_here(m);
    m->events[m->count++] = (struct event){ kind, index, value };
}

static uint32_t slot_of(struct machine *m, int kind, uint32_t index)
{
    uint32_t key = (uint32_t)kind << 24 ^ index;
    uint32_t s = (uint32_t)mix(key) & (SLOTS - 1);
    while (m->used[s] && m->keys[s] != key)
        s = (s + 1) & (SLOTS - 1);
    m->keys[s] = key;
    return s;
}

uint32_t atomhost_read(int kind, uint32_t index)
{
    struct machine *m = now;
    uint32_t s = slot_of(m, kind, index);
    uint32_t v = m->used[s] ? m->values[s]
               : (uint32_t)mix(m->seed * 0x9E3779B97F4A7C15ull ^ (uint64_t)kind << 40 ^ index);
    record((char)kind, index, v);
    return v;
}

void atomhost_write(int kind, uint32_t index, uint32_t value)
{
    struct machine *m = now;
    uint32_t s = slot_of(m, kind, index);
    m->used[s] = 1;
    m->values[s] = value;
    record((char)(kind + 'a' - 'A'), index, value);
}

void atomhost_delay_us(unsigned long us)
{
    record('D', 0, (uint32_t)us);
}

/* The clock: the value, then a millisecond on (as jiffies++). */
unsigned long atomhost_tick(void)
{
    struct machine *m = now;
    if (m->ticks == TICKS)
        cut_here(m);
    return 1000 + m->ticks++;
}

static uint32_t ours_read(void *ctx, uint32_t index)
{
    (void)ctx;
    return atomhost_read('R', index);
}

static void ours_write(void *ctx, uint32_t index, uint32_t value)
{
    (void)ctx;
    atomhost_write('R', index, value);
}

static void ours_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    atomhost_delay_us(us);
}

static uint64_t ours_now(void *ctx)
{
    (void)ctx;
    return atomhost_tick();
}

/* (37g-5c) -i: registers the machines start with. */
#define PRESET 4096
static uint32_t preset_index[PRESET], preset_value[PRESET];
static int presets;

static void reset(struct machine *m, uint64_t seed)
{
    m->count = 0;
    m->ticks = 0;
    m->cut = false;
    m->seed = seed;
    memset(m->used, 0, sizeof(m->used));
    for (int k = 0; k < presets; k++) {
        uint32_t s = slot_of(m, 'R', preset_index[k]);
        m->used[s] = 1;
        m->values[s] = preset_value[k];
    }
}

/* The image's ROM: a VFCT's first image, else the file as it is. */
static uint8_t *load(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 0;
    }
    static uint8_t file[1 << 20];
    size_t n = fread(file, 1, sizeof(file), f);
    fclose(f);
    uint8_t *rom = file;
    uint32_t len = (uint32_t)n;
    if (n >= 0x38 && !memcmp(file, "VFCT", 4)) {
        uint32_t at;
        memcpy(&at, file + 0x34, 4);
        if (at + 28 > n) {
            fprintf(stderr, "%s: a VFCT without its image\n", path);
            return 0;
        }
        memcpy(&len, file + at + 0x18, 4);
        rom = file + at + 28;
        if (at + 28 + (size_t)len > n) {
            fprintf(stderr, "%s: the VFCT's image is cut short\n", path);
            return 0;
        }
    }
    uint8_t *copy = calloc(1, len);
    memcpy(copy, rom, len);
    *size = len;
    return copy;
}

/* ---- Images of our own making (-f) ----------------------------------- */

/* The layout: the PCI ROM's header and Linux's marks, the ROM header at
 * 0x100, the master command table at 0x200, the master data table at
 * 0x300, eight data blocks of random bytes from 0x2000, the indirect I/O
 * programs at 0x4000, the command tables from 0x5000 (0x1000 each). All
 * of it in a buffer of 0x20010 bytes, the rest zeros: the farthest an ID
 * operand reaches (two 16-bit numbers) is inside, for ours as for Linux's
 * (which never checks), so both read the same zeros there. */
#define G_SIZE          0x20010
#define G_TABLES        12              /* the last one missing */
#define G_TABLE(t)      (0x5000u + 0x1000u * (t))
#define G_IIO           0x4000
#define G_REGS          16

struct gen {
    uint8_t *rom;
    uint64_t s;
    uint32_t at;                        /* where the next byte goes */
    uint32_t regs[G_REGS];              /* the registers its code uses */
    int table, ws;
    uint32_t starts[128];               /* this table's instructions, from its start */
    int n_starts;
    uint32_t fix[512];                  /* places of jump targets still to fill */
    bool forward[512];                  /* (a switch's: only past it) */
    int n_fix;
};

static uint32_t rnd(struct gen *g)
{
    g->s = mix(g->s + 0x9E3779B97F4A7C15ull);
    return (uint32_t)g->s;
}

static uint32_t below(struct gen *g, uint32_t n)
{
    return n ? rnd(g) % n : 0;
}

static void b8(struct gen *g, uint32_t v)
{
    g->rom[g->at++] = (uint8_t)v;
}

static void b16(struct gen *g, uint32_t v)
{
    b8(g, v);
    b8(g, v >> 8);
}

static void b32(struct gen *g, uint32_t v)
{
    b16(g, v);
    b16(g, v >> 16);
}

/* A value: small numbers mostly (indexes, flags, bits), sometimes any. */
static uint32_t value(struct gen *g)
{
    switch (below(g, 4)) {
    case 0: return below(g, 4);
    case 1: return below(g, 256);
    case 2: return 1u << below(g, 32);
    default: return rnd(g);
    }
}

/* An immediate of part align (the size its part says). */
static void imm(struct gen *g, int align, uint32_t v)
{
    if (align == 0)
        b32(g, v);
    else if (align <= 3)
        b16(g, v);
    else
        b8(g, v);
}

/* An operand of kind k (REG PS WS FB ID IMM) with part align. Kept to
 * what both define alike: PS indexes inside ours' 64 dwords (with every
 * call's shift), WS ones inside the table's work space or the
 * interpreter's own (0x40-0x48: not the FB window as a destination, whose
 * random values would send Linux's unchecked scratch reads anywhere), ID
 * ones near their block. */
static void operand(struct gen *g, int k, int align, bool dest)
{
    switch (k) {
    case 0:
        b16(g, below(g, 8) ? g->regs[below(g, G_REGS)] : rnd(g) & 0xFFFF);
        break;
    case 1:
        b8(g, below(g, 16));
        break;
    case 2:
        if (g->ws && below(g, 4))
            b8(g, below(g, (uint32_t)g->ws));
        else {
            uint32_t w;
            do
                w = 0x40 + below(g, 9);
            while (dest && w == 0x46);
            b8(g, w);
        }
        break;
    case 3:
        b8(g, below(g, 64));
        break;
    case 4:
        b16(g, below(g, 0x400));
        break;
    case 5:
        imm(g, align, value(g));
        break;
    }
}

/* An attribute byte: a source of kind 0-5 and part 0-7, a destination
 * part 0-3. */
static uint8_t attribute(struct gen *g, int *src, int *align)
{
    *src = (int)below(g, 6);
    *align = (int)below(g, 8);
    return (uint8_t)(*src | *align << 3 | below(g, 4) << 6);
}

/* An operation's destination and source, after its opcode. */
static void two_operands(struct gen *g, int kind, bool dest)
{
    int src, align;
    b8(g, attribute(g, &src, &align));
    operand(g, kind, 0, dest);
    operand(g, src, align, false);
}

static void jump_target(struct gen *g, bool forward)
{
    g->forward[g->n_fix] = forward;
    g->fix[g->n_fix++] = g->at;
    b16(g, 0);
}

/* One instruction (of the table g->table). */
static void instruction(struct gen *g)
{
    g->starts[g->n_starts++] = g->at - G_TABLE(g->table);
    int kind = (int)below(g, 4);                /* a destination: REG PS WS FB */
    int src, align;
    uint8_t attr;
    switch (below(g, 30)) {
    case 0: case 1: case 2: case 3: case 4: case 5: {
        /* and, or, add, sub, xor, mul, div, shl, shr (any count: a byte) */
        static const uint8_t base[] = { 7, 13, 43, 49, 103, 31, 37, 109, 115 };
        b8(g, base[below(g, 9)] + kind);
        two_operands(g, kind, true);
        break;
    }
    case 6:                                     /* shift left, right: a byte's count */
        b8(g, (below(g, 2) ? 19 : 25) + kind);
        b8(g, below(g, 256));
        operand(g, kind, 0, true);
        b8(g, below(g, 3) ? below(g, 32) : below(g, 256));
        break;
    case 7: case 8:                             /* compare, test */
        b8(g, (below(g, 2) ? 60 : 74) + kind);
        two_operands(g, kind, false);
        break;
    case 9: case 10: case 11:                   /* jumps */
        b8(g, 67 + below(g, 7));
        jump_target(g, false);
        break;
    case 12: {                                  /* switch */
        attr = attribute(g, &src, &align);
        b8(g, 66);
        b8(g, attr);
        operand(g, src, align, false);
        int cases = (int)below(g, 5);
        for (int c = 0; c < cases; c++) {
            b8(g, 0x63);
            imm(g, align, below(g, 2) ? below(g, 4) : value(g));
            jump_target(g, true);
        }
        b16(g, 0x5A5A);
        break;
    }
    case 13:                                    /* delay */
        b8(g, below(g, 2) ? 80 : 81);
        b8(g, below(g, 256));
        break;
    case 14:                                    /* call a later table (or the missing one) */
        if (g->table + 1 < G_TABLES) {
            b8(g, 82);
            b8(g, g->table + 1 + below(g, (uint32_t)(G_TABLES - 1 - g->table)));
        } else {
            b8(g, 90);
        }
        break;
    case 15:                                    /* clear */
        b8(g, 84 + kind);
        b8(g, below(g, 256));
        operand(g, kind, 0, true);
        break;
    case 16:                                    /* mask */
        b8(g, 92 + kind);
        attr = attribute(g, &src, &align);
        b8(g, attr);
        operand(g, kind, 0, true);
        imm(g, align, value(g));
        operand(g, src, align, false);
        break;
    case 17:                                    /* set_port: MM, or IIO 1 or 2 */
        b8(g, 55);
        b16(g, below(g, 3));
        break;
    case 18:
        b8(g, 58);                              /* set_reg_block */
        b16(g, below(g, 2) ? 0 : below(g, 0x200));
        break;
    case 19:
        b8(g, 59);                              /* set_fb_base: a small immediate */
        b8(g, 5 | 4 << 3);
        b8(g, below(g, 64) * 4);
        break;
    case 20:
        b8(g, 102);                             /* set_data_block */
        b8(g, below(g, 4) ? 1 + below(g, 8) : below(g, 2) ? 0 : 255);
        break;
    case 21: {                                  /* nop, postcard, beep, debug, process_ds */
        uint32_t w = below(g, 5);
        b8(g, w == 0 ? 90 : w == 1 ? 98 : w == 2 ? 99 : w == 3 ? 121 : 122);
        if (w == 1 || w == 3)
            b8(g, rnd(g));
        if (w == 4) {
            uint32_t n = below(g, 24);
            b16(g, n);
            while (n--)
                b8(g, rnd(g));
        }
        break;
    }
    case 22: {                                  /* mul32, div32: PS (odd) or WS (even) */
        uint32_t op = 123 + below(g, 4);
        b8(g, op);
        two_operands(g, op & 1 ? 1 : 2, false);
        break;
    }
    case 23:                                    /* the end now and then: no opcode */
        if (below(g, 4) == 0) {
            b8(g, below(g, 2) ? 0 : 127 + below(g, 129));
            break;
        }
        /* fall through */
    default:                                    /* moves the most */
        b8(g, 1 + kind);
        two_operands(g, kind, true);
    }
}

/* One indirect I/O program numbered n at g->at, its steps from seed. */
static void iio_program(struct gen *g, int n, uint64_t seed)
{
    uint64_t keep = g->s;
    g->s = seed;
    b8(g, 1);
    b8(g, n);
    int steps = 1 + (int)below(g, 10);
    for (int i = 0; i < steps; i++) {
        uint32_t op = below(g, 8);
        uint32_t bits = 1 + below(g, 32);
        switch (op) {
        case 0: b8(g, 0); break;
        case 1: b8(g, 2); b16(g, g->regs[below(g, G_REGS)]); break;
        case 2: b8(g, 3); b16(g, g->regs[below(g, G_REGS)]); break;
        case 3: case 4:                         /* clear, set: bits, at */
            b8(g, op + 1); b8(g, bits); b8(g, below(g, 32));
            break;
        default:                                /* moves: bits, from, to */
            b8(g, op + 1); b8(g, bits); b8(g, below(g, 32)); b8(g, below(g, 32));
        }
    }
    b8(g, 9);
    b16(g, 0);
    g->s = keep;
}

/* An image from seed, in rom (G_SIZE bytes). What Linux's interpreter
 * does differently is left out: PLL and MC operands (ours stops, there
 * are none on these chips), PCI and system I/O ports, a missing indirect
 * I/O program and a switch without its case mark (Linux goes on, ours
 * stops), work space past the table's (Linux reads past its memory),
 * repeat, savereg, restorereg (Linux skips no operands), calls back to a
 * table already running (Linux has no depth limit). Linux writes through
 * the indirect I/O program 0x80 | n, reads through n (atom.c's & 0xFF in
 * atom_put_dst): each program n is given twice, the same. */
static void make_image(uint8_t *rom, uint64_t seed)
{
    memset(rom, 0, G_SIZE);
    struct gen g = { .rom = rom, .s = seed };
    rom[0] = 0x55, rom[1] = 0xAA, rom[2] = 0x80;
    memcpy(rom + 0x30, " 761295520", 10);
    rom[0x48] = 0x00, rom[0x49] = 0x01;
    memcpy(rom + 0x80, "ATOMHOST", 8);          /* (the part number atom.c looks for) */
    memcpy(rom + 0x104, "ATOM", 4);
    rom[0x11E] = 0x00, rom[0x11F] = 0x02;
    rom[0x120] = 0x00, rom[0x121] = 0x03;
    for (int i = 0; i < G_REGS; i++)
        g.regs[i] = i == 0 ? 0 : 1 + below(&g, 0x3000);
    /* the data table: blocks 1-8, the IIO's (index 23, at 0x32) */
    g.at = 0x300;
    b16(&g, 4 + 2 * 32);
    for (int i = 1; i <= 8; i++) {
        g.at = 0x304 + 2 * i;
        b16(&g, 0x2000 + 0x400 * (i - 1));
    }
    for (uint32_t a = 0x2000; a < 0x4000; a++)
        rom[a] = (uint8_t)rnd(&g);
    g.at = 0x332;
    b16(&g, G_IIO);
    g.at = G_IIO + 4;
    for (int n = 1; n <= 2; n++) {
        uint64_t s = mix(seed ^ (uint64_t)n << 56);
        iio_program(&g, n, s);
        iio_program(&g, 0x80 | n, s);
    }
    /* the command tables */
    g.at = 0x200;
    b16(&g, 4 + 2 * G_TABLES);
    for (int t = 0; t < G_TABLES - 1; t++) {
        g.at = 0x204 + 2 * t;
        b16(&g, G_TABLE(t));
    }
    for (int t = 0; t < G_TABLES - 1; t++) {
        g.table = t;
        g.ws = (int)below(&g, 13);
        g.n_starts = g.n_fix = 0;
        g.at = G_TABLE(t);
        b16(&g, 0);
        b8(&g, 1);
        b8(&g, 1);
        b8(&g, g.ws);
        static const uint8_t ps[] = { 0, 4, 8, 16 };
        b8(&g, ps[below(&g, 4)] | (below(&g, 2) ? 0x80 : 0));
        int count = 4 + (int)below(&g, 60);
        for (int i = 0; i < count; i++)
            instruction(&g);
        g.starts[g.n_starts++] = g.at - G_TABLE(t);
        b8(&g, 91);                             /* EOT */
        uint32_t size = g.at - G_TABLE(t);
        rom[G_TABLE(t)] = (uint8_t)size, rom[G_TABLE(t) + 1] = (uint8_t)(size >> 8);
        for (int f = 0; f < g.n_fix; f++) {
            /* (a switch's case only forward: Linux looks at its clock only
             * at jumps, so a loop through a switch would never end there) */
            uint32_t to, from = g.fix[f] - G_TABLE(t);
            do
                to = g.starts[below(&g, (uint32_t)g.n_starts)];
            while (g.forward[f] && to <= from);
            rom[g.fix[f]] = (uint8_t)to, rom[g.fix[f] + 1] = (uint8_t)(to >> 8);
        }
    }
}

/* ---- Running both ----------------------------------------------------- */

static void print_event(const char *who, const struct event *e, size_t at)
{
    if (!e) {
        printf("    %s: (nothing more) at event %zu\n", who, at);
        return;
    }
    printf("    %s: event %zu: %c 0x%x = 0x%08x\n", who, at, e->kind, e->index, e->value);
}

struct totals {
    int present, same, differ, cuts;
    uint64_t events;
    char stops[16][96];
    int stop_counts[16], kinds;
};

static struct machine ours_m, theirs_m;
static uint32_t ours_scratch[SCRATCH / 4], theirs_scratch[SCRATCH / 4 + 4];

/* Every table of an image, rounds times; label names the image. */
static void run_image(const uint8_t *rom, uint32_t size, int rounds, bool verbose,
                      const char *label, struct totals *tot)
{
    struct atom a;
    struct atom_machine am = { ours_read, ours_write, ours_delay, ours_now, 0 };
    if (!atom_init(&a, rom, size, &am, ours_scratch, SCRATCH)) {
        printf("%s: ours refuses it: %s\n", label, a.why);
        tot->differ++;
        return;
    }
    static uint8_t bios[ORACLE_ROM];
    memset(bios, 0, sizeof(bios));
    memcpy(bios, rom, size < ORACLE_ROM ? size : ORACLE_ROM);
    void *ctx = oracle_parse(bios, theirs_scratch, SCRATCH);
    if (!ctx) {
        printf("%s: the oracle refuses it\n", label);
        tot->differ++;
        return;
    }
    int tables = a.tables;              /* the master command table's entries */

    for (int round = 0; round < rounds; round++) {
        for (int t = 0; t < tables; t++) {
            uint16_t tsize;
            uint8_t frev, crev, ws, ps;
            uint16_t base = atom_table(&a, t, &tsize, &frev, &crev, &ws, &ps);
            if (!base)
                continue;
            if (round == 0)
                tot->present++;
            uint32_t ours_ps[ATOM_PARAMS], theirs_ps[ORACLE_PARAMS];
            memset(ours_ps, 0, sizeof(ours_ps));
            memset(theirs_ps, 0, sizeof(theirs_ps));
            if (round > 0) {
                for (int k = 0; k < 16; k++) {
                    uint64_t r = mix((uint64_t)round << 32 ^ (uint64_t)t << 8 ^ (uint64_t)k);
                    /* small numbers mostly: indexes, flags, actions */
                    uint32_t v = (r & 3) == 0 ? (uint32_t)(r >> 8) : (uint32_t)(r >> 8) & 0xFF;
                    ours_ps[k] = theirs_ps[k] = v;
                }
            }
            memset(ours_scratch, 0, sizeof(ours_scratch));
            memset(theirs_scratch, 0, sizeof(theirs_scratch));
            uint64_t seed = (uint64_t)round * 1000 + (uint64_t)t + 1;

            int ours = 0, theirs = 0;
            reset(&ours_m, seed);
            now = &ours_m;
            if (!setjmp(ours_m.out))
                ours = atom_run(&a, t, ours_ps);
            reset(&theirs_m, seed);
            now = &theirs_m;
            if (!setjmp(theirs_m.out))
                theirs = oracle_run(ctx, t, theirs_ps);

            /* The same?  When cut, only what came before the cut. */
            bool cut = ours_m.cut || theirs_m.cut;
            size_t n = ours_m.count < theirs_m.count ? ours_m.count : theirs_m.count;
            size_t first = n;
            for (size_t e = 0; e < n; e++)
                if (memcmp(&ours_m.events[e], &theirs_m.events[e], sizeof(struct event))) {
                    first = e;
                    break;
                }
            bool ok = first == n && (cut ? ours_m.cut && theirs_m.cut
                                         : ours_m.count == theirs_m.count);
            const char *what = 0;
            if (ok && !cut) {
                if ((ours == 0) != (theirs == 0))
                    ok = false, what = "the result";
                else if (memcmp(ours_ps, theirs_ps, sizeof(ours_ps)))
                    ok = false, what = "the parameters";
                else if (memcmp(ours_scratch, theirs_scratch, sizeof(ours_scratch)))
                    ok = false, what = "the scratch";
                else
                    for (int k = ATOM_PARAMS; k < ORACLE_PARAMS; k++)
                        if (theirs_ps[k]) {
                            ok = false, what = "the oracle's parameters past ours";
                            break;
                        }
            }
            tot->events += ours_m.count;
            if (cut)
                tot->cuts++;
            if (ok) {
                tot->same++;
                if (ours == -1 && !cut) {
                    int k = 0;
                    while (k < tot->kinds && strcmp(tot->stops[k], a.why))
                        k++;
                    if (k == tot->kinds && tot->kinds < 16) {
                        strcpy(tot->stops[tot->kinds], a.why);
                        tot->stop_counts[tot->kinds++] = 0;
                    }
                    if (k < 16)
                        tot->stop_counts[k]++;
                }
            } else {
                tot->differ++;
            }
            if (!ok || verbose) {
                printf("%s: round %d table %d (0x%04x, %u bytes, %d.%d, ws %u ps %u): %s"
                       " (ours %d%s%s, the oracle's %d; %zu/%zu events%s)\n",
                       label, round, t, base, tsize, frev, crev, ws, ps,
                       ok ? "same" : "DIFFERENT", ours, a.why[0] ? ": " : "", a.why, theirs,
                       ours_m.count, theirs_m.count, cut ? ", cut" : "");
                if (!ok && what)
                    printf("    %s differ\n", what);
                else if (!ok) {
                    print_event("ours", first < ours_m.count ? &ours_m.events[first] : 0, first);
                    print_event("the oracle's",
                                first < theirs_m.count ? &theirs_m.events[first] : 0, first);
                    if (first > 0)
                        print_event("before both", &ours_m.events[first - 1], first - 1);
                }
            }
        }
    }
    oracle_destroy(ctx);
}

/* (37g-5c) The calls of -c, in order on one machine each. */
#define CALLS 64
struct call {
    int table;
    uint32_t ps[8];
    int count;
};

static char whys[CALLS][96];             /* why each of ours stopped early */

static size_t run_sequence(struct atom *a, void *ctx, struct machine *m, const struct call *calls,
                           int n, bool ours, int *results, uint32_t (*params)[ATOM_PARAMS],
                           size_t *starts)
{
    reset(m, 1);
    now = m;
    for (int c = 0; c < n; c++) {
        starts[c] = m->count;
        uint32_t *ps = params[c];
        memset(ps, 0, sizeof(uint32_t) * ATOM_PARAMS);
        for (int k = 0; k < calls[c].count; k++)
            ps[k] = calls[c].ps[k];
        if (setjmp(m->out)) {
            results[c] = 1;                 /* (cut) */
            return (size_t)c;
        }
        if (ours) {
            results[c] = atom_run(a, calls[c].table, ps);
            memcpy(whys[c], a->why, sizeof whys[c]);
        } else {
            static uint32_t big[ORACLE_PARAMS];
            memset(big, 0, sizeof(big));
            memcpy(big, ps, sizeof(uint32_t) * ATOM_PARAMS);
            results[c] = oracle_run(ctx, calls[c].table, big);
            memcpy(ps, big, sizeof(uint32_t) * ATOM_PARAMS);
        }
    }
    return (size_t)n;
}

static int run_calls(const uint8_t *rom, uint32_t size, const struct call *calls, int n,
                     const char *trace, const char *label)
{
    struct atom a;
    struct atom_machine am = { ours_read, ours_write, ours_delay, ours_now, 0 };
    if (!atom_init(&a, rom, size, &am, ours_scratch, SCRATCH)) {
        printf("%s: ours refuses it: %s\n", label, a.why);
        return 1;
    }
    static uint8_t bios[ORACLE_ROM];
    memcpy(bios, rom, size < ORACLE_ROM ? size : ORACLE_ROM);
    void *ctx = oracle_parse(bios, theirs_scratch, SCRATCH);
    if (!ctx) {
        printf("%s: the oracle refuses it\n", label);
        return 1;
    }
    static int ours_r[CALLS], theirs_r[CALLS];
    static uint32_t ours_p[CALLS][ATOM_PARAMS], theirs_p[CALLS][ATOM_PARAMS];
    static size_t ours_at[CALLS], theirs_at[CALLS];
    memset(ours_scratch, 0, sizeof(ours_scratch));
    memset(theirs_scratch, 0, sizeof(theirs_scratch));
    size_t done = run_sequence(&a, 0, &ours_m, calls, n, true, ours_r, ours_p, ours_at);
    size_t theirs_done = run_sequence(0, ctx, &theirs_m, calls, n, false, theirs_r, theirs_p,
                                      theirs_at);
    int differ = 0;
    size_t e = 0, ne = ours_m.count < theirs_m.count ? ours_m.count : theirs_m.count;
    while (e < ne && !memcmp(&ours_m.events[e], &theirs_m.events[e], sizeof(struct event)))
        e++;
    /* both cut (in the same call): what came before the cut is compared */
    bool cut = ours_m.cut && theirs_m.cut && done == theirs_done;
    if (ours_m.cut != theirs_m.cut)
        differ++;
    for (int c = 0; c < n; c++) {
        if (cut && c >= (int)done)
            break;
        bool same = c < (int)done && c < (int)theirs_done && (ours_r[c] == 0) == (theirs_r[c] == 0) &&
                    !memcmp(ours_p[c], theirs_p[c], sizeof(ours_p[c]));
        size_t end = c + 1 < n ? ours_at[c + 1] : ours_m.count;
        printf("%s: call %d: table %d (", label, c, calls[c].table);
        for (int k = 0; k < calls[c].count; k++)
            printf("%s0x%x", k ? "," : "", calls[c].ps[k]);
        printf("): ours %d%s%s, the oracle's %d; %zu events; parameters after: 0x%x 0x%x 0x%x%s\n",
               ours_r[c], ours_r[c] == -1 ? ": " : "", ours_r[c] == -1 ? whys[c] : "", theirs_r[c],
               c < (int)done ? end - ours_at[c] : (size_t)0, ours_p[c][0], ours_p[c][1],
               ours_p[c][2], same ? "" : "  DIFFERENT");
        differ += !same;
    }
    bool events_same = e == ne && (cut || ours_m.count == theirs_m.count);
    if (!events_same) {
        differ++;
        printf("%s: the events differ from event %zu on:\n", label, e);
        print_event("ours", e < ours_m.count ? &ours_m.events[e] : 0, e);
        print_event("the oracle's", e < theirs_m.count ? &theirs_m.events[e] : 0, e);
    }
    if (!cut && memcmp(ours_scratch, theirs_scratch, sizeof(ours_scratch))) {
        differ++;
        printf("%s: the scratch differs\n", label);
    }
    printf("atomhost: %s: %d calls, %zu events: %s%s\n", label, n, ours_m.count,
           differ ? "DIFFERENT" : "the same as the oracle's", cut ? " (cut)" : "");
    if (trace) {
        FILE *f = fopen(trace, "w");
        if (!f) {
            perror(trace);
            return 1;
        }
        for (int c = 0; c < (int)done; c++) {
            size_t end = c + 1 < n ? ours_at[c + 1] : ours_m.count;
            fprintf(f, "call %d: table %d, result %d\n", c, calls[c].table, ours_r[c]);
            for (size_t k = ours_at[c]; k < end; k++)
                fprintf(f, "  %c 0x%05x 0x%08x\n", ours_m.events[k].kind, ours_m.events[k].index,
                        ours_m.events[k].value);
        }
        fclose(f);
    }
    oracle_destroy(ctx);
    return differ ? 1 : 0;
}

int main(int argc, char **argv)
{
    int rounds = 4, images = 0, ncalls = 0;
    uint64_t seed = 1;
    bool verbose = false;
    const char *trace = 0;
    static struct call calls[CALLS];
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v"))
            verbose = true;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc)
            rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc)
            images = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            seed = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            trace = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            FILE *f = fopen(argv[++i], "r");
            if (!f) {
                perror(argv[i]);
                return 2;
            }
            unsigned long a, b;
            while (presets < PRESET && fscanf(f, "%lx %lx", &a, &b) == 2) {
                preset_index[presets] = (uint32_t)a;
                preset_value[presets++] = (uint32_t)b;
            }
            fclose(f);
        }
        else if (!strcmp(argv[i], "-c") && i + 1 < argc && ncalls < CALLS) {
            char *p = argv[++i];
            struct call *c = &calls[ncalls++];
            c->table = (int)strtol(p, &p, 0);
            if (*p == ':')
                for (p++; *p && c->count < 8; c->count++) {
                    c->ps[c->count] = (uint32_t)strtoul(p, &p, 0);
                    if (*p == ',')
                        p++;
                }
        } else
            break;
    }
    if (images ? i != argc : i + 1 != argc) {
        fprintf(stderr, "usage: atomhost [-v] [-r ROUNDS] IMAGE\n"
                        "       atomhost [-v] [-r ROUNDS] -f COUNT [-s SEED]\n"
                        "       atomhost [-t TRACE] [-i REGISTERS] -c TABLE:P0,P1,... [-c ...]"
                        " IMAGE | -f COUNT [-s SEED]\n");
        return 2;
    }
    ours_m.events = malloc(CUT * sizeof(struct event));
    theirs_m.events = malloc(CUT * sizeof(struct event));
    struct totals tot = { 0 };
    const char *what;
    char label[64];
    if (images) {
        uint8_t *rom = malloc(G_SIZE);
        int sequences_differ = 0;
        for (int k = 0; k < images; k++) {
            make_image(rom, seed + (uint64_t)k);
            snprintf(label, sizeof(label), "seed %llu", (unsigned long long)(seed + (uint64_t)k));
            if (ncalls)
                sequences_differ += run_calls(rom, G_SIZE, calls, ncalls, 0, label);
            else
                run_image(rom, G_SIZE, rounds, verbose, label, &tot);
        }
        if (ncalls) {
            printf("atomhost: %d images from seed %llu, %d calls each: %d different\n", images,
                   (unsigned long long)seed, ncalls, sequences_differ);
            return sequences_differ ? 1 : 0;
        }
        snprintf(label, sizeof(label), "%d images from seed %llu", images,
                 (unsigned long long)seed);
        what = label;
    } else {
        uint32_t size;
        uint8_t *rom = load(argv[i], &size);
        if (!rom)
            return 2;
        if (ncalls)
            return run_calls(rom, size, calls, ncalls, trace, argv[i]);
        run_image(rom, size, rounds, verbose, argv[i], &tot);
        what = argv[i];
    }
    printf("atomhost: %s: %d tables, %d rounds: %d the same, %d different, %d cut;"
           " %llu events\n", what, tot.present, rounds, tot.same, tot.differ, tot.cuts,
           (unsigned long long)tot.events);
    for (int k = 0; k < tot.kinds; k++)
        printf("atomhost: stopped (both) %d times: %s\n", tot.stop_counts[k], tot.stops[k]);
    int kinds_ran = 0, places = 0;
    for (int k = 0; k < 256; k++)
        kinds_ran += opcodes[k] != 0;
    for (size_t k = 0; k < sizeof(ran); k++)
        places += ran[k];
    printf("atomhost: %d opcodes ran, at %d places:", kinds_ran, places);
    for (int k = 0; k < 256; k++)
        if (opcodes[k])
            printf(" %d", k);
    printf("\n");
    return tot.differ ? 1 : 0;
}
