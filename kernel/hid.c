/*
 * HID report descriptor parsing: short items (a prefix byte: size in bits
 * 0-1 as 0, 1, 2 or 4 bytes, type in 2-3 as main, global or local, tag in
 * 4-7), long items skipped. Global state (usage page, logical range, report
 * size, count and ID) survives main items and can be pushed and popped;
 * local state (usages, a usage range) is cleared by each main item. Each
 * Input, Output or Feature item makes `count` fields of `size` bits, the
 * n-th taking the n-th usage, or usage_min + n; the elements past the last
 * usage repeat it and are one field with a count (a buffer of bytes).
 * Constant fields (padding) are kept, so a report's length counts them.
 */
#include "hid.h"
#include "string.h"

#define USAGES      32
#define STACK       4
#define DEPTH       16

struct globals {
    uint16_t page;
    int32_t logical_min, logical_max;
    int32_t physical_min, physical_max;
    uint32_t unit;
    int8_t unit_exponent;
    uint32_t size, count;
    uint8_t report_id;
};

static uint32_t bits_used(const struct hid_field *out, int n, uint8_t id, uint8_t kind)
{
    uint32_t end = 0;
    for (int i = 0; i < n; i++)
        if (out[i].report_id == id && out[i].kind == kind &&
            out[i].bit + (uint32_t)out[i].size * out[i].count > end)
            end = out[i].bit + (uint32_t)out[i].size * out[i].count;
    return end;
}

int hid_parse(const uint8_t *desc, size_t n, struct hid_field *out, int max)
{
    struct globals g = { 0 }, stack[STACK];
    int sp = 0, count = 0, depth = 0;
    uint32_t usages[USAGES];                    /* page << 16 | usage */
    int nusages = 0;
    uint32_t usage_min = 0, usage_max = 0;
    bool have_range = false;
    uint32_t collection_usage[DEPTH];
    uint32_t application = 0;
    uint8_t finger = 0;
    bool touch = false;
    for (size_t i = 0; i < n;) {
        uint8_t prefix = desc[i];
        if (prefix == 0xFE) {                   /* a long item: skipped */
            if (i + 2 >= n)
                return -1;
            i += 3 + desc[i + 1];
            continue;
        }
        size_t size = (size_t[]){ 0, 1, 2, 4 }[prefix & 3];
        if (i + 1 + size > n)
            return -1;
        uint32_t u = 0;
        for (size_t k = 0; k < size; k++)
            u |= (uint32_t)desc[i + 1 + k] << (8 * k);
        int32_t s = size == 1 ? (int8_t)u : size == 2 ? (int16_t)u : (int32_t)u;
        int type = (prefix >> 2) & 3, tag = prefix >> 4;
        i += 1 + size;
        if (type == 1) {                        /* global */
            switch (tag) {
            case 0x0: g.page = (uint16_t)u; break;
            case 0x1: g.logical_min = s; break;
            case 0x2: g.logical_max = g.logical_min >= 0 && size < 4 ? (int32_t)u : s; break;
            case 0x3: g.physical_min = s; break;
            case 0x4: g.physical_max = g.physical_min >= 0 && size < 4 ? (int32_t)u : s; break;
            case 0x5: g.unit_exponent = (int8_t)(u & 8 ? (int)(u & 0xF) - 16 : (int)(u & 0xF)); break;
            case 0x6: g.unit = u; break;
            case 0x7: g.size = u; break;
            case 0x8: g.report_id = (uint8_t)u; break;
            case 0x9: g.count = u; break;
            case 0xA: if (sp < STACK) stack[sp++] = g; break;
            case 0xB: if (sp > 0) g = stack[--sp]; break;
            }
            continue;
        }
        if (type == 2) {                        /* local */
            uint32_t full = size == 4 ? u : (uint32_t)g.page << 16 | u;
            if (tag == 0x0 && nusages < USAGES)
                usages[nusages++] = full;
            else if (tag == 0x1)
                usage_min = full, have_range = true;
            else if (tag == 0x2)
                usage_max = full, have_range = true;
            continue;
        }
        if (type != 0)
            continue;                           /* (reserved) */
        if (tag == 0xA) {                       /* Collection */
            uint32_t usage = nusages ? usages[0] : 0;
            if (depth < DEPTH)
                collection_usage[depth] = usage;
            depth++;
            if (u == 1)                         /* an application */
                application = usage, finger = 0, touch = (usage >> 16) == 0x0D;
            else if (touch && u == 2 && usage == (0x0Du << 16 | 0x22))
                finger++;                       /* a logical collection: a finger */
        } else if (tag == 0xC) {                /* End Collection */
            if (depth > 0)
                depth--;
        } else if (tag == 0x8 || tag == 0x9 || tag == 0xB) {   /* Input, Output, Feature */
            uint8_t kind = tag == 0x8 ? HID_INPUT : tag == 0x9 ? HID_OUTPUT : HID_FEATURE;
            if (g.size == 0 || g.size > 32 || g.count > 4096)
                return -1;
            uint32_t bit = bits_used(out, count, g.report_id, kind);
            bool in_finger = touch && depth > 0 && depth <= DEPTH &&
                             collection_usage[depth - 1] == (0x0Du << 16 | 0x22);
            for (uint32_t k = 0; k < g.count; k++) {
                uint32_t usage = have_range ? usage_min + k
                               : nusages ? usages[k < (uint32_t)nusages ? k : (uint32_t)nusages - 1] : 0;
                if (have_range && usage > usage_max)
                    usage = usage_max;
                /* The rest share this usage: one field for all of them. */
                bool rest = have_range ? usage == usage_max : k + 1 >= (uint32_t)nusages;
                uint32_t elements = rest ? g.count - k : 1;
                if (count == max)
                    return count;
                struct hid_field *f = &out[count];
                memset(f, 0, sizeof(*f));
                f->report_id = g.report_id;
                f->kind = kind;
                f->flags = (uint8_t)(u & 0x07);
                f->size = (uint8_t)g.size;
                f->page = (uint16_t)(usage >> 16);
                f->usage = (uint16_t)usage;
                f->bit = bit + k * g.size;
                f->count = (uint16_t)elements;
                f->logical_min = g.logical_min;
                f->logical_max = g.logical_max;
                f->physical_min = g.physical_min;
                f->physical_max = g.physical_max;
                f->unit = g.unit;
                f->unit_exponent = g.unit_exponent;
                f->application = application;
                f->finger = in_finger ? finger : 0;
                count++;
                if (rest)
                    break;
            }
        }
        if (type == 0) {                        /* every main item ends the local state */
            nusages = 0;
            have_range = false;
        }
    }
    return count;
}

