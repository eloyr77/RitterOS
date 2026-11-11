/*
 * (67a) Kernel modules: drivers kept as files in /system/<arch>/drivers/
 * (NAME.mod), loaded after the root mount when their hardware is there
 * (kernel/module.c). A module is one relocatable object; (67b) it is
 * written against the driver interface (drivers/include/: driver.h and
 * its device class's header) and may use only the functions those
 * headers declare: scripts/kexports.c checks every module at build time
 * and makes the table the loader links them with. A module stays until
 * the machine restarts.
 *
 * A driver declares itself once, at its end (driver.h's MODULE):
 *
 *     static const struct module_pci ids[] = { { 0x10EC, 0x8168 }, ... };
 *     MODULE(rtl8168, start, ids)
 *
 * The kernel calls start(device) for every PCI device whose vendor and
 * device (MODULE_ANY: any) are in the table; start says whether it took
 * the device. A module whose table matches nothing is not kept.
 */
#ifndef RITTEROS_MODULE_H
#define RITTEROS_MODULE_H

#include <stdbool.h>
#include <stdint.h>
#include "driver.h"         /* (67b) struct module_info, MODULE: the drivers' interface */

/* Load the modules whose hardware is there (one boot line says which);
 * after the root mount. */
void modules_load(void);

/* `nomodules` on the command line: none loaded (one boot line says so). */
void modules_skip(void);

/* (67e) The USB core found a device (offer it to the USB modules) or lost
 * one (their detach). */
struct usb_device;
void modules_usb_offer(struct usb_device *dev);
void modules_usb_removed(struct usb_device *dev);

/* The `drivers` report (REPORT_DRIVERS): every module file, loaded or
 * not needed or refused (why), where it lies and what it took. */
void modules_print(void);

/* The table scripts/kexports.c makes from drivers/include/'s headers. */
struct kernel_export {
    const char *name;
    void *address;
};
extern const struct kernel_export kernel_exports[];
extern const int kernel_export_count;

/* (arch/<arch>/module.c) Apply one relocation of type `type` at `where`
 * (its address when running: p) for symbol value s and addend a: 0, or -1
 * for a type the loader does not know, -2 when the value does not fit. */
int arch_module_relocate(uint32_t type, uint8_t *where, uint64_t p, uint64_t s, int64_t a);

/* (37g-2) Sleep: the first module whose devices cannot sleep (no
 * suspend), or 0; every module's devices suspended (0, or the name of the
 * one that refused: the others are resumed again) and resumed (how many
 * failed, each logged). */
const char *modules_cannot_sleep(void);
const char *modules_suspend(void);
int modules_resume(void);

#endif
