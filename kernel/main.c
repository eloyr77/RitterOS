/*
 * RitterOS kernel.
 * Step 3: receive control from the bootloader with no firmware left.
 * Step 4a: replace the firmware's GDT with our own, including a TSS.
 * Step 4b: install an IDT with handlers for all CPU exceptions.
 * Step 4c: "crash=<kind>" on the command line triggers a test exception.
 * Step 5a: text drawn with the X11 9x15 bitmap font.
 * Step 5b: text console and kprintf (a test picture shown before it was
 *          removed again after step 11d).
 * Step 6a: our own page tables with per-section permissions and guard pages.
 * Step 6b: physical page allocator; "memtest" writes every free page.
 * Step 6c: firmware information from the bootloader: boot device, random
 *          seed, ACPI, SMBIOS, UEFI runtime services.
 * Step 6d: higher-half kernel at KERNEL_VIRT_BASE, physical memory through
 *          the direct map, empty lower half.
 * Step 6e: kernel heap (kmalloc/kfree).
 * Step 7a: PCI Express scan; find the boot disk's controller.
 * Step 7b: NVMe driver (polled reads); verify the boot disk's GPT.
 * Step 7c: NVMe writes and flush; partitions as block devices; "disktest".
 * Step 8a: mount RitterFS on the RitterOS partition and check it.
 * Step 8b: RitterFS path lookup, directories and file reading; verify the
 *          test files against /test/manifest.txt.
 * Step 8c: RitterFS writing (copy-on-write commits); "fswrite", "fstorture".
 * Step 8d: RitterFS scrub and repair; "scrub" or "scrub=relocate".
 * Step 9a: interrupts: interrupt controller and a 100 Hz timer tick.
 * Step 9b: device interrupts (MSI-X): the NVMe driver sleeps instead of polling.
 * Step 9c: serial input by interrupt; a terminal with a few commands.
 * Step 9d: terminal commands ls, cat, pci, poweroff, reboot.
 * Step 9e: the kernel itself is loaded from RitterFS (by the bootloader).
 * Step 10a: USB: the xHCI controller, its command and event rings, ports.
 * Step 10b: USB devices get a slot and an address; their descriptors are read.
 * Step 10c: USB keyboards (HID boot protocol) type into the terminal.
 * Step 11a: programs in user mode, with system calls; the terminal runs them.
 * Step 11b: programs read input and files; arguments; Ctrl-C.
 * Step 11c: threads, a preemptive scheduler, locks and wait queues.
 * Step 12a/12b: USB hubs; USB mass storage (sticks) as block devices.
 * Step 12c: the system partition is the bootloader's (by GPT GUID), on any disk.
 */
#include <stdint.h>
#include "settings.h"
#include "release.h"
#include "swap.h"
#include "string.h"
#include "boot_info.h"
#include "ahci.h"
#include "ehci.h"
#include "keyboard.h"
#include "mouse.h"
#include "framebuffer.h"
#include "serial.h"
#include "arch.h"
#include "cmdline.h"
#include "tcp.h"
#include "crashtest.h"
#include "console.h"
#include "cpuclock.h"
#include "gpu.h"
#include "percpu.h"
#include "i2chid.h"
#include "kprintf.h"
#include "sync.h"
#include "panic.h"
#include "paging.h"
#include "pmm.h"
#include "memory.h"
#include "heap.h"
#include "random.h"
#include "pci.h"
#include "nvme.h"
#include "sched.h"
#include "sleep.h"
#include "sound.h"
#include "cpus.h"
#include "xhci.h"
#include "net.h"
#include "usbkbd.h"
#include "usbstorage.h"
#include "devices.h"
#include "mounts.h"
#include "module.h"
#include "gpt.h"
#include "disktest.h"
#include "ritterfs.h"
#include "fstest.h"
#include "acpi.h"
#include "acpi_events.h"
#include "smbios.h"
#include "display.h"
#include "devpath.h"
#include "uefi.h"
#include "timer.h"
#include "terminal.h"


extern char __kernel_start[], __kernel_end[];

uint64_t kernel_phys_base;

