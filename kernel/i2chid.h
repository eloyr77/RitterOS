/*
 * HID over I2C (15s-15u): touchpads and other input devices that ACPI
 * describes as PNP0C50 on an I2C controller. Found at the boot from the
 * ACPI tables (read without running AML) and confirmed by the device's own
 * HID descriptor.
 */
#ifndef RITTEROS_I2CHID_H
#define RITTEROS_I2CHID_H

/* Find the HID-over-I2C devices and set them up (after the PS/2 devices). */
void i2chid_init(void);

#endif
