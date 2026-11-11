#include "sysinfo.h"
#include "string.h"

void *find_config_table(EFI_SYSTEM_TABLE *st, const EFI_GUID *guid)
{
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &st->ConfigurationTable[i];
        if (memcmp(&t->VendorGuid, guid, sizeof(EFI_GUID)) == 0)
            return t->VendorTable;
    }
    return 0;
}

UINTN get_random_bytes(EFI_BOOT_SERVICES *bs, UINT8 *buf, UINTN len)
{
    static EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
    EFI_RNG_PROTOCOL *rng;
    if (EFI_ERROR(bs->LocateProtocol(&rng_guid, 0, (void **)&rng)))
        return 0;
    if (EFI_ERROR(rng->GetRNG(rng, 0, len, buf)))
        return 0;
    return len;
}

UINTN copy_boot_device_path(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, UINT8 *out,
                            UINTN capacity)
{
    static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    static EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    UINT8 *path;

    if (EFI_ERROR(bs->HandleProtocol(image, &loaded_image_guid, (void **)&loaded_image)))
        return 0;
    if (EFI_ERROR(bs->HandleProtocol(loaded_image->DeviceHandle, &device_path_guid,
                                     (void **)&path)))
        return 0;

    /* Walk the nodes up to and including the end node to learn the size. */
    UINTN size = 0;
    for (;;) {
        EFI_DEVICE_PATH_PROTOCOL *node = (EFI_DEVICE_PATH_PROTOCOL *)(path + size);
        UINTN length = node->Length[0] | (node->Length[1] << 8);
        if (length < sizeof(*node) || size + length > capacity)
            return 0;
        size += length;
        if (node->Type == DEVICE_PATH_END_TYPE && node->SubType == DEVICE_PATH_END_SUBTYPE)
            break;
    }
    memcpy(out, path, size);
    return size;
}
