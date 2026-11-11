/*
 * HID over I2C (Microsoft's "HID over I2C Protocol Specification" 1.00): a
 * device answers at its I2C address; writing its HID descriptor register's
 * number (2 bytes) and reading 30 bytes gives its HID descriptor: the
 * lengths and registers of its report descriptor, input reports and
 * commands, its vendor and product.
 *
 * Where it is: ACPI's device with the compatible ID PNP0C50 (ACPI0C50)
 * names its I2C bus (an I2cSerialBus resource) and the controller's
 * registers (that controller's Memory32Fixed). The address and the
 * descriptor register may be computed by AML (the laptop's firmware picks
 * them from a table by model): the pairs in that table, then the static
 * address with the usual registers, are tried, and a pair counts only when
 * the device answers with a descriptor of length 30 and version 1.00.
 */
#include "i2chid.h"
#include "acpi.h"
#include "i2c.h"
#include "heap.h"
#include "hid.h"
#include "amdgpio.h"
#include "kprintf.h"
#include "mouse.h"
#include "touchpad.h"
#include "sched.h"
#include "syscalls.h"
#include "timer.h"
#include "string.h"
#include "sleep.h"

#define MAX_DEVICES 4
#define CANDIDATES  8

struct hid_descriptor {
    uint16_t length, version;
    uint16_t report_desc_length, report_desc_register;
    uint16_t input_register, max_input_length;
    uint16_t output_register, max_output_length;
    uint16_t command_register, data_register;
    uint16_t vendor, product, product_version;
};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* Read the HID descriptor at reg of the device at address; true if it is one. */
static bool read_descriptor(struct i2c_bus *bus, uint16_t address, uint16_t reg,
                            struct hid_descriptor *d)
{
    uint8_t cmd[2] = { (uint8_t)reg, (uint8_t)(reg >> 8) }, b[30];
    if (i2c_transfer(bus, address, cmd, 2, b, sizeof(b)) < 0)
        return false;
    d->length = le16(b), d->version = le16(b + 2);
    d->report_desc_length = le16(b + 4), d->report_desc_register = le16(b + 6);
    d->input_register = le16(b + 8), d->max_input_length = le16(b + 10);
    d->output_register = le16(b + 12), d->max_output_length = le16(b + 14);
    d->command_register = le16(b + 16), d->data_register = le16(b + 18);
    d->vendor = le16(b + 20), d->product = le16(b + 22), d->product_version = le16(b + 24);
    return d->length == 30 && d->version == 0x0100;
}

/* The controller's input clock in kHz, by its ACPI ID (Linux's values for
 * these parts); 0 for one not known. */
static uint32_t controller_clock(const struct acpi_device *c)
{
    struct acpi_device amd[8];
    int n = acpi_devices_with("AMDI0010", amd, 8);
    for (int i = 0; i < n; i++)
        if (amd[i].start == c->start)
            return 150000;                      /* AMD FCH: 150 MHz */
    return 0;
}

/* --- a device --- */

#define FIELDS 128

struct device {
    struct i2c_bus *bus;
    uint16_t address;
    struct hid_descriptor d;
    struct hid_field fields[FIELDS];
    int nfields;
    uint8_t touch_report;               /* the touchpad's input report (0: none found) */
    /* the touch report's fields */
    const struct hid_field *tip, *confidence, *contact, *x, *y, *count, *button;
    struct gpio_pin *pin;               /* its interrupt line, or 0: polled */
    struct wait_queue wake;
    volatile bool kicked;               /* the line was asserted (interrupt context sets it) */
    struct mouse *mouse;
    struct touchpad *pad;
    struct touch_contact frame[TOUCH_MAX];
    int collected, expected;
    bool frame_button;
};

/* A command (HID over I2C 7.1): the command register, then its two bytes
 * (type << 4 | report ID, opcode), then for a SET_REPORT the data register
 * with the length, the report ID and the data. */
static int command(struct device *v, uint8_t opcode, uint8_t type_id, const uint8_t *data,
                   size_t n, uint8_t report_id)
{
    uint8_t b[32];
    size_t k = 0;
    b[k++] = (uint8_t)v->d.command_register, b[k++] = (uint8_t)(v->d.command_register >> 8);
    b[k++] = type_id, b[k++] = opcode;
    if (data) {
        if (n + 11 > sizeof(b))
            return -ERR_INVAL;
        size_t len = 2 + (report_id ? 1 : 0) + n;
        b[k++] = (uint8_t)v->d.data_register, b[k++] = (uint8_t)(v->d.data_register >> 8);
        b[k++] = (uint8_t)len, b[k++] = (uint8_t)(len >> 8);
        if (report_id)
            b[k++] = report_id;
        memcpy(b + k, data, n);
        k += n;
    }
    return i2c_transfer(v->bus, v->address, b, k, 0, 0);
}