int32_t hid_value(const struct hid_field *f, const uint8_t *data, size_t len)
{
    uint32_t v = 0;
    for (uint32_t b = 0; b < f->size; b++) {
        uint32_t at = f->bit + b;
        if (at / 8 >= len)
            break;
        v |= (uint32_t)((data[at / 8] >> (at % 8)) & 1) << b;
    }
    if (f->logical_min < 0 && f->size < 32 && (v >> (f->size - 1)) & 1)
        v |= ~0u << f->size;                    /* sign extended */
    return (int32_t)v;
}

const struct hid_field *hid_find(const struct hid_field *fields, int n, uint8_t report_id,
                                 uint8_t kind, uint16_t page, uint16_t usage, uint8_t finger)
{
    for (int i = 0; i < n; i++)
        if (fields[i].report_id == report_id && fields[i].kind == kind && fields[i].page == page &&
            fields[i].usage == usage && (!finger || fields[i].finger == finger) &&
            !(fields[i].flags & HID_CONSTANT))
            return &fields[i];
    return 0;
}

size_t hid_report_bytes(const struct hid_field *fields, int n, uint8_t report_id, uint8_t kind)
{
    return (bits_used(fields, n, report_id, kind) + 7) / 8;
}

int32_t hid_units_per_mm(const struct hid_field *f)
{
    int32_t extent = f->physical_max - f->physical_min, range = f->logical_max - f->logical_min;
    uint32_t system = f->unit & 0xF, length = (f->unit >> 4) & 0xF;
    if (extent <= 0 || range <= 0 || length != 1 || (system != 1 && system != 3))
        return 0;
    /* The extent in micrometres: cm = 10000 um, inch = 25400 um, times 10^exponent. */
    int64_t um = (int64_t)extent * (system == 1 ? 10000 : 25400);
    for (int e = f->unit_exponent; e > 0; e--)
        um *= 10;
    for (int e = f->unit_exponent; e < 0; e++)
        um /= 10;
    if (um <= 0)
        return 0;
    return (int32_t)(((int64_t)range * 1000 + um / 2) / um);
}
