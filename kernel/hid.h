/*
 * HID report descriptors (15u; the USB HID 1.11 specification's item
 * format, which HID over I2C uses unchanged): what a device's reports hold,
 * as a list of fields: which report, where (bit offset and size), what
 * (usage page and usage), the logical range, and for touch devices which
 * finger (the logical collection) it belongs to. For any HID device, not
 * only touchpads (USB HID can use it later).
 */
#ifndef RITTEROS_HID_H
#define RITTEROS_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_INPUT    0
#define HID_OUTPUT   1
#define HID_FEATURE  2

#define HID_CONSTANT 0x01               /* main item flags: padding */
#define HID_VARIABLE 0x02
#define HID_RELATIVE 0x04

struct hid_field {
    uint8_t report_id;                  /* 0: the device has no report IDs */
    uint8_t kind;                       /* HID_INPUT, HID_OUTPUT, HID_FEATURE */
    uint8_t flags;
    uint8_t size;                       /* bits (at most 32) */
    uint16_t count;                     /* elements of `size` bits in a row (the same usage) */
    uint16_t page, usage;
    uint32_t bit;                       /* offset in the report, after its ID byte */
    int32_t logical_min, logical_max;
    int32_t physical_min, physical_max; /* 0, 0: not given */
    uint32_t unit;                      /* HID unit code (nibbles: system, length, ...) */
    int8_t unit_exponent;
    uint32_t application;               /* page << 16 | usage of its application collection */
    uint8_t finger;                     /* in a touch application: the number of its
                                         * logical (finger) collection, 1 on; else 0 */
};

/* Parse the descriptor into up to max fields (padding
 * kept, flagged HID_CONSTANT); how many, or -1 if it is malformed. */
int hid_parse(const uint8_t *desc, size_t n, struct hid_field *out, int max);

/* The field's (first element's) value in a report (data after the ID byte,
 * len bytes); sign extended when its logical minimum is negative. */
int32_t hid_value(const struct hid_field *f, const uint8_t *data, size_t len);

/* The first field of this report, kind, page and usage (finger 0: any). */
const struct hid_field *hid_find(const struct hid_field *fields, int n, uint8_t report_id,
                                 uint8_t kind, uint16_t page, uint16_t usage, uint8_t finger);

/* Units of the field per millimetre, from its physical range and unit
 * (centimetre or inch lengths); 0 if the descriptor does not say. */
int32_t hid_units_per_mm(const struct hid_field *f);

/* A report's length in bytes (after the ID byte). */
size_t hid_report_bytes(const struct hid_field *fields, int n, uint8_t report_id, uint8_t kind);

#endif