/* Set a feature report: value's bits from the field on (later bits reach
 * the fields after it); the rest zero. */
static bool set_feature(struct device *v, uint8_t id, uint16_t page, uint16_t usage, uint32_t value)
{
    const struct hid_field *f = hid_find(v->fields, v->nfields, id, HID_FEATURE, page, usage, 0);
    size_t bytes = hid_report_bytes(v->fields, v->nfields, id, HID_FEATURE);
    if (!f || bytes == 0 || bytes > 16)
        return false;
    uint8_t data[16] = { 0 };
    for (uint32_t bit = 0; bit < 32 && (f->bit + bit) / 8 < bytes; bit++)
        if (value >> bit & 1)
            data[(f->bit + bit) / 8] |= (uint8_t)(1u << ((f->bit + bit) % 8));
    return command(v, 0x03, (uint8_t)(0x30 | (id < 15 ? id : 15)), data, bytes, id) == 0;
}

/* An input report: [length (2), report ID, data]; its length, 0 for none. */
static size_t read_input(struct device *v, uint8_t *b, size_t max)
{
    size_t n = v->d.max_input_length < max ? v->d.max_input_length : max;
    if (i2c_transfer(v->bus, v->address, 0, 0, b, n) < 0)
        return 0;
    size_t len = (size_t)(b[0] | b[1] << 8);
    return len > 2 && len <= n ? len : 0;
}

/* Interrupt context (the GPIO controller's handler): the line went low. */
static void kick(void *arg)
{
    struct device *v = arg;
    v->kicked = true;
    wait_queue_wake_all(&v->wake);
}

/* One touch report: a contact; in "hybrid" reporting the first report of a
 * frame carries the number of contacts and the rest follow one a report. */
static void touch_report(struct device *v, const uint8_t *data, size_t n)
{
    int32_t count = v->count ? hid_value(v->count, data, n) : 1;
    if (count > 0) {                            /* a new frame */
        if (v->collected)
            touchpad_frame(v->pad, v->frame, v->collected, v->frame_button, timer_elapsed_ms(0));
        v->collected = 0;
        v->expected = count < TOUCH_MAX ? count : TOUCH_MAX;
        v->frame_button = v->button && hid_value(v->button, data, n);
    }
    if (v->collected < TOUCH_MAX) {
        struct touch_contact *c = &v->frame[v->collected++];
        c->id = (uint8_t)(v->contact ? hid_value(v->contact, data, n) : 0);
        c->tip = v->tip && hid_value(v->tip, data, n);
        c->confident = !v->confidence || hid_value(v->confidence, data, n);
        c->x = v->x ? hid_value(v->x, data, n) : 0;
        c->y = v->y ? hid_value(v->y, data, n) : 0;
    }
    if (v->collected >= v->expected) {
        touchpad_frame(v->pad, v->frame, v->collected, v->frame_button, timer_elapsed_ms(0));
        v->collected = 0;
    }
}

/* The reader: woken by the interrupt line (every 100 ms anyway, a safety
 * net), it reads reports while the line is held low; without a line it
 * polls. */
static void reader(void *arg)
{
    struct device *v = arg;
    uint8_t b[64];
    for (;;) {
        if (v->pin) {
            wait_event_until(&v->wake, v->kicked, timer_ticks() + TIMER_HZ / 10);
            v->kicked = false;
        } else {
            timer_sleep_ms(5);
        }
        for (int i = 0; i < 32; i++) {          /* (at most this many in a row) */
            if (v->pin && i > 0 && !gpio_pin_asserted(v->pin))
                break;
            size_t len = read_input(v, b, sizeof(b));
            if (!len)
                break;
            mouse_count_event(v->mouse);
            if (b[2] == v->touch_report)
                touch_report(v, b + 3, len - 3);
            if (!v->pin)
                break;
        }
        if (v->pin)
            gpio_pin_rearm(v->pin);
    }
}

/* Power on, reset, the report descriptor, touchpad mode, the reader. */
/* SET_POWER on, then RESET: it answers with an empty report. The start's,
 * and (37g-3) again after a sleep. */
static void power_on_reset(struct device *v)
{
    command(v, 0x08, 0x00, 0, 0, 0);                    /* SET_POWER on */
    timer_sleep_ms(2);
    command(v, 0x01, 0x00, 0, 0, 0);                    /* RESET */
    uint8_t b[64];
    for (int i = 0; i < 100; i++) {
        if (i2c_transfer(v->bus, v->address, 0, 0, b, 2) == 0 && b[0] == 0 && b[1] == 0)
            break;
        timer_sleep_ms(10);
    }
}

