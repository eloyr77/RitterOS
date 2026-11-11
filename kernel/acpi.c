#include "acpi.h"
#include "acpi_events.h"
#include "aml.h"
#include "arch_api.h"
#include "kprintf.h"
#include "boot_info.h"
#include "ec.h"
#include "heap.h"
#include "memory.h"
#include "paging.h"
#include "pci.h"
#include "string.h"
#include "sync.h"
#include "timer.h"

/* Root System Description Pointer (ACPI 2.0+ layout; 1.0 ends after rsdt). */
struct rsdp {
    char signature[8];         /* "RSD PTR " */
    uint8_t checksum;          /* first 20 bytes sum to 0 */
    char oem_id[6];
    uint8_t revision;          /* 0: ACPI 1.0, 2: ACPI 2.0 and later */
    uint32_t rsdt_address;     /* 32-bit table list */
    uint32_t length;
    uint64_t xsdt_address;     /* 64-bit table list (ACPI 2.0+) */
    uint8_t extended_checksum; /* whole structure sums to 0 */
    uint8_t reserved[3];
} __attribute__((packed));

#define MAX_TABLES 64

static const struct acpi_header *tables[MAX_TABLES];
static int table_count;

static uint8_t sum(const void *p, uint64_t n)
{
    const uint8_t *b = p;
    uint8_t s = 0;
    while (n--)
        s += *b++;
    return s;
}

/* ACPI tables refer to each other by physical address. */
static const void *acpi_ptr(uint64_t phys)
{
    return phys ? phys_to_virt(phys) : 0;
}

static void add_table(uint64_t address)
{
    const struct acpi_header *h = acpi_ptr(address);
    if (!h || table_count == MAX_TABLES)
        return;
    if (sum(h, h->length) != 0) {
        kprintf("    %.4s at %p: bad checksum, ignored\n", h->signature, (void *)address);
        return;
    }
    tables[table_count++] = h;
}

int acpi_init(uint64_t rsdp_address)
{
    const struct rsdp *rsdp = acpi_ptr(rsdp_address);
    if (!rsdp || memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || sum(rsdp, 20) != 0) {
        kprintf("  ACPI:          no valid RSDP\n");
        return 0;
    }

    uint64_t root_phys;
    int entry_size;
    if (rsdp->revision >= 2 && rsdp->xsdt_address && sum(rsdp, rsdp->length) == 0) {
        root_phys = rsdp->xsdt_address;
        entry_size = 8;
    } else {
        root_phys = rsdp->rsdt_address;
        entry_size = 4;
    }
    const struct acpi_header *root = acpi_ptr(root_phys);
    if (sum(root, root->length) != 0) {
        kprintf("  ACPI:          %.4s has a bad checksum\n", root->signature);
        return 0;
    }

    /* The root table's body is an array of table addresses. */
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    int n = (int)((root->length - sizeof(*root)) / entry_size);
    for (int i = 0; i < n; i++) {
        uint64_t a = 0;
        memcpy(&a, entries + i * entry_size, entry_size);   /* may be unaligned */
        add_table(a);
    }

    kprintf("  ACPI:          RSDP %p, revision %u, OEM '%.6s', %.4s at %p with %d tables\n",
            (void *)rsdp_address, rsdp->revision, rsdp->oem_id, root->signature, (void *)root_phys,
            table_count);
    for (int i = 0; i < table_count; i++)
        kprintf("    %.4s  %p  %5u bytes  rev %u  '%.6s' '%.8s'\n", tables[i]->signature,
                (void *)virt_to_phys(tables[i]), tables[i]->length, tables[i]->revision,
                tables[i]->oem_id, tables[i]->oem_table_id);
    return table_count;
}

/* Device IDs that tell where a touchpad is: HID over I2C (its compatible
 * ID), touchpad makers' IDs, and I2C controllers. */
static const char *const INPUT_IDS[] = {
    "PNP0C50", "ACPI0C50", "MSFT0001", "ELAN", "SYNA", "ALPS", "ATML", "CYAP", "FTCS", "GXTP",
    "AMDI0010", "AMDI0019", "AMDI0510", "INT33C3", "INT3432", "INT3433", "80860F41", 0
};

static void scan_ids(const struct acpi_header *t, char found[][10], int *count, int max)
{
    const uint8_t *p = (const uint8_t *)t, *end = p + t->length;
    for (const uint8_t *q = p + sizeof(*t); q + 10 < end && *count < max; q++) {
        char id[10];
        size_t n = 0;
        if (q[0] == 0x0D) {                     /* StringPrefix "ID" NUL */
            while (n < 9 && q + 1 + n < end && q[1 + n] >= '0' && q[1 + n] <= 'Z' &&
                   (q[1 + n] <= '9' || q[1 + n] >= 'A'))
                n++;
            if (n < 4 || q[1 + n] != 0)
                continue;
            memcpy(id, q + 1, n);
            id[n] = 0;
        } else if (q[0] == 0x0C && q[1] == 0xD0 && q[2] == 0x41 && q[3] == 0x0C && q[4] == 0x50) {
            memcpy(id, "PNP0C50", 8);           /* EisaId ("PNP0C50") */
        } else {
            continue;
        }
        bool wanted = false;
        for (int i = 0; INPUT_IDS[i] && !wanted; i++)
            wanted = strncmp(id, INPUT_IDS[i], strlen(INPUT_IDS[i])) == 0;
        for (int i = 0; i < *count && wanted; i++)
            wanted = strcmp(found[i], id) != 0;
        if (wanted)
            memcpy(found[(*count)++], id, sizeof(id));
    }
}

void acpi_report_input_devices(void)
{
    char found[16][10];
    int count = 0;
    const struct acpi_fadt *f = acpi_fadt();
    if (f) {
        uint64_t address = f->header.length >= 148 && f->x_dsdt ? f->x_dsdt : f->dsdt;
        const struct acpi_header *dsdt = acpi_ptr(address);
        if (dsdt && memcmp(dsdt->signature, "DSDT", 4) == 0)
            scan_ids(dsdt, found, &count, 16);
    }
    for (int i = 0; i < table_count; i++)
        if (memcmp(tables[i]->signature, "SSDT", 4) == 0)
            scan_ids(tables[i], found, &count, 16);
    if (!count)
        return;
    char line[200];
    size_t o = 0;
    for (int i = 0; i < count && o + 12 < sizeof(line); i++)
        o += (size_t)ksnprintf(line + o, sizeof(line) - o, "%s%s", i ? " " : "", found[i]);
    kprintf("  acpi:          touchpad and I2C device IDs: %s\n", line);
}

const struct acpi_header *acpi_find_table(const char *signature)
{
    for (int i = 0; i < table_count; i++)
        if (memcmp(tables[i]->signature, signature, 4) == 0)
            return tables[i];
    return 0;
}

_Static_assert(__builtin_offsetof(struct acpi_fadt, flags) == 112, "FADT layout");
_Static_assert(__builtin_offsetof(struct acpi_fadt, arm_boot_arch) == 129, "FADT layout");
_Static_assert(__builtin_offsetof(struct acpi_fadt, x_dsdt) == 140, "FADT layout");

const struct acpi_fadt *acpi_fadt(void)
{
    const struct acpi_header *h = acpi_find_table("FACP");
    return h && h->length >= 116 ? (const struct acpi_fadt *)h : 0;
}

/* An AML integer constant: Zero, One, Ones, or BytePrefix + value. */
static bool aml_byte(const uint8_t **p, const uint8_t *end, uint8_t *value)
{
    if (*p >= end)
        return false;
    uint8_t op = *(*p)++;
    if (op == 0x00 || op == 0x01 || op == 0xFF) {
        *value = op;
        return true;
    }
    if (op == 0x0A && *p < end) {
        *value = *(*p)++;
        return true;
    }
    return false;
}