/* The kernel's copy of the boot info, with pointers converted to virtual. */
#define MAX_DISKS 16
#define MAX_XHCI  4

static struct boot_info boot;

static const char *type_name(uint32_t type)
{
    switch (type) {
    case MEMORY_USABLE:           return "usable";
    case MEMORY_LOADER:           return "kernel/loader";
    case MEMORY_ACPI_RECLAIMABLE: return "ACPI tables";
    case MEMORY_ACPI_NVS:         return "ACPI NVS";
    case MEMORY_RESERVED:         return "reserved";
    case MEMORY_MMIO:             return "MMIO";
    case MEMORY_UEFI_RUNTIME_CODE: return "UEFI runtime code";
    case MEMORY_UEFI_RUNTIME_DATA: return "UEFI runtime data";
    default:                      return "unknown";
    }
}

/* Size rounded to KiB or MiB, e.g. "640 KiB". */
static const char *size_text(uint64_t bytes, char *buf, size_t size)
{
    if (bytes < 1024 * 1024)
        ksnprintf(buf, size, "%lu KiB", (bytes + 512) / 1024);
    else
        ksnprintf(buf, size, "%lu MiB", (bytes + 512 * 1024) / (1024 * 1024));
    return buf;
}

static void report(const struct boot_info *info)
{
    const struct framebuffer *fb = &info->framebuffer;
    char size[16];

    kprintf("  command line:  %s\n", info->cmdline[0] ? info->cmdline : "(none)");
    kprintf("  kernel image:  %p - %p (%s), physical %p\n", (void *)__kernel_start,
            (void *)(__kernel_end - 1),
            size_text((uint64_t)(__kernel_end - __kernel_start), size, sizeof(size)),
            (void *)info->kernel_physical_base);
    kprintf("  kernel file:   %.64s on RitterFS, %lu bytes, file system generation %lu\n",
            info->kernel_path, info->kernel_file_size, info->kernel_fs_generation);
    kprintf("  framebuffer:   %ux%u, stride %u, at %p\n", fb->width, fb->height, fb->stride,
            (void *)fb->base);
    kprintf("  memory map:    %lu regions\n", info->memory_map_count);

    uint64_t usable = 0;
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        const struct memory_region *r = &info->memory_map[i];
        kprintf("    0x%016lX - 0x%016lX  %9s  %s\n", r->base, r->base + r->length - 1,
                size_text(r->length, size, sizeof(size)), type_name(r->type));
        if (r->type == MEMORY_USABLE)
            usable += r->length;
    }
    kprintf("  usable RAM:    %lu MiB\n", usable / (1024 * 1024));
}

/* What the bootloader collected from the firmware (boot info version 2). */
static void report_firmware(const struct boot_info *info)
{
    if (info->boot_device_path_size) {
        char text[512];
        struct devpath_partition part;
        devpath_to_text(info->boot_device_path, info->boot_device_path_size, text, sizeof(text));
        kprintf("  boot device:   %s\n", text);
        if (devpath_partition(info->boot_device_path, info->boot_device_path_size, &part)) {
            char guid[37];
            guid_to_text(part.guid, guid);
            kprintf("  boot partition: #%u, %s, GUID %s, sectors %lu-%lu\n", part.number,
                    part.is_gpt ? "GPT" : "MBR", guid, part.start_lba,
                    part.start_lba + part.sector_count - 1);
        }
    } else {
        kprintf("  boot device:   unknown\n");
    }

    if (info->random_seed_size >= 8) {
        uint64_t first;
        memcpy(&first, info->random_seed, 8);
        kprintf("  random seed:   %u bytes from the firmware RNG, begins 0x%016lX\n",
                info->random_seed_size, first);
    } else {
        kprintf("  random seed:   none (firmware has no RNG)\n");
    }

    display_init(info);                     /* (65e-34) the EDID, the firmware's modes */
    acpi_init(info->acpi_rsdp);
    smbios_report(info->smbios_entry);
    uefi_init(info->uefi_runtime_services);
    uefi_report_time();
}

/* Character sample: ASCII, Latin-1, box drawing and block elements. */
/* A disk found at the boot: its table logged, it and its partitions in
 * /dev; the system partition if it is on it (by the bootloader's GPT GUID,
 * or without one the first RitterOS partition). */
