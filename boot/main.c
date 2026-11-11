/*
 * RitterOS UEFI bootloader.
 *
 * 1. Read the command line and report what the firmware provides.
 * 2. Collect firmware information for the kernel: ACPI, SMBIOS, the boot
 *    device, runtime services, random seed.
 * 3. Find the RitterOS partition on the boot disk and load
 *    /system/<arch>/kernel.elf from its RitterFS into free memory.
 * 4. Build the kernel's first page tables (common/vmlayout.h).
 * 5. Switch the display to the chosen resolution.
 * 6. Take the final memory map and exit UEFI boot services; give the
 *    runtime services their new virtual addresses (SetVirtualAddressMap).
 * 7. Load the page tables and jump to the kernel's entry point with a
 *    struct boot_info.
 *
 * Command line options used here: video=<width>x<height> (default: keep the
 * firmware's mode, normally the display's native resolution).
 */
#include "efi.h"
#include "console.h"
#include "memmap.h"
#include "gop.h"
#include "loader.h"
#include "rfs_boot.h"
#include "sysinfo.h"
#include "boot_arch.h"
#include "vmlayout.h"
#include "boot_info.h"
#include "cmdline.h"
#include "serial.h"
#include "string.h"

#define CMDLINE_PATH u"\\ritteros\\cmdline.txt"

static void halt_with_error(EFI_SYSTEM_TABLE *st, const CHAR16 *what, EFI_STATUS status)
{
    set_color(EFI_YELLOW | EFI_BACKGROUND_BLACK);
    print(u"\r\nERROR: ");
    print(what);
    print(u" failed, status ");
    print_hex(status, 16);
    print(u"\r\n");
    for (;;)
        st->BootServices->Stall(1000000);
}

/*
 * Copy the optional command line file into out: printable ASCII only, line
 * breaks become spaces, surrounding spaces are trimmed. No file: "".
 */
