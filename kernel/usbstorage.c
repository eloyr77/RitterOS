#include "usbstorage.h"
#include "devname.h"
#include "devices.h"
#include "mounts.h"
#include "gpt.h"
#include "ritterfs.h"
#include "heap.h"
#include "kprintf.h"
#include "memory.h"
#include "pmm.h"
#include "string.h"
#include "sched.h"
#include "sync.h"
#include "timer.h"

#define MAX_DISKS           8
#define BUFFER_PAGES        16              /* 64 KiB per command */
#define MEDIUM_POLL_MS      2000            /* how often a disk without its medium is asked */
#define CBW_SIGNATURE       0x43425355      /* "USBC" */
#define CSW_SIGNATURE       0x53425355      /* "USBS" */
#define CBW_DATA_IN         0x80
#define CSW_PASSED          0
#define CSW_FAILED          1               /* the command failed: ask REQUEST SENSE */

/* Class requests to the interface. */
#define REQ_RESET           0xFF            /* Bulk-Only Mass Storage Reset */
#define REQ_GET_MAX_LUN     0xFE

/* SCSI commands (SPC, SBC). */
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2A
#define SCSI_SYNCHRONIZE_10     0x35
#define SCSI_READ_16            0x88
#define SCSI_WRITE_16           0x8A
#define SCSI_SERVICE_IN_16      0x9E        /* READ CAPACITY (16): service action 0x10 */

struct cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t length;                /* data bytes expected */
    uint8_t flags;                  /* bit 7: data from the device */
    uint8_t lun;
    uint8_t cb_length;
    uint8_t cb[16];
} __attribute__((packed));

struct csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;               /* data bytes not transferred */
    uint8_t status;
} __attribute__((packed));

_Static_assert(sizeof(struct cbw) == 31, "CBW size");
_Static_assert(sizeof(struct csw) == 13, "CSW size");

struct usb_disk {
    struct usb_device *dev;
    uint8_t interface, in, out, lun;
    uint32_t tag;
    uint8_t *buffer;                /* data of one command (DMA) */
    uint64_t buffer_phys;
    uint8_t *wrappers;              /* CBW at 0, CSW at 64 (DMA) */
    uint64_t wrappers_phys;
    struct mutex lock;              /* one command at a time */
    uint8_t sense_key, asc;         /* of the last failed command */
    uint64_t commands;
    int number;                     /* usbdisk<number>, reserved (devname.h) */
    volatile bool gone;             /* unplugged: every access fails at once */
    bool again;                     /* its name was this device's before */
    uint8_t max_lun;
    char vendor[9], product[17], revision[5];
    struct block_device bd;
};

static bool late;                   /* after the boot: new disks are checked here */

static struct usb_disk *disks[MAX_DISKS];
static int disk_count;
/* Disks whose medium is not there yet (a card reader without a card, or a
 * device still preparing it, as the PlutoSDR at its start): each has a
 * thread asking every MEDIUM_POLL_MS until it is ready or unplugged. */
static struct usb_disk *waiting[MAX_DISKS];
static struct mutex list_lock = MUTEX_INIT("usb disks");

static uint32_t big32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t big64(const uint8_t *p)
{
    return (uint64_t)big32(p) << 32 | big32(p + 4);
}

static void put_big32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* After a confused exchange: reset the interface and un-stall both
 * endpoints (BOT 5.3.4, "reset recovery"). */
static void reset_recovery(struct usb_disk *d)
{
    usb_control(d->dev, USB_TYPE_CLASS_INTERFACE, REQ_RESET, 0, d->interface, 0, 0);
    usb_bulk_clear_halt(d->dev, d->in);
    usb_bulk_clear_halt(d->dev, d->out);
}

/* The status wrapper; a stalled IN endpoint is cleared and asked once more. */
static bool read_csw(struct usb_disk *d, struct csw *out)
{
    bool stalled;
    for (int attempt = 0; attempt < 2; attempt++) {
        int n = usb_bulk_transfer(d->dev, d->in, d->wrappers_phys + 64, sizeof(struct csw),
                                  &stalled);
        if (n == (int)sizeof(struct csw)) {
            memcpy(out, d->wrappers + 64, sizeof(*out));
            return out->signature == CSW_SIGNATURE && out->tag == d->tag;
        }
        if (!stalled || !usb_bulk_clear_halt(d->dev, d->in))
            break;
    }
    return false;
}