static void look_for_system(struct block_device *disk, const struct boot_info *info,
                            bool have_guid, struct block_device **part,
                            struct block_device **system_disk)
{
    bool gpt = gpt_report(disk);
    devices_add_disk(disk);                         /* /dev: it and its partitions */
    if (!gpt)
        return;
    struct block_device *p = have_guid ? devices_find_partition(disk, info->boot_partition_guid, 0)
                                       : devices_find_partition(disk, 0, GPT_TYPE_RITTEROS);
    if (p && !*part) {
        *part = p;
        *system_disk = disk;
        disk->system = true;                /* for the message if it is unplugged */
    } else if (p) {
        kprintf("  system partition: %s has the same GPT GUID as %s (a copied disk?); "
                "using %s\n", p->name, (*part)->name, (*part)->name);
    }
}

/* (65e-37) The thread fscheck: the boot's check of the root file system
 * (its committed state, under its lock held to read); its last line says
 * when it ended (the tests wait for it). */
static bool fscheck_running;
static struct wait_queue fscheck_done;
static bool swap_selftest;              /* (35a) the boot's `selftest`, for swap_settings */

static void fscheck(void *arg)
{
    struct ritterfs *fs = arg;
    uint64_t started = arch_counter();
    bool ok = ritterfs_check(fs);
    /* (37e-1b) The kernel's missing settings files, written after the
     * check (a change would wait for it in the boot), not on a disk it
     * found wrong. */
    if (ok)
        settings_write_missing(mounts_root());
    /* (35a) Swap goes on here, after the check: turning it on takes the
     * file system to write, which would wait for the check in the boot. */
    swap_settings(swap_selftest);
    kprintf("  RitterFS check: done in %lu ms, beside the boot%s\n", timer_elapsed_ms(started),
            ok ? "" : ": PROBLEMS FOUND");
    __atomic_store_n(&fscheck_running, false, __ATOMIC_RELEASE);
    wait_queue_wake_all(&fscheck_done);
}

