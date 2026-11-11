/* Collecting system information from the firmware for the kernel. */
#ifndef RITTEROS_SYSINFO_H
#define RITTEROS_SYSINFO_H

#include "efi.h"

/* Address of the configuration table with this GUID, or 0. */
void *find_config_table(EFI_SYSTEM_TABLE *st, const EFI_GUID *guid);

/* Fill buf with random bytes from EFI_RNG_PROTOCOL. Returns the number of
 * bytes obtained: len, or 0 if the firmware has no random number generator. */
UINTN get_random_bytes(EFI_BOOT_SERVICES *bs, UINT8 *buf, UINTN len);

/* Copy the device path of the partition this image was loaded from into
 * out. Returns its size in bytes, or 0 if unknown or larger than capacity. */
UINTN copy_boot_device_path(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, UINT8 *out,
                            UINTN capacity);

#endif