/* Input Mode 3 (touch pad), and the surface and button switches on: true
 * when it took the mode. */
static bool touch_pad_modes(struct device *v)
{
    bool mode = false;
    if (v->touch_report)
        for (int i = 0; i < v->nfields && !mode; i++)
            if (v->fields[i].kind == HID_FEATURE && v->fields[i].page == 0x0D &&
                v->fields[i].usage == 0x52)
                mode = set_feature(v, v->fields[i].report_id, 0x0D, 0x52, 3);
    bool switches = false;
    for (int i = 0; i < v->nfields && !switches; i++)
        if (v->fields[i].kind == HID_FEATURE && v->fields[i].page == 0x0D &&
            v->fields[i].usage == 0x57) {
            const struct hid_field *button = hid_find(v->fields, v->nfields,
                                                      v->fields[i].report_id, HID_FEATURE, 0x0D,
                                                      0x58, 0);
            uint32_t value = 1u << 0 | (button ? 1u << (button->bit - v->fields[i].bit) : 0);
            switches = set_feature(v, v->fields[i].report_id, 0x0D, 0x57, value);
        }
    return mode;
}

/* (37g-3) Sleep, as Linux's i2c-hid: the bus held over it (the reader
 * waits), the touch pad told to sleep (SET_POWER, state 1); after it the
 * I2C controller and the GPIO pin set up again (both lost theirs), the
 * touch pad on, reset, and in touch pad mode again. */
static bool hid_suspend(void *ctx)
{
    struct device *v = ctx;
    i2c_bus_hold(v->bus);
    command(v, 0x08, 0x01, 0, 0, 0);                    /* SET_POWER sleep */
    return true;
}

static bool hid_resume(void *ctx)
{
    struct device *v = ctx;
    bool ok = i2c_bus_wake(v->bus);
    if (v->pin)
        gpio_pin_wake(v->pin);
    i2c_bus_let_go(v->bus);
    if (ok) {
        power_on_reset(v);
        ok = touch_pad_modes(v);
    }
    if (!ok)
        kprintf("  i2c-hid:       %s 0x%02x: the touch pad did not come back after the sleep\n",
                i2c_name(v->bus), v->address);
    wait_queue_wake_all(&v->wake);
    return ok;
}

static void start(struct i2c_bus *bus, uint16_t address, const struct hid_descriptor *d,
                  const char *gpio, uint16_t gpio_pin)
{
    struct device *v = kzalloc(sizeof(*v));
    uint8_t *desc = kmalloc((size_t)d->report_desc_length + 1);
    if (!v || !desc) {
        kfree(v);
        kfree(desc);
        return;
    }
    v->bus = bus, v->address = address, v->d = *d;
    power_on_reset(v);
    uint8_t reg[2] = { (uint8_t)d->report_desc_register, (uint8_t)(d->report_desc_register >> 8) };
    if (i2c_transfer(bus, address, reg, 2, desc, d->report_desc_length) < 0 ||
        (v->nfields = hid_parse(desc, d->report_desc_length, v->fields, FIELDS)) <= 0) {
        kprintf("  i2c-hid:       %s 0x%02x: its report descriptor does not read\n", i2c_name(bus),
                address);
        kfree(desc);
        kfree(v);
        return;
    }
    kfree(desc);
    for (int i = 0; i < v->nfields && !v->touch_report; i++)
        if (v->fields[i].kind == HID_INPUT && v->fields[i].application == (0x0Du << 16 | 0x05) &&
            v->fields[i].page == 0x0D && v->fields[i].usage == 0x42)
            v->touch_report = v->fields[i].report_id;   /* a touch pad's tip switch */
    bool mode = touch_pad_modes(v);
    uint8_t id = v->touch_report;
    if (!id || !mode) {
        kprintf("  i2c-hid:       %s 0x%02x: not a touch pad (or it did not take touch pad mode)\n",
                i2c_name(bus), address);
        kfree(v);
        return;
    }
    v->tip = hid_find(v->fields, v->nfields, id, HID_INPUT, 0x0D, 0x42, 0);
    v->confidence = hid_find(v->fields, v->nfields, id, HID_INPUT, 0x0D, 0x47, 0);
    v->contact = hid_find(v->fields, v->nfields, id, HID_INPUT, 0x0D, 0x51, 0);
    v->x = hid_find(v->fields, v->nfields, id, HID_INPUT, 0x01, 0x30, 0);
    v->y = hid_find(v->fields, v->nfields, id, HID_INPUT, 0x01, 0x31, 0);
    v->count = hid_find(v->fields, v->nfields, id, HID_INPUT, 0x0D, 0x54, 0);
    v->button = hid_find(v->fields, v->nfields, id, HID_INPUT, 0x09, 0x01, 0);
    char path[24];
    ksnprintf(path, sizeof(path), "%s 0x%02x", i2c_name(bus), address);
    struct devname_id did = { d->vendor, d->product, "", path };
    v->mouse = mouse_new("reports", &did);
    int32_t xu = v->x ? hid_units_per_mm(v->x) : 0, yu = v->y ? hid_units_per_mm(v->y) : 0;
    v->pad = v->mouse ? touchpad_new(v->mouse, xu, yu) : 0;
    if (!v->pad || !v->x || !v->y || !v->tip) {
        if (v->mouse)
            mouse_drop(v->mouse);
        kfree(v);
        return;
    }
    if (gpio[0])
        v->pin = gpio_pin_listen(gpio, gpio_pin, kick, v);
    char label[16];
    ksnprintf(label, sizeof(label), "%s:", mouse_name(v->mouse));
    char how[32];
    if (v->pin)
        ksnprintf(how, sizeof(how), "interrupt: %s pin %u", gpio, gpio_pin);
    else
        ksnprintf(how, sizeof(how), "polled");
    kprintf("  %-15stouch pad %04x:%04x on %s, %d x %d mm, %s\n", label, d->vendor, d->product,
            path, xu ? (v->x->logical_max - v->x->logical_min) / xu : 0,
            yu ? (v->y->logical_max - v->y->logical_min) / yu : 0, how);
    mouse_ready(v->mouse);
    thread_create("i2c-hid", reader, v);
    sleep_register("i2c-hid", hid_suspend, hid_resume, v);     /* (37g-3) */
}

