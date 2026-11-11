/*
 * GPT partition tables on a disk opened through /dev (gpt.h): reading
 * (the copy at the end when the first is damaged), new empty tables,
 * writing (the copy at the end first, so an interrupted change leaves one
 * whole table), the protective MBR. Shared by disks and osinstall.
 */
#include <string.h>
#include <unistd.h>
#include "gpt.h"
#include "ritteros.h"

static bool read_at(int fd, uint64_t offset, void *buffer, size_t n)
{
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0)
        return false;
    for (size_t done = 0; done < n;) {
        ssize_t r = read(fd, (uint8_t *)buffer + done, n - done);
        if (r <= 0)
            return false;
        done += (size_t)r;
    }
    return true;
}

static bool write_at(int fd, uint64_t offset, const void *buffer, size_t n)
{
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0)
        return false;
    for (size_t done = 0; done < n;) {
        ssize_t r = write(fd, (const uint8_t *)buffer + done, n - done);
        if (r <= 0)
            return false;
        done += (size_t)r;
    }
    return true;
}

static uint8_t block[4096];

/* The header at lba, if its signature and checksum are right. */
static bool read_header(int fd, const struct sys_device *disk, uint64_t lba, struct gpt_header *h)
{
    if (disk->block_size > sizeof(block) || !read_at(fd, lba * disk->block_size, block,
                                                     disk->block_size))
        return false;
    memcpy(h, block, sizeof(*h));
    if (memcmp(h->signature, "EFI PART", 8) != 0 || h->header_size < sizeof(*h) ||
        h->header_size > disk->block_size)
        return false;
    memset(block + 16, 0, 4);                       /* the checksum's own field as 0 */
    return crc32(block, h->header_size) == h->header_crc;
}

static uint8_t entries_buffer[GPT_ENTRIES * GPT_ENTRY_SIZE * 8];

/* The disk's GPT (the copy at the end if the first is damaged); false if
 * neither is whole. */
bool gpt_read(int fd, const struct sys_device *disk, struct gpt_table *t)
{
    for (int copy = 0; copy < 2; copy++) {
        if (!read_header(fd, disk, copy == 0 ? 1 : disk->block_count - 1, &t->h))
            continue;
        t->entries_bytes = (size_t)t->h.entry_count * t->h.entry_size;
        if (t->h.entry_size < sizeof(struct gpt_entry) || t->h.entry_size % 8 ||
            t->entries_bytes > sizeof(entries_buffer) ||
            !read_at(fd, t->h.entries_lba * disk->block_size, entries_buffer, t->entries_bytes) ||
            crc32(entries_buffer, t->entries_bytes) != t->h.entries_crc)
            continue;
        t->entries = entries_buffer;
        t->entries_lba = copy == 0 ? t->h.entries_lba : 2;
        return true;
    }
    return false;
}

struct gpt_entry *gpt_entry_at(struct gpt_table *t, uint32_t i)
{
    return (struct gpt_entry *)(t->entries + (size_t)i * t->h.entry_size);
}

bool gpt_unused(const struct gpt_entry *e)
{
    static const uint8_t zero[16];
    return memcmp(e->type_guid, zero, 16) == 0;
}

/* A random GUID (version 4), in the GPT's byte order. */
void gpt_new_guid(uint8_t *g)
{
    ritteros_random(g, 16);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);         /* the version, in the third group */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);         /* the variant */
}

/* "B5CA1B3E-6278-48F0-BBEF-87907326A8B3" to the GPT's bytes (the first
 * three groups little-endian); false if it is not one. */
bool gpt_parse_guid(const char *s, uint8_t *g)
{
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    uint8_t bytes[16];
    int n = 0;
    for (int i = 0; s[i]; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return false;
            continue;
        }
        int v = s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : (s[i] | 0x20) >= 'a' &&
                (s[i] | 0x20) <= 'f' ? (s[i] | 0x20) - 'a' + 10 : -1;
        if (v < 0 || n == 32)
            return false;
        bytes[n / 2] = (uint8_t)(n % 2 ? bytes[n / 2] << 4 | v : v);
        n++;
    }
    if (n != 32 || strlen(s) != 36)
        return false;
    for (int i = 0; i < 16; i++)
        g[i] = bytes[order[i]];
    return true;
}

