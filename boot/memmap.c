#include "memmap.h"
#include "console.h"

EFI_STATUS memmap_get(EFI_BOOT_SERVICES *bs, MEMORY_MAP *map)
{
    for (;;) {
        map->size = map->capacity;
        EFI_STATUS status = bs->GetMemoryMap(&map->size, map->descriptors, &map->key,
                                             &map->desc_size, &map->desc_version);
        if (status != EFI_BUFFER_TOO_SMALL)
            return status;

        /* map->size now holds the required size. Allocating the buffer can
         * itself split a free region, and more allocations may follow before
         * ExitBootServices, so leave room for extra entries. */
        if (map->descriptors)
            bs->FreePool(map->descriptors);
        map->descriptors = 0;
        map->capacity = map->size + 32 * map->desc_size;
        status = bs->AllocatePool(EfiLoaderData, map->capacity, (void **)&map->descriptors);
        if (EFI_ERROR(status)) {
            map->capacity = 0;
            return status;
        }
    }
}

/* Sort descriptors by address in place; UEFI does not guarantee any order. */
void memmap_sort(MEMORY_MAP *map)
{
    UINTN count = MEMMAP_COUNT(map);
    for (UINTN i = 1; i < count; i++) {
        for (UINTN j = i; j > 0; j--) {
            UINT8 *a = (UINT8 *)MEMMAP_ENTRY(map, j - 1);
            UINT8 *b = (UINT8 *)MEMMAP_ENTRY(map, j);
            if (((EFI_MEMORY_DESCRIPTOR *)a)->PhysicalStart <=
                ((EFI_MEMORY_DESCRIPTOR *)b)->PhysicalStart)
                break;
            for (UINTN k = 0; k < map->desc_size; k++) {
                UINT8 t = a[k];
                a[k] = b[k];
                b[k] = t;
            }
        }
    }
}

/* Translate a UEFI memory type into the kernel's simplified type. */
static uint32_t kernel_type(UINT32 type)
{
    switch (type) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:
        return MEMORY_USABLE;
    case EfiLoaderCode:
    case EfiLoaderData:
        return MEMORY_LOADER;
    case EfiACPIReclaimMemory:
        return MEMORY_ACPI_RECLAIMABLE;
    case EfiACPIMemoryNVS:
        return MEMORY_ACPI_NVS;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace:
        return MEMORY_MMIO;
    case EfiRuntimeServicesCode:
        return MEMORY_UEFI_RUNTIME_CODE;
    case EfiRuntimeServicesData:
        return MEMORY_UEFI_RUNTIME_DATA;
    default:
        return MEMORY_RESERVED;
    }
}

void memmap_print_summary(const MEMORY_MAP *map)
{
    UINTN count = MEMMAP_COUNT(map);
    UINT64 free_now = 0, usable = 0;
    for (UINTN i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *d = MEMMAP_ENTRY(map, i);
        UINT64 bytes = d->NumberOfPages * EFI_PAGE_SIZE;
        if (d->Type == EfiConventionalMemory)
            free_now += bytes;
        uint32_t t = kernel_type(d->Type);
        if (t == MEMORY_USABLE || t == MEMORY_LOADER)
            usable += bytes;
    }
    print_dec(count, 0);
    print(u" descriptors, ");
    print_size(free_now, 0);
    print(u" free now, ");
    print_size(usable, 0);
    print(u" usable after boot\r\n");
}

UINTN memmap_to_regions(const MEMORY_MAP *map, struct memory_region *out, UINTN capacity)
{
    UINTN n = 0;
    for (UINTN i = 0; i < MEMMAP_COUNT(map); i++) {
        EFI_MEMORY_DESCRIPTOR *d = MEMMAP_ENTRY(map, i);
        uint32_t type = kernel_type(d->Type);
        UINT64 length = d->NumberOfPages * EFI_PAGE_SIZE;

        if (n > 0 && out[n - 1].type == type &&
            out[n - 1].base + out[n - 1].length == d->PhysicalStart) {
            out[n - 1].length += length;
            continue;
        }
        if (n == capacity)
            break;
        out[n].base = d->PhysicalStart;
        out[n].length = length;
        out[n].type = type;
        out[n].reserved = 0;
        n++;
    }
    return n;
}

UINT64 memmap_ram_top(const MEMORY_MAP *map)
{
    UINT64 top = 4ULL << 30;
    for (UINTN i = 0; i < MEMMAP_COUNT(map); i++) {
        EFI_MEMORY_DESCRIPTOR *d = MEMMAP_ENTRY(map, i);
        uint32_t t = kernel_type(d->Type);
        UINT64 end = d->PhysicalStart + d->NumberOfPages * EFI_PAGE_SIZE;
        if (t != MEMORY_RESERVED && t != MEMORY_MMIO && end > top)
            top = end;
    }
    return top;
}
