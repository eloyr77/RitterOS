#include "crc32.h"

static uint32_t table_ieee[256], table_castagnoli[256];

/* Table for a reflected polynomial: one entry per possible low byte. */
static void build_table(uint32_t *table, uint32_t polynomial)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? polynomial ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
}

static uint32_t compute(const uint32_t *table, const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (size--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t crc32(const void *data, size_t size)
{
    if (table_ieee[1] == 0)
        build_table(table_ieee, 0xEDB88320u);         /* 0x04C11DB7 reflected */
    return compute(table_ieee, data, size);
}

uint32_t crc32c(const void *data, size_t size)
{
    if (table_castagnoli[1] == 0)
        build_table(table_castagnoli, 0x82F63B78u);   /* 0x1EDC6F41 reflected */
    return compute(table_castagnoli, data, size);
}