static void setup(const struct acpi_device *dev)
{
    char seg[5];
    uint16_t address;
    uint32_t speed;
    if (!acpi_device_i2c(dev, seg, &address, &speed))
        return;                                 /* (one naming PNP0C50 for another, e.g. PS/2) */
    struct acpi_device controller;
    uint64_t base;
    uint32_t length;
    if (!acpi_device_named(seg, &controller) || !acpi_device_memory(&controller, &base, &length)) {
        kprintf("  i2c-hid:       %s: its bus %s has no registers in ACPI\n", dev->name, seg);
        return;
    }
    uint32_t clock = controller_clock(&controller);
    if (!clock) {
        kprintf("  i2c-hid:       %s: the clock of its bus %s is not known\n", dev->name, seg);
        return;
    }
    char what[24];
    ksnprintf(what, sizeof(what), "%s for %s", seg, dev->name);
    struct i2c_bus *bus = i2c_bus_at(base, clock, speed ? speed : 400000, what);
    if (!bus)
        return;
    uint16_t addr[CANDIDATES], reg[CANDIDATES];
    int n = acpi_device_i2c_hid_pairs(dev, addr, reg, CANDIDATES - 2);
    if (address && n < CANDIDATES - 1)
        addr[n] = address, reg[n++] = 0x0001;
    if (address && n < CANDIDATES)
        addr[n] = address, reg[n++] = 0x0020;
    for (int i = 0; i < n; i++) {
        struct hid_descriptor d;
        if (!read_descriptor(bus, addr[i], reg[i], &d))
            continue;
        kprintf("  i2c-hid:       %s address 0x%02x (%s): HID %04x:%04x version 0x%04x, report "
                "descriptor %u bytes at 0x%04x, input 0x%04x (up to %u bytes)\n",
                i2c_name(bus), addr[i], dev->name, d.vendor, d.product, d.product_version,
                d.report_desc_length, d.report_desc_register, d.input_register,
                d.max_input_length);
        char gpio[5] = "";
        uint16_t pin = 0;
        acpi_device_gpio_int(dev, gpio, &pin);
        start(bus, addr[i], &d, gpio, pin);
        return;
    }
    kprintf("  i2c-hid:       %s on %s: no HID descriptor at the %d address(es) tried\n", dev->name,
            i2c_name(bus), n);
}

void i2chid_init(void)
{
    struct acpi_device devs[MAX_DEVICES];
    int n = acpi_devices_with("PNP0C50", devs, MAX_DEVICES);
    if (!n)
        n = acpi_devices_with("ACPI0C50", devs, MAX_DEVICES);
    for (int i = 0; i < n; i++)
        setup(&devs[i]);
}
