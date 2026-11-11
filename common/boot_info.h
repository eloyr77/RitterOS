/*
 * The hand-over from bootloader to kernel. The bootloader fills this in,
 * exits UEFI boot services, switches to its page tables (see vmlayout.h) and
 * calls the kernel entry point with a pointer to it (System V ABI: first
 * argument in RDI). That pointer is a direct-map virtual address.
 *
 * All addresses *inside* the structure are physical, except
 * uefi_runtime_services (a direct-map address after SetVirtualAddressMap).
 * The kernel converts them with HHDM_BASE + physical.
 */
#ifndef RITTEROS_BOOT_INFO_H
#define RITTEROS_BOOT_INFO_H

#include <stdint.h>
#include "framebuffer.h"

#define BOOT_INFO_MAGIC 0x4F4F42534F525452ULL   /* "RTROSBOO" */
#define BOOT_INFO_VERSION 6                       /* bump when the layout changes */
#define BOOT_CMDLINE_MAX 256
#define BOOT_KERNEL_PATH_MAX 64
#define BOOT_DEVICE_PATH_MAX 256
#define BOOT_RANDOM_SEED_SIZE 32
#define BOOT_EDID_MAX 512                         /* (65e-34) the base block and 3 extensions */
#define BOOT_MODES_MAX 32

/* Simplified memory types; the bootloader translates the UEFI types. */
enum memory_type {
    MEMORY_USABLE = 1,       /* free RAM (UEFI conventional + boot services) */
    MEMORY_LOADER,           /* kernel image, boot info, bootloader: in use */
    MEMORY_ACPI_RECLAIMABLE, /* ACPI tables: usable once they have been read */
    MEMORY_ACPI_NVS,         /* firmware storage: never touch */
    MEMORY_RESERVED,         /* reserved, faulty RAM, unknown */
    MEMORY_MMIO,             /* device registers, not RAM */
    MEMORY_UEFI_RUNTIME_CODE,/* firmware code for runtime services: never touch */
    MEMORY_UEFI_RUNTIME_DATA,/* firmware data for runtime services: never touch */
};

struct memory_region {
    uint64_t base;
    uint64_t length;
    uint32_t type;           /* enum memory_type */
    uint32_t reserved;
};

struct boot_info {
    uint64_t magic;                      /* BOOT_INFO_MAGIC */
    uint32_t version;                    /* BOOT_INFO_VERSION */
    uint32_t size;                       /* sizeof(struct boot_info) */
    struct framebuffer framebuffer;
    struct memory_region *memory_map;    /* sorted by base, adjacent regions merged */
    uint64_t memory_map_count;
    uint64_t kernel_physical_base;       /* where the kernel image was loaded */
    uint64_t kernel_virtual_base;        /* where it is mapped (its link address) */
    uint64_t kernel_size;                /* bytes, page-aligned */
    uint64_t hhdm_offset;                /* HHDM_BASE: virtual = physical + this */
    char cmdline[BOOT_CMDLINE_MAX];      /* from \ritteros\cmdline.txt, "" if none */

    /* The kernel file: its path on the RitterOS partition (RitterFS), its
     * size and the file system generation it was read from. */
    char kernel_path[BOOT_KERNEL_PATH_MAX];
    uint64_t kernel_file_size;
    uint64_t kernel_fs_generation;
    /* The unique GPT GUID of that partition (as EFI_GUID stores it): the
     * kernel uses the disk that has it, whatever kind of disk. All zero if
     * unknown. */
    uint8_t boot_partition_guid[16];

    /* Firmware tables, physical addresses; 0 if the firmware has none. */
    uint64_t acpi_rsdp;                  /* ACPI Root System Description Pointer */
    uint64_t smbios_entry;               /* SMBIOS 3 ("_SM3_") or 2 ("_SM_") entry point */
    uint64_t uefi_system_table;          /* physical; stays valid after ExitBootServices */
    uint64_t uefi_runtime_services;      /* EFI_RUNTIME_SERVICES, virtual (direct map);
                                            0 if SetVirtualAddressMap failed */

    /* UEFI device path of the partition the bootloader was loaded from. */
    uint32_t boot_device_path_size;      /* bytes, 0 if unknown */
    uint8_t boot_device_path[BOOT_DEVICE_PATH_MAX];

    /* Random bytes from the firmware's EFI_RNG_PROTOCOL. */
    uint32_t random_seed_size;           /* 0 if the firmware has no RNG */
    uint8_t random_seed[BOOT_RANDOM_SEED_SIZE];

    /* (version 6, 65e-34; a version 5 bootloader's info ends before
     * these: the kernel takes both) The screen's EDID as the firmware
     * gave it (its base block and extension blocks), and the modes the
     * GOP can set. */
    uint32_t edid_size;                  /* bytes, 0 if the firmware gave none */
    uint8_t edid[BOOT_EDID_MAX];
    uint32_t mode_count;
    struct boot_mode { uint32_t width, height; } modes[BOOT_MODES_MAX];
};

/* (65e-34) The size of a version 5 boot info: everything before edid_size. */
#define BOOT_INFO_V5_SIZE ((__builtin_offsetof(struct boot_info, edid_size) + 7) & ~(size_t)7)

/* The kernel entry point: never returns. On x86-64 the bootloader (built with
 * the Microsoft convention) must call it with the System V convention. */
#if defined(__x86_64__)
#define KERNEL_ABI __attribute__((sysv_abi))
#else
#define KERNEL_ABI
#endif
typedef void (KERNEL_ABI __attribute__((noreturn)) *kernel_entry_fn)(struct boot_info *info);

#endif
