/*
 * NVMe (Non-Volatile Memory Express) SSD driver.
 *
 * The host talks to the controller through queues in RAM: it writes 64-byte
 * commands into a submission queue and tells the controller via a "doorbell"
 * register; the controller fetches them by DMA, executes them and writes
 * 16-byte entries into a completion queue and, with MSI-X, raises an
 * interrupt. The driver sleeps until that interrupt (or the next timer tick)
 * and then checks the completion queue; without MSI-X it polls.
 */
#ifndef RITTEROS_NVME_H
#define RITTEROS_NVME_H

#include "blockdev.h"
#include "pci.h"

/* Initialize the controller and its first namespace; returns the namespace
 * as a block device ("nvme<index>n1"), or 0 on failure (the reason is
 * logged). */
struct block_device *nvme_init(struct pci_device *pci, int index);

/* Log how many commands the controller completed and how many interrupts
 * it raised. */
void nvme_report(struct block_device *dev);

#endif
