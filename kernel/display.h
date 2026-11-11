/*
 * (65e-34) The screen as the firmware and the monitor describe it: the EDID
 * and the modes the bootloader collected (boot info version 6), parsed once
 * at the boot, for the boot log's `display:` line and DISPLAY_MODES.
 */
#ifndef RITTEROS_DISPLAY_H
#define RITTEROS_DISPLAY_H

#include "boot_info.h"
#include "syscalls.h"

/* Keep and parse what the bootloader passed on, and log it. */
void display_init(const struct boot_info *info);

/* What DISPLAY_MODES gives: the screen's mode now (width x height). */
void display_modes(struct sys_display_modes *out, uint32_t width, uint32_t height);

#endif
