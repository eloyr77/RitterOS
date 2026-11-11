# Installing RitterOS

How to put RitterOS on a USB stick, start a PC from it, and install it on
the PC's own disk. On RitterOS itself this guide is `man install`; every
program has its page too (`man NAME`, `man` lists them).

## What you need

- **A PC with UEFI firmware** and an x86-64 processor (Intel or AMD; most
  PCs since about 2012). RitterOS has run on an Intel Celeron J1900 board
  and on an AMD laptop. 4 GiB of memory or more.
- **Secure Boot switched off** in the firmware's settings: RitterOS's
  bootloader is its own and not signed, so firmware with Secure Boot on
  refuses to start it. The old BIOS mode (CSM) is not needed.
- **A USB stick of 256 MB or more.** Everything on it is lost. At the
  first start RitterOS grows its file system to the whole stick.
- **The stick image**: from the [Releases](https://github.com/eloyr77/RitterOS/releases)
  page, `ritteros-2026.11-x86_64.img.gz` (uncompress it with `gunzip`
  first; Rufus and balenaEtcher take the `.gz` as it is), or made from the
  source (below) as `stick-STEP-x86_64.img`; 256 MiB either way. An image
  for 64-bit ARM (`ritteros-2026.11-aarch64.img.gz`) runs in QEMU and in
  UTM on Apple Silicon Macs; real ARM machines are not supported yet.

### Making the image from the source

On a Mac with the project built (`make`):

```
make
python3 scripts/mkdisk.py --arch x86_64 --size 256 build/stick-mine-x86_64.img build/esp build/system
```

`--size 256` makes a 256 MiB image (without it, 1 GiB). Always make the
stick as a new file (delete an old one first): mkdisk keeps the RitterFS
of an image that is there, and a stick that was booted already (`make
run` boots `build/ritteros-x86_64.img` and stores root's password and
settings in it) skips the first start, so you could not log in. A fresh
image's `/system/config` holds only `README` (check with `python3
scripts/ritterfs.py ls IMAGE 133120 389120 /system/config` for a 256 MiB
image, `133120 1961984` for 1 GiB: no `passwords` file).

## Writing the stick

Find the stick's disk first, and check it twice: writing to the wrong disk
destroys what is on it.

**macOS** (in the Terminal app):

```
diskutil list external physical
diskutil unmountDisk /dev/diskN
sudo dd if=stick-STEP-x86_64.img of=/dev/rdiskN bs=4m
diskutil eject /dev/diskN
```

`N` is the stick's number from the list (its size tells it apart). Use
`rdiskN`, not `diskN`: it is many times faster.

**Linux:**

```
lsblk
sudo umount /dev/sdX?*
sudo dd if=stick-STEP-x86_64.img of=/dev/sdX bs=4M conv=fsync status=progress
```

`sdX` is the whole stick (`sdb`, not `sdb1`).

**Windows:** use a tool that writes a raw disk image, such as Rufus in its
"DD image" mode or Win32 Disk Imager, and choose the stick as the target.

## Starting from the stick

1. Plug the stick in (any USB socket) and switch the PC on.
2. Open the firmware's boot menu: a key at the very start, often F12, F11,
   F8 or Esc (the PC's manual or its first screen says which), and choose
   the stick (its UEFI entry, if it is listed twice).
3. The screen shows small gray squares in its top left corner while the
   bootloader works, then RitterOS's boot messages, then `login:`.

A USB 3 stick can take a few seconds to connect: the boot then says
`system partition: not found yet; waiting for USB disks (at most 10 s)`
and goes on as soon as it is there.

## The first start

A new disk first asks for a password for `root`, the administrator:

```
RitterOS: first start. Choose a password for root, the administrator.
New password for root:
Again:
The password for root is set.

login:
```

On a stick bigger than the image, the first start then grows RitterOS's
partition and file system to the end of the stick (`disks grow`, since
68j):

```
disks: usbdisk0p2 grows from 190.0 MiB to 28.8 GiB, to the end of usbdisk0
  usbdisk0p2: grown to 60414976 blocks (29499 MiB), still mounted at /
  RitterFS:      usbdisk0p2 grown from 48640 to 7551744 blocks of 4 KiB (29499 MiB)
disks: the RitterFS on usbdisk0p2 now has 28.8 GiB (7551744 blocks of 4 KiB)
```

Then log in as `root` with that password. The shell starts in
`/home/root`; `help` lists the programs, `man NAME` shows a program's
page.

## Installing on the PC's disk

From the stick, `osinstall` copies RitterOS onto an internal disk (a SATA
SSD or an NVMe drive), using the whole disk:

```
ritteros:/home/root$ osinstall
osinstall: installs RitterOS (x86_64) from usbdisk0 on another disk, the whole disk.
Disks:
  1  nvme0n1       465.7 GiB  NVMe  'Sandisk Optimus 5100 500GB': GPT, 3 partitions
Which disk (its number or name; Enter to stop)? 1
nvme0n1 (465.7 GiB) gets a new partition table:
  nvme0n1p1  64.0 MiB, EFI system partition, FAT32 'EFI' (the bootloader)
  nvme0n1p2  about 465.6 GiB, RitterOS, RitterFS 'RitterOS' (the system)
Everything on nvme0n1 is lost (now: GPT, 3 partitions).
Type the disk's name (nvme0n1) to install: nvme0n1
...
RitterOS is installed on nvme0n1. Remove the stick and restart (reboot); the firmware starts it from nvme0n1, and its first start asks for root's password.
```

- Enter at the first question stops it. Nothing is written until you type
  the disk's name.
- **Everything on that disk is lost**, other systems included: RitterOS
  takes the whole disk.
- It adds a boot entry `RitterOS` to the firmware and puts it first, and
  also puts the bootloader where firmware looks by itself
  (`\EFI\BOOT\BOOTX64.EFI`), so the PC starts RitterOS without the stick.
- Users, passwords and settings are not copied: the installed system's
  first start asks for root's password again.

Then `reboot`, and take the stick out when the screen goes dark. To start
the stick again later, choose it in the firmware's boot menu.

## After installing

**The network.** A cable is used at once: every card asks for its address
by DHCP (`net` shows the cards, their addresses and the routes). Each card
keeps its name (`eth0`, `wlan0`, ...) by its MAC address
(`/system/config/network/names`; `man net`).

**Wi-Fi:**

```
ritteros:/home/root$ wifi country US
ritteros:/home/root$ wifi scan
ritteros:/home/root$ wifi join MyNetwork "my passphrase"
```

The elf `wifid` joins the network and keeps it joined, also after a
restart. `wifi status` shows the link (`man wifi`).

**Logging in from another machine.** The SSH server `sshd` runs on every
machine with a network card. Find the address with `net`, then from the
other machine:

```
ssh root@192.168.1.20
```

The firewall (the elf `guardd`) lets in SSH (TCP port 22), DNS (UDP port
53, for this machine's `dnsd`) and ping; anything else that comes in is
dropped until a rule opens it (`man guard`). What the machine sends out is
allowed.

**Changing a setting.** The system's settings are plain text files in
`/system/config` (its `README` lists them); `edit FILE` changes one, and
`net up` applies the network's.

## Installing a newer version

There is no update in place yet: write a newer stick image and run
`osinstall` again. The disk is erased, so copy your own files off first:
onto a USB stick (`cp -r /home/root /volumes/STICK/`), or from another
machine over SSH (`ssh root@ADDRESS 'tar cf - /home/root' > home.tar`).

Do not replace the system's own files one by one with a shell redirection
(`cat new > /system/x86_64/lib/libritteros.so.1`): the shell empties the
target before the copying program starts, and a program or library that
the copy itself needs is then gone (nothing starts any more). Copy with
`cp NEW OLD`, which loads what it needs first.

## When it does not start

- **Nothing but the firmware's own screen, or "no bootable device":**
  Secure Boot is on, or the stick was not chosen in the boot menu, or the
  firmware is in BIOS (CSM) mode only.
- **The gray squares stop and a red one appears:** the bootloader failed
  at that stage (1 the display, 2 leaving the firmware, 3 moving its
  runtime services, 4 starting the kernel). Some firmware hangs moving
  its runtime services: add `noruntime` to the kernel command line,
  `\ritteros\cmdline.txt` on the stick's EFI partition (any computer can
  edit it: it is FAT32).
- **`rescue>` instead of `login:`:** the system could not start (no system
  partition found, or `login` could not run). The rescue console's `help`
  lists its commands; `mem`, `pci`, `usb` and `ls` show what the machine
  found. See the manual's section 11 (`man manual`).
- **A stick made from a used image** skips the first start and accepts
  no password: write it again from a fresh image.
