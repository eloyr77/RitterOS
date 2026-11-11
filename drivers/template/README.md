# example: the template of a RitterOS driver

A kernel module built outside RitterOS's tree. It drives QEMU's PCI test
device (`-device pci-testdev`): it maps the device's first BAR, reads a
register and logs a line. Copy this folder to start a driver of your own.

```sh
make RITTEROS=/path/to/RitterOS              # build/x86_64/example.mod
make RITTEROS=/path/to/RitterOS ARCH=aarch64 # build/aarch64/example.mod
```

Copy the `.mod` to `/system/ARCH/drivers/` on RitterOS (as root) and
restart: the kernel loads it when its hardware is there. `drivers` shows
it loaded, not needed, or refused and why. Read `drivers/CLAUDE.md` first:
the driver interface, its rules and how to test a driver.