/*
 * One SCSI command with up to length bytes of data in d->buffer (in: from
 * the device). Returns the CSW status (CSW_PASSED, CSW_FAILED) or -1 if the
 * exchange itself went wrong (then the device is reset).
 */
static int scsi(struct usb_disk *d, const uint8_t *cb, unsigned cb_length, bool in,
                uint32_t length)
{
    struct cbw cbw = {
        .signature = CBW_SIGNATURE, .tag = ++d->tag, .length = length,
        .flags = in ? CBW_DATA_IN : 0, .lun = d->lun, .cb_length = (uint8_t)cb_length,
    };
    memcpy(cbw.cb, cb, cb_length);
    memcpy(d->wrappers, &cbw, sizeof(cbw));
    d->commands++;
    bool stalled;
    if (usb_bulk_transfer(d->dev, d->out, d->wrappers_phys, sizeof(cbw), &stalled) !=
        (int)sizeof(cbw)) {
        reset_recovery(d);
        return -1;
    }
    /* The data phase; a stall here only ends it early (the status says
     * more), so the endpoint is cleared and the status read. */
    if (length > 0 && usb_bulk_transfer(d->dev, in ? d->in : d->out, d->buffer_phys, length,
                                        &stalled) < 0) {
        if (!stalled || !usb_bulk_clear_halt(d->dev, in ? d->in : d->out)) {
            reset_recovery(d);
            return -1;
        }
    }
    struct csw csw;
    if (!read_csw(d, &csw) || csw.status > CSW_FAILED) {
        reset_recovery(d);                  /* garbled, or a phase error */
        return -1;
    }
    return csw.status;
}

/* Why the last command failed: sense key and additional sense code. */
static void request_sense(struct usb_disk *d)
{
    uint8_t cb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    d->sense_key = d->asc = 0;
    if (scsi(d, cb, sizeof(cb), true, 18) == CSW_PASSED) {
        d->sense_key = d->buffer[2] & 0x0F;
        d->asc = d->buffer[12];
    }
}

/* Read or write count blocks at lba through the buffer (count fits it). */
static bool transfer(struct usb_disk *d, bool write, uint64_t lba, uint32_t count)
{
    uint8_t cb[16];
    unsigned cb_length;
    memset(cb, 0, sizeof(cb));
    if (lba + count <= 0xFFFFFFFFull) {
        cb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
        put_big32(cb + 2, (uint32_t)lba);
        cb[7] = (uint8_t)(count >> 8);
        cb[8] = (uint8_t)count;
        cb_length = 10;
    } else {
        cb[0] = write ? SCSI_WRITE_16 : SCSI_READ_16;   /* disks over 2 TiB */
        put_big32(cb + 2, (uint32_t)(lba >> 32));
        put_big32(cb + 6, (uint32_t)lba);
        put_big32(cb + 10, count);
        cb_length = 16;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        int status = scsi(d, cb, cb_length, !write, count * d->bd.block_size);
        if (status == CSW_PASSED)
            return true;
        if (status == CSW_FAILED)
            request_sense(d);
    }
    kprintf("  %s: %s of %u blocks at %lu failed (sense key %u, code 0x%02x)\n", d->bd.name,
            write ? "write" : "read", count, lba, d->sense_key, d->asc);
    return false;
}

static uint32_t blocks_per_command(const struct usb_disk *d)
{
    return BUFFER_PAGES * PMM_PAGE_SIZE / d->bd.block_size;
}

static bool disk_read(struct block_device *bd, uint64_t lba, uint32_t count, void *buffer)
{
    struct usb_disk *d = bd->driver_data;
    uint8_t *out = buffer;
    bool ok = !d->gone;
    mutex_lock(&d->lock);
    while (count > 0 && ok && !d->gone) {
        uint32_t n = count < blocks_per_command(d) ? count : blocks_per_command(d);
        ok = transfer(d, false, lba, n);
        if (ok)
            memcpy(out, d->buffer, (size_t)n * bd->block_size);
        out += (size_t)n * bd->block_size;
        lba += n;
        count -= n;
    }
    mutex_unlock(&d->lock);
    return ok;
}

