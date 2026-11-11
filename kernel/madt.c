#include "madt.h"

const struct madt *madt_get(void)
{
    const struct acpi_header *h = acpi_find_table("APIC");
    return h && h->length >= sizeof(struct madt) ? (const struct madt *)h : 0;
}

const uint8_t *madt_entry(uint8_t type, int index)
{
    const struct madt *m = madt_get();
    if (!m)
        return 0;
    const uint8_t *p = (const uint8_t *)m + sizeof(*m);
    const uint8_t *end = (const uint8_t *)m + m->header.length;
    while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
        if (p[0] == type && index-- == 0)
            return p;
        p += p[1];
    }
    return 0;
}

int madt_count(uint8_t type)
{
    int n = 0;
    while (madt_entry(type, n))
        n++;
    return n;
}
