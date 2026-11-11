/*
 * USB mass storage: sticks and card readers, in the "bulk-only transport"
 * (BOT) with SCSI commands.
 *
 * Every command is a SCSI command block wrapped in a 31-byte Command Block
 * Wrapper ("USBC") sent on the bulk OUT endpoint; the data follows on the
 * bulk IN or OUT endpoint; a 13-byte Command Status Wrapper ("USBS") on
 * the bulk IN endpoint says whether it worked. The disk becomes a block
 * device (usbdisk0, usbdisk1, ...), so partitions and RitterFS work as on NVMe.
 */
#ifndef RITTEROS_USBSTORAGE_H
#define RITTEROS_USBSTORAGE_H

#include <stdbool.h>
#include "blockdev.h"
#include "usb.h"

/* Set up the mass storage interface of a configured device. */
bool usbstorage_attach(struct usb_device *dev, const struct usb_interface_descriptor *interface,
                       const struct usb_endpoint_descriptor *in,
                       const struct usb_endpoint_descriptor *out, unsigned in_burst,
                       unsigned out_burst);

/* The device was unplugged: its disk fails every access from now on (the
 * structures stay: partitions and file systems may still point to it). */
void usbstorage_detach(struct usb_device *dev);

/* From now on, disks plugged in are checked right away (partitions,
 * RitterFS): the boot has done its own disks. */
void usbstorage_start_hotplug(void);

/* The USB disks found so far (unplugged ones too). */
int usbstorage_count(void);
struct block_device *usbstorage_disk(int index);

#endif
