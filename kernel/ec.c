/*
 * The ACPI embedded controller (37b-5). See ec.h.
 *
 * Every transfer waits for the controller: IBF (input buffer full) to
 * clear before a byte goes in, OBF (output buffer full) before a byte
 * comes out, each wait at most EC_WAIT_MS. The controller is polled (its
 * GPE comes with the events of 37c). A controller that does not answer
 * EC_GIVE_UP times in a row is left alone, so a missing one cannot hold
 * the boot for a wait per byte. It is only used by the interpreter, under
 * its mutex.
 */
#include "ec.h"
#include "acpi.h"
#include "aml.h"
#include "arch.h"
#include "arch_api.h"
#include "kprintf.h"
#include "string.h"

#define EC_OBF 0x01
#define EC_IBF 0x02
#define EC_SCI_EVT 0x20                 /* (37c-2c) an event waits for a query */
#define EC_READ 0x80
#define EC_WRITE 0x81
#define EC_QUERY 0x84
#define QUERIES_LOGGED 16
#define QUERIES_AT_ONCE 32
#define EC_WAIT_MS 100
#define EC_GIVE_UP 8

static struct {
    bool ready, dead;
    uint16_t data, command;
    const char *from;                   /* "ECDT" or "_CRS" */
    struct aml_node *device;
    uint32_t reads, writes, timeouts, failed_in_a_row;
    int gpe;                            /* (37c-2c) its GPE, -1 none */
    uint32_t queries, unknown;          /* queries run, and those without a method */
} ec = { .gpe = -1 };

static uint8_t status(void)
{
    uint32_t v = 0xFF;
    arch_port_access(ec.command, 8, &v, false);
    return (uint8_t)v;
}

static bool wait_for(uint8_t mask, uint8_t want)
{
    uint64_t hz = arch_counter_hz(), start = arch_counter();
    uint64_t limit = hz / 1000 * EC_WAIT_MS;
    do {
        if ((status() & mask) == want)
            return true;
        arch_cpu_relax();
    } while (hz && arch_counter() - start < limit);
    ec.timeouts++;
    if (++ec.failed_in_a_row >= EC_GIVE_UP && !ec.dead) {
        ec.dead = true;
        kprintf("aml: the embedded controller does not answer: left alone\n");
    }
    return false;
}

static bool put(uint16_t port, uint8_t byte)
{
    if (!wait_for(EC_IBF, 0))
        return false;
    uint32_t v = byte;
    return arch_port_access(port, 8, &v, true);
}

static bool read_byte(uint8_t address, uint8_t *byte)
{
    if (!put(ec.command, EC_READ) || !put(ec.data, address) || !wait_for(EC_OBF, EC_OBF))
        return false;
    uint32_t v = 0;
    if (!arch_port_access(ec.data, 8, &v, false))
        return false;
    *byte = (uint8_t)v;
    ec.failed_in_a_row = 0;
    ec.reads++;
    return true;
}

static bool write_byte(uint8_t address, uint8_t byte)
{
    if (!put(ec.command, EC_WRITE) || !put(ec.data, address) || !put(ec.data, byte) ||
        !wait_for(EC_IBF, 0))
        return false;
    ec.failed_in_a_row = 0;
    ec.writes++;
    return true;
}

bool ec_access(uint64_t address, int width, uint64_t *value, bool write)
{
    uint32_t n = (uint32_t)width / 8;
    if (!ec.ready || ec.dead || !n || address + n > 256)
        return false;
    uint64_t v = write ? *value : 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t byte = (uint8_t)(v >> (8 * i));
        if (write ? !write_byte((uint8_t)(address + i), byte)
                  : !read_byte((uint8_t)(address + i), &byte))
            return false;
        if (!write)
            v |= (uint64_t)byte << (8 * i);
    }
    if (!write)
        *value = v;
    return true;
}

/* The ECDT (ACPI 6.5 5.2.16): the command/status and data registers as
 * generic addresses (I/O ports here), then the device's path. */
struct ecdt {
    struct acpi_header header;
    struct { uint8_t space, bits, offset, access; uint64_t address; } __attribute__((packed))
        control, data;
    uint32_t uid;
    uint8_t gpe;
    char id[];
} __attribute__((packed));

void ec_start_early(void)
{
    const struct ecdt *t = (const struct ecdt *)acpi_find_table("ECDT");
    if (!t || t->header.length < sizeof(*t) + 1)
        return;
    if (t->control.space != 1 || t->data.space != 1 || !t->control.address ||
        !t->data.address || t->control.address > 0xFFFF || t->data.address > 0xFFFF)
        return;                         /* (an empty or memory ECDT: the device's _CRS) */
    ec.command = (uint16_t)t->control.address;
    ec.data = (uint16_t)t->data.address;
    ec.gpe = t->gpe;
    ec.from = "ECDT";
    ec.ready = true;
    char id[96];
    uint32_t n = t->header.length - sizeof(*t);
    if (n >= sizeof(id))
        n = sizeof(id) - 1;
    memcpy(id, t->id, n);
    id[n] = 0;
    ec.device = aml_find(id);
}

/* The first two I/O port descriptors of a resource template: the data
 * register, then the command/status register (ACPI 6.5 12.11). */
