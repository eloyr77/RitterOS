/*
 * USB serial ports in the CDC ACM class (step 50e): the serial console of a
 * device such as the PlutoSDR, one function of its composite gadget, or a
 * USB modem. Each is `usbserialN` (a device plugged in again gets its name
 * back) and /dev/usbserialN for programs (`cu`). Bytes are kept in a ring
 * from the moment the port is found: a program opening it later still sees
 * what came before (up to the ring's size).
 *
 * The receive thread keeps a read pending on the bulk IN endpoint; writes
 * go out on the bulk OUT one. The line is set to 115200 8N1 with DTR and
 * RTS on (a gadget's port ignores the speed).
 */
#ifndef RITTEROS_USBSERIAL_H
#define RITTEROS_USBSERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "usb.h"

struct usbserial;

/* Set up the ACM function of a configured device: its control interface
 * (for the line settings) and its data interface's bulk endpoints. */
bool usbserial_attach(struct usb_device *dev, uint8_t control_interface,
                      const struct usb_endpoint_descriptor *in,
                      const struct usb_endpoint_descriptor *out, unsigned in_burst,
                      unsigned out_burst);

/* The device was unplugged: reads end (0, the end of the input) once the
 * bytes already there are taken, writes fail. */
void usbserial_detach(struct usb_device *dev);

/* The port usbserialN as it is now (0 if there is none); an open file keeps
 * its port, also after an unplug. */
struct usbserial *usbserial_find(int number);

/* Up to size bytes into buf (a kernel buffer): waits until there are some,
 * unless the calling thread does not wait (-ERR_AGAIN); -ERR_INTR on a
 * signal; 0 once the port is gone and its bytes are taken. */
int64_t usbserial_read(struct usbserial *s, uint8_t *buf, size_t size);

/* Send size bytes from buf; -ERR_IO if the device does not take them. */
int64_t usbserial_write(struct usbserial *s, const uint8_t *buf, size_t size);

/* POLLIN when there are bytes (or the port is gone), POLLOUT while it is there. */
int usbserial_poll(struct usbserial *s);

/* Whether usbserialN is there now (for /dev). */
bool usbserial_present(int number);

#endif