/* Write the table: the copy at the end first (entries, then header), then
 * the first one. */
bool gpt_write(int fd, const struct sys_device *disk, struct gpt_table *t)
{
    uint64_t bs = disk->block_size;
    uint64_t array_blocks = (t->entries_bytes + bs - 1) / bs;
    struct gpt_header h = t->h;
    h.entries_crc = crc32(t->entries, t->entries_bytes);
    for (int copy = 1; copy >= 0; copy--) {
        h.my_lba = copy ? disk->block_count - 1 : 1;
        h.alternate_lba = copy ? 1 : disk->block_count - 1;
        h.entries_lba = copy ? disk->block_count - 1 - array_blocks : t->entries_lba;
        h.header_crc = 0;
        memset(block, 0, bs);
        memcpy(block, &h, sizeof(h));
        h.header_crc = crc32(block, h.header_size);
        memcpy(block, &h, sizeof(h));
        if (!write_at(fd, h.entries_lba * bs, t->entries, t->entries_bytes) ||
            !write_at(fd, h.my_lba * bs, block, bs))
            return false;
    }
    return true;
}

/* A new, empty table that fills the disk. */
void gpt_empty(const struct sys_device *disk, struct gpt_table *t)
{
    uint64_t bs = disk->block_size;
    memset(t, 0, sizeof(*t));
    memset(entries_buffer, 0, sizeof(entries_buffer));
    t->entries = entries_buffer;
    t->entries_bytes = GPT_ENTRIES * GPT_ENTRY_SIZE;
    t->entries_lba = 2;
    uint64_t array_blocks = (t->entries_bytes + bs - 1) / bs;
    memcpy(t->h.signature, "EFI PART", 8);
    t->h.revision = 0x00010000;
    t->h.header_size = sizeof(t->h);
    t->h.first_usable = 2 + array_blocks;
    t->h.last_usable = disk->block_count - 2 - array_blocks;
    gpt_new_guid(t->h.disk_guid);
    t->h.entry_count = GPT_ENTRIES;
    t->h.entry_size = GPT_ENTRY_SIZE;
}

/* The protective MBR: one partition of type 0xEE over the whole disk, so
 * that older tools see the disk as taken. */
bool gpt_write_protective_mbr(int fd, const struct sys_device *disk)
{
    uint64_t count = disk->block_count - 1 < 0xFFFFFFFFu ? disk->block_count - 1 : 0xFFFFFFFFu;
    static const uint8_t chs_first[3] = { 0x00, 0x02, 0x00 }, chs_last[3] = { 0xFF, 0xFF, 0xFF };
    memset(block, 0, disk->block_size);
    uint8_t *e = block + 446;
    memcpy(e + 1, chs_first, 3);
    e[4] = 0xEE;
    memcpy(e + 5, chs_last, 3);
    e[8] = 1;
    for (int i = 0; i < 4; i++)
        e[12 + i] = (uint8_t)(count >> (8 * i));
    block[510] = 0x55;
    block[511] = 0xAA;
    return write_at(fd, 0, block, disk->block_size);
}

/* A protective MBR's partition over the whole disk again (after the GPT
 * was fitted to it); other MBRs are left alone. */
bool gpt_fit_protective_mbr(int fd, const struct sys_device *disk)
{
    if (!read_at(fd, 0, block, disk->block_size))
        return false;
    uint8_t *e = block + 446;
    if (block[510] != 0x55 || block[511] != 0xAA || e[4] != 0xEE)
        return true;
    uint64_t count = disk->block_count - 1 < 0xFFFFFFFFu ? disk->block_count - 1 : 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++)
        e[12 + i] = (uint8_t)(count >> (8 * i));
    return write_at(fd, 0, block, disk->block_size);
}

void gpt_set_name(struct gpt_entry *e, const char *name)
{
    memset(e->name, 0, sizeof(e->name));
    for (int i = 0; i < GPT_NAME_CHARS && name[i]; i++)
        e->name[i] = (uint8_t)name[i];
}