bool acpi_s5_sleep_type(uint8_t *pm1a, uint8_t *pm1b)
{
    const struct acpi_fadt *f = acpi_fadt();
    if (!f)
        return false;
    uint64_t address = f->header.length >= 148 && f->x_dsdt ? f->x_dsdt : f->dsdt;
    const struct acpi_header *dsdt = acpi_ptr(address);
    if (!dsdt || memcmp(dsdt->signature, "DSDT", 4) != 0 || sum(dsdt, dsdt->length) != 0)
        return false;

    /* NameOp (0x08) "_S5_" PackageOp (0x12) PkgLength NumElements a b ... */
    const uint8_t *code = (const uint8_t *)dsdt, *end = code + dsdt->length;
    for (const uint8_t *p = code + sizeof(*dsdt); p + 8 < end; p++) {
        if (memcmp(p, "_S5_", 4) != 0 || (p[-1] != 0x08 && !(p[-1] == '\\' && p[-2] == 0x08)))
            continue;
        p += 4;
        if (*p++ != 0x12)
            return false;
        p += 1 + (*p >> 6);                 /* PkgLength: 1-4 bytes */
        p++;                                /* NumElements */
        return aml_byte(&p, end, pm1a) && aml_byte(&p, end, pm1b);
    }
    return false;
}

/* --- devices, read without running AML (15s) --- */

static uint32_t pkg_length(const uint8_t *p, const uint8_t *end, int *bytes)
{
    int extra = p[0] >> 6;
    if (p + 1 + extra > end) {
        *bytes = 1;
        return 0;
    }
    uint32_t len = extra ? (uint32_t)(p[0] & 0x0F) : (uint32_t)(p[0] & 0x3F);
    for (int i = 0; i < extra; i++)
        len |= (uint32_t)p[1 + i] << (4 + 8 * i);
    *bytes = 1 + extra;
    return len;
}

