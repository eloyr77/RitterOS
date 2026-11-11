#include "disktest.h"
#include "crc32.h"
#include "heap.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "uefi.h"
#include <stdbool.h>
#include <stddef.h>

#define PATTERN_BYTES (64 * 1024)
#define RECORD_MAGIC "RTROSREC"

struct boot_record {
    char magic[8];
    uint64_t boot_number;
    uint64_t seed;
    char time[32];
    uint32_t crc;                   /* CRC32 of everything before it */
} __attribute__((packed));

static uint64_t xorshift(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

void disk_test(struct block_device *part, uint64_t seed)
{
    uint32_t bs = part->block_size;
    uint32_t pattern_blocks = PATTERN_BYTES / bs;
    if (!part->write || part->block_count < pattern_blocks + 1 || bs < sizeof(struct boot_record)) {
        kprintf("  disk test:     %s is not usable for the test\n", part->name);
        return;
    }
    uint64_t record_lba = part->block_count - 1;
    uint64_t pattern_lba = record_lba - pattern_blocks;
    uint8_t *out = kmalloc(PATTERN_BYTES);
    uint8_t *in = kmalloc(PATTERN_BYTES);
    if (!out || !in)
        panic("disk test: out of memory");

    kprintf("  disk test:     %s (%lu MiB), using LBA %lu-%lu\n", part->name,
            part->block_count * bs / (1024 * 1024), pattern_lba, record_lba);

    /* 1. What did the previous boot leave behind? */
    struct boot_record prev;
    if (!part->read(part, record_lba, 1, in))
        panic("disk test: cannot read the boot record");
    memcpy(&prev, in, sizeof(prev));
    bool have_prev = memcmp(prev.magic, RECORD_MAGIC, 8) == 0 &&
                     crc32(&prev, offsetof(struct boot_record, crc)) == prev.crc;
    if (have_prev)
        kprintf("                 previous boot record: boot #%lu, seed 0x%016lX, at %.31s\n",
                prev.boot_number, prev.seed, prev.time);
    else
        kprintf("                 previous boot record: none\n");

    /* 2. Write a pattern derived from this boot's seed, flush, read it back. */
    uint64_t state = seed ? seed : 1;
    for (uint32_t i = 0; i < PATTERN_BYTES / 8; i++) {
        uint64_t v = xorshift(&state);
        memcpy(out + i * 8, &v, 8);
    }
    if (!part->write(part, pattern_lba, pattern_blocks, out) || !part->flush(part))
        panic("disk test: writing the pattern failed");
    memset(in, 0, PATTERN_BYTES);
    if (!part->read(part, pattern_lba, pattern_blocks, in))
        panic("disk test: reading the pattern back failed");
    for (uint32_t i = 0; i < PATTERN_BYTES; i++)
        if (in[i] != out[i])
            panic("disk test: byte %u read back as 0x%02x, written 0x%02x", i, in[i], out[i]);
    kprintf("                 wrote %u KiB at LBA %lu, flushed, read back: identical\n",
            PATTERN_BYTES / 1024, pattern_lba);

    /* 3. Leave a record for the next boot. */
    struct boot_record rec;
    memset(&rec, 0, sizeof(rec));
    memcpy(rec.magic, RECORD_MAGIC, 8);
    rec.boot_number = have_prev ? prev.boot_number + 1 : 1;
    rec.seed = seed;
    if (!uefi_time_text(rec.time, sizeof(rec.time)))
        memcpy(rec.time, "unknown", 8);
    rec.crc = crc32(&rec, offsetof(struct boot_record, crc));
    memset(out, 0, bs);
    memcpy(out, &rec, sizeof(rec));
    if (!part->write(part, record_lba, 1, out) || !part->flush(part) ||
        !part->read(part, record_lba, 1, in) || memcmp(in, out, bs) != 0)
        panic("disk test: writing the boot record failed");
    kprintf("                 boot record #%lu written: seed 0x%016lX, at %s\n", rec.boot_number,
            rec.seed, rec.time);

    kfree(in);
    kfree(out);
}