static bool disk_write(struct block_device *bd, uint64_t lba, uint32_t count, const void *buffer)
{
    struct usb_disk *d = bd->driver_data;
    const uint8_t *in = buffer;
    bool ok = !d->gone;
    mutex_lock(&d->lock);
    while (count > 0 && ok && !d->gone) {
        uint32_t n = count < blocks_per_command(d) ? count : blocks_per_command(d);
        memcpy(d->buffer, in, (size_t)n * bd->block_size);
        ok = transfer(d, true, lba, n);
        in += (size_t)n * bd->block_size;
        lba += n;
        count -= n;
    }
    mutex_unlock(&d->lock);
    return ok;
}

static bool disk_flush(struct block_device *bd)
{
    struct usb_disk *d = bd->driver_data;
    uint8_t cb[10] = { SCSI_SYNCHRONIZE_10 };
    if (d->gone)
        return false;
    mutex_lock(&d->lock);
    int status = scsi(d, cb, sizeof(cb), false, 0);
    if (status == CSW_FAILED)
        request_sense(d);
    mutex_unlock(&d->lock);
    /* Some sticks have no cache to flush and say "illegal request" (5). */
    return status == CSW_PASSED || (status == CSW_FAILED && d->sense_key == 5);
}

/* A disk plugged in after the boot: its partitions, and the RitterFS on it
 * checked and mounted, as the boot does for every disk. */
static void check_new_disk(struct block_device *bd)
{
    bool gpt = gpt_report(bd);
    devices_add_disk(bd);
    struct block_device *p = gpt ? devices_find_partition(bd, 0, GPT_TYPE_RITTEROS) : 0;
    if (p)
        kprintf("  RitterOS partition: %s, %lu blocks\n", p->name, p->block_count);
    devices_mount_volumes(bd);
}

/* Copy n bytes of SCSI text (space-padded ASCII) and trim the spaces. */
static void text(char *out, const uint8_t *in, int n)
{
    int len = 0;
    for (int i = 0; i < n; i++)
        out[len++] = in[i] >= 0x20 && in[i] < 0x7F ? (char)in[i] : '?';
    while (len > 0 && out[len - 1] == ' ')
        len--;
    out[len] = 0;
}

static bool fail(const char *name, const char *why)
{
    kprintf("                 mass storage: %s%s%s\n", name ? name : "", name ? ": " : "", why);
    return false;
}

/* A ready disk: its size, then it becomes a block device (and, after the
 * boot, its partitions are read and its volumes mounted). */
static bool finish_attach(struct usb_disk *d)
{
    const char *name = d->bd.name;
    /* Its size: READ CAPACITY (10), or (16) for 2^32 blocks or more. */
    uint8_t capacity10[10] = { SCSI_READ_CAPACITY_10 };
    if (scsi(d, capacity10, sizeof(capacity10), true, 8) != CSW_PASSED)
        return fail(name, "READ CAPACITY failed");
    uint64_t last = big32(d->buffer);
    uint32_t block_size = big32(d->buffer + 4);
    if (last == 0xFFFFFFFF) {
        uint8_t capacity16[16] = { SCSI_SERVICE_IN_16, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32 };
        if (scsi(d, capacity16, sizeof(capacity16), true, 32) != CSW_PASSED)
            return fail(name, "READ CAPACITY (16) failed");
        last = big64(d->buffer);
        block_size = big32(d->buffer + 8);
    }
    if (block_size < 512 || block_size > PMM_PAGE_SIZE || PMM_PAGE_SIZE % block_size)
        return fail(name, "unsupported block size");

    /* A place in the list: a new one, or that of an unplugged disk. */
    mutex_lock(&list_lock);
    int place = disk_count < MAX_DISKS ? disk_count : MAX_DISKS;
    for (int i = 0; i < disk_count && place == MAX_DISKS; i++)
        if (disks[i]->gone)
            place = i;
    if (place == MAX_DISKS) {
        mutex_unlock(&list_lock);
        return fail(name, "too many disks");
    }
    d->bd.block_size = block_size;
    d->bd.kind = "USB";
    ksnprintf(d->bd.model, sizeof(d->bd.model), "%s %s", d->vendor, d->product);
    d->bd.block_count = last + 1;
    d->bd.read = disk_read;
    d->bd.write = disk_write;
    d->bd.flush = disk_flush;
    d->bd.driver_data = d;
    disks[place] = d;
    if (place == disk_count)
        disk_count++;
    mutex_unlock(&list_lock);
    char label[16];
    ksnprintf(label, sizeof(label), "%s:", name);
    kprintf("  %-15s'%s' '%s' rev %s on %s, %lu blocks of %u bytes (%lu MiB), "
            "bulk-only, LUN 0 of %u%s\n", label, d->vendor, d->product, d->revision, d->dev->name,
            d->bd.block_count, block_size, d->bd.block_count * block_size >> 20,
            d->max_lun + 1u, d->again ? ", back again" : "");
    if (late)
        check_new_disk(&d->bd);
    return true;
}

