# osinstall: install RitterOS on another disk, the whole disk (root; it asks)

```
osinstall
```

Installs RitterOS from the disk it is running from (usually the stick) on
another disk, such as a PC's SATA SSD or NVMe drive, using the whole disk.
It came with step 48a; the firmware boot entry with 48c. Only root may run
it. It takes no options and asks its questions one by one:

1. It lists every disk except the one the system runs from (its number,
   name, size, kind, model and what it holds now) and asks which one, by
   its number or its name. Enter (or Ctrl-D) stops it.
2. It shows the new partition table and what is lost, lists the disk's
   mounted volumes, and asks you to type the disk's name. Anything else
   stops it. Nothing is written before this answer.

Then it unmounts the disk's volumes and writes:

- a new GPT partition table, with new random identifiers (GUIDs), so the
  system can tell the new disk from the stick when both are plugged in;
- partition 1: an EFI system partition of 64 MiB (FAT32, label `EFI`) with
  the bootloader for this machine (`BOOTX64.EFI` or `BOOTAA64.EFI`) at the
  path firmware starts by itself, `\EFI\BOOT\`, and the kernel command line
  `\ritteros\cmdline.txt` (of the running one only `noruntime` and `video`
  go along);
- partition 2, the rest: a RitterFS `RitterOS` with `/system/<arch>` (the
  kernel, the programs in `bin` and `apps`, the libraries in `lib`, the
  kernel's modules in `drivers` (network, Wi-Fi, sound, graphics; until
  42b's fix they were left out: install them by hand on a machine set up
  before it); this
  machine's architecture only, without the test programs; setuid programs
  stay setuid), `/system/fonts`, `/system/certs`, `/system/firmware`,
  `/system/include`, `/system/sdk` and `/system/doc` where the running
  system has them, `/system/config/README` (no settings files: since
  37e-1b the new system writes its own with their defaults at its first
  start), the links `/bin`, `/usr/bin`, `/lib` and `/etc`,
  and `/tmp` (mode 1777).

Nothing else is copied: no users, passwords, settings, homes or logs. The
installed system starts like a new stick: it asks for root's password
first.

Last, `osinstall` adds an entry `RitterOS` to the firmware's boot menu (a
variable the firmware keeps across restarts) and puts it first in the boot
order: some firmware starts an internal disk only through such an entry
(the J1900's did). Installing again reuses the entry. If the firmware
cannot keep variables, `osinstall` says so and the disk may still start
through the fallback path. To start the stick while the entry comes first,
choose the stick in the firmware's boot menu.

## Exit status and messages

- `0`: RitterOS is installed: `RitterOS is installed on DISK. Remove the
  stick and restart (reboot); the firmware starts it from DISK, and its
  first start asks for root's password.` A missing boot entry does not
  change this (`osinstall: no firmware boot entry: REASON`, with `(no
  runtime services)` when the kernel was started with `noruntime`; or
  `osinstall: the firmware's boot entry BootNNNN 'RitterOS' written, but
  not put in its boot order: REASON`).
- `1`: nothing was installed:
  - `Nothing written.`: Enter at the first question, or not the disk's
    name at the second.
  - `osinstall: only root may install`
  - `osinstall: no other disk to install on`
  - `osinstall: /boot/EFI/BOOT/BOOTX64.EFI: No such file or directory (the
    bootloader to copy)`: the system's EFI partition is not at `/boot`.
  - `osinstall: DISK: SIZE is too small (at least 256 MiB)`
  - `osinstall: DISK: blocks of N bytes`: a block size it cannot use.
  - `osinstall: DISK: unmounting PATH: REASON`, `osinstall: DISK: writing
    the partition table: REASON`, `osinstall: PART: writing: REASON`,
    `osinstall: copying PATH: REASON`, `osinstall: DISK: the new partitions
    did not appear`, `osinstall: PART: the new RitterFS is not mounted`:
    the disk could not be written as planned (it may be half written: run
    `osinstall` again).

A typed answer that is not one of the disks is `'ANSWER' is not one of the
disks above.`, and the question comes again.

## Examples

```
ritteros:/$ osinstall
osinstall: installs RitterOS (x86_64) from usbdisk0 on another disk, the whole disk.
Disks:
  1  sata0         223.5 GiB  SATA  (the disk's model): GPT, 2 partitions
Which disk (its number or name; Enter to stop)? 1
sata0 (223.5 GiB) gets a new partition table:
  sata0p1  64.0 MiB, EFI system partition, FAT32 'EFI' (the bootloader)
  sata0p2  about 223.4 GiB, RitterOS, RitterFS 'RitterOS' (the system)
Everything on sata0 is lost (now: GPT, 2 partitions).
Type the disk's name (sata0) to install: sata0
osinstall: sata0: a new GPT partition table
osinstall: sata0p1: FAT32 'EFI', 64.0 MiB
osinstall: sata0p2: RitterFS 'RitterOS', 223.4 GiB
osinstall: the bootloader copied (\EFI\BOOT\BOOTX64.EFI, 37888 bytes), command line ""
osinstall: /system/x86_64 copied: the kernel and 49 programs
osinstall: the firmware's boot entry Boot0003 'RitterOS', first in its boot order
RitterOS is installed on sata0. Remove the stick and restart (reboot); the firmware starts it from sata0, and its first start asks for root's password.
```

## See also

[`disks`](disks.md), [`format`](format.md), [`mount`](mount.md),
[`unmount`](unmount.md), [`reboot`](reboot.md), [`passwd`](passwd.md)
