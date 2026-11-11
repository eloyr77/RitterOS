/*
 * GPT partition tables (user/lib/gpt.c), for programs that write them
 * through /dev (disks, osinstall). Sector 0 a protective MBR, sector 1
 * the header, then 128 entries of 128 bytes; a copy of both at the end of
 * the disk. Header and entries carry CRC-32 checksums. GUIDs are kept in
 * the GPT's byte order (the first three groups little-endian).
 */
#ifndef RITTEROS_GPT_H
#define RITTEROS_GPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../common/syscalls.h"

#define GPT_ENTRIES         128             /* a new table's entries */
#define GPT_ENTRY_SIZE      128
#define GPT_NAME_CHARS      36              /* UTF-16 characters in an entry's name */
#define GPT_TYPE_RITTEROS   "B5CA1B3E-6278-48F0-BBEF-87907326A8B3"
#define GPT_TYPE_EFI        "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

struct gpt_header {
    char signature[8];                  /* "EFI PART" */
    uint32_t revision, header_size, header_crc, reserved;
    uint64_t my_lba, alternate_lba, first_usable, last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size, entries_crc;
} __attribute__((packed));

struct gpt_entry {
    uint8_t type_guid[16], unique_guid[16];
    uint64_t first_lba, last_lba, attributes;
    uint16_t name[GPT_NAME_CHARS];
} __attribute__((packed));

/* A table being changed, as read from the disk (or made new). */
struct gpt_table {
    struct gpt_header h;
    uint8_t *entries;
    size_t entries_bytes;
    uint64_t entries_lba;               /* the first copy's entries (2 if made here) */
};

/* The disk's GPT (the copy at the end if the first is damaged); false if
 * neither is whole. */
bool gpt_read(int fd, const struct sys_device *disk, struct gpt_table *t);
/* A new, empty table that fills the disk, with a new random disk GUID. */
void gpt_empty(const struct sys_device *disk, struct gpt_table *t);
/* Write the table: the copy at the end first (entries, then header), then
 * the first one. */
bool gpt_write(int fd, const struct sys_device *disk, struct gpt_table *t);
/* The protective MBR: one partition of type 0xEE over the whole disk. */
bool gpt_write_protective_mbr(int fd, const struct sys_device *disk);
/* A protective MBR's partition over the whole disk again (after the GPT
 * was fitted to it); other MBRs are left alone. */
bool gpt_fit_protective_mbr(int fd, const struct sys_device *disk);

struct gpt_entry *gpt_entry_at(struct gpt_table *t, uint32_t i);
bool gpt_unused(const struct gpt_entry *e);
void gpt_set_name(struct gpt_entry *e, const char *name);   /* ASCII */
/* A random GUID (version 4). */
void gpt_new_guid(uint8_t *g);
/* "B5CA1B3E-6278-48F0-BBEF-87907326A8B3" to the GPT's bytes; false if it
 * is not one. */
bool gpt_parse_guid(const char *s, uint8_t *g);

#endif
