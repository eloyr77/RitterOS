/*
 * xHCI (eXtensible Host Controller Interface): the USB controller of every
 * PC since about 2012, for all USB speeds.
 *
 * Like NVMe, the controller is driven through rings in RAM. Rings are made
 * of 16-byte TRBs ("transfer request blocks"): the driver puts commands on
 * the command ring and rings a doorbell; the controller reports results and
 * changes (a device plugged in, a transfer done) on the event ring and
 * raises an interrupt. A "cycle bit" in every TRB tells which side owns it,
 * so neither side needs to read the other's position.
 */
#ifndef RITTEROS_XHCI_H
#define RITTEROS_XHCI_H

#include <stdbool.h>
#include "pci.h"

struct xhci;

/* Reset and start the controller, check that commands work and log its
 * ports. Returns 0 on failure (the reason is logged). */
struct xhci *xhci_init(struct pci_device *pci, const char *name, bool scan);

/* Log the events and interrupts seen so far. */
void xhci_report(struct xhci *c);

/* Start the usb thread, which sets up devices plugged in from now on and
 * releases unplugged ones. Called after the boot has used the devices
 * found at its start. */
bool xhci_start_hotplug(struct xhci *c);

/* Before the hot plug thread runs: set up devices connected to the
 * controller's ports that have none yet (a USB 3 link takes a moment to
 * come up after the controller's reset; main.c looks again while it waits
 * for the system disk). Returns how many it attached. */
int xhci_attach_new(struct xhci *c);

#endif
