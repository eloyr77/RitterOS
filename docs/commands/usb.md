# usb: list the USB devices

```
usb
```

Lists the USB devices the kernel has set up, in the order they were
plugged in: a line with their number, then one line per device with its
place (controller and port; behind hubs the hub's port after a dot), its
vendor and product IDs, its speed, what it is for the kernel (hub,
keyboard, mass storage, ...; `-` for a device no driver took) and the
manufacturer's and product's names the device reports. It is the kernel's
own report, the same that the rescue console's `usb` command prints; a
program since step 11d. Devices plugged in or pulled out later are added
and removed (hot plug, since steps 16a and 16b).

After the devices, each USB controller adds its counters (since 37g-8),
the same line the boot ends with: the commands it was given, the events
it reported (port changes among them), its interrupts, and the events the
timer tick found before an interrupt did. Many found by the tick mean its
interrupts are lost, and every transfer then waits for a tick (10 ms).
After a sleep it also says how many devices it kept over the sleeps (as
the same device: a stick the system runs from stays mounted) and how many
it let go of as unplugged, with the reason for the last one.

`usb` takes no options; arguments are ignored.

## Exit status and messages

- `0`: the list was printed.
- `1`: the kernel gave no report.

## Examples

```
ritteros:/$ usb
5 USB device(s):
    usb0 port 5      0409:55aa  full speed (12 Mbit/s)   hub, 'QEMU' 'QEMU USB Hub'
    usb0 port 5.1    0409:55aa  full speed (12 Mbit/s)   hub, 'QEMU' 'QEMU USB Hub'
    usb0 port 5.1.1  0627:0001  full speed (12 Mbit/s)   HID keyboard (boot interface), 'QEMU' 'QEMU USB Keyboard'
    usb0 port 8      0627:0001  high speed (480 Mbit/s)  HID keyboard (boot interface), 'QEMU' 'QEMU USB Keyboard'
    usb0 port 3      46f4:0001  SuperSpeed (5 Gbit/s)    mass storage (bulk-only), 'QEMU' 'QEMU USB HARDDRIVE'
  usb0:          26 commands, 106606 events (9 port changes), 106536 interrupts, 0 events found by the tick instead
ritteros:/$ usb | grep storage
    usb0 port 3      46f4:0001  SuperSpeed (5 Gbit/s)    mass storage (bulk-only), 'QEMU' 'QEMU USB HARDDRIVE'
```

A laptop's controller after a sleep (the VivoBook, which runs from a
USB stick, at 37g-8):

```
ritteros:/$ usb | grep usb0:
  usb0:          33 commands, 90895 events (7 port changes), 90890 interrupts, 13 events found by the tick instead
  usb0:          over 1 sleep(s): 2 device(s) kept, 0 let go of
```

The speeds are `low speed (1.5 Mbit/s)`, `full speed (12 Mbit/s)`, `high
speed (480 Mbit/s)`, `SuperSpeed (5 Gbit/s)` and `SuperSpeed+ (10
Gbit/s)`.

## See also

[`pci`](pci.md), [`disks`](disks.md) (USB sticks as `usbdisk0`, ...),
[`net`](net.md) (USB network adapters)
