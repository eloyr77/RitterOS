# drivers: list the kernel's modules (drivers): loaded, not needed or refused

```
drivers
```

Prints the kernel's report on its modules (since step 67a, roadmap item
34), the same as the rescue console's `drivers` command. A module is a
driver kept as a file in `/system/<arch>/drivers/` (`NAME.mod`) instead
of inside the kernel: after the system's disk is mounted, the kernel reads
each one and loads it only when its hardware is there (a device on PCI
whose IDs, or class, are in the module's table), so a machine carries only the
drivers it needs. A module stays loaded until the machine restarts.

The first line counts the module files, the ones loaded, and the names
the kernel lets modules use (the functions of its driver interface, 67b:
`drivers/include/` in the sources, `/system/include/drivers/` on the
machine). Then one line per file:

- loaded: its name, where in memory it lies, its code, read-only data and
  data, the file's size, how many PCI IDs its table lists, and the devices
  it took (bus address, vendor:device);
- `not needed`: none of its hardware is here (nothing was kept in memory);
- `refused`: why it could not be loaded (not a module for this
  processor, made for another version of the driver interface, a name the
  kernel does not export to modules, a relocation the loader does not
  know...): `made for the driver interface 1.9, this kernel has 1.0`.

The boot's log says the same in one line: `modules: rtl8168 (00:04.0,
00:05.0, 00:06.0) loaded; 0 not needed`. With `nomodules` on the kernel command
line no module is loaded (for a machine where a driver misbehaves): the
report then says so.

So far the network cards' drivers are modules: Realtek RTL8111/8168 and
RTL8125 (`rtl8168`), Intel 8254x (`e1000`, QEMU's card), Intel 82576
(`igb`), MediaTek MT7921 Wi-Fi (`mt7921`), the tests' Wi-Fi card
(`ritterwifi`) and USB network adapters of the CDC-NCM class (`usbnet`);
so are the graphics drivers (67f): Intel Bay Trail (`intelgpu`) and AMD
(`amdgpu`), and the sound driver (67g): HD Audio of any maker (`hda`,
found by its PCI class, 04.03). The other drivers are still part of the
kernel. A USB module (67e) is
always loaded, since its devices may be plugged in later: until one is,
the boot line says `usbnet (no device yet)` and `drivers` `took no device
yet`; then it names the devices it took, by their USB port. Drivers for
other hardware can be written outside RitterOS's sources: the guide is
`drivers/CLAUDE.md`, and `drivers/template/` is a driver to start from.

## Exit status

- `0`: the report was printed.
- `1`: the kernel did not give it.

## Examples

On QEMU (ARM) with three Realtek cards:

```
ritteros:/$ drivers
1 module(s) in /system/aarch64/drivers, 1 loaded; the kernel exports 63 names to them
  rtl8168.mod      rtl8168 at 0xffffffff82000000: code 10 KB, read-only 2 KB, data 0 KB (file 28 KB); 6 PCI IDs; took 00:04.0 (10ec:2600) 00:05.0 (10ec:8168) 00:06.0 (10ec:3000)
```

On a machine without a Realtek card:

```
ritteros:/$ drivers
1 module(s) in /system/aarch64/drivers, 0 loaded; the kernel exports 63 names to them
  rtl8168.mod      not needed: none of its 6 PCI IDs is here (file 28 KB)
```

## See also

[`net`](net.md), [`displays`](displays.md), [`cpus`](cpus.md)