static bool name_char(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* The Device op at d (5B 82): its name and body; false if it does not read. */
static bool read_device(const uint8_t *d, const uint8_t *end, struct acpi_device *out)
{
    if (d + 8 > end || d[0] != 0x5B || d[1] != 0x82)
        return false;
    int nb;
    uint32_t len = pkg_length(d + 2, end, &nb);
    const uint8_t *body_end = d + 2 + len, *n = d + 2 + nb;
    if (len < (uint32_t)nb + 4 || body_end > end)
        return false;
    while (n < body_end && (*n == '\\' || *n == '^'))
        n++;
    if (n < body_end && *n == 0x2E)             /* DualNamePrefix: the second segment */
        n += 5;
    else if (n < body_end && *n == 0x2F)        /* MultiNamePrefix count segments */
        n += 2 + 4 * (n[1] - 1);
    if (n + 4 > body_end)
        return false;
    for (int i = 0; i < 4; i++) {
        if (!name_char(n[i]))
            return false;
        out->name[i] = (char)n[i];
    }
    out->name[4] = 0;
    out->start = n + 4;
    out->end = body_end;
    return true;
}

/* The tables with AML: the DSDT, then the SSDTs. */
static int aml_tables(const struct acpi_header **out, int max)
{
    int n = 0;
    const struct acpi_fadt *f = acpi_fadt();
    if (f) {
        uint64_t address = f->header.length >= 148 && f->x_dsdt ? f->x_dsdt : f->dsdt;
        const struct acpi_header *dsdt = acpi_ptr(address);
        if (dsdt && memcmp(dsdt->signature, "DSDT", 4) == 0 && n < max)
            out[n++] = dsdt;
    }
    for (int i = 0; i < table_count && n < max; i++)
        if (memcmp(tables[i]->signature, "SSDT", 4) == 0)
            out[n++] = tables[i];
    return n;
}

/* Does the AML at q name id (a string, or a PNP id's EisaId)? */
static bool names_id(const uint8_t *q, const uint8_t *end, const char *id)
{
    size_t n = strlen(id);
    if (q[0] == 0x0D && q + 2 + n <= end && memcmp(q + 1, id, n) == 0 && q[1 + n] == 0)
        return true;
    if (q[0] == 0x0C && q + 5 <= end && n == 7) {           /* EisaId: 3 letters, 4 hex */
        uint16_t v = (uint16_t)((id[0] - 0x40) << 10 | (id[1] - 0x40) << 5 | (id[2] - 0x40));
        uint8_t b[4] = { (uint8_t)(v >> 8), (uint8_t)v, 0, 0 };
        int hex[4];
        for (int i = 0; i < 4; i++) {
            char c = id[3 + i];
            hex[i] = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (hex[i] < 0)
                return false;
        }
        b[2] = (uint8_t)(hex[0] << 4 | hex[1]);
        b[3] = (uint8_t)(hex[2] << 4 | hex[3]);
        return memcmp(q + 1, b, 4) == 0;
    }
    return false;
}

int acpi_devices_with(const char *id, struct acpi_device *out, int max)
{
    const struct acpi_header *t[32];
    int count = 0, tn = aml_tables(t, 32);
    for (int k = 0; k < tn && count < max; k++) {
        const uint8_t *base = (const uint8_t *)t[k], *end = base + t[k]->length;
        for (const uint8_t *q = base + sizeof(*t[k]); q + 6 < end && count < max; q++) {
            if (!names_id(q, end, id))
                continue;
            /* the innermost Device that holds q: the nearest start covering it */
            struct acpi_device d;
            bool found = false;
            for (const uint8_t *s = q; s > base + sizeof(*t[k]) && q - s < 0x10000 && !found; s--)
                found = s[0] == 0x5B && s[1] == 0x82 && read_device(s, end, &d) && d.end > q &&
                        d.start <= q;
            if (!found)
                continue;
            bool again = false;
            for (int i = 0; i < count && !again; i++)
                again = out[i].start == d.start;
            if (!again)
                out[count++] = d;
            q = d.end - 1;                          /* (one entry per device) */
        }
    }
    return count;
}

bool acpi_device_named(const char *seg, struct acpi_device *out)
{
    const struct acpi_header *t[32];
    int tn = aml_tables(t, 32);
    for (int k = 0; k < tn; k++) {
        const uint8_t *base = (const uint8_t *)t[k], *end = base + t[k]->length;
        for (const uint8_t *q = base + sizeof(*t[k]); q + 8 < end; q++)
            if (q[0] == 0x5B && q[1] == 0x82 && read_device(q, end, out) &&
                memcmp(out->name, seg, 4) == 0)
                return true;
    }
    return false;
}

bool acpi_device_memory(const struct acpi_device *d, uint64_t *base, uint32_t *length)
{
    for (const uint8_t *q = d->start; q + 12 <= d->end; q++)
        if (q[0] == 0x86 && q[1] == 0x09 && q[2] == 0x00) {        /* Memory32Fixed */
            *base = (uint32_t)(q[4] | q[5] << 8 | q[6] << 16 | (uint32_t)q[7] << 24);
            *length = (uint32_t)(q[8] | q[9] << 8 | q[10] << 16 | (uint32_t)q[11] << 24);
            return *length != 0;
        }
    return false;
}

/* The last segment of the NUL-ended path at p (before end) into seg. */
static bool last_segment(const uint8_t *p, const uint8_t *end, char seg[5])
{
    const uint8_t *s = 0;
    for (const uint8_t *q = p; q < end && *q; q++)
        if (q == p || q[-1] == '.' || q[-1] == '\\' || q[-1] == '^')
            s = q;
    if (!s || s + 4 > end)
        return false;
    for (int i = 0; i < 4; i++) {
        if (!name_char(s[i]))
            return false;
        seg[i] = (char)s[i];
    }
    seg[4] = 0;
    return true;
}

bool acpi_device_i2c(const struct acpi_device *d, char bus[5], uint16_t *address, uint32_t *speed)
{
    for (const uint8_t *q = d->start; q + 20 <= d->end; q++) {
        /* I2cSerialBus: 8E len(2) rev resource-index type(1 = I2C) flags(2)
         * type-flags(2) type-rev type-data-length(2) speed(4) address(2) source */
        if (q[0] != 0x8E || q[5] != 1)
            continue;
        uint16_t len = (uint16_t)(q[1] | q[2] << 8);
        if (len < 15 || q + 3 + len > d->end)
            continue;
        *speed = (uint32_t)(q[12] | q[13] << 8 | q[14] << 16 | (uint32_t)q[15] << 24);
        *address = (uint16_t)(q[16] | q[17] << 8);
        return last_segment(q + 18, q + 3 + len, bus);
    }
    return false;
}

bool acpi_device_interrupt(const struct acpi_device *d, uint32_t *gsi, bool *level, bool *active_low)
{
    for (const uint8_t *q = d->start; q + 9 <= d->end; q++) {
        /* Extended Interrupt: 89 len(2) flags count interrupts(4 each) */
        if (q[0] != 0x89 || q[1] < 6 || q[2] != 0 || q[4] == 0 || q + 3 + q[1] > d->end)
            continue;
        *level = !(q[3] & 0x02);
        *active_low = q[3] & 0x04;
        *gsi = (uint32_t)(q[5] | q[6] << 8 | q[7] << 16 | (uint32_t)q[8] << 24);
        return true;
    }
    return false;
}

bool acpi_device_gpio_int(const struct acpi_device *d, char controller[5], uint16_t *pin)
{
    for (const uint8_t *q = d->start; q + 23 <= d->end; q++) {
        /* GpioInt: 8C len(2) rev type(0 = interrupt) ... pin-table-offset at 14,
         * resource-source-name offset at 17 (from the descriptor's start) */
        if (q[0] != 0x8C || q[4] != 0)
            continue;
        uint16_t len = (uint16_t)(q[1] | q[2] << 8);
        uint16_t pins = (uint16_t)(q[14] | q[15] << 8), name = (uint16_t)(q[17] | q[18] << 8);
        if (len < 20 || q + 3 + len > d->end || pins + 2u > 3u + len || name >= 3u + len)
            continue;
        *pin = (uint16_t)(q[pins] | q[pins + 1] << 8);
        return last_segment(q + name, q + 3 + len, controller);
    }
    return false;
}

/* An AML integer constant at *p (Zero, One, Ones, Byte/Word/DWordConst). */
static bool aml_integer(const uint8_t **p, const uint8_t *end, uint32_t *v)
{
    if (*p >= end)
        return false;
    uint8_t op = *(*p)++;
    if (op == 0x00 || op == 0x01) {
        *v = op;
        return true;
    }
    if (op == 0xFF) {
        *v = 0xFFFFFFFF;
        return true;
    }
    int n = op == 0x0A ? 1 : op == 0x0B ? 2 : op == 0x0C ? 4 : 0;
    if (!n || *p + n > end)
        return false;
    *v = 0;
    for (int i = 0; i < n; i++)
        *v |= (uint32_t)(*p)[i] << (8 * i);
    *p += n;
    return true;
}

int acpi_device_i2c_hid_pairs(const struct acpi_device *d, uint16_t *address, uint16_t *reg,
                              int max)
{
    int count = 0;
    for (const uint8_t *q = d->start; q + 8 < d->end && count < max; q++) {
        if (q[0] != 0x12)                                   /* PackageOp */
            continue;
        int nb;
        uint32_t len = pkg_length(q + 1, d->end, &nb);
        const uint8_t *p = q + 1 + nb, *pend = q + 1 + len;
        if (len < 8 || pend > d->end || *p != 5)
            continue;
        p++;
        uint32_t id, a, r;
        if (!aml_integer(&p, pend, &id) || !aml_integer(&p, pend, &a) || !aml_integer(&p, pend, &r))
            continue;
        if (p >= pend || *p != 0x0D || a == 0 || a > 0x7F || r > 0xFFFF)
            continue;
        const uint8_t *s = p + 1;                           /* the HID string, then the CID */
        while (s < pend && *s)
            s++;
        if (s + 1 >= pend || !(names_id(s + 1, pend, "PNP0C50") || names_id(s + 1, pend, "ACPI0C50")))
            continue;
        address[count] = (uint16_t)a, reg[count] = (uint16_t)r;
        count++;
    }
    return count;
}

/* --- (37a) what the firmware offers for power management, read only ---- */


/* Sleep state x's package (\_Sx_: NameOp, the name, PackageOp, its length,
 * the count, then the values for PM1a and PM1b), in any AML table: some
 * firmware puts it in an SSDT, or inside an If (still found here: whether
 * it applies needs the AML run, step 37b). */
static bool sleep_state(int x, uint8_t *a, uint8_t *b)
{
    char seg[5] = { '_', 'S', (char)('0' + x), '_', 0 };
    const struct acpi_header *t[32];
    int tn = aml_tables(t, 32);
    for (int k = 0; k < tn; k++) {
        const uint8_t *base = (const uint8_t *)t[k], *end = base + t[k]->length;
        for (const uint8_t *p = base + sizeof(*t[k]) + 2; p + 10 < end; p++) {
            if (memcmp(p, seg, 4) != 0 || (p[-1] != 0x08 && !(p[-1] == '\\' && p[-2] == 0x08)))
                continue;
            const uint8_t *q = p + 4;
            if (*q++ != 0x12)
                continue;
            q += 1 + (*q >> 6);                 /* PkgLength */
            q++;                                /* NumElements */
            if (aml_byte(&q, end, a) && aml_byte(&q, end, b))
                return true;
        }
    }
    return false;
}

/* Where an object named seg is defined (a Name, or a Method: its PkgLength
 * takes 1-4 bytes): how many times, in all the AML tables. */
static int defined(const char *seg)
{
    const struct acpi_header *t[32];
    int tn = aml_tables(t, 32), n = 0;
    for (int k = 0; k < tn; k++) {
        const uint8_t *base = (const uint8_t *)t[k], *end = base + t[k]->length;
        for (const uint8_t *p = base + sizeof(*t[k]) + 5; p + 4 < end; p++) {
            if (memcmp(p, seg, 4) != 0)
                continue;
            bool method = false;
            for (int len = 1; len <= 4 && !method; len++)
                method = p[-1 - len] == 0x14 && (p[-len] >> 6) == len - 1;
            n += p[-1] == 0x08 || method;
        }
    }
    return n;
}

static void aml_print(void);

void acpi_power_print(void)
{
    const struct acpi_fadt *f = acpi_fadt();
    if (!f) {
        kprintf("no ACPI tables (FADT) from the firmware: nothing known about power\n");
        return;
    }
    static const char *const profiles[] = { "unspecified", "desktop", "mobile", "workstation",
        "enterprise server", "SOHO server", "appliance PC", "performance server", "tablet" };
    uint32_t fl = f->flags;
    kprintf("firmware: ACPI FADT revision %u.%u, profile %s; flags 0x%X%s%s%s%s%s%s\n",
            f->header.revision, f->header.length >= 132 ? f->minor_version & 0xF : 0,
            f->preferred_pm_profile < 9 ? profiles[f->preferred_pm_profile] : "other", fl,
            fl & FADT_HARDWARE_REDUCED ? ", hardware-reduced (no PM1 registers)" : "",
            fl & FADT_LOW_POWER_S0 ? ", low-power S0 idle (Modern Standby)" : "",
            fl & FADT_PWR_BUTTON ? ", power button a device" : ", power button fixed",
            fl & FADT_SLP_BUTTON ? ", sleep button a device or none" : ", sleep button fixed",
            fl & FADT_PROC_C1 ? ", C1 on every processor" : "",
            fl & FADT_P_LVL2_UP ? ", C2 with several processors" : "");
    if (!(fl & FADT_HARDWARE_REDUCED))
        kprintf("registers: PM1a event 0x%X, PM1a control 0x%X, PM1b control 0x%X, PM timer 0x%X, "
                "GPE0 0x%X (%u bytes); SCI %u; C2 latency %u us%s, C3 %u us%s\n",
                f->pm1a_event_block, f->pm1a_control_block, f->pm1b_control_block,
                f->pm_timer_block, f->gpe0_block, f->gpe0_length, f->sci_interrupt,
                f->c2_latency, f->c2_latency > 100 ? " (none)" : "", f->c3_latency,
                f->c3_latency > 1000 ? " (none)" : "");
    char states[96] = "", missing[32] = "";
    size_t at = 0, mt = 0;
    for (int x = 0; x <= 5; x++) {
        uint8_t a = 0, b = 0;
        if (sleep_state(x, &a, &b))
            at += (size_t)ksnprintf(states + at, sizeof(states) - at, "%s S%d (%u, %u)",
                                    at ? "," : "", x, a, b);
        else
            mt += (size_t)ksnprintf(missing + mt, sizeof(missing) - mt, " S%d", x);
    }
    kprintf("sleep states in the firmware's tables:%s; not there:%s\n",
            at ? states : " none", mt ? missing : " none");
    uint64_t fa = f->header.length >= 140 && f->x_firmware_ctrl ? f->x_firmware_ctrl
                                                                 : f->firmware_ctrl;
    const struct acpi_facs *facs = acpi_ptr(fa);
    if (facs && memcmp(facs->signature, "FACS", 4) == 0)
        kprintf("FACS: %u bytes, version %u, waking vector 0x%X (64-bit 0x%lX); 64-bit wake %s, "
                "S4BIOS %s\n", facs->length, facs->length >= 33 ? facs->version : 0,
                facs->waking_vector, facs->length >= 32 ? facs->x_waking_vector : 0,
                facs->flags & 2 ? "supported" : "not supported",
                facs->flags & 1 ? "yes" : "no");
    else
        kprintf("FACS: none%s\n", fl & FADT_HARDWARE_REDUCED ? " (hardware-reduced)" : "");
    /* The devices that matter for power, by their ACPI IDs (names as the
     * firmware gives them; what they do needs their methods run, 37b). */
    static const struct { const char *id, *what; } kinds[] = {
        { "PNP0C0A", "battery" }, { "ACPI0003", "AC adapter" }, { "PNP0C0D", "lid" },
        { "PNP0C0C", "power button" }, { "PNP0C0E", "sleep button" },
        { "PNP0C09", "embedded controller" },
        { "PNP0D80", "low-power S0 device (LPS0)" },
        { "AMDI0005", "AMD power management controller" },
        { "AMDI0006", "AMD power management controller" },
        { "AMDI0007", "AMD power management controller" },
        { "INT33A1", "Intel power engine" },
        { "ACPI0007", "processor" }, { "ACPI0010", "processor container" },
        { "PNP0C14", "WMI (vendor's methods)" }, { "ACPI000E", "time and alarm (wake timer)" },
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        struct acpi_device d[8];
        int n = acpi_devices_with(kinds[i].id, d, 8);
        if (!n)
            continue;
        char names[64] = "";
        size_t nt = 0;
        for (int k = 0; k < n && nt + 6 < sizeof(names); k++)
            nt += (size_t)ksnprintf(names + nt, sizeof(names) - nt, " %s", d[k].name);
        kprintf("device: %s (%s):%s\n", kinds[i].what, kinds[i].id, names);
    }
    kprintf("methods defined: _CST (processor sleep states) %d, _CPC (CPPC) %d, _PSS (P-states) "
            "%d, _PS3 (device off) %d, _PR3 %d, _PTS (prepare to sleep) %d, _WAK %d, _DSM %d\n",
            defined("_CST"), defined("_CPC"), defined("_PSS"), defined("_PS3"), defined("_PR3"),
            defined("_PTS"), defined("_WAK"), defined("_DSM"));
    aml_print();
    acpi_events_print();                /* (37c-2) */
}

/* (37b-1) The tables in the order power tables saves them. */
const struct acpi_header *acpi_table_at(int i)
{
    const struct acpi_fadt *f = acpi_fadt();
    const struct acpi_header *dsdt = 0, *facs = 0;
    if (f) {
        uint64_t d = f->header.length >= 148 && f->x_dsdt ? f->x_dsdt : f->dsdt;
        uint64_t fa = f->header.length >= 140 && f->x_firmware_ctrl ? f->x_firmware_ctrl
                                                                     : f->firmware_ctrl;
        dsdt = acpi_ptr(d);
        facs = acpi_ptr(fa);
        if (dsdt && memcmp(dsdt->signature, "DSDT", 4) != 0)
            dsdt = 0;
        if (facs && memcmp(facs->signature, "FACS", 4) != 0)
            facs = 0;
    }
    const struct acpi_header *first[2] = { dsdt, facs };
    for (int k = 0; k < 2; k++) {
        if (!first[k])
            continue;
        if (i == 0)
            return first[k];
        i--;
    }
    return i >= 0 && i < table_count ? tables[i] : 0;
}

/* ---- (37b-2) The AML interpreter: one caller at a time (its state is
 * global), under this mutex ---- */

static struct mutex aml_mutex = MUTEX_INIT("aml");

static uint64_t aml_clock_ns(void)
{
    uint64_t hz = arch_counter_hz(), c = arch_counter();
    return hz ? c / hz * 1000000000u + c % hz * 1000000000u / hz : 0;
}

static void aml_sleep(uint32_t ms)
{
    timer_sleep_ms(ms);
}

static void aml_stall(uint32_t us)
{
    uint64_t hz = arch_counter_hz(), start = arch_counter();
    while (hz && arch_counter() - start < hz / 1000000 * us)
        arch_cpu_relax();
}

/* ---- (37b-4) The machine through the firmware's operation regions:
 * memory (the kernel's own RAM refused: firmware's regions are its own
 * storage and devices), I/O ports (x86), PCI configuration; other spaces
 * (the embedded controller until 37b-5) have no way there ---- */

static const struct memory_region *memory_map;
static uint64_t memory_map_count;
static struct {
    uint32_t reads[4], writes[4];       /* memory, I/O, PCI configuration, EC */
    uint32_t refused, missing, notifies;
} regions;
static struct aml_init boot_init;
static bool aml_ready;

static bool kernel_ram(uint64_t a, uint64_t n)
{
    for (uint64_t i = 0; i < memory_map_count; i++) {
        const struct memory_region *r = &memory_map[i];
        if ((r->type == MEMORY_USABLE || r->type == MEMORY_LOADER) && a < r->base + r->length &&
            a + n > r->base)
            return true;
    }
    return false;
}

/* Is [a, a + n) firmware memory the direct map has on every architecture
 * (its tables, its storage, its runtime parts)? */
static bool direct_mapped(uint64_t a, uint64_t n)
{
    for (uint64_t i = 0; i < memory_map_count; i++) {
        const struct memory_region *r = &memory_map[i];
        if (a >= r->base && a + n <= r->base + r->length)
            return r->type == MEMORY_ACPI_RECLAIMABLE || r->type == MEMORY_ACPI_NVS ||
                   r->type == MEMORY_UEFI_RUNTIME_CODE || r->type == MEMORY_UEFI_RUNTIME_DATA;
    }
    return false;
}

/* Anything else (devices, reserved memory) is mapped a page at a time as
 * the firmware's code reaches it: ARM's direct map has no devices (QEMU
 * virt's GED register faulted there), x86's has them below 4 GiB only.
 * Never a whole region: the boot's page-table pool is small. */
#define AML_PAGES 64
static uint64_t aml_pages[AML_PAGES];
static int aml_page_count;

static volatile uint8_t *memory_at(uint64_t a, uint32_t n)
{
    if (a + n < a)
        return 0;
    if (direct_mapped(a, n))
        return phys_to_virt(a);
    uint64_t page = a & ~0xFFFull;
    if (((a + n - 1) & ~0xFFFull) != page)
        return 0;
    for (int i = 0; i < aml_page_count; i++)
        if (aml_pages[i] == page)
            return phys_to_virt(a);
    if (aml_page_count == AML_PAGES)
        return 0;
    volatile uint8_t *v = paging_map_mmio(page, 4096);
    if (!v)
        return 0;
    aml_pages[aml_page_count++] = page;
    return v + (a - page);
}

static bool memory_access(uint64_t a, int width, uint64_t *value, bool write)
{
    uint32_t n = (uint32_t)width / 8;
    if (kernel_ram(a, n)) {
        if (regions.refused++ < 4)
            kprintf("aml: a region at 0x%lX in the kernel's RAM: refused\n", a);
        return false;
    }
    volatile uint8_t *p = memory_at(a, n);
    if (!p)
        return false;
    if (a % n) {                        /* unaligned: a byte at a time (ARM's devices fault) */
        uint64_t v = write ? *value : 0;
        for (uint32_t i = 0; i < n; i++) {
            if (write)
                mmio_write8(p + i, (uint8_t)(v >> (8 * i)));
            else
                v |= (uint64_t)mmio_read8(p + i) << (8 * i);
        }
        if (!write)
            *value = v;
        return true;
    }
    switch (width) {
    case 8:
        if (write) mmio_write8(p, (uint8_t)*value); else *value = mmio_read8(p);
        return true;
    case 16:
        if (write) mmio_write16(p, (uint16_t)*value); else *value = mmio_read16(p);
        return true;
    case 32:
        if (write) mmio_write32(p, (uint32_t)*value); else *value = mmio_read32(p);
        return true;
    case 64:
        if (write) mmio_write64(p, *value); else *value = mmio_read64(p);
        return true;
    }
    return false;
}

static bool port_access(uint64_t a, int width, uint64_t *value, bool write)
{
    if (a + (uint64_t)width / 8 > 0x10000)
        return false;
    if (width == 64) {                  /* (two of 32, low first) */
        uint32_t lo = (uint32_t)*value, hi = (uint32_t)(*value >> 32);
        if (!arch_port_access((uint16_t)a, 32, &lo, write) ||
            !arch_port_access((uint16_t)(a + 4), 32, &hi, write))
            return false;
        if (!write)
            *value = lo | (uint64_t)hi << 32;
        return true;
    }
    uint32_t v = (uint32_t)*value;
    if (!arch_port_access((uint16_t)a, width, &v, write))
        return false;
    if (!write)
        *value = v;
    return true;
}

static bool region_access(uint8_t space, uint64_t a, int width, uint64_t *value, bool write)
{
    bool ok = false;
    if (space == 0)
        ok = memory_access(a, width, value, write);
    else if (space == 1)
        ok = port_access(a, width, value, write);
    else if (space == 2)
        ok = pci_config_access(AML_PCI_SEGMENT(a), AML_PCI_BUS(a), AML_PCI_DEVICE(a),
                               AML_PCI_FUNCTION(a), AML_PCI_REGISTER(a), width, value, write);
    else if (space == 3)                /* (37b-5) the embedded controller */
        ok = ec_access(a, width, value, write);
    if (ok && space <= 3)
        (write ? regions.writes : regions.reads)[space]++;
    else if (!ok)
        regions.missing++;
    return ok;
}

static bool hardware_read(uint8_t space, uint64_t address, int width, uint64_t *value)
{
    *value = 0;
    return region_access(space, address, width, value, false);
}

static bool hardware_write(uint8_t space, uint64_t address, int width, uint64_t value)
{
    return region_access(space, address, width, &value, true);
}

/* Notify(object, value): (37c-2b) a power button device's 0x80 is a
 * press (ACPI 6.5 4.8.2.2.1.2); (37c-2c) a lid's 0x80 a change, an AC
 * adapter's 0x80 and a battery's 0x80/0x81 their state's (10.2.1, 10.3):
 * the thread `acpi` looks (not here, inside the interpreter's run). The
 * others are logged (the first NOTIFIES_LOGGED) until their drivers come. */
#define NOTIFIES_LOGGED 32
static struct aml_node *buttons[4], *lids[2], *adapters[2], *batteries[4];
static int button_count, lid_count, adapter_count, battery_count;

static bool one_of(struct aml_node *node, struct aml_node **list, int count)
{
    for (int i = 0; i < count; i++)
        if (node == list[i])
            return true;
    return false;
}

static void aml_notify(struct aml_node *node, uint64_t value)
{
    regions.notifies++;
    if (value == 0x80 && one_of(node, buttons, button_count)) {
        acpi_events_button();
        return;
    }
    if (value == 0x80 && one_of(node, lids, lid_count)) {
        acpi_events_notify(ACPI_EVENT_LID);
        return;
    }
    if (value == 0x80 && one_of(node, adapters, adapter_count)) {
        acpi_events_notify(ACPI_EVENT_AC);
        return;
    }
    if ((value == 0x80 || value == 0x81) && one_of(node, batteries, battery_count)) {
        acpi_events_notify(ACPI_EVENT_BATTERY);
        return;
    }
    if (regions.notifies <= NOTIFIES_LOGGED) {
        char path[96];
        aml_path(node, path, sizeof(path));
        kprintf("acpi: Notify(%s, 0x%lX): nothing takes it yet%s\n", path, value,
                regions.notifies == NOTIFIES_LOGGED ? " (the last one logged)" : "");
    }
}

static const struct aml_os hardware = {
    .region_read = hardware_read,
    .region_write = hardware_write,
    .now_ns = aml_clock_ns,
    .sleep_ms = aml_sleep,
    .stall_us = aml_stall,
    .notify = aml_notify,
};

void acpi_aml_load(const struct boot_info *info)
{
    const struct acpi_header *t[64];
    int n = aml_tables(t, 64);
    if (!n)
        return;
    memory_map = info->memory_map;
    memory_map_count = info->memory_map_count;
    uint64_t started = arch_counter();
    mutex_lock(&aml_mutex);
    aml_init(&hardware);
    int loaded = 0;
    for (int i = 0; i < n; i++)
        loaded += aml_load(t[i], t[i]->length) >= 0;
    aml_loaded();
    uint64_t load_ms = timer_elapsed_ms(started);
    /* (37b-4) the boot's _INI, _REG for PCI configuration, as Linux;
     * (37b-5) the ECDT's embedded controller before it, the device's after */
    ec_start_early();
    aml_initialize(&boot_init);
    ec_start();
    button_count = aml_find_devices("PNP0C0C", buttons, 4);    /* (37c-2b) */
    lid_count = aml_find_devices("PNP0C0D", lids, 2);          /* (37c-2c) */
    adapter_count = aml_find_devices("ACPI0003", adapters, 2);
    battery_count = aml_find_devices("PNP0C0A", batteries, 4);
    aml_ready = true;
    struct aml_stats s;
    aml_stats(&s);
    mutex_unlock(&aml_mutex);
    kprintf("  ACPI: AML of %d of %d tables: %u objects, %u errors, %lu ms, stack %u bytes\n",
            loaded, n, s.nodes, s.errors, load_ms, s.stack_deepest);
    kprintf("  ACPI: _INI %u run (%u _STA, %u devices absent, %u failed) in %lu ms\n",
            boot_init.ini, boot_init.sta, boot_init.absent, boot_init.failed,
            timer_elapsed_ms(started) - load_ms);
}

/* ---- (37c-1) Batteries and AC adapters, as the firmware gives them
 * (ACPI 6.5 10.2: _BIF's units, capacities and names, _BST's state, rate,
 * remaining capacity and voltage; _PSR) ---- */

static bool element(const struct aml_value *pkg, uint32_t i, uint64_t *n)
{
    if (pkg->type != AML_PACKAGE || i >= aml_length(pkg))
        return false;
    struct aml_value e = {0};
    bool ok = aml_package_get(pkg, i, &e) && aml_to_integer_value(&e, n);
    aml_release(&e);
    return ok;
}

static void text_of(const struct aml_value *pkg, uint32_t i, char *out, size_t size)
{
    out[0] = 0;
    struct aml_value e = {0};
    if (pkg->type != AML_PACKAGE || i >= aml_length(pkg) || !aml_package_get(pkg, i, &e))
        return;
    const uint8_t *b = aml_bytes(&e);
    uint32_t n = aml_length(&e), k = 0;
    for (uint32_t j = 0; b && j < n && b[j] && k + 1 < size; j++)
        out[k++] = b[j] >= 32 && b[j] < 127 ? (char)b[j] : '.';
    while (k && out[k - 1] == ' ')
        k--;
    out[k] = 0;
    aml_release(&e);
}

static bool evaluate_child(struct aml_node *dev, const char *name, struct aml_value *out)
{
    struct aml_node *n = aml_find_in(dev, name);
    return n && aml_evaluate(n, 0, 0, out);
}

/* A number, or "?" for the firmware's unknown (0xFFFFFFFF). */
static const char *amount(uint64_t n, char *buf, size_t size)
{
    if (n == 0xFFFFFFFF)
        return "?";
    ksnprintf(buf, size, "%lu", n);
    return buf;
}

static void battery_print(struct aml_node *bat)
{
    char path[96];
    aml_path(bat, path, sizeof(path));
    uint64_t sta = 0x1F;
    struct aml_value v = {0};
    if (evaluate_child(bat, "_STA", &v))
        aml_to_integer_value(&v, &sta);
    aml_release(&v);
    if (!(sta & 0x10)) {
        kprintf("battery: %s: not in its place\n", path);
        return;
    }
    struct aml_value bif = {0}, bst = {0};
    uint64_t units = 0, design = 0, full = 0, state = 0, rate = 0, left = 0, volts = 0;
    bool info = evaluate_child(bat, "_BIF", &bif) && element(&bif, 0, &units) &&
                element(&bif, 1, &design) && element(&bif, 2, &full);
    bool now = evaluate_child(bat, "_BST", &bst) && element(&bst, 0, &state) &&
               element(&bst, 1, &rate) && element(&bst, 2, &left) && element(&bst, 3, &volts);
    if (!info || !now) {
        kprintf("battery: %s: not read (the firmware's methods failed)\n", path);
        aml_release(&bif);
        aml_release(&bst);
        return;
    }
    char model[40], kind[24], maker[40], a[24], b[24], c[24], d[24], e[24];
    text_of(&bif, 9, model, sizeof(model));
    text_of(&bif, 11, kind, sizeof(kind));
    text_of(&bif, 12, maker, sizeof(maker));
    const char *cap = units ? "mAh" : "mWh", *flow = units ? "mA" : "mW";
    char percent[16] = "?%";
    if (full && full != 0xFFFFFFFF && left != 0xFFFFFFFF)
        ksnprintf(percent, sizeof(percent), "%lu%%", left * 100 / full);
    const char *doing = state & 2 ? "charging" : state & 1 ? "discharging" : "neither charging nor discharging";
    kprintf("battery: %s: %s (%s of %s %s, designed %s %s), %s%s, %s %s, %s mV; %s %s, model %s\n",
            path, percent, amount(left, a, sizeof(a)), amount(full, b, sizeof(b)), cap,
            amount(design, c, sizeof(c)), cap, doing, state & 4 ? ", critical" : "",
            amount(rate, d, sizeof(d)), flow, amount(volts, e, sizeof(e)), maker, kind, model);
    aml_release(&bif);
    aml_release(&bst);
}

/* (37c-2c) _LID: 1 open, 0 closed, -1 not read. */
static int lid_state(struct aml_node *lid)
{
    struct aml_value v = {0};
    uint64_t open = 0;
    bool ok = evaluate_child(lid, "_LID", &v) && aml_to_integer_value(&v, &open);
    aml_release(&v);
    return ok ? open != 0 : -1;
}

static void supplies_print(void)
{
    struct aml_node *found[4];
    mutex_lock(&aml_mutex);
    int n = aml_find_devices("PNP0C0A", found, 4);
    for (int i = 0; i < n; i++)
        battery_print(found[i]);
    n = aml_find_devices("ACPI0003", found, 4);
    for (int i = 0; i < n; i++) {
        char path[96];
        aml_path(found[i], path, sizeof(path));
        struct aml_value v = {0};
        uint64_t on = 0;
        bool ok = evaluate_child(found[i], "_PSR", &v) && aml_to_integer_value(&v, &on);
        aml_release(&v);
        kprintf("AC adapter: %s: %s\n", path, !ok ? "not read" : on ? "plugged in" : "not plugged in");
    }
    for (int i = 0; i < lid_count; i++) {             /* (37c-2c) */
        char path[96];
        aml_path(lids[i], path, sizeof(path));
        int open = lid_state(lids[i]);
        kprintf("lid: %s: %s\n", path, open < 0 ? "not read" : open ? "open" : "closed");
    }
    mutex_unlock(&aml_mutex);
}

/* (37d) A _CST (ACPI 6.5 8.4.1.1): the count, then per state a package of
 * its register (a generic register descriptor: space, bit width, bit
 * offset, access size, address), its type (C1-C3), latency (us) and
 * power (mW). Up to max states into out: how many. */
static int cstates_of(struct aml_node *cpu, struct acpi_cstate *out, int max)
{
    struct aml_value v = {0};
    int n = 0;
    if (!evaluate_child(cpu, "_CST", &v) || v.type != AML_PACKAGE) {
        aml_release(&v);
        return 0;
    }
    for (uint32_t i = 1; i < aml_length(&v) && n < max; i++) {
        struct aml_value e = {0}, reg = {0};
        uint64_t type, latency, power;
        if (aml_package_get(&v, i, &e) && e.type == AML_PACKAGE && aml_length(&e) >= 4 &&
            aml_package_get(&e, 0, &reg) && reg.type == AML_BUFFER && aml_length(&reg) >= 15 &&
            aml_bytes(&reg)[0] == 0x82 && element(&e, 1, &type) && element(&e, 2, &latency) &&
            element(&e, 3, &power) && type >= 1 && type <= 3) {
            const uint8_t *b = aml_bytes(&reg);
            struct acpi_cstate *c = &out[n++];
            memset(c, 0, sizeof(*c));           /* (padding too: compared whole) */
            c->type = (uint8_t)type;
            c->space = b[3];
            c->bit_width = b[4];
            c->bit_offset = b[5];
            c->access = b[6];
            c->address = 0;
            for (int k = 0; k < 8; k++)
                c->address |= (uint64_t)b[7 + k] << (8 * k);
            c->latency_us = (uint32_t)latency;
            c->power_mw = (uint32_t)power;
        }
        aml_release(&reg);
        aml_release(&e);
    }
    aml_release(&v);
    return n;
}

#define PROCESSORS_MOST 64
#define IDLE_CSTATES 8

int acpi_aml_cstates(struct acpi_cstate *out, int max, int *processors, bool *alike)
{
    *processors = 0;
    *alike = true;
    if (!aml_ready)
        return 0;
    struct aml_node **cpu = kmalloc(PROCESSORS_MOST * sizeof(*cpu));
    struct acpi_cstate *other = kmalloc((size_t)max * sizeof(*other));
    int n = 0;
    mutex_lock(&aml_mutex);
    int count = cpu && other ? aml_find_processors(cpu, PROCESSORS_MOST) : 0;
    for (int i = 0; i < count; i++) {
        int k = i == 0 ? (n = cstates_of(cpu[i], out, max)) : cstates_of(cpu[i], other, max);
        if (!k)
            continue;
        ++*processors;
        if (i > 0 && (k != n || memcmp(other, out, (size_t)k * sizeof(*other))))
            *alike = false;
    }
    mutex_unlock(&aml_mutex);
    kfree(other);
    kfree(cpu);
    return n;
}

/* (37d) power's line about the processors' sleep states. */
static void cstates_print(void)
{
    struct acpi_cstate c[8];
    int processors;
    bool alike;
    int n = acpi_aml_cstates(c, 8, &processors, &alike);
    if (!n) {
        kprintf("processor states: none from the firmware (_CST)\n");
        return;
    }
    char text[256];
    size_t at = (size_t)ksnprintf(text, sizeof(text), "processor states (_CST of %d processors, "
                                  "%s):", processors, alike ? "all alike" : "the first's");
    for (int i = 0; i < n && at < sizeof(text); i++) {
        char how[48];
        if (c[i].space == 0x7F && c[i].bit_offset == 2)
            ksnprintf(how, sizeof(how), "MWAIT (hint 0x%02lX)", c[i].address);
        else if (c[i].space == 0x7F && c[i].bit_offset == 1)
            ksnprintf(how, sizeof(how), "HLT");
        else if (c[i].space == 1)
            ksnprintf(how, sizeof(how), "reading I/O 0x%lX", c[i].address);
        else
            ksnprintf(how, sizeof(how), "space 0x%02X at 0x%lX", c[i].space, c[i].address);
        at += (size_t)ksnprintf(text + at, sizeof(text) - at, "%s C%u by %s, %u us", i ? ";" : "",
                                c[i].type, how, c[i].latency_us);
    }
    kprintf("%s\n", text);
}

/* (37b-4) power's line about the interpreter. */
static void aml_print(void)
{
    if (!aml_ready)
        return;
    struct aml_stats s;
    aml_stats(&s);
    kprintf("AML: %u objects from %u tables, %u methods run, %u errors; the boot's _INI %u "
            "(%u _STA, %u absent); regions: memory %u/%u, I/O %u/%u, PCI %u/%u, EC %u/%u "
            "reads/writes, %u refused (the kernel's RAM), %u with no way there; "
            "%u notifications\n",
            s.nodes, s.loads, s.methods_run, s.errors, boot_init.ini, boot_init.sta,
            boot_init.absent, regions.reads[0], regions.writes[0], regions.reads[1],
            regions.writes[1], regions.reads[2], regions.writes[2], regions.reads[3],
            regions.writes[3], regions.refused, regions.missing, regions.notifies);
    ec_print();
    supplies_print();
    cstates_print();                    /* (37d) */
    arch_processor_power_print();
}

static void (*names_out)(const char *line, void *ctx);
static void *names_ctx;

static void names_line(const char *line)
{
    names_out(line, names_ctx);
}

void acpi_aml_names(void (*out)(const char *line, void *ctx), void *ctx)
{
    mutex_lock(&aml_mutex);
    names_out = out;
    names_ctx = ctx;
    aml_print_names(names_line);
    mutex_unlock(&aml_mutex);
}

int acpi_aml_eval(const char *path, void (*out)(const char *line, void *ctx), void *ctx)
{
    mutex_lock(&aml_mutex);
    struct aml_node *n = aml_find(path);
    int result = -1;
    if (n) {
        char first[160];
        aml_path(n, first, sizeof(first) - 2);
        size_t at = strlen(first);
        first[at++] = ':', first[at++] = ' ', first[at] = 0;
        struct aml_value v = {0};
        result = -2;
        if (aml_evaluate(n, 0, 0, &v)) {
            names_out = out;
            names_ctx = ctx;
            aml_print_value(&v, first, names_line);
            result = 0;
        }
        aml_release(&v);
    }
    mutex_unlock(&aml_mutex);
    return result;
}

/* ---- (37e-2a) A method under a PCI device's ACPI node, for drivers (an
 * interface's entry point such as AMD's ATIF: its function number, then a
 * buffer or 0), under the interpreter's mutex ---- */

int acpi_pci_method(const struct pci_device *d, const char *method, uint64_t function,
                    const void *in, uint32_t in_length, void *out, uint32_t out_size)
{
    mutex_lock(&aml_mutex);
    struct aml_node *n = aml_pci_child(AML_PCI_ADDRESS(0, d->bus, d->device, d->function, 0),
                                       method);
    int result = -1;
    if (n) {
        struct aml_value args[2] = {0}, v = {0};
        aml_set_integer(&args[0], function);
        result = -2;
        if (in ? aml_set_buffer(&args[1], in, in_length) : (aml_set_integer(&args[1], 0), true)) {
            if (aml_evaluate(n, args, 2, &v) && v.type == AML_BUFFER) {
                uint32_t length = aml_length(&v);
                memcpy(out, aml_bytes(&v), length < out_size ? length : out_size);
                result = (int)length;
            }
        }
        aml_release(&v);
        aml_release(&args[0]);
        aml_release(&args[1]);
    }
    mutex_unlock(&aml_mutex);
    return result;
}

/* ---- (37g-1) Sleeping, under the interpreter's mutex ---- */

bool acpi_sleep_type(int x, uint8_t *a, uint8_t *b)
{
    char path[8] = { '\\', '_', 'S', (char)('0' + x), '_', 0 };
    mutex_lock(&aml_mutex);
    struct aml_node *n = aml_find(path);
    struct aml_value v = {0}, e = {0};
    uint64_t va = 0, vb = 0;
    bool got = n && aml_evaluate(n, 0, 0, &v) && v.type == AML_PACKAGE &&
               aml_package_get(&v, 0, &e) && aml_to_integer_value(&e, &va);
    aml_release(&e);
    if (!got || !aml_package_get(&v, 1, &e) || !aml_to_integer_value(&e, &vb))
        vb = 0;
    aml_release(&e);
    aml_release(&v);
    mutex_unlock(&aml_mutex);
    if (got) {
        *a = (uint8_t)va, *b = (uint8_t)vb;
        return true;
    }
    return sleep_state(x, a, b);            /* (no interpreter's namespace: the tables' bytes) */
}

struct acpi_facs *acpi_facs(void)
{
    const struct acpi_fadt *f = acpi_fadt();
    if (!f)
        return 0;
    uint64_t fa = f->header.length >= 140 && f->x_firmware_ctrl ? f->x_firmware_ctrl
                                                                 : f->firmware_ctrl;
    struct acpi_facs *facs = (struct acpi_facs *)acpi_ptr(fa);
    return facs && !memcmp(facs->signature, "FACS", 4) ? facs : 0;
}

int acpi_aml_sleep_method(const char *path, uint64_t state)
{
    mutex_lock(&aml_mutex);
    struct aml_node *n = aml_find(path);
    int result = 0;
    if (n) {
        struct aml_value arg = {0}, v = {0};
        aml_set_integer(&arg, state);
        result = aml_evaluate(n, &arg, 1, &v) ? 1 : -1;
        aml_release(&v);
        aml_release(&arg);
    }
    mutex_unlock(&aml_mutex);
    return result;
}

/* ---- (37c-2b) For the events (kernel/acpi_events.c), each under the
 * interpreter's mutex ---- */

static struct aml_node *gpe_method(unsigned gpe, char kind)
{
    char path[12];
    ksnprintf(path, sizeof(path), "_GPE._%c%02X", kind, gpe);
    struct aml_node *n = aml_find(path);
    return n && n->type == AML_METHOD ? n : 0;
}

/* A device's _PRW: its first element, the GPE that wakes it (an integer;
 * a GPE block device's reference and index are not followed). */
static bool wake_gpe(struct aml_node *dev, uint64_t *gpe)
{
    struct aml_node *prw = aml_find_in(dev, "_PRW");
    struct aml_value v = {0}, e = {0};
    bool ok = prw && aml_evaluate(prw, 0, 0, &v) && v.type == AML_PACKAGE &&
              aml_package_get(&v, 0, &e) && e.type == AML_INTEGER;
    if (ok)
        *gpe = e.integer;
    aml_release(&e);
    aml_release(&v);
    return ok;
}

#define DEVICES_MOST 2048

void acpi_aml_gpes(uint8_t *kind, bool *wake, unsigned count)
{
    memset(kind, 0, count);
    memset(wake, 0, count * sizeof(bool));
    if (!aml_ready)
        return;
    mutex_lock(&aml_mutex);
    for (unsigned g = 0; g < count; g++)
        kind[g] = gpe_method(g, 'L') ? 'L' : gpe_method(g, 'E') ? 'E' : 0;
    /* The GPEs present devices wake by are for waking only (ACPICA's
     * CAN_WAKE: never enabled to run, as Linux's scan marks them first). */
    struct aml_node **list = kmalloc(DEVICES_MOST * sizeof(*list));
    int n = list ? aml_find_devices(0, list, DEVICES_MOST) : 0;
    for (int i = 0; i < n; i++) {
        uint64_t gpe;
        if (wake_gpe(list[i], &gpe) && gpe < count)
            wake[gpe] = true;
    }
    kfree(list);
    mutex_unlock(&aml_mutex);
}

bool acpi_aml_gpe_run(unsigned gpe, char kind)
{
    mutex_lock(&aml_mutex);
    struct aml_node *m = gpe_method(gpe, kind);
    struct aml_value v = {0};
    bool ok = m && aml_evaluate(m, 0, 0, &v);
    aml_release(&v);
    mutex_unlock(&aml_mutex);
    return ok;
}

/* The extended interrupt descriptors of a resource template (ACPI 6.5
 * 6.4.3.6: flags bit 1 edge, bit 2 active low; then the count and the
 * numbers, 4 bytes each). */
static int ged_interrupts(struct aml_node *dev, struct acpi_ged_line *out, int found, int max)
{
    struct aml_node *crs = aml_find_in(dev, "_CRS");
    struct aml_value v = {0};
    if (!crs || !aml_evaluate(crs, 0, 0, &v) || v.type != AML_BUFFER) {
        aml_release(&v);
        return found;
    }
    const uint8_t *b = aml_bytes(&v);
    uint32_t length = aml_length(&v);
    for (uint32_t i = 0; i < length;) {
        uint8_t tag = b[i];
        if (!(tag & 0x80)) {
            if ((tag >> 3 & 0xF) == 0xF)
                break;                      /* the end tag */
            i += 1u + (tag & 7);
            continue;
        }
        if (i + 3 > length)
            break;
        uint32_t item = (uint32_t)(b[i + 1] | b[i + 2] << 8);
        if (tag == 0x89 && item >= 2 && i + 3 + item <= length) {
            uint8_t flags = b[i + 3], count = b[i + 4];
            for (uint32_t k = 0; k < count && 2 + 4 * (k + 1) <= item; k++) {
                if (found < max) {
                    const uint8_t *p = b + i + 5 + 4 * k;
                    out[found].device = dev;
                    out[found].gsi = (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 |
                                                (uint32_t)p[3] << 24);
                    out[found].level = !(flags & 2);
                    out[found].active_low = flags & 4;
                }
                found++;
            }
        }
        i += 3 + item;
    }
    aml_release(&v);
    return found;
}

int acpi_aml_ged_lines(struct acpi_ged_line *out, int max)
{
    if (!aml_ready)
        return 0;
    mutex_lock(&aml_mutex);
    struct aml_node *dev[4];
    int n = aml_find_devices("ACPI0013", dev, 4), found = 0;
    for (int i = 0; i < n; i++)
        found = ged_interrupts(dev[i], out, found, max);
    mutex_unlock(&aml_mutex);
    return found < max ? found : max;
}

bool acpi_aml_ged_run(struct aml_node *device, uint32_t gsi)
{
    mutex_lock(&aml_mutex);
    struct aml_node *evt = aml_find_in(device, "_EVT");
    struct aml_value arg = { .type = AML_INTEGER, .integer = gsi }, v = {0};
    bool ok = evt && aml_evaluate(evt, &arg, 1, &v);
    aml_release(&v);
    mutex_unlock(&aml_mutex);
    return ok;
}

void acpi_aml_buttons(char *paths, size_t size)
{
    size_t at = 0;
    paths[0] = 0;
    for (int i = 0; i < button_count && at + 2 < size; i++) {
        if (i)
            paths[at++] = ' ', paths[at] = 0;
        aml_path(buttons[i], paths + at, size - at);
        at += strlen(paths + at);
    }
}

/* (37c-2c) The embedded controller's events, the lid and the AC adapter
 * for the thread `acpi`. */
int acpi_aml_ec_queries(void)
{
    mutex_lock(&aml_mutex);
    int n = ec_run_queries();
    mutex_unlock(&aml_mutex);
    return n;
}

int acpi_aml_lid(char *path, size_t size)
{
    if (!lid_count)
        return -1;
    mutex_lock(&aml_mutex);
    aml_path(lids[0], path, size);
    int open = lid_state(lids[0]);
    mutex_unlock(&aml_mutex);
    return open;
}

int acpi_aml_ac(char *path, size_t size)
{
    if (!adapter_count)
        return -1;
    mutex_lock(&aml_mutex);
    aml_path(adapters[0], path, size);
    struct aml_value v = {0};
    uint64_t on = 0;
    bool ok = evaluate_child(adapters[0], "_PSR", &v) && aml_to_integer_value(&v, &on);
    aml_release(&v);
    mutex_unlock(&aml_mutex);
    return ok ? on != 0 : -1;
}

/* (37d-2) The idle thread's sleep states: the first processor's _CST when
 * every processor's is the same (else C1 alone), to the architecture. */
void acpi_idle_start(void)
{
    struct acpi_cstate c[IDLE_CSTATES];
    int processors;
    bool alike;
    int n = acpi_aml_cstates(c, IDLE_CSTATES, &processors, &alike);
    arch_idle_setup(c, alike ? n : 0);
}
