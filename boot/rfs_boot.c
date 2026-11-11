#include "rfs_boot.h"
#include "crc32.h"
#include "string.h"

/* GPT partition type of RitterOS data (scripts/mkdisk.py, kernel/gpt.h). */
#define RITTEROS_PARTITION_TYPE \
    { 0xb5ca1b3e, 0x6278, 0x48f0, { 0xbb, 0xef, 0x87, 0x90, 0x73, 0x26, 0xa8, 0xb3 } }

static EFI_BOOT_SERVICES *boot_services;

/* The shared reader's buffers come from the firmware's pool. */
void *rfs_alloc_block(void)
{
    void *block;
    if (EFI_ERROR(boot_services->AllocatePool(EfiLoaderData, RFS_BLOCK_SIZE, &block)))
        return 0;
    return block;
}

void rfs_free_block(void *block)
{
    boot_services->FreePool(block);
}

static bool volume_read(struct rfs_volume *vol, uint64_t block, void *buf)
{
    BOOT_FS *fs = (BOOT_FS *)vol;
    return !EFI_ERROR(fs->disk->ReadDisk(fs->disk, fs->media_id, block * RFS_BLOCK_SIZE,
                                         RFS_BLOCK_SIZE, buf));
}

static UINTN node_length(const EFI_DEVICE_PATH_PROTOCOL *node)
{
    return node->Length[0] | (node->Length[1] << 8);
}

/* Bytes of a device path before its last node: the path of the disk when
 * the last node is a partition. *last is set to that node. */
static UINTN parent_length(const UINT8 *path, const EFI_DEVICE_PATH_PROTOCOL **last)
{
    UINTN size = 0, previous = 0;
    *last = 0;
    for (;;) {
        const EFI_DEVICE_PATH_PROTOCOL *node = (const EFI_DEVICE_PATH_PROTOCOL *)(path + size);
        UINTN length = node_length(node);
        if (node->Type == DEVICE_PATH_END_TYPE || length < sizeof(*node) || size > 4096)
            return previous;
        *last = node;
        previous = size;
        size += length;
    }
}

static EFI_STATUS failed(const CHAR16 **why, const CHAR16 *reason, EFI_STATUS status)
{
    *why = reason;
    return status;
}

/* A GPT header (UEFI 2.3, 5.3.2) and partition entry (5.3.3). */
struct gpt_header {
    char signature[8];              /* "EFI PART" */
    UINT32 revision;
    UINT32 header_size;
    UINT32 header_crc;              /* CRC32 of the header with this field 0 */
    UINT32 reserved;
    UINT64 current_lba, backup_lba, first_usable_lba, last_usable_lba;
    UINT8 disk_guid[16];
    UINT64 entries_lba;
    UINT32 entry_count, entry_size;
    UINT32 entries_crc;
} __attribute__((packed));

struct gpt_entry {
    EFI_GUID type;
    EFI_GUID unique;
    UINT64 first_lba, last_lba, attributes;
    UINT16 name[36];
} __attribute__((packed));

#define GPT_ENTRIES_MAX (128 * 1024)    /* bytes of entries we read */

/* The handle whose device path is exactly path[0..length) (and its end). */
static EFI_HANDLE handle_with_path(EFI_BOOT_SERVICES *bs, EFI_HANDLE *handles, UINTN count,
                                   const UINT8 *path, UINTN length)
{
    static EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    for (UINTN i = 0; i < count; i++) {
        const UINT8 *p;
        const EFI_DEVICE_PATH_PROTOCOL *end;
        if (EFI_ERROR(bs->HandleProtocol(handles[i], &device_path_guid, (void **)&p)))
            continue;
        end = (const EFI_DEVICE_PATH_PROTOCOL *)(p + length);
        if (memcmp(p, path, length) == 0 && end->Type == DEVICE_PATH_END_TYPE)
            return handles[i];
    }
    return 0;
}