static void read_cmdline(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, char *out)
{
    UINT8 *data;
    UINTN size, n = 0;
    const CHAR16 *why;
    out[0] = 0;
    if (EFI_ERROR(read_file(bs, image, CMDLINE_PATH, &data, &size, &why)))
        return;
    for (UINTN i = 0; i < size && n < BOOT_CMDLINE_MAX - 1; i++) {
        UINT8 c = data[i];
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
        if (c < ' ' || c > '~' || (c == ' ' && (n == 0 || out[n - 1] == ' ')))
            continue;
        out[n++] = (char)c;
    }
    if (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
    bs->FreePool(data);
}

/* Parse "<width>x<height>"; returns 0 if s has another form. */
static int parse_resolution(const char *s, UINT32 *width, UINT32 *height)
{
    UINT32 v[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        if (*s < '0' || *s > '9')
            return 0;
        while (*s >= '0' && *s <= '9')
            v[i] = v[i] * 10 + (UINT32)(*s++ - '0');
        if (i == 0 && *s++ != 'x')
            return 0;
    }
    if (*s)
        return 0;
    *width = v[0];
    *height = v[1];
    return 1;
}

static void label(const CHAR16 *s)
{
    set_color(EFI_WHITE | EFI_BACKGROUND_BLACK);
    print(s);
    set_color(EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
}

static void print_resolution(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 mode)
{
    UINTN size;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
    if (EFI_ERROR(gop->QueryMode(gop, mode, &size, &mi)))
        return;
    print_dec(mi->HorizontalResolution, 0);
    print(u"x");
    print_dec(mi->VerticalResolution, 0);
}

/* Pick the display mode: video=WxH from the command line, else the firmware's
 * current mode (on real hardware normally the display's native resolution). */
static UINT32 choose_mode(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                          const char *cmdline)
{
    UINT32 mode = gop->Mode->Mode;
    char value[24];
    UINT32 w, h;

    label(u"Graphics:  ");
    print_dec(gop->Mode->MaxMode, 0);
    print(u" modes, firmware mode ");
    print_resolution(gop, mode);
    if (cmdline_get(cmdline, "video", value, sizeof(value))) {
        if (parse_resolution(value, &w, &h) && !EFI_ERROR(gop_find_mode(bs, gop, w, h, &mode))) {
            print(u", using ");
            print_resolution(gop, mode);
            print(u" (video=)\r\n");
            return mode;
        }
        set_color(EFI_YELLOW | EFI_BACKGROUND_BLACK);
        print(u", video=");
        print_ascii(value);
        print(u" not available");
        set_color(EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
    }
    print(u", using it (native)\r\n");
    return mode;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_STATUS status;

    /* The firmware reboots the machine after 5 minutes unless we disable this. */
    bs->SetWatchdogTimer(0, 0, 0, 0);

    console_init(st);
    set_color(EFI_LIGHTCYAN | EFI_BACKGROUND_BLACK);
    print(u"RitterOS bootloader\r\n\r\n");
    label(u"Firmware:  ");
    print(st->FirmwareVendor);
    print(u", UEFI ");
    print_dec(st->Hdr.Revision >> 16, 0);
    print(u".");
    print_dec((st->Hdr.Revision & 0xFFFF) / 10, 0);
    print(u"\r\n");

    char cmdline[BOOT_CMDLINE_MAX];
    read_cmdline(bs, image, cmdline);
    label(u"Cmdline:   ");
    print_ascii(cmdline[0] ? cmdline : "(none)");
    print(u"\r\n");

    MEMORY_MAP map = {0};
    status = memmap_get(bs, &map);
    if (EFI_ERROR(status))
        halt_with_error(st, u"GetMemoryMap", status);
    label(u"Memory:    ");
    memmap_print_summary(&map);

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    status = gop_locate(bs, &gop);
    if (EFI_ERROR(status))
        halt_with_error(st, u"Locating the Graphics Output Protocol", status);
    UINT32 gfx_mode = choose_mode(bs, gop, cmdline);

    /* Everything the kernel receives must be allocated now: after
     * ExitBootServices there is no allocator until the kernel has its own. */
    struct boot_info *info;
    status = bs->AllocatePool(EfiLoaderData, sizeof(*info), (void **)&info);
    if (EFI_ERROR(status))
        halt_with_error(st, u"Allocating boot info", status);
    memset(info, 0, sizeof(*info));
    UINTN region_capacity = MEMMAP_COUNT(&map) + 64;
    struct memory_region *regions;
    status = bs->AllocatePool(EfiLoaderData, region_capacity * sizeof(*regions),
                              (void **)&regions);
    if (EFI_ERROR(status))
        halt_with_error(st, u"Allocating the region table", status);
    memcpy(info->cmdline, cmdline, sizeof(info->cmdline));

    /* Firmware information the kernel cannot get by itself later. */
    static const EFI_GUID acpi2 = EFI_ACPI_20_TABLE_GUID, acpi1 = EFI_ACPI_10_TABLE_GUID;
    static const EFI_GUID smbios3 = EFI_SMBIOS3_TABLE_GUID, smbios2 = EFI_SMBIOS_TABLE_GUID;
    void *t;
    if ((t = find_config_table(st, &acpi2)) || (t = find_config_table(st, &acpi1)))
        info->acpi_rsdp = (uint64_t)t;
    if ((t = find_config_table(st, &smbios3)) || (t = find_config_table(st, &smbios2)))
        info->smbios_entry = (uint64_t)t;
    info->uefi_system_table = (uint64_t)st;
    info->boot_device_path_size = (uint32_t)copy_boot_device_path(
        bs, image, info->boot_device_path, sizeof(info->boot_device_path));
    info->random_seed_size = (uint32_t)get_random_bytes(bs, info->random_seed,
                                                        sizeof(info->random_seed));

    label(u"ACPI:      ");
    if (info->acpi_rsdp) {
        print(u"RSDP at ");
        print_hex(info->acpi_rsdp, 8);
    } else {
        print(u"(none)");
    }
    label(u"\r\nSMBIOS:    ");
    if (info->smbios_entry) {
        print(u"entry point at ");
        print_hex(info->smbios_entry, 8);
    } else {
        print(u"(none)");
    }
    label(u"\r\nBoot from: ");
    print_dec(info->boot_device_path_size, 0);
    print(u" byte device path");
    label(u"\r\nRandom:    ");
    if (info->random_seed_size) {
        print_dec(info->random_seed_size, 0);
        print(u" bytes from EFI_RNG_PROTOCOL");
    } else {
        print(u"(no random number generator)");
    }
    print(u"\r\n");

    /* The kernel comes from RitterFS: every block it passes through is
     * checksum-verified, with the second copy of metadata as a fallback. */
    static BOOT_FS fs;
    const CHAR16 *why;
    label(u"RitterFS:  ");
    status = rfs_boot_open(bs, image, &fs, &why);
    if (EFI_ERROR(status)) {
        print(why);
        halt_with_error(st, u"Opening the RitterOS partition", status);
    }
    print(u"partition #");
    print_dec(fs.partition, 0);
    print(u" of the boot disk, '");
    char fs_label[sizeof(fs.vol.sb.label) + 1] = {0};
    memcpy(fs_label, fs.vol.sb.label, sizeof(fs.vol.sb.label));
    print_ascii(fs_label);
    print(u"', generation ");
    print_dec(fs.vol.sb.generation, 0);
    print(fs.vol.slot == RFS_SLOT_A ? u" (slot A)\r\n" : u" (slot B)\r\n");

    label(u"Kernel:    ");
    print_ascii(KERNEL_PATH);
    UINT8 *file;
    UINTN file_size;
    status = rfs_boot_read_file(&fs, KERNEL_PATH, &file, &file_size, &why);
    if (EFI_ERROR(status)) {
        print(u": ");
        print(why);
        halt_with_error(st, u"Reading the kernel", status);
    }
    LOADED_KERNEL kernel;
    status = load_kernel(bs, file, file_size, &kernel);
    bs->FreePool(file);
    if (EFI_ERROR(status))
        halt_with_error(st, u"Loading the kernel", status);
    print(u", ");
    print_dec(kernel.file_size, 0);
    print(u" bytes, checksums ok");
    if (fs.vol.bad_copies)
        print(u" (damaged metadata copy skipped, second copy used)");
    print(u"\r\n           ");
    print_dec(kernel.segments, 0);
    print(u" segments, ");
    print_dec(kernel.size / 1024, 0);
    print(u" KiB at physical ");
    print_hex(kernel.phys_base, 10);
    print(u", virtual ");
    print_hex(kernel.virt_base, 16);
    print(u"\r\n");
    memcpy(info->kernel_path, KERNEL_PATH, sizeof(KERNEL_PATH));
    info->kernel_file_size = kernel.file_size;
    info->kernel_fs_generation = fs.vol.sb.generation;
    memcpy(info->boot_partition_guid, &fs.partition_guid, sizeof(info->boot_partition_guid));

    /* The kernel's first page tables: memory for them must come from the
     * firmware now; they are filled in without its help. */
    UINT64 top = memmap_ram_top(&map);
    status = boot_paging_alloc(bs, top, kernel.size);
    if (EFI_ERROR(status))
        halt_with_error(st, u"Allocating page tables", status);
    UINT64 tables = boot_paging_build(top, kernel.virt_base, kernel.phys_base, kernel.size,
                                      &map);
    if (!tables)
        halt_with_error(st, u"Building page tables", EFI_LOAD_ERROR);
    label(u"Paging:    ");
    print(u"direct map of 0-");
    print_hex(top - 1, 9);
    print(u" at ");
    print_hex(HHDM_BASE, 16);
    print(u", tables at ");
    print_hex(tables, 10);

    /* (65e-34) The screen's EDID and the modes the firmware can set. */
    info->edid_size = gop_edid(bs, gop, info->edid, BOOT_EDID_MAX);
    uint32_t widths[BOOT_MODES_MAX], heights[BOOT_MODES_MAX];
    info->mode_count = gop_modes(bs, gop, widths, heights, BOOT_MODES_MAX);
    for (uint32_t i = 0; i < info->mode_count; i++)
        info->modes[i] = (struct boot_mode){ widths[i], heights[i] };
    label(u"\r\nDisplay:   ");
    print(u"EDID ");
    print_dec(info->edid_size, 0);
    print(u" bytes, ");
    print_dec(info->mode_count, 0);
    print(u" modes");

    print(u"\r\n\r\nStarting kernel...\r\n");

    /* The display switch clears the firmware console; from here on we
     * report through the serial port only. */
    status = gop->SetMode(gop, gfx_mode);
    if (EFI_ERROR(status))
        halt_with_error(st, u"GOP SetMode", status);
    if (EFI_ERROR(gop_get_framebuffer(gop, &info->framebuffer)))
        halt_with_error(st, u"Getting a linear framebuffer", EFI_UNSUPPORTED);
    const struct framebuffer *fb = &info->framebuffer;
    if (!boot_paging_map_framebuffer((UINT64)fb->base, (UINT64)fb->stride * fb->height * 4))
        halt_with_error(st, u"Mapping the framebuffer", EFI_OUT_OF_RESOURCES);
    fb_progress_mark(fb, 1);

    /*
     * ExitBootServices needs the key of the *current* memory map. If anything
     * changed the map since we read it (the firmware's own timers may
     * allocate memory at any moment), the call fails and we simply retry.
     * Each attempt leaves an orange dot below the progress squares.
     */
    for (int attempt = 0;; attempt++) {
        fb_progress_dot(fb, (unsigned)attempt);
        status = memmap_get(bs, &map);
        if (!EFI_ERROR(status))
            status = bs->ExitBootServices(image, map.key);
        if (!EFI_ERROR(status))
            break;
        if (attempt == 15) {
            fb_progress_failed(fb, 2);
            serial_write("RitterOS boot: ExitBootServices failed\n");
            boot_halt();
        }
    }

    /* The firmware's boot services are gone. No more UEFI boot service calls
     * from here; runtime services remain. */
    fb_progress_mark(fb, 2);
    serial_write("RitterOS boot: exited boot services\n");

    /*
     * From here on (x86) the kernel's first page tables are active (see
     * boot_paging_activate): the framebuffer is drawn on through the
     * direct map, which has it wherever it lies.
     */
    UINT64 offset = boot_paging_activate(tables);
    struct framebuffer direct_fb = *fb;
    direct_fb.base = (volatile uint32_t *)(offset + (uint64_t)fb->base);

    /*
     * Runtime services will be called through the direct map. Tell the
     * firmware the new address of every runtime region; it then converts its
     * internal pointers (once only, and only allowed after ExitBootServices).
     * "noruntime" on the command line skips this, for firmware that fails
     * at it; the kernel then runs without runtime services.
     */
    char flag[4];
    if (cmdline_get(cmdline, "noruntime", flag, sizeof(flag))) {
        serial_write("RitterOS boot: runtime services not used (noruntime)\n");
        info->uefi_runtime_services = 0;
    } else {
        for (UINTN i = 0; i < MEMMAP_COUNT(&map); i++) {
            EFI_MEMORY_DESCRIPTOR *d = MEMMAP_ENTRY(&map, i);
            if (d->Attribute & EFI_MEMORY_RUNTIME)
                d->VirtualStart = HHDM_BASE + d->PhysicalStart;
        }
        status = st->RuntimeServices->SetVirtualAddressMap(map.size, map.desc_size,
                                                           map.desc_version, map.descriptors);
        if (EFI_ERROR(status)) {
            serial_write("RitterOS boot: SetVirtualAddressMap failed, no runtime services\n");
            info->uefi_runtime_services = 0;
        } else {
            /* The firmware has converted this pointer to its virtual address. */
            info->uefi_runtime_services = (uint64_t)st->RuntimeServices;
            serial_write("RitterOS boot: runtime services moved to ");
            serial_write_hex(info->uefi_runtime_services);
            serial_write("\n");
        }
    }

    fb_progress_mark(&direct_fb, 3);

    memmap_sort(&map);
    info->magic = BOOT_INFO_MAGIC;
    info->version = BOOT_INFO_VERSION;
    info->size = sizeof(*info);
    info->memory_map = regions;
    info->memory_map_count = memmap_to_regions(&map, regions, region_capacity);
    info->kernel_physical_base = kernel.phys_base;
    info->kernel_virtual_base = kernel.virt_base;
    info->kernel_size = kernel.size;
    info->hhdm_offset = HHDM_BASE;

    serial_write("RitterOS boot: switching to the kernel's page tables, jumping to ");
    serial_write_hex(kernel.entry);
    serial_write("\n");
    fb_progress_mark(&direct_fb, 4);

    boot_enter_kernel(tables, kernel.entry, (struct boot_info *)(HHDM_BASE + (uint64_t)info));
}
