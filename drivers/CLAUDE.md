# Writing a RitterOS driver

A driver for RitterOS is a **kernel module**: one file, `NAME.mod`, in
`/system/<arch>/drivers/` (`x86_64` or `aarch64`). After the system's disk
is mounted the kernel reads every module there and keeps the ones whose
hardware is present (a PCI device in the module's table); the others cost
nothing. A module stays loaded until the machine restarts. `drivers` (or
the rescue console's `drivers`) lists each one: loaded (where, how large,
which devices it took), not needed, or refused and why. `nomodules` on the
kernel command line loads none (for a machine where a driver misbehaves).

This guide is for anyone writing one, inside RitterOS's tree or outside it.
Read it whole before you start.

## The interface

A driver sees exactly one thing of the kernel: the **driver interface** in
`drivers/include/`.

- `driver.h`, the general part: logging, memory and DMA pages, device
  registers, PCI, time, threads, locks, waiting, and the module's own
  declaration (`MODULE`). Its opening comment and the comments on each
  function are the reference.
- One header per **device class**, the contract between a kind of device
  and the part of the kernel that uses it: `driver_net.h` (network
  cards), `driver_wifi.h` (Wi-Fi cards), `driver_usb.h` (USB functions),
  `driver_gpu.h` (graphics), `driver_sound.h` (sound).

A driver includes only these. It is compiled with `drivers/include/` as its
only include folder, so a kernel header is not even in reach, and the
build checks every name the module leaves undefined against the functions
the interface declares (`scripts/kexports.c`): using anything else stops
the build (`kexports: ... uses NAME, which the driver interface does not
declare`).

The interface has a version, `DRIVER_API_MAJOR.DRIVER_API_MINOR` (now 6.1:
65e-44 gave `struct wait_queue` its own lock, so its layout changed; 37e-2a
added `acpi_pci_method`; 37e-2b gave `struct gpu_card` its `backlight`,
37e-2c `brightness` and `brightness_now`; 37g-2 `MODULE`'s `suspend` and
`resume`; 37g-5c-2 `struct gpu_card`'s `report_sleep`: 6.0; 38b `struct
net_card`'s `set_promiscuous`, at its end, recorded as agreed: 6.1),
which every module carries. The kernel refuses a module built for another
major version, or a newer minor one, than its own (`made for the driver
interface 1.9, this kernel has 1.0`). `drivers/abi/driver-api.N.txt` records
the interface (every function, macro, type and constant; checked by
`scripts/abicheck.py`): adding to it is compatible and raises the minor
version; removing or changing anything breaks the modules built before and
needs a new major version. So the interface only grows, carefully: if your
driver needs something it lacks, that is a change to RitterOS (proposed,
agreed, added with its minor version raised and recorded with
`python3 scripts/abicheck.py --update`), never a way around it.

## A module's shape

At its end, after everything else:

```c
static bool start(struct pci_device *pci)
{
    ...                                 /* set the device up; false if you did not take it */
}

static const struct module_pci ids[] = {
    { 0x10EC, 0x8168 },                 /* vendor, device (MODULE_ANY: any of the vendor's) */
};                                      /* or MODULE_CLASS(class, subclass): anyone's */

MODULE(rtl8168, start, suspend, resume, ids)     /* (5.0) 0, 0 if it cannot sleep yet */
```

- The kernel calls `start` once for every PCI device whose IDs are in the
  table, in bus order, from its boot thread. A device class that every
  maker builds to one standard (HD Audio, 04.03) is matched by its class:
  `MODULE_CLASS(0x04, 0x03)`; check the class again in `start`. `start` sets the device up
  (or decides it is not one it can drive, and says so in the log) and
  returns whether it took it. A module whose table matches no device is
  not kept at all.
- (5.0) `suspend` and `resume` are for sleep (ACPI S3, where only memory
  keeps its contents): before it, for every device the module took (the
  last first), `suspend(pci)` stops the device's work and keeps in memory
  what it needs again; after it, in the order taken, `resume(pci)` sets
  the device up as before (its registers, rings and interrupts: the
  kernel has put its PCI configuration back). `suspend` returning false
  calls the sleep off. With 0, 0 a machine with one of the module's
  devices does not sleep (the log names the module).
- The name in `MODULE` (at most 15 characters) is the module's name in
  `drivers` and in the boot line; use it, followed by `:`, at the start of
  every log line the driver prints (`rtl8168: ...`, or the device's own
  name once it has one: `eth0: ...`).
- A module is one relocatable object. Several C files are fine
  (`driver.mk` joins them); its global names are its own, but keep them
  `static` anyway.

## Device classes

### Network cards (`driver_net.h`)

The network stack is the same for every card: a driver says what its card
is and how to send, and hands every received frame on. How a chip does it
is the driver's own business.

1. `net_card_new()` gives a zeroed `struct net_card` (never make one
   yourself: the stack keeps its own state with it). Fill in `mac`,
   `driver` (the module's name), `model`, `where` (the PCI address,
   `"00:03.0"`), `pci`, `priv` (your own state), and the operations:
   `link_speed` (Mbit/s, 0: no link), `send` (false when the ring is
   full), optionally `details` (more lines for `net`'s report),
   `set_multicast` (set the chip's filter so at least those groups come
   in) and (6.1) `set_promiscuous` (every frame, whatever its destination,
   or again only the card's own: a card without it cannot be a bridge's
   port; keep it on across the chip's own resets, as `rtl8168.c` does).
2. When the chip works, `net_register(card)`: the stack names it (`eth0`,
   ...), prints its line, watches its link and sends through it.
3. Every received frame goes to `net_receive(card, frame, length)` from a
   thread of the driver's own (`thread_create`), never from an interrupt
   handler. Mark a burst of frames with `net_batch_begin()` and
   `net_batch_end()` (at most `NET_BATCH_MAX` frames): the stack then
   acknowledges once per connection at the burst's end.
4. Count what the chip did in `rx_frames`, `rx_bytes`, `tx_frames`,
   `tx_bytes`, `tx_dropped`.

`drivers/rtl8168.c` (Realtek RTL8111/8168 and RTL8125) is a complete
example: rings, interrupts by MSI, a receive thread, a send watchdog on the
timer's tick, the chip's own counters in `details`. A send that would wait
for room must not wait on the thread that is handing a frame up
(`net_receiving()` says so): the stack may send from inside `net_receive`.

### Wi-Fi cards (`driver_wifi.h`)

A Wi-Fi card is a network card whose frames are 802.11's. The kernel's
Wi-Fi layer turns Ethernet frames into 802.11 and back (with CCMP under the
keys the elf `wifid` sets) and hands management frames to `wifid`, which
scans, joins and runs the handshakes. A driver moves 802.11 frames and
tunes channels; what its chip's firmware does itself it offers through the
optional operations.

1. Keep a `struct wifi_card` (in your own state): `transmit` (one 802.11
   frame, without its FCS), `set_channel`, the `channels` the card has and
   `nchannels`; optionally `scan` (a scan in the firmware: hand the beacons
   up with `wlan_receive`, end it with `wlan_scan_done`), `set_country`,
   `join`, `set_key` (true: the card encrypts and decrypts with that key
   from now on; hand its decrypted frames up with
   `wlan_receive_decrypted`), `block_ack`, `leave`.
2. Fill in a `struct net_card` from `net_card_new` as for a wired card, but
   leave `send` and `link_speed` alone (they are the layer's).
3. `wlan_register(card, &wifi)`: the card is named `wlan0`, ... .
4. Every received frame goes to `wlan_receive(card, frame, length, channel,
   signal)` from the driver's thread; a firmware that lost the access point
   says so with `wlan_link_lost`.

`drivers/ritterwifi.c` (our QEMU model) is the smallest Wi-Fi driver;
`drivers/mt7921.c` (MediaTek MT7921) a real one, with its firmware loaded
from `/system/firmware/` by `firmware_load`.

### USB functions (`driver_usb.h`)

A USB module drives one function of a USB device (a network adapter, ...)
that the kernel's USB core has found and addressed; the core keeps its own
drivers for hubs, keyboards, mice, storage and serial ports. Instead of
`MODULE` it ends with

```c
static const struct module_usb ids[] = {
    { USB_ANY, USB_ANY, 0x02, 0x0D, USB_ANY },  /* vendor, product, interface class, subclass, protocol */
};

USB_MODULE(usbnet, attach, detach, ids)
```

- A USB module is always kept: its devices may come later. It is offered
  every device found before it was loaded and every one plugged in after,
  when the device's IDs and one of its interfaces fit an entry (`USB_ANY`:
  any value). The device's configuration is selected before.
- `attach(dev)` reads the configuration itself (`usb_configuration`: the
  interfaces, their endpoints and class descriptors, as the device sent
  them), finds its interfaces and endpoints (and a SuperSpeed endpoint's
  burst in its companion descriptor), sets its function up with
  `usb_control` and the bulk and interrupt functions, and returns whether
  it took the device. `detach(dev)` comes when it is unplugged: its
  transfers fail from then on; end the driver's threads and keep whatever
  the user sees (a network card stays listed, without a link).
- `usb_device_name` (`"usb0 port 5.1"`, the start of its log lines with
  `usb_log_label`), `usb_device_product`, `usb_device_descriptor` and
  `usb_string` tell what the device is; `usb_report` adds its host
  controller's lines to the driver's own report.

`drivers/usbnet.c` (CDC-NCM network adapters: the PlutoSDR's) is the
example: it finds its control and data interfaces in the configuration,
and a card plugged in again (the same MAC) gets its name back
(`net_rename`).

### Graphics (`driver_gpu.h`)

The kernel's graphics layer is the same for every chip: it names the
device (`gpu0`) and the outputs across all devices (`vga0`, `hdmi0`,
`dp0`, `edp0`), watches every output for monitors every 2 s, prints
`displays`, and runs the system calls (a mode, the vertical blank,
flipping screen buffers) through the driver's operations.

1. Keep a `struct gpu_card`: `model`, `pci`, the outputs (`kind` and
   `where`, from what the chip and its firmware report: Intel's VBT,
   AMD's AtomBIOS, ...), `priv`, and the operations the chip can do:
   `read_edid` (an output's EDID: over its DDC wires, or AUX on
   DisplayPort), `showing` (which output shows the picture), `report`
   (your lines in `displays`: the state as read back, counters),
   `set_mode` (the whole chain: the new screen in a `struct gpu_screen`),
   `wait_vblank` (the vertical blank interrupt; give up when
   `thread_stop_requested()`), `flip` and `buffers` (screen buffers),
   (3.0) `backlight` (a panel's backlight off when the screen goes off, on
   again as it was; an external monitor's signal, so it goes to standby;
   also called at a crash, so no lock and no sleep),
   (4.0) `brightness` and `brightness_now` (a panel's level of 0xffff, set
   no lower than its firmware allows, and read), (6.0) `report_sleep`
   (your lines in `displays sleep`: what you kept over the last sleep). Leave
   0 what the chip cannot do: the layer and the programs fall back (windowd
   draws on its timer and copies).
2. `gpu_register(&card)` once the chip is understood; the layer then calls
   the operations with that card. `gpu_output_name` and
   `gpu_output_monitor` (the EDID, parsed, of a connected monitor) tell a
   driver what the layer knows.

`drivers/intelgpu.c` (Bay Trail, the J1900's) and `drivers/amdgpu.c` (AMD
Cezanne, its video BIOS from the ACPI table VFCT: `acpi_find_table`; the
firmware's ATIF interface under the device's ACPI node: `acpi_pci_method`)
are the examples.

### Sound (`driver_sound.h`)

The kernel's sound layer numbers the cards as the sound devices
`/dev/pcm0`, `pcm1`, ...: those with speakers or headphones first, then
line outs, digital outputs (HDMI) last, in the order registered within a
kind; programs write 48 kHz 16-bit stereo PCM (`SOUND_RATE`,
`SOUND_BITS`, `SOUND_CHANNELS`) to them.

1. Keep a `struct sound_card`: `driver`, `pci`, `kind` (`SOUND_SPEAKERS`,
   `SOUND_LINE_OUT`, `SOUND_DIGITAL`: from what the chip says its outputs
   are wired to), `outputs` (the boot line's description), `priv`, and the
   operations `write` (take the PCM, waiting while your buffer is full;
   start playing if stopped) and `drain` (play out what was written, then
   stop with silence: the player closed the device).
2. `sound_register(&card)` once the chip plays.

`drivers/hda.c` (Intel HD Audio, any maker's: QEMU's intel-hda, the
laptop's AMD controllers) is the example.

## Rules

These are RitterOS's rules for all kernel code; a driver runs in the
kernel, so they are its rules too.

- **Ask the hardware.** What the device or the firmware can report (its
  IDs and revision, capabilities, the BARs' sizes, a version register, the
  firmware's tables) is read, not assumed. A driver's own tables are only
  for what nothing reports (register layouts, the order of steps, a
  formula), and they say so in a comment.
- **No unbounded waits on hardware.** Every loop that waits for a device
  has a limit and a way on: real hardware may be missing, half there or
  never answer. Count what went wrong (timeouts, errors) and show it in a
  report rather than guessing.
- **Interrupt context** (an MSI's handler, a tick handler): be quick; never
  sleep, wait, take a mutex, use the heap or print. Take the event's state,
  wake a thread (`wait_queue_wake_all`), and let the thread do the work.
- **Locks.** A spinlock (`spin_lock`/`spin_unlock`, interrupts off while
  held) for short sections and anything an interrupt handler touches; a
  mutex for long ones. Never sleep, take a mutex or allocate while holding
  a spinlock. Give every lock a name.
- **Device registers only through `mmio_read*`/`mmio_write*`** (simple
  loads and stores: a hypervisor emulates nothing else). Order memory
  against the device with `arch_write_barrier` (before a doorbell) and
  `arch_read_barrier` (after seeing a completion).
- **DMA** goes to pages from `pmm_alloc_page(s)` (`_below` for a 32-bit
  device), reached through `phys_to_virt`. A module's own static data is
  not physically contiguous: never give a device its address.
- **Register only what works.** Set the chip up completely (and log why
  not, if it cannot be) before `net_register`.
- **Nothing on disk.** A driver never writes to a disk the user did not
  ask it to. Its firmware, if its chip needs one, is a file in
  `/system/firmware/` that `firmware_load` reads whole (from a thread; free
  it with `kfree`); a firmware's licence must allow it on RitterOS's disk
  (ask before adding one).

## Building

**Inside RitterOS's tree:** put `NAME.c` in `drivers/`. `make` builds it
into `build/system/<arch>/drivers/NAME.mod` for both architectures (and
checks it), and the disk image carries it. A driver too big for one file
keeps its parts beside it as `NAME_PART.c` and `NAME_PART.h` and includes
them (`#include "amdgpu_atom.c"` in amdgpu.c): files with an underscore
and headers in `drivers/` are parts, never modules of their own, and every
module is rebuilt when one changes.

**Outside the tree:** copy `drivers/template/` (a working driver for
QEMU's PCI test device, `-device pci-testdev`), set `DRIVER` and `SRCS` in
its Makefile, and point `RITTEROS` at a RitterOS checkout:

```sh
make RITTEROS=/path/to/RitterOS              # build/x86_64/NAME.mod
make RITTEROS=/path/to/RitterOS ARCH=aarch64 # build/aarch64/NAME.mod
```

`drivers/driver.mk` uses the checkout's Zig (`scripts/setup-toolchain.sh`
makes it) and the same flags as the in-tree modules. Install the `.mod` by
copying it, as root, to `/system/<arch>/drivers/` and restarting.

## Testing

- **In QEMU first**, where the hardware has a model: a device QEMU has, or
  a model of our own (`scripts/qemu/`, built into QEMU by
  `scripts/setup-toolchain.sh`; a model must behave as the real chip is
  known to, or a test only proves the driver agrees with the model). The
  tests are `scripts/test.sh` (`ONLY=name ONLY_ARCH=x86 make test`, then
  `ONLY_ARCH=arm`): a driver gets checks on its boot lines, `drivers`, and
  what its device does (`realtek_shell_test`, `drivertemplate_test`).
- A check is only proven when it has been seen to fail: break the driver
  on purpose (a wrong ID in its table, a register off by one) and watch the
  test catch it.
- **On real hardware** before calling it done: only the chip shows whether
  the facts hold. Counters in the driver's report (`details`) tell what
  happened when it does not.

## Sources and licence

RitterOS is GPL-3.0-or-later and its code is written for it. Facts about a
chip (register layouts, sequences, quirks) may come from any source: a
datasheet, another system's driver read for facts. Code is never copied
from elsewhere, whatever its licence. Name where the facts came from in
the driver's opening comment.