/*
 * The RitterOS partition's entry in the boot disk's GPT, read through the
 * whole disk's Disk I/O (every UEFI version has that; the Partition Info
 * protocol only exists since UEFI 2.7). Header and entries are checked
 * against their CRC32s. Returns its number (1 = first) or 0.
 */
static UINT32 find_gpt_entry(EFI_BOOT_SERVICES *bs, EFI_HANDLE disk, struct gpt_entry *out,
                             const CHAR16 **why)
{
    static EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    static EFI_GUID disk_io_guid = EFI_DISK_IO_PROTOCOL_GUID;
    static const EFI_GUID ritteros_type = RITTEROS_PARTITION_TYPE;
    EFI_BLOCK_IO_PROTOCOL *block_io;
    EFI_DISK_IO_PROTOCOL *disk_io;
    if (EFI_ERROR(bs->HandleProtocol(disk, &block_io_guid, (void **)&block_io)) ||
        EFI_ERROR(bs->HandleProtocol(disk, &disk_io_guid, (void **)&disk_io))) {
        *why = u"no disk access to the boot disk";
        return 0;
    }
    UINT32 media = block_io->Media->MediaId, block = block_io->Media->BlockSize;
    struct gpt_header h;
    if (EFI_ERROR(disk_io->ReadDisk(disk_io, media, block, sizeof(h), &h)) ||
        memcmp(h.signature, "EFI PART", 8) != 0 || h.header_size < sizeof(h) ||
        h.header_size > sizeof(h)) {
        *why = u"the boot disk has no GPT";
        return 0;
    }
    UINT32 crc = h.header_crc;
    h.header_crc = 0;
    if (crc32(&h, h.header_size) != crc) {
        *why = u"the boot disk's GPT header is damaged (CRC)";
        return 0;
    }
    UINT64 bytes = (UINT64)h.entry_count * h.entry_size;
    UINT8 *entries;
    if (h.entry_size < sizeof(struct gpt_entry) || bytes > GPT_ENTRIES_MAX ||
        EFI_ERROR(bs->AllocatePool(EfiLoaderData, bytes ? bytes : 1, (void **)&entries))) {
        *why = u"the boot disk's GPT has unusable entries";
        return 0;
    }
    UINT32 number = 0;
    if (EFI_ERROR(disk_io->ReadDisk(disk_io, media, h.entries_lba * block, bytes, entries)) ||
        crc32(entries, bytes) != h.entries_crc) {
        *why = u"the boot disk's GPT entries are damaged (CRC)";
    } else {
        for (UINT32 i = 0; i < h.entry_count && !number; i++) {
            const struct gpt_entry *e = (const struct gpt_entry *)(entries + (UINT64)i * h.entry_size);
            if (memcmp(&e->type, &ritteros_type, sizeof(EFI_GUID)) == 0) {
                *out = *e;
                number = i + 1;
            }
        }
        if (!number)
            *why = u"no RitterOS partition on the boot disk";
    }
    bs->FreePool(entries);
    return number;
}

