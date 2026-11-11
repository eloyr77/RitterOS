/*
 * PCI Express: finding devices and reading their configuration.
 *
 * Each device function has a 4 KiB configuration space. With PCIe's
 * "enhanced configuration access mechanism" (ECAM) all of them are mapped
 * into memory: the ACPI MCFG table gives the base address, and the space of
 * bus B, device D, function F is at base + (B << 20 | D << 15 | F << 12).
 */
#ifndef RITTEROS_PCI_H
#define RITTEROS_PCI_H

#include <stdbool.h>
#include <stdint.h>
#include "arch.h"
#include "driver.h"     /* (67b) struct pci_bar, struct pci_device: the drivers' too */

/* (67b) A device's part only the kernel sees (struct pci_device's kernel). */
struct pci_device_kernel {
    uint8_t header_type;       /* 0: device, 1: PCI-to-PCI bridge */
    volatile uint8_t *config;  /* its configuration space (direct map) */
    /* (65d) Its message interrupt, if pci_msi(x)_enable set one up: where
     * the message lives (MSI: the capability; MSI-X: table entry 0), what
     * it is, and the core it is routed to. */
    uint8_t msi_cap, msix_cap;
    volatile uint32_t *msix_entry;
    uint64_t msi_address;
    uint32_t msi_data;
    int msi_core;
};

/* Scan all buses described by the ACPI MCFG table and log the devices.
 * Returns the number of devices found. */
int pci_init(void);

int pci_device_count(void);

/* (37g-1) Every function's configuration kept before a sleep, and back
 * after it (how many). */
void pci_sleep_save(void);
int pci_wake(void);

/* Log one device: address, IDs, kind, vendor and its BARs. */
void pci_print_device(const struct pci_device *dev);
struct pci_device *pci_device_at(int index);

/* Let the device use memory and do DMA: its memory and bus master bits,
 * and the bus master bits of the bridges above it (logged when changed). */
void pci_enable_dma(struct pci_device *dev);

/* One line for net's report: the command and status registers of each
 * bridge from the root down to the device, and of the device (the status
 * shows a refused DMA: bits 12-13, target and master abort). */
void pci_print_path(const struct pci_device *dev);

/* First device with this class/subclass/programming interface, or 0. */
struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

/* The device a UEFI device path leads to (following PCI bridges), or 0. */
struct pci_device *pci_find_by_device_path(const uint8_t *path, uint64_t size);

/* Configuration space access. */
uint32_t pci_read32(const struct pci_device *dev, uint16_t offset);
void pci_write32(const struct pci_device *dev, uint16_t offset, uint32_t value);
uint16_t pci_read16(const struct pci_device *dev, uint16_t offset);
void pci_write16(const struct pci_device *dev, uint16_t offset, uint16_t value);

/* (37b-4) Any function's configuration register by its address (the AML
 * interpreter's PCI_Config): width 8, 16, 32 or 64 bits (two of 32), at an
 * offset that keeps it inside the 4 KiB; false outside the ECAM's segment
 * 0 buses. */
bool pci_config_access(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t fn, uint32_t offset,
                       int width, uint64_t *value, bool write);
uint8_t pci_read8(const struct pci_device *dev, uint16_t offset);


#define PCI_CAP_MSI             0x05
#define PCI_CAP_MSIX            0x11

/* Offset of the capability with this ID in configuration space, or 0.
 * Capabilities are a linked list of optional features, e.g. MSI-X. */
uint8_t pci_find_capability(const struct pci_device *dev, uint8_t id);

/* (65e) Switch the PCIe link's power saving (ASPM, L0s and L1) off for dev
 * and the port above it, the device first (the order the PCIe specification
 * asks for switching L1 off). What was on goes into *was_dev and *was_port
 * (Link Control's bits 0-1: 1 L0s, 2 L1; -1 where there is no PCIe
 * capability or no port above). */
void pci_aspm_off(struct pci_device *dev, int *was_dev, int *was_port);

/*
 * MSI-X: the device raises interrupts by writing a message (data to an
 * address) taken from a table in one of its BARs; each table entry is one
 * interrupt source. Route entry 0 to a new interrupt (arch_msi_allocate) that
 * calls handler(context), leave the others masked, and switch MSI-X on.
 * Returns false (and changes nothing) if the device or the machine cannot.
 * On success, *address and *data are the message, *entries the table size.
 */
/*
 * MSI, the older form (PCI 2.2): one message address and data value in the
 * capability itself. For devices without MSI-X (e.g. the xHCI controller of
 * an Intel J1900). One vector; same results as pci_msix_enable.
 */
bool pci_msi_enable(struct pci_device *dev, irq_handler_fn handler, void *context,
                    uint64_t *address, uint32_t *data);

bool pci_msix_enable(struct pci_device *dev, irq_handler_fn handler, void *context,
                     uint64_t *address, uint32_t *data, uint16_t *entries);

/* (65d) Once the other cores run: the network cards' message interrupts go
 * to cores other than the boot core, one card per core in turn (the boot
 * core keeps the others and the tick); logs a line per card moved. */
void pci_spread_interrupts(void);

#endif