__attribute__((noreturn)) void kernel_main(struct boot_info *handover)
{
    percpu_init_boot();                 /* (65a) this core's block: kprintf asks for it */
    /* Before the console exists, panic() reaches the serial port only. */
    if (handover->magic != BOOT_INFO_MAGIC)
        panic("bad boot info magic");
    /* (65e-34) Version 5 (a bootloader from before the EDID) is taken too:
     * its info is ours up to edid_size; the rest stays zero (no EDID). A
     * kernel installed over SSH then boots with the disk's bootloader. */
    bool v5 = handover->version == 5 && handover->size == BOOT_INFO_V5_SIZE;
    if (!v5 && (handover->version != BOOT_INFO_VERSION || handover->size != sizeof(*handover)))
        panic("boot info version %u (%u bytes), kernel expects version %u (%u bytes) or 5 (%u)",
              handover->version, handover->size, BOOT_INFO_VERSION,
              (unsigned)sizeof(*handover), (unsigned)BOOT_INFO_V5_SIZE);

    /* Keep our own copy and turn its physical pointers into direct-map ones. */
    memset(&boot, 0, sizeof(boot));
    memcpy(&boot, handover, handover->size);
    kernel_phys_base = boot.kernel_physical_base;
    boot.memory_map = phys_to_virt((uint64_t)boot.memory_map);
    boot.framebuffer.base = phys_to_virt((uint64_t)boot.framebuffer.base);
    struct boot_info *info = &boot;
    fb_progress_mark(&info->framebuffer, 5);

    /* The console is drawn from the start (the bootloader maps the
     * framebuffer), so that a boot that hangs on a PC without a serial port
     * shows how far it got. */
    console_init(&info->framebuffer);
    console_show();
    console_set_color(COLOR_HEADING);
    console_set_bold(true);
    /* (68i) with the release and the commit it was built from */
    kprintf("RitterOS kernel started: %s%s%s%s, %s\n", release_version, release_commit[0] ? " (" : "",
            release_commit, release_commit[0] ? ")" : "", ARCH_NAME);
    console_set_bold(false);
    console_set_color(COLOR_TEXT);

    report(info);
    /* The large self-tests run on request ("selftest"; make test asks for
     * them): a normal boot is quicker without them. Cheap checks that
     * protect data (the RitterFS check, ChaCha20's test vector) always run. */
    char flag[4];
    bool selftest = cmdline_get(info->cmdline, "selftest", flag, sizeof(flag));
    /* (65a-5) The order of spinlocks checked from here on (each core keeps
     * what it holds anyway): a test option, for the work on several cores. */
    if (cmdline_get(info->cmdline, "lockcheck", flag, sizeof(flag))) {
        lock_checking = true;
        kprintf("  lockcheck:     on (the order spinlocks are taken in is checked)\n");
    }
    arch_cpu_init();
    paging_init(info);
    pmm_init(info);
    if (selftest)
        pmm_self_test();
    uint64_t seed = 0;
    memcpy(&seed, info->random_seed, info->random_seed_size >= 8 ? 8 : 0);
    if (selftest) {
        heap_self_test(seed);
        kprintf_self_test();
    }
    random_init(info->random_seed, info->random_seed_size);
    if (!random_selftest())
        kprintf("  random:        ChaCha20 self-test FAILED (wrong RFC 8439 test vector)\n");
    console_use_buffer();
    report_firmware(info);
    timer_init();
    uefi_clock_start();                 /* the counter's rate is known now */
    sched_init();
    if (selftest)
        sched_self_test();
    console_start_updates();
    terminal_init();

    pci_init();
    acpi_aml_load(info);                /* (37b-2, 37b-4) the firmware's AML and its _INI */
    acpi_events_start();                /* (37c-2) the SCI, the power button */
    acpi_idle_start();                  /* (37d-2) the processors' sleep states */
    struct pci_device *disk = pci_find_by_device_path(info->boot_device_path,
                                                      info->boot_device_path_size);
    if (disk)
        kprintf("  boot disk:     controller %02x:%02x.%u (%04x:%04x, class %02x.%02x.%02x)\n",
                disk->bus, disk->device, disk->function, disk->vendor_id, disk->device_id,
                disk->class_code, disk->subclass, disk->prog_if);
    else
        kprintf("  boot disk:     controller not found on PCI\n");

    /* (66a, 66e, 67f) The graphics drivers (intelgpu, amdgpu) are modules,
     * loaded after the root mount: the firmware's screen until then; (67g)
     * the sound driver (hda) too. */

    /* The disks: every NVMe and AHCI (SATA) controller, and the USB
     * controller with its sticks. Those on the controller the firmware
     * booted from come first (first[]), then the others (rest[]). */
    struct block_device *disks[MAX_DISKS], *nvme[MAX_DISKS];
    struct block_device *first[MAX_DISKS], *rest[MAX_DISKS];
    int disk_count = 0, nvme_count = 0, first_count = 0, rest_count = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < pci_device_count() && nvme_count < MAX_DISKS; i++) {
            struct pci_device *d = pci_device_at(i);
            /* NVMe: class 01 (storage), subclass 08 (non-volatile memory),
             * interface 02. */
            if (d->class_code != 0x01 || d->subclass != 0x08 || d->prog_if != 0x02 ||
                (pass == 0) != (d == disk))
                continue;
            struct block_device *bd = nvme_init(d, nvme_count);
            if (bd) {
                nvme[nvme_count++] = bd;
                if (d == disk)
                    first[first_count++] = bd;
                else
                    rest[rest_count++] = bd;
            }
        }
    }
    /* SATA: class 01, subclass 06, interface 01 (AHCI); every ATA disk. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < pci_device_count(); i++) {
            struct pci_device *d = pci_device_at(i);
            if (d->class_code != 0x01 || d->subclass != 0x06 || d->prog_if != 0x01 ||
                (pass == 0) != (d == disk))
                continue;
            if (d == disk)
                first_count += ahci_init(d, first + first_count, MAX_DISKS - first_count);
            else
                rest_count += ahci_init(d, rest + rest_count, MAX_DISKS - rest_count);
        }
    }

    arch_user_init();

    /* USB: class 0C (serial bus), subclass 03 (USB), interface 30 (xHCI):
     * every such controller (laptops often have two), usb0, usb1, ... in
     * PCI order. EHCI controllers (interface 20) are only taken from the
     * firmware: on Intel chipsets their sockets then move to xHCI.
     * "usblate" imitates devices whose links come up late: the controllers'
     * first look at their ports finds nothing. */
    ehci_release_all();
    static char usb_names[MAX_XHCI][8];
    struct xhci *xhcis[MAX_XHCI];
    int xhci_count = 0, xhci_found = 0;
    bool usblate = cmdline_get(info->cmdline, "usblate", flag, sizeof(flag));
    for (int i = 0; i < pci_device_count() && xhci_count < MAX_XHCI; i++) {
        struct pci_device *d = pci_device_at(i);
        if (d->class_code != 0x0C || d->subclass != 0x03 || d->prog_if != 0x30)
            continue;
        xhci_found++;
        ksnprintf(usb_names[xhci_count], sizeof(usb_names[0]), "usb%d", xhci_count);
        struct xhci *x = xhci_init(d, usb_names[xhci_count], !usblate);
        if (x)
            xhcis[xhci_count++] = x;
    }
    if (!xhci_found)
        kprintf("  xHCI:          no USB controller\n");
    arch_input_init();
    i2chid_init();                              /* touchpads on I2C (15s) */

    /* (67a-67d) The network cards' drivers are modules (drivers/: rtl8168,
     * e1000, igb, ritterwifi, mt7921), loaded after the root mount; the USB
     * adapters found so far are announced here. */
    net_announce();
    kprintf("  lo:            the loopback, 127.0.0.1/8\n");
    /* "netloss=N": TCP loses one segment in N (the tests' lossy network). */
    char loss[12];
    if (cmdline_get(info->cmdline, "netloss", loss, sizeof(loss))) {
        uint32_t n = 0;
        for (const char *c = loss; *c >= '0' && *c <= '9' && n < 100000; c++)
            n = n * 10 + (uint32_t)(*c - '0');
        tcp_set_loss(n);
        kprintf("  network:       TCP loses 1 segment in %u (netloss)\n", n);
    }
    bool booted_from_usb = disk && disk->class_code == 0x0C && disk->subclass == 0x03;
    for (int i = 0; i < usbstorage_count(); i++) {
        if (booted_from_usb && first_count < MAX_DISKS)
            first[first_count++] = usbstorage_disk(i);
        else if (!booted_from_usb && rest_count < MAX_DISKS)
            rest[rest_count++] = usbstorage_disk(i);
    }
    for (int i = 0; i < first_count && disk_count < MAX_DISKS; i++)
        disks[disk_count++] = first[i];

    /* The system partition: the one the bootloader loaded the kernel from,
     * found by its GPT GUID on whatever disk holds it (NVMe, a USB stick).
     * Without that GUID: the first RitterOS partition on the first disk.
     * The disks on the controller the firmware booted from are searched
     * first, and a late USB stick is waited for, before the others: a
     * copy of the stick's image on an internal disk has the same GUIDs,
     * and on the user's laptop the NVMe copy was taken while the stick's
     * link was still coming up (48b). */
    static const uint8_t no_guid[16];
    bool have_guid = memcmp(info->boot_partition_guid, no_guid, 16) != 0;
    struct block_device *part = 0, *system_disk = 0;
    for (int i = 0; i < disk_count; i++)
        look_for_system(disks[i], info, have_guid, &part, &system_disk);

    /* Started from a USB stick that is not there yet: a USB 3 stick's link
     * comes up a moment after its controller's reset (an AMD laptop's took
     * longer than the boot needed to get here). Look at the ports again,
     * for at most 10 s. */
    if (!part && booted_from_usb && have_guid && xhci_count) {
        kprintf("  system partition: not found yet; waiting for USB disks (at most 10 s)\n");
        uint64_t started = arch_counter();
        int seen = usbstorage_count();
        for (int waited = 0; !part && waited <= 10000; waited += 100) {
            for (int k = 0; k < xhci_count; k++)
                xhci_attach_new(xhcis[k]);
            for (; seen < usbstorage_count() && disk_count < MAX_DISKS; seen++) {
                disks[disk_count] = usbstorage_disk(seen);
                look_for_system(disks[disk_count++], info, have_guid, &part, &system_disk);
            }
            if (!part)
                timer_sleep_ms(100);
        }
        if (part)
            kprintf("  system partition: found after %lu ms\n", timer_elapsed_ms(started));
    }
    for (int i = 0; i < rest_count && disk_count < MAX_DISKS; i++) {
        disks[disk_count] = rest[i];
        look_for_system(disks[disk_count++], info, have_guid, &part, &system_disk);
    }
    char guid[37];
    guid_to_text(info->boot_partition_guid, guid);
    if (part) {
        kprintf("  RitterOS partition: %s, %lu blocks\n", part->name, part->block_count);
        kprintf("  system partition: %s, %s\n", part->name,
                have_guid ? "where the bootloader found the kernel" : "the first RitterOS one");
        uint64_t started = arch_counter();
        struct ritterfs *fs = ritterfs_mount(part);
        uint64_t mount_ms = timer_elapsed_ms(started);
        if (fs) {
            terminal_set_filesystem(fs);
            devices_set_mounted(part, "/", &fs->vfs);
            mounts_set_root(part, &fs->vfs);
            net_names_load();                   /* the cards' names from the disk (50f) */
            if (cmdline_get(info->cmdline, "nomodules", flag, sizeof(flag)))
                modules_skip();
            else
                modules_load();                 /* (67a) the drivers whose hardware is here */
            sound_announce();                   /* (67g) pcm0, ... from the sound cards */
            if (cmdline_get(info->cmdline, "hdatone", flag, sizeof(flag)))
                sound_test_tone();              /* (a short tone on pcm0) */
            cpu_clock_load();                   /* the processor's clock setting (64t) */
            gpu_load_settings();                /* the screens' modes (66b-6) */
            acpi_events_settings();             /* (37c-2) what the power button does */
            /* (65e-37) The check beside the boot: 1.08 s of the laptop's
             * 4.78 s kernel boot was the check's. Its thread holds the
             * file system to read meanwhile (changes wait for it). */
            fscheck_running = true;
            swap_selftest = selftest;
            if (thread_create("fscheck", fscheck, fs)) {
                kprintf("  RitterFS check: beside the boot (the thread fscheck)\n");
            } else {
                fscheck_running = false;
                if (ritterfs_check(fs))             /* (no thread: as before, first) */
                    settings_write_missing(mounts_root());
                swap_settings(selftest);
            }
            started = arch_counter();
            if (selftest) {
                fs_test(fs);
                kprintf("  RitterFS time: mount %lu ms, tree and files %lu ms\n", mount_ms,
                        timer_elapsed_ms(started));
            } else {
                kprintf("  RitterFS time: mount %lu ms\n", mount_ms);
            }
            ritterfs_cache_report(fs);
            if (cmdline_get(info->cmdline, "fswrite", flag, sizeof(flag)))
                fs_write_test(fs);
            if (cmdline_get(info->cmdline, "fssnap", flag, sizeof(flag)))
                fs_snapshot_test(fs);
            if (cmdline_get(info->cmdline, "fscopy", flag, sizeof(flag)))
                fs_copy_test(fs);
            if (cmdline_get(info->cmdline, "fsattr", flag, sizeof(flag)))
                fs_attr_test(fs);
            if (cmdline_get(info->cmdline, "fstxn", flag, sizeof(flag)))
                fs_abort_test(fs);
            if (cmdline_get(info->cmdline, "fstorture", flag, sizeof(flag)))
                fs_torture(fs, seed);
            char mode[16];
            if (cmdline_get(info->cmdline, "scrub", mode, sizeof(mode))) {
                ritterfs_scrub(fs, strcmp(mode, "relocate") == 0);
                ritterfs_check(fs);
            }
        }
        if (cmdline_get(info->cmdline, "disktest", flag, sizeof(flag)))
            disk_test(part, seed);
    } else {
        kprintf("  system partition: not found (partition GUID %s)\n", have_guid ? guid : "?");
    }

    /* The system disk's EFI system partition (the bootloaders): /boot. */
    struct block_device *esp = system_disk ? devices_find_partition(system_disk, 0, GPT_TYPE_EFI)
                                           : 0;
    if (esp)
        mounts_auto_at(esp, "/boot", true);         /* the bootloaders: kept safe */

    /* RitterFS and FAT on the other disks (USB sticks, SATA disks, more
     * NVMe) and on the system disk's other partitions, found by content:
     * checked and mounted at /volumes/<label> (mounting writes nothing),
     * unless "usbdisktest" or "satadisktest" asks for a write test on that
     * kind of disk (on its RitterOS partition). */
    for (int i = 0; i < disk_count; i++) {
        struct block_device *d = disks[i];
        struct block_device *p = d == system_disk ? 0
                                 : devices_find_partition(d, 0, GPT_TYPE_RITTEROS);
        if (p) {
            kprintf("  RitterOS partition: %s, %lu blocks\n", p->name, p->block_count);
            char flag[4];
            bool usb = strncmp(d->name, "usbdisk", 7) == 0, sata = strncmp(d->name, "sata", 4) == 0;
            if ((usb && cmdline_get(info->cmdline, "usbdisktest", flag, sizeof(flag))) ||
                (sata && cmdline_get(info->cmdline, "satadisktest", flag, sizeof(flag)))) {
                struct ritterfs *fs = ritterfs_mount(p);    /* checked, not mounted: */
                if (fs)                                     /* the test writes raw blocks */
                    ritterfs_check(fs);
                disk_test(p, seed);
                continue;
            }
        }
        devices_mount_volumes(d);
    }
    /* From here on, USB devices may come and go (a usb thread for each
     * controller). */
    usbstorage_start_hotplug();
    for (int k = 0; k < xhci_count; k++)
        if (!xhci_start_hotplug(xhcis[k]))
            kprintf("  %s:          no thread for devices plugged in later\n", usb_names[k]);
    for (int i = 0; i < nvme_count; i++)
        nvme_report(nvme[i]);
    ahci_report();

    /* Prove the exception path works in both directions. */
    if (selftest)
        arch_exception_selftest();
    else
        kprintf("  self-tests:    skipped (\"selftest\" on the command line runs them)\n");

    char value[16];
    /* The memory test counts every free page and takes them all: the
     * check beside the boot ends first (on ARM it gave its pages back
     * during the test, and while it runs it could get none). */
    if (cmdline_get(info->cmdline, "memtest", value, sizeof(value))) {
        wait_event(&fscheck_done, !__atomic_load_n(&fscheck_running, __ATOMIC_ACQUIRE));
        pmm_memtest();
    }

    /* (65b-65c) The other cores, last: everything above that sets up this
     * core itself (FP and system calls for programs, interrupt routing)
     * must run here, and once they share the threads, this code (thread
     * main) may go on on any core. */
    cpus_start(info->cmdline);

    if (cmdline_get(info->cmdline, "crash", value, sizeof(value)))
        crash_test(value);
    /* (37g-1) sleeptest: S3 for 3 s at the end of the boot, every core up
     * (the devices' state is not brought back yet: with `rescue`). */
    if (cmdline_get(info->cmdline, "sleeptest", value, sizeof(value)))
        sleep_for(3);

    for (int k = 0; k < xhci_count; k++)
        xhci_report(xhcis[k]);
    console_report();
    keyboard_report();
    mouse_report();
    uint64_t t = timer_ticks();
    kprintf("  uptime:        %lu.%02lu s since the timer started (%lu ticks)\n", t / TIMER_HZ,
            t % TIMER_HZ * 100 / TIMER_HZ, t);
    console_set_color(COLOR_OK);
    kprintf("RitterOS kernel: boot complete\n");
    console_set_status(STATUS_OK);
    console_set_color(COLOR_TEXT);
    if (terminal_has_input())
        terminal_run(cmdline_get(info->cmdline, "rescue", value, sizeof(value)));
    kprintf("no serial input and no keyboard: halting CPU\n");
    halt_forever();
}