static bool ports_from(const struct aml_value *crs)
{
    if (crs->type != AML_BUFFER)
        return false;
    const uint8_t *b = aml_bytes(crs);
    uint32_t length = aml_length(crs), found = 0;
    uint16_t port[2] = {0, 0};
    for (uint32_t i = 0; i < length && found < 2;) {
        uint8_t tag = b[i];
        if (tag & 0x80) {               /* a large item: its 16-bit length */
            if (i + 3 > length)
                break;
            i += 3u + (uint32_t)(b[i + 1] | b[i + 2] << 8);
            continue;
        }
        uint32_t item = 1u + (tag & 7);
        uint8_t kind = tag >> 3 & 0xF;
        if (kind == 0xF || i + item > length)
            break;                      /* the end tag */
        if (kind == 0x8 && item >= 8)   /* I/O port: its minimum */
            port[found++] = (uint16_t)(b[i + 2] | b[i + 3] << 8);
        else if (kind == 0x9 && item >= 4)  /* fixed I/O port */
            port[found++] = (uint16_t)((b[i + 1] | b[i + 2] << 8) & 0x3FF);
        i += item;
    }
    if (found < 2)
        return false;
    ec.data = port[0];
    ec.command = port[1];
    return true;
}

void ec_start(void)
{
    if (!ec.device) {
        struct aml_node *dev = 0;
        if (aml_find_devices("PNP0C09", &dev, 1) < 1)
            return;
        ec.device = dev;
    }
    if (!ec.ready) {
        struct aml_node *crs = aml_find_in(ec.device, "_CRS");
        struct aml_value v = {0};
        bool ok = crs && aml_evaluate(crs, 0, 0, &v) && ports_from(&v);
        aml_release(&v);
        if (!ok) {
            kprintf("aml: the embedded controller's _CRS names no ports\n");
            return;
        }
        ec.from = "_CRS";
        ec.ready = true;
    }
    /* Is one there? Nothing at the ports reads 0xFF (no controller drives
     * them); else byte 0 is read (reading its memory changes nothing). */
    uint8_t byte;
    if (status() == 0xFF) {
        ec.dead = true;
        kprintf("aml: the embedded controller does not answer: left alone\n");
    } else if (!read_byte(0, &byte) && !ec.dead) {
        ec.dead = true;
        kprintf("aml: the embedded controller does not answer: left alone\n");
    }
    if (!ec.dead)
        aml_run_regs(ec.device, 3);     /* the firmware may use it now (not if it is not there) */
    /* (37c-2c) its GPE: the device's _GPE (an integer: a GPE block's
     * package is not followed), else the ECDT's */
    struct aml_node *g = aml_find_in(ec.device, "_GPE");
    struct aml_value gv = {0};
    uint64_t number;
    if (g && aml_evaluate(g, 0, 0, &gv) && gv.type == AML_INTEGER &&
        aml_to_integer_value(&gv, &number) && number < 256)
        ec.gpe = (int)number;
    aml_release(&gv);
    char path[96];
    aml_path(ec.device, path, sizeof(path));
    char gpe[16] = "no GPE";
    if (ec.gpe >= 0)
        ksnprintf(gpe, sizeof(gpe), "GPE 0x%02X", (unsigned)ec.gpe);
    kprintf("  ACPI: embedded controller %s, ports 0x%X/0x%X (%s), %s: %s\n", path,
            (unsigned)ec.data, (unsigned)ec.command, ec.from, gpe,
            ec.dead ? "does not answer" : "answers");
}

int ec_gpe(void)
{
    return ec.ready && !ec.dead && ec.device ? ec.gpe : -1;
}

/* QR_EC: the controller's next event's number (0: none). */
static bool query(uint8_t *number)
{
    if (!put(ec.command, EC_QUERY) || !wait_for(EC_OBF, EC_OBF))
        return false;
    uint32_t v = 0;
    if (!arch_port_access(ec.data, 8, &v, false))
        return false;
    ec.failed_in_a_row = 0;
    *number = (uint8_t)v;
    return true;
}

int ec_run_queries(void)
{
    int ran = 0;
    for (int i = 0; i < QUERIES_AT_ONCE && ec.ready && !ec.dead && (status() & EC_SCI_EVT);
         i++) {
        uint8_t q;
        if (!query(&q) || !q)
            break;
        char name[5];
        ksnprintf(name, sizeof(name), "_Q%02X", q);
        struct aml_node *m = aml_find_in(ec.device, name);
        struct aml_value v = {0};
        bool ok = m && aml_evaluate(m, 0, 0, &v);
        aml_release(&v);
        ec.queries++;
        ec.unknown += !m;
        if (ec.queries <= QUERIES_LOGGED)
            kprintf("acpi: the embedded controller's event 0x%02X: %s%s\n", q,
                    !m ? "no method " : ok ? "ran " : "failed ", name);
        ran++;
    }
    return ran;
}

void ec_print(void)
{
    if (!ec.ready)
        return;
    char path[96] = "?";
    if (ec.device)
        aml_path(ec.device, path, sizeof(path));
    kprintf("embedded controller: %s, ports 0x%X/0x%X (from the %s); %u reads, %u writes, "
            "%u timeouts; %u events (%u without a method)%s\n", path, (unsigned)ec.data,
            (unsigned)ec.command, ec.from, ec.reads, ec.writes, ec.timeouts, ec.queries,
            ec.unknown, ec.dead ? "; it does not answer" : "");
}
