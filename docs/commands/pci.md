# pci: list the PCI devices

```
pci
```

Lists the devices the kernel found on the PCI bus at the boot: the
controllers on the motherboard and in its slots (disks, USB, network,
graphics, sound). The first line counts them; then each device has a line
with its place (bus:device.function), its vendor and device IDs, what kind
of device it is and, for well-known vendors, the vendor's name, followed by
a line for each memory or I/O region (BAR) it has, with its address and
size. The text is the kernel's own report, the same the rescue console's
`pci` command prints. `pci` has been a program since step 11d.

The IDs are what to look up when a device is not recognised: `10ec:8168`
is a Realtek RTL8111/8168 network chip, `8086:10c9` an Intel 82576.

`pci` takes no options; arguments are ignored.

## Exit status and messages

- `0`: the list was printed.
- `1`: the kernel did not answer (nothing is printed).

## Examples

```
ritteros:/$ pci
6 PCI devices:
    00:00.0 8086:29c0  host bridge              Intel
    00:01.0 1234:1111  VGA display controller   QEMU/Bochs
               BAR0 mem32 0xc0000000, 16384 KiB, prefetchable
               BAR2 mem32 0xc1084000, 4 KiB
    00:02.0 1b36:0010  NVMe controller          Red Hat (QEMU)
               BAR0 mem64 0xc1080000, 16 KiB
    00:1f.0 8086:2918  ISA bridge               Intel
    00:1f.2 8086:2922  SATA controller (AHCI)   Intel
               BAR4 I/O  0x6040, 32 bytes
               BAR5 mem32 0xc1085000, 4 KiB
    00:1f.3 8086:2930  SMBus controller         Intel
               BAR4 I/O  0x6000, 64 bytes
ritteros:/$ pci | grep Ethernet
```

## See also

[`usb`](usb.md), [`net`](net.md), [`disks`](disks.md),
[`mem`](mem.md)
