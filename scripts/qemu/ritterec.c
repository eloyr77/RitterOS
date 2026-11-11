/*
 * ritterec: an ACPI embedded controller for RitterOS's tests (step 37b-5),
 * our own model of the interface the ACPI specification (6.5, chapter 12)
 * gives every embedded controller: a data port and a command/status port
 * (0x62 and 0x66 by default, as on PCs), 256 bytes of the controller's
 * memory, the commands RD_EC (0x80), WR_EC (0x81), BE_EC (0x82), BD_EC
 * (0x83) and QR_EC (0x84). The memory starts as a known pattern (byte i
 * is i * 7 + 3), so a test can tell what it reads; the firmware's side is
 * an SSDT the test hands QEMU (-acpitable).
 *
 * Status (read from the command port): OBF (bit 0) when a byte waits in
 * the data port, IBF (1) never (the model takes every byte at once), CMD
 * (3) when the last byte written was a command, BURST (4) in burst mode.
 *
 * (37c-2b) The property "gpe" raises general-purpose events: setting it
 * (the monitor's `qom-set /machine/peripheral/ID gpe MASK`) sets those
 * bits of the machine's GPE0 status (q35: GPE 0-7) through its ACPI device,
 * as a laptop's controller raises its GPE, and the SCI follows.
 * (37c-2c) The property "query" queues an event (up to 8): SCI_EVT (status
 * bit 5) is set while one waits, QR_EC gives the oldest (0 when none),
 * and queuing one raises the controller's GPE ("sci-gpe", default 7), as
 * the controller of a laptop's lid, adapter and battery does.
 *
 * Built into QEMU by scripts/setup-toolchain.sh (hw/misc). GPL-2.0-or-later
 * as QEMU, RitterOS's own code.
 */
#include "qemu/osdep.h"
#include "hw/isa/isa.h"
#include "hw/core/qdev-properties.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/acpi_dev_interface.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_RITTEREC "ritterec"
OBJECT_DECLARE_SIMPLE_TYPE(RitterEcState, RITTEREC)

enum { ST_OBF = 0x01, ST_IBF = 0x02, ST_CMD = 0x08, ST_BURST = 0x10, ST_SCI_EVT = 0x20 };
#define QUERIES 8
enum { EC_READ = 0x80, EC_WRITE = 0x81, EC_BURST_ON = 0x82, EC_BURST_OFF = 0x83,
       EC_QUERY = 0x84 };
/* what the data port expects next */
enum { WANT_NOTHING, WANT_READ_ADDRESS, WANT_WRITE_ADDRESS, WANT_WRITE_DATA };

struct RitterEcState {
    ISADevice parent_obj;
    uint32_t data_port, command_port;
    MemoryRegion data_io, command_io;
    uint8_t ram[256];
    uint8_t status, output, address;
    int want;
    uint8_t sci_gpe, queries[QUERIES];
    int queued;
};

static void give(RitterEcState *s, uint8_t byte)
{
    s->output = byte;
    s->status |= ST_OBF;
}

static uint64_t data_read(void *opaque, hwaddr addr, unsigned size)
{
    RitterEcState *s = opaque;
    s->status &= ~ST_OBF;
    return s->output;
}

static void data_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    RitterEcState *s = opaque;
    uint8_t byte = val;
    s->status &= ~ST_CMD;
    switch (s->want) {
    case WANT_READ_ADDRESS:
        give(s, s->ram[byte]);
        s->want = WANT_NOTHING;
        break;
    case WANT_WRITE_ADDRESS:
        s->address = byte;
        s->want = WANT_WRITE_DATA;
        break;
    case WANT_WRITE_DATA:
        s->ram[s->address] = byte;
        s->want = WANT_NOTHING;
        break;
    default:
        break;                      /* a stray byte: ignored, as a controller does */
    }
}

static uint64_t command_read(void *opaque, hwaddr addr, unsigned size)
{
    RitterEcState *s = opaque;
    return s->status;
}

