/*
 * SATA disks through AHCI (Advanced Host Controller Interface, version
 * 1.3.1), the standard SATA controller of PCs since about 2005.
 *
 * The controller (PCI class 01.06.01) has one register block (BAR5) with up
 * to 32 ports, one per SATA connector. For each port the driver provides
 * three structures in RAM: a command list (32 slots), a command table per
 * command (the ATA command as a "FIS" plus the data's memory areas) and an
 * area where the disk's answers arrive. A command goes into slot 0; setting
 * its bit in PxCI starts it, the controller moves the data by DMA and
 * clears the bit (with an MSI interrupt) when the disk is done.
 *
 * Disks are named sata0, sata1, ... in the order found; their partitions
 * sata0p1, sata0p2, ... (gpt.c).
 */
#ifndef RITTEROS_AHCI_H
#define RITTEROS_AHCI_H

#include "blockdev.h"
#include "pci.h"

/* Take the controller over from the firmware, reset it and start every
 * port with an ATA disk; the disks go into out[] (at most max). Returns
 * how many; the controller and each disk are logged. */
int ahci_init(struct pci_device *pci, struct block_device **out, int max);

/* Log each controller's commands and interrupts. */
void ahci_report(void);
/* (37g-6) `disks`: each controller's ports after the last sleep (nothing
 * before one). */
void ahci_sleep_print(void);

#endif
