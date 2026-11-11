/*
 * The RitterOS partition of the boot disk, read with the shared RitterFS
 * reader (common/rfs_read.h) through the firmware's Disk I/O protocol.
 */
#ifndef RITTEROS_RFS_BOOT_H
#define RITTEROS_RFS_BOOT_H

#include "efi.h"
#include "rfs_read.h"

typedef struct {
    struct rfs_volume vol;         /* first member: the read callback relies on it */
    EFI_DISK_IO_PROTOCOL *disk;
    UINT32 media_id;
    UINT32 partition;              /* partition number on the disk (1 = first) */
    EFI_GUID partition_guid;       /* its unique GPT GUID */
} BOOT_FS;

/*
 * Find the partition with the RitterOS type GUID on the disk this bootloader
 * was loaded from (by reading that disk's GPT; works on any UEFI version)
 * and open its file system. On failure, *why says what went wrong; nothing
 * is printed.
 */
EFI_STATUS rfs_boot_open(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, BOOT_FS *fs,
                         const CHAR16 **why);

/* Read a whole file into a pool allocation (free it with FreePool). Every
 * block's checksum is verified. On failure, *why says what went wrong. */
EFI_STATUS rfs_boot_read_file(BOOT_FS *fs, const char *path, UINT8 **data, UINTN *size,
                              const CHAR16 **why);

#endif