static void command_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    RitterEcState *s = opaque;
    s->status |= ST_CMD;
    s->want = WANT_NOTHING;
    switch ((uint8_t)val) {
    case EC_READ:
        s->want = WANT_READ_ADDRESS;
        break;
    case EC_WRITE:
        s->want = WANT_WRITE_ADDRESS;
        break;
    case EC_BURST_ON:
        s->status |= ST_BURST;
        give(s, 0x90);              /* the burst acknowledge byte */
        break;
    case EC_BURST_OFF:
        s->status &= ~ST_BURST;
        break;
    case EC_QUERY:
        give(s, s->queued ? s->queries[0] : 0);     /* (0: no event waiting) */
        if (s->queued) {
            memmove(s->queries, s->queries + 1, --s->queued);
            if (!s->queued)
                s->status &= ~ST_SCI_EVT;
        }
        break;
    }
}

static const MemoryRegionOps data_ops = {
    .read = data_read,
    .write = data_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps command_ops = {
    .read = command_read,
    .write = command_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void ritterec_reset(DeviceState *d)
{
    RitterEcState *s = RITTEREC(d);
    for (int i = 0; i < 256; i++)
        s->ram[i] = (uint8_t)(i * 7 + 3);
    s->status = 0;
    s->output = 0;
    s->want = WANT_NOTHING;
    s->queued = 0;
}

static void ritterec_realize(DeviceState *d, Error **errp)
{
    ISADevice *dev = ISA_DEVICE(d);
    RitterEcState *s = RITTEREC(d);
    memory_region_init_io(&s->data_io, OBJECT(dev), &data_ops, s, "ritterec-data", 1);
    memory_region_init_io(&s->command_io, OBJECT(dev), &command_ops, s, "ritterec-command", 1);
    memory_region_add_subregion(isa_address_space_io(dev), s->data_port, &s->data_io);
    memory_region_add_subregion(isa_address_space_io(dev), s->command_port, &s->command_io);
    ritterec_reset(d);
}

static const Property ritterec_properties[] = {
    DEFINE_PROP_UINT32("data", RitterEcState, data_port, 0x62),
    DEFINE_PROP_UINT32("command", RitterEcState, command_port, 0x66),
    DEFINE_PROP_UINT8("sci-gpe", RitterEcState, sci_gpe, 7),
};

static bool send_gpe(uint8_t mask, Error **errp)
{
    bool ambiguous = false;
    Object *acpi = object_resolve_path_type("", TYPE_ACPI_DEVICE_IF, &ambiguous);
    if (!acpi) {
        error_setg(errp, "ritterec: no ACPI device to raise GPEs on");
        return false;
    }
    acpi_send_event(DEVICE(acpi), (AcpiEventStatusBits)mask);
    return true;
}

static void raise_gpe(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t mask;
    if (visit_type_uint8(v, name, &mask, errp))
        send_gpe(mask, errp);
}

static void queue_query(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    RitterEcState *s = RITTEREC(obj);
    uint8_t number;
    if (!visit_type_uint8(v, name, &number, errp))
        return;
    if (s->queued == QUERIES) {
        error_setg(errp, "ritterec: %d events wait already", QUERIES);
        return;
    }
    s->queries[s->queued++] = number;
    s->status |= ST_SCI_EVT;
    if (s->sci_gpe < 8)
        send_gpe((uint8_t)(1u << s->sci_gpe), errp);
}

static void ritterec_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    object_class_property_add(klass, "gpe", "uint8", NULL, raise_gpe, NULL, NULL);
    object_class_property_add(klass, "query", "uint8", NULL, queue_query, NULL, NULL);
    dc->realize = ritterec_realize;
    device_class_set_legacy_reset(dc, ritterec_reset);
    device_class_set_props(dc, ritterec_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ritterec_info = {
    .name          = TYPE_RITTEREC,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(RitterEcState),
    .class_init    = ritterec_class_init,
};

static void ritterec_register_types(void)
{
    type_register_static(&ritterec_info);
}

type_init(ritterec_register_types)
