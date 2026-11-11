#include "smbios.h"
#include "kprintf.h"
#include "memory.h"
#include "string.h"

/* SMBIOS 3.x entry point ("_SM3_"). */
struct smbios3_entry {
    char anchor[5];
    uint8_t checksum;
    uint8_t length;
    uint8_t major, minor, docrev;
    uint8_t entry_revision;
    uint8_t reserved;
    uint32_t table_max_size;
    uint64_t table_address;
} __attribute__((packed));

/* SMBIOS 2.x entry point ("_SM_"). */
struct smbios2_entry {
    char anchor[4];
    uint8_t checksum;
    uint8_t length;
    uint8_t major, minor;
    uint16_t max_structure_size;
    uint8_t entry_revision;
    uint8_t formatted[5];
    char intermediate_anchor[5];   /* "_DMI_" */
    uint8_t intermediate_checksum;
    uint16_t table_length;
    uint32_t table_address;
    uint16_t structure_count;
    uint8_t bcd_revision;
} __attribute__((packed));

/* Every structure starts with this header, followed by its formatted area
 * and then a set of NUL-terminated strings ending with an extra NUL. */
struct smbios_header {
    uint8_t type;
    uint8_t length;            /* formatted area, header included */
    uint16_t handle;
} __attribute__((packed));

enum {
    TYPE_FIRMWARE = 0,
    TYPE_SYSTEM = 1,
    TYPE_PROCESSOR = 4,
    TYPE_MEMORY_DEVICE = 17,
    TYPE_END = 127,
};

/* String number n (1-based) of a structure, or "" if absent. */
static const char *string_at(const struct smbios_header *h, uint8_t n)
{
    const char *s = (const char *)h + h->length;
    if (n == 0)
        return "";
    while (--n > 0 && *s) {
        while (*s)
            s++;
        s++;
    }
    return s;
}

static const struct smbios_header *next_structure(const struct smbios_header *h)
{
    const char *s = (const char *)h + h->length;
    while (s[0] || s[1])       /* the string set ends with two NULs */
        s++;
    return (const struct smbios_header *)(s + 2);
}

static uint8_t byte_at(const struct smbios_header *h, int offset)
{
    return offset < h->length ? ((const uint8_t *)h)[offset] : 0;
}

void smbios_report(uint64_t entry_point)
{
    uint64_t table_phys;
    uint64_t table_size;
    unsigned major, minor;
    const void *entry = entry_point ? phys_to_virt(entry_point) : 0;

    if (entry && memcmp(entry, "_SM3_", 5) == 0) {
        const struct smbios3_entry *e = entry;
        table_phys = e->table_address;
        table_size = e->table_max_size;
        major = e->major;
        minor = e->minor;
    } else if (entry && memcmp(entry, "_SM_", 4) == 0) {
        const struct smbios2_entry *e = entry;
        table_phys = e->table_address;
        table_size = e->table_length;
        major = e->major;
        minor = e->minor;
    } else {
        kprintf("  SMBIOS:        (none)\n");
        return;
    }
    kprintf("  SMBIOS:        version %u.%u, tables at %p\n", major, minor, (void *)table_phys);
    const uint8_t *table = phys_to_virt(table_phys);

    unsigned memory_devices = 0;
    uint64_t memory_mib = 0;
    const struct smbios_header *h = (const struct smbios_header *)table;
    while ((const uint8_t *)h + sizeof(*h) <= table + table_size && h->type != TYPE_END) {
        if (h->length < sizeof(*h))
            break;                                  /* corrupt table */
        switch (h->type) {
        case TYPE_FIRMWARE:
            kprintf("    firmware:    %s, version %s, %s\n", string_at(h, byte_at(h, 4)),
                    string_at(h, byte_at(h, 5)), string_at(h, byte_at(h, 8)));
            break;
        case TYPE_SYSTEM:
            kprintf("    system:      %s %s\n", string_at(h, byte_at(h, 4)),
                    string_at(h, byte_at(h, 5)));
            break;
        case TYPE_PROCESSOR:
            kprintf("    processor:   %s, %s\n", string_at(h, byte_at(h, 4)),
                    string_at(h, byte_at(h, 0x10)));
            break;
        case TYPE_MEMORY_DEVICE: {
            /* Size in MiB at offset 0x0C; 0x7FFF means "see the 32-bit field
             * at 0x1C"; 0 means an empty slot. */
            uint16_t size = (uint16_t)(byte_at(h, 0x0C) | byte_at(h, 0x0D) << 8);
            uint64_t mib = size & 0x7FFF;
            if (size == 0x7FFF && h->length >= 0x20)
                memcpy(&mib, (const uint8_t *)h + 0x1C, 4);
            if (size != 0 && size != 0xFFFF) {
                memory_devices++;
                memory_mib += (size & 0x8000) ? mib / 1024 : mib;   /* bit 15: KiB units */
            }
            break;
        }
        }
        h = next_structure(h);
    }
    kprintf("    memory:      %u module%s, %lu MiB installed\n", memory_devices,
            memory_devices == 1 ? "" : "s", memory_mib);
}
