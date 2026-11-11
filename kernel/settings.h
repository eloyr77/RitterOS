/*
 * (37e-1b) The kernel's settings files written when missing.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include "vfs.h"

/* Writes each settings file the kernel reads (cpu, power, displays, swap)
 * that fs lacks, with its default text (common/settings_defaults.h), and
 * logs which. Run after the boot's check of fs, from its thread. */
void settings_write_missing(struct vfs *fs);

#endif
