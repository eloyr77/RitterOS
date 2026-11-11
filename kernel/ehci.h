/*
 * EHCI (USB 2) controllers. RitterOS has no driver for them: it uses xHCI.
 * But Intel chipsets can wire each USB socket to either controller, and
 * firmware may leave the sockets on EHCI and keep using that controller
 * itself (booting from it; emulating a PS/2 keyboard through System
 * Management Mode). Before xhci.c moves the sockets over, each EHCI
 * controller is taken over from the firmware (USB legacy support, EHCI 1.0
 * section 5.1) and stopped.
 */
#ifndef RITTEROS_EHCI_H
#define RITTEROS_EHCI_H

#include "pci.h"

/* Take every EHCI controller over from the firmware and stop it. */
void ehci_release_all(void);

#endif