EFI_STATUS rfs_boot_open(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, BOOT_FS *fs,
                         const CHAR16 **why)
{
    static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    static EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    static EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    static EFI_GUID disk_io_guid = EFI_DISK_IO_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    const UINT8 *boot_path;
    const EFI_DEVICE_PATH_PROTOCOL *last;
    EFI_STATUS status;

    boot_services = bs;
    memset(fs, 0, sizeof(*fs));

    /* The boot disk: the device path of our own partition without its last
     * node, e.g. PciRoot(0x0)/Pci(0x1,0x0)/NVMe(0x1,...) without HD(1,GPT,...). */
    status = bs->HandleProtocol(image, &loaded_image_guid, (void **)&loaded_image);
    if (!EFI_ERROR(status))
        status = bs->HandleProtocol(loaded_image->DeviceHandle, &device_path_guid,
                                    (void **)&boot_path);
    if (EFI_ERROR(status))
        return failed(why, u"no device path for the boot partition", status);
    UINTN disk_length = parent_length(boot_path, &last);

    /* Every disk and every partition the firmware knows has a handle with
     * Disk I/O: the whole boot disk is the one at disk_length. */
    EFI_HANDLE *handles;
    UINTN count;
    status = bs->LocateHandleBuffer(ByProtocol, &disk_io_guid, 0, &count, &handles);
    if (EFI_ERROR(status))
        return failed(why, u"the firmware lists no disks", status);
    EFI_HANDLE disk = handle_with_path(bs, handles, count, boot_path, disk_length);
    struct gpt_entry entry;
    UINT32 number = disk ? find_gpt_entry(bs, disk, &entry, why) : 0;
    if (!disk)
        *why = u"the boot disk itself was not found";

    /* The partition's handle: its device path is the disk's plus a hard
     * drive node with that number and the entry's unique GUID. */
    EFI_HANDLE found = 0;
    for (UINTN i = 0; i < count && number && !found; i++) {
        const UINT8 *path;
        if (EFI_ERROR(bs->HandleProtocol(handles[i], &device_path_guid, (void **)&path)) ||
            parent_length(path, &last) != disk_length || memcmp(path, boot_path, disk_length) ||
            !last || last->Type != DEVICE_PATH_MEDIA_TYPE ||
            last->SubType != DEVICE_PATH_HARD_DRIVE_SUBTYPE)
            continue;
        UINT32 n;
        memcpy(&n, (const UINT8 *)last + 4, sizeof(n));
        if (n == number && memcmp((const UINT8 *)last + 24, &entry.unique, 16) == 0)
            found = handles[i];
    }
    bs->FreePool(handles);
    if (!number)
        return failed(why, *why, EFI_NOT_FOUND);
    if (!found)
        return failed(why, u"the firmware has no handle for the RitterOS partition",
                      EFI_NOT_FOUND);
    fs->partition = number;
    fs->partition_guid = entry.unique;

    EFI_BLOCK_IO_PROTOCOL *block_io;
    status = bs->HandleProtocol(found, &block_io_guid, (void **)&block_io);
    if (!EFI_ERROR(status))
        status = bs->HandleProtocol(found, &disk_io_guid, (void **)&fs->disk);
    if (EFI_ERROR(status))
        return failed(why, u"no disk access to the RitterOS partition", status);
    fs->media_id = block_io->Media->MediaId;
    fs->vol.read = volume_read;
    fs->vol.capacity = (block_io->Media->LastBlock + 1) * block_io->Media->BlockSize /
                       RFS_BLOCK_SIZE;
    if (!rfs_open(&fs->vol))
        return failed(why, u"no valid RitterFS superblock", EFI_VOLUME_CORRUPTED);
    return EFI_SUCCESS;
}

EFI_STATUS rfs_boot_read_file(BOOT_FS *fs, const char *path, UINT8 **data, UINTN *size,
                              const CHAR16 **why)
{
    uint64_t id;
    struct rfs_inode inode;
    if (!rfs_lookup(&fs->vol, path, &id, &inode))
        return failed(why, u"file not found", EFI_NOT_FOUND);
    if (inode.type != RFS_TYPE_FILE)
        return failed(why, u"not a file", EFI_NOT_FOUND);
    if (EFI_ERROR(boot_services->AllocatePool(EfiLoaderData, inode.size ? inode.size : 1,
                                              (void **)data)))
        return failed(why, u"out of memory for the file", EFI_OUT_OF_RESOURCES);
    if (rfs_read(&fs->vol, id, &inode, 0, *data, inode.size) != (int64_t)inode.size) {
        boot_services->FreePool(*data);
        return failed(why, u"a block of the file is damaged (checksum) or unreadable",
                      EFI_CRC_ERROR);
    }
    *size = inode.size;
    return EFI_SUCCESS;
}