/* The thread of a disk waiting for its medium (see `waiting`). Once it is
 * ready, the disk is added after the boot's own disk work, so that it is
 * read and mounted like a disk plugged in later. */
static void wait_for_medium(void *context)
{
    struct usb_disk *d = context;
    uint8_t tur[6] = { SCSI_TEST_UNIT_READY };
    bool ready = false;
    while (!d->gone && !ready) {
        timer_sleep_ms(MEDIUM_POLL_MS);
        if (d->gone)
            break;
        mutex_lock(&d->lock);
        int status = scsi(d, tur, sizeof(tur), false, 0);
        if (status == CSW_FAILED)
            request_sense(d);
        mutex_unlock(&d->lock);
        ready = status == CSW_PASSED;
    }
    while (ready && !late && !d->gone)
        timer_sleep_ms(100);
    mutex_lock(&list_lock);
    for (int i = 0; i < MAX_DISKS; i++)
        if (waiting[i] == d)
            waiting[i] = 0;
    mutex_unlock(&list_lock);
    if (ready && !d->gone) {
        char label[16];
        ksnprintf(label, sizeof(label), "%s:", d->bd.name);
        kprintf("  %-15sits medium is ready\n", label);
        mutex_lock(&d->lock);
        bool ok = finish_attach(d);
        mutex_unlock(&d->lock);
        if (!ok)
            devname_detach("usbdisk", d->number);
    }
}

bool usbstorage_attach(struct usb_device *dev, const struct usb_interface_descriptor *interface,
                       const struct usb_endpoint_descriptor *in,
                       const struct usb_endpoint_descriptor *out, unsigned in_burst,
                       unsigned out_burst)
{
    /* A place in the list: a new one, or that of an unplugged disk. */
    int place = disk_count;
    for (int i = 0; i < disk_count && place == MAX_DISKS; i++)
        if (disks[i]->gone)
            place = i;
    if (place == MAX_DISKS)
        return fail(0, "too many disks");
    struct usb_disk *d = kzalloc(sizeof(*d));
    if (!d)
        return fail(0, "out of memory");
    d->dev = dev;
    d->interface = interface->number;
    d->in = in->address;
    d->out = out->address;
    d->lock = (struct mutex)MUTEX_INIT("usb disk");
    d->buffer_phys = pmm_alloc_pages(BUFFER_PAGES);
    d->wrappers_phys = pmm_alloc_page();
    if (!d->buffer_phys || !d->wrappers_phys)
        return fail(0, "out of memory");
    d->buffer = phys_to_virt(d->buffer_phys);
    d->wrappers = phys_to_virt(d->wrappers_phys);
    const struct devname_id id = { dev->descriptor.vendor_id, dev->descriptor.product_id,
                                   dev->serial, dev->name };
    bool again;
    d->number = devname_attach("usbdisk", &id, &again);
    if (d->number < 0)
        return fail(0, "too many disks");
    ksnprintf(d->bd.name, sizeof(d->bd.name), "usbdisk%d", d->number);
    const char *name = d->bd.name;
    if (!usb_bulk_configure(dev, in, out, in_burst, out_burst)) {
        devname_detach("usbdisk", d->number);
        return fail(name, "bulk endpoints cannot be set up");
    }

    /* Several logical units (LUNs) are card readers' slots; we use LUN 0.
     * Devices with only one may answer this request with a stall. */
    uint8_t max_lun = 0;
    usb_control(dev, USB_DIR_IN | USB_TYPE_CLASS_INTERFACE, REQ_GET_MAX_LUN, 0, d->interface,
                &max_lun, 1);

    /* What it is: INQUIRY. Peripheral type 0 is a "direct access" disk. */
    uint8_t inquiry[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    if (scsi(d, inquiry, sizeof(inquiry), true, 36) != CSW_PASSED)
        return fail(name, "INQUIRY failed");
    if ((d->buffer[0] & 0x1F) != 0)
        return fail(name, "not a disk (SCSI peripheral type is not 0)");
    char vendor[9], product[17], revision[5];
    text(vendor, d->buffer + 8, 8);
    text(product, d->buffer + 16, 16);
    text(revision, d->buffer + 32, 4);

    d->max_lun = max_lun;
    d->again = again;
    memcpy(d->vendor, vendor, sizeof(vendor));
    memcpy(d->product, product, sizeof(product));
    memcpy(d->revision, revision, sizeof(revision));

    /* Ready? A device usually reports "unit attention" (it was reset) at
     * first, then becomes ready. */
    uint8_t tur[6] = { SCSI_TEST_UNIT_READY };
    bool ready = false;
    for (int attempt = 0; attempt < 10 && !ready; attempt++) {
        int status = scsi(d, tur, sizeof(tur), false, 0);
        ready = status == CSW_PASSED;
        if (status == CSW_FAILED) {
            request_sense(d);
            timer_sleep_ms(100);
        }
    }
    if (!ready) {
        /* Not ready (sense key 2) or still reporting a change (6): its
         * medium may come later. Anything else is a failure. */
        if (d->sense_key != 2 && d->sense_key != 6) {
            devname_detach("usbdisk", d->number);
            return fail(name, "not ready");
        }
        kprintf("                 mass storage: %s: no medium yet (sense key %u, code 0x%02x); "
                "waiting for it\n", name, d->sense_key, d->asc);
        mutex_lock(&list_lock);
        int w = 0;
        while (w < MAX_DISKS && waiting[w])
            w++;
        if (w < MAX_DISKS)
            waiting[w] = d;
        mutex_unlock(&list_lock);
        if (w == MAX_DISKS || !thread_create(name, wait_for_medium, d)) {
            mutex_lock(&list_lock);
            if (w < MAX_DISKS)
                waiting[w] = 0;
            mutex_unlock(&list_lock);
            devname_detach("usbdisk", d->number);
            return fail(name, "cannot wait for its medium");
        }
        return true;
    }
    return finish_attach(d);
}

void usbstorage_detach(struct usb_device *dev)
{
    mutex_lock(&list_lock);
    for (int i = 0; i < MAX_DISKS; i++) {
        struct usb_disk *d = waiting[i];
        if (!d || d->dev != dev || d->gone)
            continue;
        d->gone = true;                 /* its thread ends at its next look */
        waiting[i] = 0;
        devname_detach("usbdisk", d->number);
        char label[16];
        ksnprintf(label, sizeof(label), "%s:", d->bd.name);
        kprintf("  %-15sremoved (it had no medium)\n", label);
    }
    mutex_unlock(&list_lock);
    for (int i = 0; i < disk_count; i++) {
        struct usb_disk *d = disks[i];
        if (d->dev != dev || d->gone)
            continue;
        d->gone = true;
        mounts_disk_gone(&d->bd);
        devices_remove_disk(&d->bd);
        devname_detach("usbdisk", d->number);
        char label[16];
        ksnprintf(label, sizeof(label), "%s:", d->bd.name);
        kprintf("  %-15sremoved (its name stays reserved)%s\n", label,
                d->bd.system ? "; it held the system partition: files cannot be read "
                               "any more" : "");
    }
}

void usbstorage_start_hotplug(void)
{
    late = true;
}

int usbstorage_count(void)
{
    return disk_count;
}

struct block_device *usbstorage_disk(int index)
{
    return index >= 0 && index < disk_count ? &disks[index]->bd : 0;
}
